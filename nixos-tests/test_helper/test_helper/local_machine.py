"""Runs a Cloud Hypervisor guest directly on the local host."""

import atexit
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from pathlib import Path
from typing import Any, Literal

from test_driver.logger import TerminalLogger
from test_driver.machine import BaseMachine

from .test_helper import ssh, wait_for_ssh


class LocalMachine(BaseMachine):
    """Hides the local Cloud Hypervisor process lifecycle."""

    def __init__(
        self,
        guest_ip: str,
        guest_mac: str,
        tap_device: str,
        platform: str,
        cloud_hypervisor: Path,
        firmware: Path,
        guest_image: Path,
        image_type: Literal["raw", "qcow2", "vhd", "vhdx"] = "raw",
    ) -> None:
        """Initializes a direct Cloud Hypervisor machine."""
        runtime_dir = Path(tempfile.mkdtemp(prefix="local-machine-"))
        super().__init__(
            out_dir=Path.cwd(),
            name="local",
            logger=TerminalLogger(),
            tmp_dir=runtime_dir,
            callbacks=[],
            keep_machine_state=False,
        )

        self.guest_ip = guest_ip
        self.guest_mac = guest_mac
        self.tap_device = tap_device
        self.platform = platform
        self.cloud_hypervisor = cloud_hypervisor
        self.firmware = firmware
        self.guest_image = guest_image
        self.image_type = image_type
        self.runtime_dir: Path | None = runtime_dir
        self.api_socket: Path | None = None
        self.serial_log: Path | None = None
        self.ch_log: Path | None = None
        self.process_log: Path | None = None
        self.process: subprocess.Popen[str] | None = None
        self._process_log_file = None
        self._previous_signal_handlers = {
            signum: signal.getsignal(signum)
            for signum in (signal.SIGINT, signal.SIGTERM)
        }
        for signum in self._previous_signal_handlers:
            signal.signal(signum, self._handle_signal)
        atexit.register(self.cleanup)

    def _handle_signal(self, signum: int, _frame: Any) -> None:
        """Cleans up the guest before terminating on an external signal."""
        try:
            self.cleanup()
        finally:
            # Python handlers suppress the default action. Re-deliver with SIG_DFL.
            signal.signal(signum, signal.SIG_DFL)
            os.kill(os.getpid(), signum)

    def _execute(
        self,
        command: str,
        check_return: bool = True,
        check_output: bool = True,
        timeout: int | None = 900,
    ) -> tuple[int, str]:
        """Executes a host command and returns its exit code and output.

        Standard output and standard error are combined. A timeout returns
        exit code 124, the same code the `timeout(1)` wrapper in `QemuMachine`
        produces.
        """
        try:
            proc = subprocess.run(
                ["bash", "-c", f"set -euo pipefail\n{command}"],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or ""
            stderr = error.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")
            return 124, stdout + stderr

        output = proc.stdout + proc.stderr

        # Match BaseMachine._execute. It returns -2 when output is not checked
        # and -1 when the exit code is not checked.
        if not check_output:
            return -2, ""
        if not check_return:
            return -1, output
        return proc.returncode, output

    def _command(self) -> list[str]:
        """Builds the Cloud Hypervisor invocation and records its log paths."""
        if not self.runtime_dir:
            raise RuntimeError("runtime directory is not initialized")

        self.api_socket = self.runtime_dir / "chv.sock"
        self.serial_log = self.runtime_dir / "serial.log"
        self.ch_log = self.runtime_dir / "cloud-hypervisor.log"
        self.process_log = self.runtime_dir / "process.log"

        return [
            str(self.cloud_hypervisor),
            "--api-socket",
            f"path={self.api_socket}",
            "--platform",
            f"{self.platform}=on",
            "--firmware",
            str(self.firmware),
            "--cpus",
            "boot=4",
            "--memory",
            "size=8G",
            # The default guest image is an immutable ISO from isoImage.
            # The test does not write to the disk.
            "--disk",
            f"path={self.guest_image},image_type={self.image_type},readonly=on",
            "--net",
            f"tap={self.tap_device},mac={self.guest_mac}",
            "--serial",
            f"file={self.serial_log}",
            "--console",
            "off",
            "--log-file",
            str(self.ch_log),
        ]

    def _logs(self) -> str:
        """Returns available runtime logs for failure diagnostics."""
        logs = []
        for path in [self.process_log, self.ch_log, self.serial_log]:
            if path and path.exists():
                logs.append(
                    f"\n--- {path.name} ---\n{path.read_text(errors='replace')}"
                )
        return "".join(logs)

    def save_logs(self, destination: Path) -> None:
        """Copies host-side Cloud Hypervisor logs to the test output."""
        destination.mkdir(exist_ok=True, parents=True)
        for path in [self.process_log, self.ch_log, self.serial_log]:
            if path and path.is_file():
                shutil.copy2(path, destination / path.name)

    def _remote(self, command: str) -> str:
        """Executes a Cloud Hypervisor control command."""
        if not self.api_socket:
            raise RuntimeError("Cloud Hypervisor API socket is not initialized")
        ch_remote = self.cloud_hypervisor.with_name("ch-remote")
        result = subprocess.run(
            [str(ch_remote), "--api-socket", str(self.api_socket), command],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"ch-remote {command} failed:\n{result.stdout}{result.stderr}{self._logs()}"
            )
        return result.stdout

    def start(self) -> None:
        """Starts the guest and waits for its control socket."""
        with self.nested("starting guest"):
            self._start()

    def _start(self) -> None:
        """Starts the guest and waits for its control socket."""
        for path in [self.cloud_hypervisor, self.firmware, self.guest_image]:
            if not path.is_file():
                raise RuntimeError(f"Required guest artifact does not exist: {path}")

        command = self._command()
        assert self.process_log is not None
        self._process_log_file = self.process_log.open("w")
        self.process = subprocess.Popen(
            command,
            stdout=self._process_log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )

        # Cloud Hypervisor binds its API socket during early VMM startup.
        # It appears well before the guest boots. The 100 x 100ms wait for
        # the socket is arbitrary.
        assert self.api_socket is not None
        for _ in range(100):
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"Cloud Hypervisor exited during startup{self._logs()}"
                )
            if self.api_socket.exists():
                try:
                    self._remote("ping")
                    return
                except RuntimeError:
                    pass
            time.sleep(0.1)

        raise RuntimeError(
            f"Cloud Hypervisor API socket did not become ready{self._logs()}"
        )

    def wait_for_ssh(self) -> None:
        """Waits until the guest accepts SSH connections."""
        with self.nested("waiting for SSH"):
            try:
                wait_for_ssh(self, ip=self.guest_ip)
            except Exception as error:
                raise RuntimeError(
                    f"Guest did not become reachable{self._logs()}"
                ) from error

    def ssh(self, command: str) -> str:
        """Executes a command in the guest over SSH."""
        with self.nested(f"SSH command: {command}"):
            return ssh(self, command, ip=self.guest_ip)

    def copy_from_machine(self, source: str, target_dir: str = "") -> None:
        """Copies a file or directory from the guest to the test output."""
        source_path = Path(source)
        target = self.out_dir / target_dir / source_path.name
        target.parent.mkdir(exist_ok=True, parents=True)

        sshpass = shutil.which("sshpass")
        scp = shutil.which("scp")
        if sshpass is None or scp is None:
            raise RuntimeError("copying from the guest requires sshpass and scp")

        result = subprocess.run(
            [
                sshpass,
                "-p",
                "root",
                scp,
                "-r",
                "-o",
                "UserKnownHostsFile=/dev/null",
                "-o",
                "StrictHostKeyChecking=no",
                f"root@{self.guest_ip}:{source_path}",
                str(target),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"failed to copy {source} from guest:\n{result.stdout}{result.stderr}"
            )

    def reboot(self) -> None:
        """Reboots the guest and waits for SSH to become available again."""
        with self.nested("rebooting guest"):
            self._remote("reboot")
            self.wait_for_ssh()

    def wait_for_shutdown(self) -> None:
        """Waits for Cloud Hypervisor to exit without initiating shutdown."""
        with self.nested("waiting for guest shutdown"):
            if self.process:
                self.process.wait()

    def shutdown(self) -> None:
        """Requests guest shutdown and waits for Cloud Hypervisor to exit."""
        with self.nested("shutting down guest"):
            if not self.process:
                raise RuntimeError("Cloud Hypervisor is not running")
            self._remote("power-button")
            # The 60s timeout is arbitrary.
            try:
                self.process.wait(timeout=60)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f"Guest did not shut down{self._logs()}") from error

    def is_running(self) -> bool:
        """Reports whether the process and guest VM are running."""
        with self.nested("checking guest state"):
            if self.process is None or self.process.poll() is not None:
                return False

            try:
                info = json.loads(self._remote("info"))
            except (RuntimeError, json.JSONDecodeError):
                return False

            return info.get("state") == "Running"

    def is_up(self) -> bool:
        """Reports whether the direct Cloud Hypervisor guest is running."""
        return self.is_running()

    def _stop_process(self) -> None:
        """Stops Cloud Hypervisor and verifies that its process exited."""
        process = self.process
        if process is None:
            return

        if process.poll() is None:
            # Ask the VMM to exit before we send signals. The API socket may
            # already be gone. Failures here are expected and ignored.
            with suppress(Exception):
                self._remote("shutdown-vmm")

            # Escalate from SIGTERM to SIGKILL if the VMM does not exit.
            for stop in (process.terminate, process.kill):
                try:
                    process.wait(timeout=10)
                    break
                except subprocess.TimeoutExpired:
                    with suppress(ProcessLookupError):
                        stop()
            else:
                # SIGKILL cannot be ignored. A process that is still alive is
                # stuck in uninterruptible I/O. Wait for it instead of leaking
                # it. This can block.
                process.wait()

        if process.poll() is None:
            print(
                f"ERROR: Cloud Hypervisor process {process.pid} could not be cleaned up",
                file=sys.stderr,
            )
        else:
            self.process = None

    def _close_process_log(self) -> None:
        """Closes the Cloud Hypervisor process log."""
        if self._process_log_file:
            self._process_log_file.close()
            self._process_log_file = None

    def _remove_runtime_dir(self) -> None:
        """Removes the temporary runtime directory."""
        if not self.runtime_dir:
            return

        runtime_dir = self.runtime_dir
        shutil.rmtree(runtime_dir, ignore_errors=True)
        if runtime_dir.exists():
            print(
                f"ERROR: runtime directory {runtime_dir} could not be removed",
                file=sys.stderr,
            )
        self.runtime_dir = None

    def _restore_signal_handlers(self) -> None:
        """Restores signal handlers replaced for guest cleanup."""
        atexit.unregister(self.cleanup)
        # Do not leave the handlers installed. They would shadow the test
        # runner's own SIGINT/SIGTERM handling for the rest of the process.
        for signum, handler in self._previous_signal_handlers.items():
            signal.signal(signum, handler)
        self._previous_signal_handlers = {}

    def cleanup(self) -> None:
        """Stops Cloud Hypervisor and removes all transient artifacts."""
        self._stop_process()
        self._close_process_log()
        self._remove_runtime_dir()
        self._restore_signal_handlers()
