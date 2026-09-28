import os
import sys
import textwrap
import time
import unittest
from pathlib import Path
from typing import Literal, cast

from colorama import Fore, Style

# This import supports both IDE usage from the source tree and execution
# through the Nix test wrapper.
try:
    from ..test_helper.local_machine import LocalMachine  # type: ignore
except Exception:
    from test_helper.local_machine import LocalMachine


def assert_tdx_guest_is_initialized(machine: LocalMachine) -> None:
    """Checks that the guest exposes and initializes TDX."""
    machine.ssh("test -c /dev/tdx_guest")
    # The TDX core support prints this line when it detects a Trust Domain.
    machine.ssh("dmesg --color=never | grep -F 'tdx: Guest detected'")


def assert_tdx_guest_generates_quote(machine: LocalMachine) -> None:
    """Checks that the guest obtains a non-empty quote from the host's Intel
    Quote Generation Service (QGS).

    The test only checks that a quote came back. Checking the signature and
    the quote contents is the verifier's job.
    """
    output = machine.ssh(
        textwrap.dedent("""
        set -euo pipefail

        if ! mountpoint -q /sys/kernel/config; then
            mount -t configfs none /sys/kernel/config
        fi

        # configfs-tsm creates one directory per attestation report. Write the
        # REPORTDATA to `inblob` to trigger quote generation. `outblob` then
        # holds the provider-specific attestation blob.
        report_dir=/sys/kernel/config/tsm/report
        report="$report_dir/report0"
        quote=/tmp/tdx-quote.bin

        mkdir -p "$report"
        trap 'rmdir "$report"' EXIT

        provider=$(cat "$report/provider")
        if [ "$provider" != tdx_guest ]; then
            echo "unexpected TSM report provider: $provider" >&2
            exit 1
        fi

        # The TDX guest driver requires exactly 64 bytes of REPORTDATA. Use a
        # random nonce.
        head -c 64 /dev/urandom > "$report/inblob"

        cat "$report/outblob" > "$quote"

        size=$(wc -c < "$quote")
        if [ "$size" -eq 0 ]; then
            echo "the guest returned an empty TD quote. Is the Intel Quote Generation Service running on the host and listening on /var/run/tdx-qgs/qgs.socket?" >&2
            exit 1
        fi

        echo "TD quote size: $size bytes"
        hexdump -C "$quote"
        """).strip()
    )
    print(output)


class TdxTestResult(unittest.TextTestResult):
    """Prints a colored result line after each test."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.dots = False
        self.showAll = False
        self._start_time = 0.0

    def startTest(self, test: unittest.TestCase) -> None:
        super().startTest(test)
        self._start_time = time.monotonic()
        self.stream.writeln(f"\nRunning test: {test._testMethodName}")

    def _write_result(self, test: unittest.TestCase, status: str, color: str) -> None:
        duration = int(time.monotonic() - self._start_time)
        if self.stream.isatty():
            status = f"{Style.BRIGHT}{color}{status}{Style.RESET_ALL}"
        self.stream.writeln(f"{status} {test._testMethodName} ({duration}s)")

    def addSuccess(self, test: unittest.TestCase) -> None:
        super().addSuccess(test)
        self._write_result(test, "OK", Fore.GREEN)

    def addFailure(self, test, err) -> None:
        super().addFailure(test, err)
        self._write_result(test, "FAILED", Fore.RED)

    def addError(self, test, err) -> None:
        super().addError(test, err)
        self._write_result(test, "FAILED", Fore.RED)


class TdxTests(unittest.TestCase):
    """Exercises confidential computing in an Intel TDX guest directly with Cloud Hypervisor, without libvirt."""

    def setUp(self) -> None:
        # Keep this address and MAC in sync with the dnsmasq reservation in
        # hardware/modules/host-services.nix from the hardware repository.
        # The reservation maps be:e3:00:00:00:01 to 192.168.100.70.
        self.machine = LocalMachine(
            guest_ip="192.168.100.70",
            guest_mac="be:e3:00:00:00:01",
            tap_device="tap15",
            platform="tdx",
            cloud_hypervisor=Path(os.environ["TDX_CLOUD_HYPERVISOR"]),
            firmware=Path(os.environ["TDX_FIRMWARE"]),
            guest_image=Path(os.environ["TDX_IMAGE"]),
            image_type=cast(
                Literal["raw", "qcow2", "vhd", "vhdx"],
                os.environ.get("TDX_IMAGE_TYPE", "raw"),
            ),
        )
        # Cleanups run in reverse order. _save_logs copies the logs before
        # cleanup() deletes the runtime directory.
        self.addCleanup(self.machine.cleanup)
        self.addCleanup(self._save_logs)
        self.machine.start()
        self.machine.wait_for_ssh()

    def _save_logs(self) -> None:
        log_dir = os.environ.get("DBG_LOG_DIR")
        if log_dir:
            self.machine.save_logs(Path(log_dir) / self._testMethodName / "tdx")

    def test_tdx_guest_generates_quote(self) -> None:
        """Verifies that the guest can obtain a TD quote from the host QGS."""
        assert_tdx_guest_generates_quote(self.machine)

    def test_tdx_guest_is_initialized(self) -> None:
        """Verifies that the TDX guest is initialized."""
        assert_tdx_guest_is_initialized(self.machine)

    def test_tdx_guest_shuts_down(self) -> None:
        """Verifies that the TDX guest can shut down cleanly."""
        self.machine.shutdown()
        self.assertFalse(self.machine.is_running())

    def test_tdx_guest_survives_reboot(self) -> None:
        """Verifies that TDX remains active after a guest reboot."""
        self.machine.reboot()
        assert_tdx_guest_is_initialized(self.machine)


def suite() -> unittest.TestSuite:
    testcases = [
        TdxTests.test_tdx_guest_generates_quote,
        TdxTests.test_tdx_guest_is_initialized,
        TdxTests.test_tdx_guest_shuts_down,
        TdxTests.test_tdx_guest_survives_reboot,
    ]

    test_suite = unittest.TestSuite()
    for testcase_method in testcases:
        test_suite.addTest(TdxTests(testcase_method.__name__))
    return test_suite


runner = unittest.TextTestRunner(resultclass=TdxTestResult)
if not runner.run(suite()).wasSuccessful():
    sys.exit(1)
