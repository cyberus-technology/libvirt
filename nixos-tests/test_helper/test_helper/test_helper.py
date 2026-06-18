import ipaddress
import json
import os
import time
import unittest
from unittest import TestCase

try:
    from .nixos_test_stubs import QemuMachine  # type: ignore
except ImportError:
    pass

from test_driver.machine import QemuMachine  # type: ignore
from typing import Any, Callable, List, Literal, Self

# VIRTIO PCI constants
VIRTIO_NETWORK_DEVICE = "1af4:1041"
VIRTIO_BLOCK_DEVICE = "1af4:1042"
VIRTIO_ENTROPY_SOURCE = "1af4:1044"
COMMAND_TIMEOUT_EXIT_CODES = {124, 125}
VIRTCHD_RESTART_TIMEOUT_SEC = 15
CLOUD_HYPERVISOR_EXIT_RETRIES = 50
MAX_EXPECTED_WAIT_SEC = 60


def testcase_start_marker(name: str) -> str:
    """
    Returns the marker in the (journalctl) log unambiguously identifying the
    start of a certain test case.

    :param name: Test case
    :return: Marker
    """
    return f"Running test: {name}"


class LibvirtTestsBase(unittest.TestCase):
    """
    Custom test base class handling multiple things:
    * per test case setup and teardown
    * log exporting on error
    """

    def __init__(
        self,
        methodName,
        controllerVM: QemuMachine | None,
        computeVM: QemuMachine | None = None,  # Allow for tests with only a single VM
    ):
        super().__init__(methodName)
        self.controllerVM = controllerVM
        self.computeVM = computeVM
        self.test_start_time = 0

    def setUp(self):
        if self.controllerVM:
            setupTestControllerVM(self.controllerVM, self)

        if self.computeVM:
            setupTestComputeVM(self.computeVM, self)
        print(f"\n\n{testcase_start_marker(self._testMethodName)}\n\n")
        self.test_start_time = time.time()

    def tearDown(self):
        if self.controllerVM:
            teardownTestControllerVM(self.controllerVM, self)

        if self.computeVM:
            teardownTestComputeVM(self.computeVM, self)

        duration_s = int(time.time() - self.test_start_time)
        print(f"\n\nRan test {self._testMethodName} in {duration_s}s\n\n")

    def run(self, result=None):
        if result is None:
            result = self.defaultTestResult()

        original_addError = result.addError
        original_addFailure = result.addFailure

        def custom_addError(test, err):
            self.save_logs(test, f"Error in {test._testMethodName}")
            original_addError(test, err)

        def custom_addFailure(test, err):
            self.save_logs(test, f"Failure in {test._testMethodName}")
            original_addFailure(test, err)

        result.addError = custom_addError
        result.addFailure = custom_addFailure

        return super().run(result)

    def save_machine_log(self, machine: QemuMachine, log_path, dst_path):
        try:
            machine.copy_from_machine(log_path, dst_path)
        # Non-existing logs lead to an Exception that we ignore
        except Exception:
            pass

    def save_machine_journal(self, machine: QemuMachine, test, dst_path):
        marker = testcase_start_marker(test._testMethodName)
        machine.execute(
            f"journalctl --quiet | sed -n '/{marker}/,$p' > /tmp/journalctl.log"
        )
        self.save_machine_log(machine, "/tmp/journalctl.log", dst_path)

    def save_logs(self, test, message):
        print(f"{message}")

        if "DBG_LOG_DIR" not in os.environ:
            return

        for machine in [
            m for m in [self.controllerVM, self.computeVM] if m is not None
        ]:
            dst_path = os.path.join(
                os.environ["DBG_LOG_DIR"], f"{test._testMethodName}", f"{machine.name}"
            )
            self.save_machine_log(machine, "/var/log/libvirt/ch/testvm.log", dst_path)
            self.save_machine_log(machine, "/var/log/libvirt/libvirtd.log", dst_path)
            self.save_machine_log(machine, "/tmp/vm_serial.log", dst_path)
            self.save_machine_journal(machine, test, dst_path)


def initialControllerVMSetup(
    controllerVM: QemuMachine, target_os: Literal["linux", "windows"] = "linux"
) -> None:
    """
    This method configures the controllerVM initially, before the test
    suite runs. It sets up e.g. the NFS share with the correct OS
    images.

    :param controllerVM: machine object of the controllerVM
    :param target_os: If "windows", prepare the NFS with the Windows
        Server image. Otherwise places the Linux images in the NFS.
    :raises RuntimeError: If the machine object is not the controllerVM
    """
    if controllerVM.name != "controllerVM":
        raise RuntimeError(
            f"Setup method called with unexpected VM {controllerVM.name}"
        )

    controllerVM.wait_for_unit("multi-user.target")
    if target_os == "windows":
        controllerVM.succeed("cp /etc/windows-server.img /nfs-root/windows-server.img")
        controllerVM.succeed("chmod 0666 /nfs-root/windows-server.img")
    else:
        controllerVM.succeed("cp /etc/nixos.img /nfs-root/")
        controllerVM.succeed("chmod 0666 /nfs-root/nixos.img")
        controllerVM.succeed("cp /etc/cirros.img /nfs-root/")
        controllerVM.succeed("chmod 0666 /nfs-root/cirros.img")
        controllerVM.succeed("cp /etc/ubuntu.raw /nfs-root/")
        controllerVM.succeed("chmod 0666 /nfs-root/ubuntu.raw")
        controllerVM.succeed("cp /etc/ubuntu-seed.iso /nfs-root/")
        controllerVM.succeed("chmod 0666 /nfs-root/ubuntu-seed.iso")

    controllerVM.succeed("mkdir -p /var/lib/libvirt/storage-pools/nfs-share")

    controllerVM.wait_until_succeeds(
        "ssh -n -o BatchMode=yes -o UserKnownHostsFile=/dev/null "
        "-o StrictHostKeyChecking=no computeVM true",
        timeout=MAX_EXPECTED_WAIT_SEC,
    )

    controllerVM.succeed(
        'virsh pool-define-as --name "nfs-share" --type netfs --source-host "localhost" --source-path "nfs-root" --source-format "nfs" --target "/var/lib/libvirt/storage-pools/nfs-share"'
    )
    controllerVM.succeed("virsh pool-start nfs-share")

    # Define a libvirt network and automatically starts it
    controllerVM.succeed("virsh net-create /etc/libvirt_test_network.xml")


def initialComputeVMSetup(computeVM: QemuMachine) -> None:
    """
    This method configures the computeVM initially, before the test suite
    runs. It sets up e.g. the NFS share in client mode.

    :param computeVM: machine object of the computeVM
    :raises RuntimeError: If the machine object is not the computeVM
    """
    if computeVM.name != "computeVM":
        raise RuntimeError(f"Setup method called with unexpected VM {computeVM.name}")

    computeVM.wait_for_unit("multi-user.target")
    computeVM.succeed("mkdir -p /var/lib/libvirt/storage-pools/nfs-share")

    computeVM.wait_until_succeeds(
        "ssh -n -o BatchMode=yes -o UserKnownHostsFile=/dev/null "
        "-o StrictHostKeyChecking=no controllerVM true",
        timeout=MAX_EXPECTED_WAIT_SEC,
    )

    computeVM.succeed(
        'virsh pool-define-as --name "nfs-share" --type netfs --source-host "controllerVM" --source-path "nfs-root" --source-format "nfs" --target "/var/lib/libvirt/storage-pools/nfs-share"'
    )
    computeVM.succeed("virsh pool-start nfs-share")


def assert_domain_domstate(
    machine: QemuMachine,
    expected_state: Literal["running", "paused", "shut off"],
    vm_name: str = "testvm",
) -> None:
    actual_state = machine.succeed(f"virsh domstate {vm_name}")

    if expected_state not in actual_state:
        raise AssertionError(
            f"expected {vm_name!r} to be {expected_state!r}, got {actual_state!r}"
        )


def _kill_cloud_hypervisor(machine: QemuMachine) -> bool:
    status, _ = machine.execute("pidof cloud-hypervisor")
    if status != 0:
        return False

    machine.execute("kill -9 $(pidof cloud-hypervisor)")
    wait_until_succeed(
        lambda: machine.execute("pidof cloud-hypervisor")[0] != 0,
        retries=CLOUD_HYPERVISOR_EXIT_RETRIES,
    )
    return True


def restart_virtchd(
    machine: QemuMachine, timeout_sec: int = VIRTCHD_RESTART_TIMEOUT_SEC
) -> None:
    """
    Restart virtchd during teardown.

    If the restart times out, kill a still-running cloud-hypervisor process and
    retry once for cleanup. Requiring that kill still fails teardown because it
    indicates cloud-hypervisor hung during shutdown.

    :param machine: machine object used to run the command
    :param timeout_sec: maximum runtime of a single restart attempt
    :raises AssertionError: If the restart fails or killing CHV becomes
        necessary
    """
    status, out = machine.execute("systemctl restart virtchd", timeout=timeout_sec)
    if status == 0:
        return

    if status not in COMMAND_TIMEOUT_EXIT_CODES:
        raise AssertionError(
            f"systemctl restart virtchd failed\nstatus={status}\noutput:\n{out}"
        )

    killed_chv = _kill_cloud_hypervisor(machine)
    retry_status, retry_out = machine.execute(
        "systemctl restart virtchd", timeout=timeout_sec
    )
    if killed_chv:
        raise AssertionError(
            "systemctl restart virtchd timed out because cloud-hypervisor hung\n"
            f"initial_status={status}\n"
            f"initial_output:\n{out}\n"
            f"retry_status={retry_status}\n"
            f"retry_output:\n{retry_out}"
        )

    if retry_status != 0:
        raise AssertionError(
            "systemctl restart virtchd failed after retrying a timeout\n"
            f"initial_status={status}\n"
            f"initial_output:\n{out}\n"
            f"retry_status={retry_status}\n"
            f"retry_output:\n{retry_out}"
        )


def setupTestControllerVM(controllerVM: QemuMachine, test: unittest.TestCase) -> None:
    if controllerVM.name != "controllerVM":
        raise RuntimeError(
            f"Setup method called with unexpected VM {controllerVM.name}"
        )
    # A restart of the libvirt daemon resets the logging configuration, so
    # apply it freshly for every test
    controllerVM.succeed(
        'virt-admin -c virtchd:///system daemon-log-outputs "2:journald 1:file:/var/log/libvirt/libvirtd.log"'
    )
    controllerVM.succeed("virt-admin -c virtchd:///system daemon-timeout --timeout 0")

    # In order to be able to differentiate the journal log for different
    # tests, we print a message with the test name as a marker
    controllerVM.succeed(
        f'echo "{testcase_start_marker(test._testMethodName)}" | systemd-cat -t testscript -p info'
    )


def setupTestComputeVM(computeVM: QemuMachine, test: unittest.TestCase) -> None:
    if computeVM.name != "computeVM":
        raise RuntimeError(f"Setup method called with unexpected VM {computeVM.name}")

    # A restart of the libvirt daemon resets the logging configuration, so
    # apply it freshly for every test
    computeVM.succeed(
        'virt-admin -c virtchd:///system daemon-log-outputs "2:journald 1:file:/var/log/libvirt/libvirtd.log"'
    )
    computeVM.succeed("virt-admin -c virtchd:///system daemon-timeout --timeout 0")

    # In order to be able to differentiate the journal log for different
    # tests, we print a message with the test name as a marker
    computeVM.succeed(
        f'echo "{testcase_start_marker(test._testMethodName)}" | systemd-cat -t testscript -p info'
    )


def tearDownCommands(test: unittest.TestCase) -> List[str]:
    """
    Return a list of strings of generic commands used for the cleanup on all
    host VMs (e.g. controllerVM).

    :param test: Test case currently tearing down
    """
    return [
        # Ensure we can access specific test case logs afterward.
        f"mv /var/log/libvirt/ch/testvm.log /var/log/libvirt/ch/{test._testMethodName}_vmm.log || true",
        # libvirt bug: can't cope with new or truncated log files
        # f"mv /var/log/libvirt/libvirtd.log /var/log/libvirt/{timestamp}_{self._testMethodName}_libvirtd.log",
        f"mv /var/log/vm_serial.log /var/log/{test._testMethodName}_vm-serial.log || true",
        # Various cleanup commands to be executed on all machines
        "rm -f /tmp/*.expect",
    ]


def seccompViolationJournalCmd(test: unittest.TestCase) -> str:
    """
    Checks if CH caused seccomp violations by looking for audit
    messages of type AUDIT_SECCOMP(1326) in the kernel log.
    """
    return (
        "journalctl --quiet SYSLOG_IDENTIFIER=kernel + SYSLOG_IDENTIFIER=testscript "
        f"| sed -n '/{testcase_start_marker(test._testMethodName)}/,$p' "
        "| grep '\\btype=1326\\b'"
    )


def teardownTestControllerVM(
    controllerVM: QemuMachine, test: unittest.TestCase
) -> None:
    """
    Tear down of the actual test case on the controllerVM. Takes care of
    resetting the nixos image back to the golden state.

    :param controllerVM: controllerVM object reference
    :param test: the current test case to tear down
    """
    if controllerVM.name != "controllerVM":
        raise RuntimeError(
            f"Setup method called with unexpected VM {controllerVM.name}"
        )

    # Trigger output of the sanitizers. At least the leak sanitizer output
    # is only triggered if the program under inspection terminates.
    restart_virtchd(controllerVM)

    # Make sure there are no reports of the sanitizers. We retrieve the
    # journal for only the recent test run, by looking for the test run
    # marker. We then check for any ERROR messages of the sanitizers.
    jrnCmd = f"journalctl _SYSTEMD_UNIT=virtchd.service + SYSLOG_IDENTIFIER=testscript | sed -n '/{testcase_start_marker(test._testMethodName)}/,$p' | grep ERROR"
    statusController, outController = controllerVM.execute(jrnCmd)

    # Destroy and undefine all running and persistent domains
    controllerVM.execute(
        'virsh list --name | while read domain; do [[ -n "$domain" ]] && virsh destroy "$domain"; done'
    )
    controllerVM.execute(
        'virsh list --all --name | while read domain; do [[ -n "$domain" ]] && virsh undefine "$domain"; done'
    )

    # After undefining and destroying all domains, there should not be any .xml files left
    # Any files left here, indicate that we do not clean up properly
    controllerVM.fail("find /run/libvirt/ch -name *.xml | grep .")
    controllerVM.fail("find /var/lib/libvirt/ch -name *.xml | grep .")

    for cmd in tearDownCommands(test):
        print(f"cmd: {cmd}")
        controllerVM.succeed(cmd)

    # Reset the (possibly modified) system image. This helps avoid
    # situations where the image has been modified by a test and thus
    # doesn't boot in subsequent tests.
    controllerVM.succeed(
        "rsync -aL --no-perms --inplace --checksum /etc/nixos.img /nfs-root/nixos.img"
    )

    # Currently, we don't store any data about if we copied the windows image to
    # the NFS. We therefore have to check if it's there before resetting it
    # `test -e` returns 0 if a file exists and 1 otherwise.
    test_return_code, _ = controllerVM.execute("test -e /nfs-root/windows-server.img")
    if test_return_code == 0:
        controllerVM.succeed(
            "rsync -aL --no-perms --inplace --checksum /etc/windows-server.img /nfs-root/windows-server.img"
        )

    # Check the sanitizer last, as the assertion can fail. Otherwise, we might
    # skip some clean up.
    test.assertNotEqual(
        statusController, 0, msg=f"Sanitizer detected an issue: {outController}"
    )

    # Check the the kernel log since the test began for seccomp violations
    _, seccompOut = controllerVM.execute(seccompViolationJournalCmd(test))
    if seccompOut != "":
        raise RuntimeError(f"Cloud Hypervisor caused seccomp violations\n{seccompOut}")


def teardownTestComputeVM(computeVM: QemuMachine, test: unittest.TestCase) -> None:
    """
    Tear down of the actual test case on the computeVM.

    :param computeVM: computeVM object reference
    :param test: the current test case to tear down
    """
    if computeVM.name != "computeVM":
        raise RuntimeError(f"Setup method called with unexpected VM {computeVM.name}")

    # Trigger output of the sanitizers. At least the leak sanitizer output
    # is only triggered if the program under inspection terminates.
    restart_virtchd(computeVM)

    # Make sure there are no reports of the sanitizers. We retrieve the
    # journal for only the recent test run, by looking for the test run
    # marker. We then check for any ERROR messages of the sanitizers.
    jrnCmd = f"journalctl _SYSTEMD_UNIT=virtchd.service + SYSLOG_IDENTIFIER=testscript | sed -n '/Running test: {test._testMethodName}/,$p' | grep ERROR"
    statusCompute, outCompute = computeVM.execute(jrnCmd)

    # Destroy and undefine all running and persistent domains
    computeVM.execute(
        'virsh list --name | while read domain; do [[ -n "$domain" ]] && virsh destroy "$domain"; done'
    )
    computeVM.execute(
        'virsh list --all --name | while read domain; do [[ -n "$domain" ]] && virsh undefine "$domain"; done'
    )

    # After undefining and destroying all domains, there should not be any .xml files left
    # Any files left here, indicate that we do not clean up properly
    computeVM.fail("find /run/libvirt/ch -name *.xml | grep .")
    computeVM.fail("find /var/lib/libvirt/ch -name *.xml | grep .")

    for cmd in tearDownCommands(test):
        print(f"cmd: {cmd}")
        computeVM.succeed(cmd)

    test.assertNotEqual(
        statusCompute, 0, msg=f"Sanitizer detected an issue: {outCompute}"
    )

    # Check the the kernel log since the test began for seccomp violations
    _, seccompOut = computeVM.execute(seccompViolationJournalCmd(test))
    if seccompOut != "":
        raise RuntimeError(f"Cloud Hypervisor caused seccomp violations\n{seccompOut}")


class CommandGuard:
    """
    Python context manager that executes a cleanup command when leaving a scope.

    The cleanup command is run when the with-block exits, both on success and
    on failure.
    """

    def __init__(
        self,
        command: Callable[[Any], None],
        machine: QemuMachine,
        machine2: QemuMachine | None = None,
    ) -> None:
        """
        Initializes the guard with a cleanup command for a given machine.

        :param command: Cleanup function to call
        :param machine: QemuMachine passed to the cleanup function
        :param machine2: Second machine passed to the cleanup function
        """

        self._command = command
        self._machine = machine
        self._machine2 = machine2

    def __enter__(self) -> Self:
        """
        Enters the guarded scope.

        :return: This guard instance
        """

        return self

    def __exit__(
        self,
        _exc_type,
        _exc_val,
        _exc_tb,
    ) -> bool:
        """
        Exits the guarded scope and runs cleanup.

        Returning False keeps exceptions from the with-block visible.
        """

        self._command(self._machine)
        if self._machine2 is not None:
            self._command(self._machine2)
        return False


def MigrationThrottleGuard(
    controllerVM: QemuMachine, computeVM: QemuMachine
) -> CommandGuard:
    """
    Creates a guard that throttles the migration.
    """
    start_migration_throttling(controllerVM)
    start_migration_throttling(computeVM)
    return CommandGuard(
        lambda machine: stop_migration_throttling(machine), controllerVM, computeVM
    )


def stop_migration_throttling(machine: QemuMachine) -> None:
    """
    Remove the controllerVM migration network bandwidth limit.
    """
    machine.execute("tc qdisc del dev eth1 root")


def start_migration_throttling(machine: QemuMachine, bandwidth: str = "1gbit") -> None:
    """
    Limit controllerVM migration network bandwidth to 1gbit/s for outgoing
    traffic.

    This enables timing-sensitive use-cases, such as aborting live migrations.
    """

    # Attach a CAKE shaping qdisc to eth1's egress path, hard-capping outbound
    # throughput at 1 Gbit/s. CAKE (sch_cake.ko, mainline since Linux 4.19)
    # handles burst and scheduling internally, unlike TBF which requires manual
    # HZ-dependent burst tuning.
    machine.succeed(f"tc qdisc add dev eth1 root cake bandwidth {bandwidth}")


def measure_ms(func: Callable[[], Any]) -> float:
    """
    Measure the execution time of a given function in ms.
    """
    start = time.time()
    func()
    return (time.time() - start) * 1000


def wait_until_succeed(func: Callable[[], bool], retries: int = 600) -> None:
    """
    Waits for the command to succeed.
    After each failure, it waits 100ms.

    :param func: Function that is expected to succeed. It must not throw
           exceptions but translate its exceptions to True/False.
    :param retries: Amount of retries.
    """
    for _i in range(retries):
        if func():
            return
        time.sleep(0.1)
    raise RuntimeError("function didn't succeed")


def wait_until_fail(func: Callable[[], bool], retries: int = 600) -> None:
    """
    Waits for the command to fail.
    After each success, it waits 100ms.

    :param func: Function that is expected to fail. It must not throw
           exceptions but translate its exceptions to True/False.
    :param retries: Amount of retries.
    """
    for i in range(retries):
        if not func():
            return
        time.sleep(0.1)
    raise RuntimeError("function didn't fail")


def wait_for_host_shares_ipv4_network(
    machine: QemuMachine, ip: str = "192.168.1.2", retries=20
):
    """
    Wait until the host has an IPv4 address in the same /24 network as the
    given IP address.

    This is used in tests that dynamically create TAP devices. Since udev and
    systemd-networkd configure interfaces asynchronously, the host may need a
    short grace period before the correct IPv4 address is assigned. To avoid
    race conditions, the check is retried a small amount of times.

    Under normal conditions, this returns immediately. If the condition is not
    met after all retries, a RuntimeError is raised and includes the output of
    `ip a` for debugging.

    :param machine: Host
    :param ip: IP to wait for
    :param retries: Amount of retries
    """

    try:
        wait_until_succeed(
            lambda: ip_in_local_192_168_net24(machine, ip), retries=retries
        )
    except RuntimeError as e:
        _, output = machine.execute("ip a")
        msg = f"Host doesn't have an IP in the same IPv4 network as {ip}!\nip a:\n{output}"
        raise RuntimeError(msg) from e


def wait_for_ping(
    machine: QemuMachine, ip: str = "192.168.1.2", retries: int = 200
) -> None:
    """
    Waits for the VM to become pingable.

    Calls `wait_for_host_shares_ipv4_network()` beforehand.

    :param machine: VM host
    :param ip: IP to ping from `machine`
    :param retries: number of retries
    :raises RuntimeError: If the IP could not be pinged after `retries` times.
    """

    wait_for_host_shares_ipv4_network(machine, ip)

    for i in range(retries):
        print(f"Checking ping to {ip} ({i + 1}/{retries}) ...")
        status, _ = machine.execute(f"ping -c 1 -W 1 {ip}")
        if status == 0:
            return
        time.sleep(0.2)

    raise RuntimeError(f"{ip} does not respond to pings after {retries} attempts")


def wait_for_ssh(
    machine: QemuMachine,
    user: str = "root",
    password: str = "root",
    ip: str = "192.168.1.2",
    retries: int = 100,
) -> None:
    """
    Waits for the VM to become accessible via SSH.

    Calls `wait_for_ping()` beforehand.

    :param machine: VM host
    :param user: user for SSH login
    :param password: password for SSH login
    :param ip: SSH host to log into
    :param retries: Retries for the SSH. Note that this adds on top of the
           retries that are done in `wait_for_ping()`.
    """

    # We first check pings, before we connect to the SSH daemon.
    wait_for_ping(machine, ip)

    for i in range(retries):
        print(f"Wait for ssh {i}/{retries}")
        try:
            ssh(
                machine,
                "echo hello",
                user,
                password,
                ip,
                # 1: we checked ping above already
                # 2: we want to prevent unnecessary log clutter
                ping_check=False,
            )
            return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError(f"Could not establish SSH connection to {ip}")


def ssh(
    machine: QemuMachine,
    cmd: str,
    user: str = "root",
    password: str = "root",
    ip: str = "192.168.1.2",
    ping_check: bool = True,
    extra_ssh_params: str = "",
) -> str:
    """
    Runs the specified command in the Cloud Hypervisor VM via SSH.

    The specified machine is used as SSH jump host.

    :param machine: QemuMachine to run SSH on
    :param cmd: The command to execute via SSH
    :param user: user for SSH login
    :param password: password for SSH login
    :param ip: SSH host to log into
    :param ping_check: whether the function should check first if the VM is
           pingable
    :param extra_ssh_params: extra SSH params
    """

    # Check VM is still pingable and we didn't lose the network.
    # This way, we prevent spammy logs.
    if ping_check:
        # One retry is fine as tests gracefully wait for pings+ssh before
        # calling this function.
        wait_for_ping(machine, ip, retries=1)

    # And here we check if the guest also responds via SSH.
    status, out = machine.execute(
        f"sshpass -p {password} ssh {extra_ssh_params} -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no {user}@{ip} {cmd}"
    )
    if status != 0:
        raise RuntimeError(f"failed to execute cmd in VM: `{cmd}`")
    return out


def number_of_devices(machine: QemuMachine, filter: str = "") -> int:
    """
    Returns the number of PCI devices in the VM.

    :param filter: Optional filter for the PCI device, e.g., vendor ID or device class.
    :param machine: VM host
    :return: number of PCI devices in VM
    :return: output from command
    """
    if filter == "":
        cmd = "lspci | wc -l"
    else:
        cmd = f"lspci -n | grep {filter} | wc -l"
    out = ssh(machine, cmd)
    return int(out)


def number_of_network_devices(machine: QemuMachine) -> int:
    """
    Returns the number of PCI virtio-net devices in the VM.

    :param machine: VM host
    :return: number of PCI devices in VM
    """
    PCI_CLASS_GENERIC_ETHERNET_CONTROLLER = "0200"
    return number_of_devices(machine, PCI_CLASS_GENERIC_ETHERNET_CONTROLLER)


def number_of_storage_devices(machine: QemuMachine) -> int:
    """
    Returns the number of PCI virtio-blk devices in the VM.

    :param machine: VM host
    :return: number of PCI devices in VM
    """
    PCI_CLASS_GENERIC_STORAGE_CONTROLLER = "0180"
    return number_of_devices(machine, PCI_CLASS_GENERIC_STORAGE_CONTROLLER)


def hotplug(machine: QemuMachine, cmd: str, expect_success: bool = True) -> None:
    """
    Hotplugs (attaches or detaches) a device and waits for the guest to
    acknowledge that.

    :param machine: The VM host
    :param cmd: virsh command to perform the detach or attach
    :param expect_success: whether the command is expected to succeed
    :return:
    """
    if cmd.startswith("virsh attach-"):
        is_attach = True
    elif cmd.startswith("virsh detach-"):
        is_attach = False
    else:
        raise RuntimeError(
            f"command is neither `virsh attach-*` nor `virsh detach-*`: `{cmd}`"
        )

    num_old = number_of_devices(machine)
    num_new_expected = -1

    match (is_attach, expect_success):
        case (True, True):
            machine.succeed(cmd)
            num_new_expected = num_old + 1
        case (True, False):
            machine.fail(cmd)
            num_new_expected = num_old
        case (False, True):
            machine.succeed(cmd)
            num_new_expected = num_old - 1
        case (False, False):
            machine.fail(cmd)
            num_new_expected = num_old

    wait_for_guest_pci_device_enumeration(machine, num_new_expected)


def hotplug_fail(machine: QemuMachine, cmd: str) -> None:
    """
    Hotplugs (attaches or detaches) a device and expect that to fail.

    :param machine: The VM host
    :param cmd: virsh command to perform the detach or attach
    :return:
    """
    hotplug(machine, cmd, False)


def reset_system_image(machine: QemuMachine) -> None:
    """
    Replaces the (possibly modified) system image with its original
    image.

    This helps avoid situations where a VM hangs during boot after the
    underlying disk’s BDF was changed, since OVMF may store NVRAM
    entries that reference specific BDF values.
    """
    machine.succeed(
        "rsync -aL --no-perms --inplace --checksum /etc/nixos.img /nfs-root/nixos.img"
    )


def pci_devices_by_bdf(machine: QemuMachine) -> dict[str, str]:
    """
    Creates a dict of all PCI devices addressable by their BDF in the VM.

    BDFs are keys, while the combination of vendor and device IDs form the
    associated value.

    :param machine: Host machine of the nested VM
    :return: BDF mapped to devices, example: {'00:00.0': '8086:0d57'}
    :rtype: dict[str, str]
    """
    lines = ssh(
        machine,
        "lspci -n | awk '/^[0-9a-f]{2}:[0-9a-f]{2}\\.[0-9]/{bdf=$1}{class=$3} {print bdf \",\" class}'",
    )
    out = {}
    for line in lines.splitlines():
        bdf, device_class = line.split(",")
        out[bdf] = device_class
    return out


def wait_for_guest_pci_device_enumeration(machine: QemuMachine, new_count: int) -> None:
    """
    Block until the guest operating system has observed a PCI topology change
    (hotplug or unplug) by verifying that the number of enumerated PCI devices
    matches the expected value.

    Guest-side acknowledgment is inferred by polling the PCI device count
    (via `lspci`) until it converges to `expected_count`.

    Raises a RuntimeError if the expected PCI device count is not observed
    within the retry window, otherwise continues.

    :param machine: VM host
    :param new_count: New device count
    :return:
    """
    # retries=20 => max 2s => we expect hotplug events to be relatively quick
    wait_until_succeed(lambda: number_of_devices(machine) == new_count, 20)


def number_of_free_hugepages(machine: QemuMachine) -> int:
    """
    Returns the number of free hugepages on the given machine.

    :param machine: VM host
    :return: Number of free hugepages
    """
    _, out = machine.execute("awk '/HugePages_Free/ { print $2; exit }' /proc/meminfo")
    return int(out)


def allocate_hugepages(machine: QemuMachine, nr_hugepages: int) -> None:
    """
    Allocates the given amount of hugepages on the given machine, and checks
    whether the allocation was successful.

    Raises a RuntimeError if the amount of available hugepages after allocation
    is below the number of expected hugepages.

    :param machine: VM host
    :param nr_hugepages: The amount of desired hugepages
    :return:
    """
    machine.succeed(f"echo {nr_hugepages} > /proc/sys/vm/nr_hugepages")

    # To make sure that allocating the hugepages doesn't just take a moment,
    # we check few times.
    wait_until_succeed(lambda: number_of_free_hugepages(machine) == nr_hugepages, 10)


def get_local_192_168_net24_networks(
    machine: QemuMachine,
) -> List[ipaddress.IPv4Network]:
    """
    Discover local IPv4 interface networks that match all the following:
      - IPv4 only
      - Prefix length exactly /24
      - Network is within 192.168.0.0/16

    :param machine: QemuMachine to execute the SSH command on.
    """
    status, result = machine.execute("ip -j a")
    assert status == 0

    interfaces: list[dict[str, object]] = json.loads(result)
    networks: List[ipaddress.IPv4Network] = []

    for iface in interfaces:
        addr_info = iface.get("addr_info")
        if not isinstance(addr_info, list):
            continue

        for addr in addr_info:
            if not isinstance(addr, dict):
                continue

            family = addr.get("family")
            ip = addr.get("local")
            prefix = addr.get("prefixlen")

            if (
                family == "inet"
                and isinstance(ip, str)
                and isinstance(prefix, int)
                and prefix == 24
            ):
                network = ipaddress.IPv4Network(f"{ip}/24", strict=False)

                if network.network_address in ipaddress.IPv4Network("192.168.0.0/16"):
                    networks.append(network)

    return sorted(networks)


def ip_in_local_192_168_net24(machine: QemuMachine, ip: str) -> bool:
    """
    Checks if the given IPv4 address belongs to one of the machine's local
    192.168.x.0/24 networks.

    The function enumerates all local /24 networks in the private
    192.168.0.0/16 range configured on the machine's network interfaces and
    checks whether the provided IP address is contained in any of them.

    This is primarily intended as a sanity check to verify that the host
    is still on a network that can directly reach a guest VM via a
    192.168.x.x address (e.g. after network reconfiguration, hotplug,
    or unintended interface changes).

    :param machine: QemuMachine on which local network interfaces are inspected.
    :param ip: Target IPv4 address expected to be reachable via a local /24
               192.168.x.0 network.
    :return: Whether the host shares a local IPv4 network with the given IP.
    """
    target = ipaddress.IPv4Address(ip)

    if not target.is_private or not target.exploded.startswith("192.168."):
        raise RuntimeError(f"invalid IP: {ip} / {target}")

    networks = get_local_192_168_net24_networks(machine)
    for network in networks:
        if target in network:
            return True

    return False


def parse_devices_from_dom_def(machine: QemuMachine, path: str) -> dict[str, str]:
    """
    Parses `devices` from a domain XML given by `path` on `machine` and returns them in a dict.

    The dict returned contains the device PCI slot as keys and an identification string of a device as value. The string
    differs between device types.

    :param path: Location of the domain definition on `machine`
    :param machine: Host on which the domain definition is located
    :return: dict[str, str] = ['<PCI slot in hex>' : '<info:about:device>']

    :raises RuntimeError: If we couldn't find the domain XML on the target machine
    """
    import xml.etree.ElementTree as ET

    command = "cat " + path
    status, cat_result = machine.execute(command)
    if status != 0:
        raise RuntimeError(
            f"unable to retrieve domain config from {path} on machine {machine}"
        )

    root = ET.fromstring(cat_result)
    result: dict[str, str] = dict()
    # Use ".//devices" because in persistent conf, `devices` is direct child and in transient config it's one more level
    # of nesting.
    for device in root.find(".//devices") or []:
        value = ""
        value += device.tag
        match device.tag:
            case "disk":
                source = device.find("source")
                if source is not None:
                    value += ":" + source.get("file", "").strip()
                target = device.find("target")
                if target is not None:
                    value += ":" + target.get("dev", "").strip()
            case "interface":
                value += ":" + device.attrib.get("type", "")
                mac = device.find("mac")
                if mac is not None:
                    value += ":" + mac.get("address", "")
                target = device.find("target")
                if target is not None:
                    value += ":" + target.get("dev", "").strip()
            case "rng":
                backend = device.find("backend")
                if backend is not None:
                    value += ":" + (backend.text or "").strip()
        address = device.find("address")
        if address is not None:
            if address.get("type") == "pci":
                slot = address.get("slot") or ""
                if slot != "":
                    result[slot] = (value or "").strip()
    return result


def vm_unresponsive(machine: QemuMachine) -> bool:
    """
    Tells whether the VM is unresponsive.
    """
    try:
        ssh(machine, "echo hi")
        return False
    except RuntimeError:
        return True


def tid_of(machine: QemuMachine, thread_name: str) -> int:
    """
    Returns the tid of a given thread name of the first Cloud Hypervisor
    process found.
    """
    chv_pid_controller = int(machine.succeed("pidof cloud-hypervisor").strip())
    # We use grep -w internally to match only whole words, e.g. thread_name=vmm
    # will match to thread vmm but not thread vmm_signal_handler.
    tid = int(
        machine.succeed(
            f"ps -Lo tid,comm --pid {chv_pid_controller} | grep -w {thread_name} | awk '{{print $1}}'"
        ).rstrip()
    )

    return tid


def taskset_of(machine: QemuMachine, tid: int) -> str:
    taskset = machine.succeed(f"taskset -p {tid} | awk '{{print $6}}'").rstrip()

    return taskset


def validate_pinning(machine: QemuMachine, expected_pinning: dict[str, str]):
    """
    Check that the pinning of the Cloud Hypervisor threads are as expected.

    :param machine: The machine where the Cloud Hypervisor process is
                    running
    :param expected_pinning: A dict mapping thread names to expected
                             taskset string values
    :raises RuntimeError: If an actual taskset of a thread is not as expected
    """

    for thread_name, expected_taskset in expected_pinning.items():
        tid = tid_of(machine, thread_name)
        taskset = taskset_of(machine, tid)

        if taskset != expected_taskset:
            raise RuntimeError(
                f"Unexpected Taskset of thread: {thread_name} "
                f"TID: {tid} Taskset: {taskset} "
                f"Expected Taskset: {expected_taskset}"
            )


def vcpu_affinity_checks(testcase: TestCase, machine: QemuMachine, context: str = ""):
    """
    Asserts that vCPU thread CPU affinity matches the pinning defined in
    /etc/domain-chv-numa.xml.

    :param testcase: Testcase
    :param machine: VM host
    :param context: Additional context for logs
    """

    chv_pid_controller = int(machine.succeed("pidof cloud-hypervisor").strip())

    if context != "":
        print(f"Checks {context}:")
    tid_vcpu0_controller = machine.succeed(
        f"ps -Lo tid,comm --pid {chv_pid_controller} | grep vcpu0 | awk '{{print $1}}'"
    ).rstrip()
    tid_vcpu2_controller = machine.succeed(
        f"ps -Lo tid,comm --pid {chv_pid_controller} | grep vcpu2 | awk '{{print $1}}'"
    ).rstrip()

    taskset_vcpu0_controller = machine.succeed(
        f"taskset -p {tid_vcpu0_controller} | awk '{{print $6}}'"
    ).rstrip()
    taskset_vcpu2_controller = machine.succeed(
        f"taskset -p {tid_vcpu2_controller} | awk '{{print $6}}'"
    ).rstrip()

    testcase.assertEqual(
        int(taskset_vcpu0_controller, 16), 0x3, "vCPU taskset should match"
    )
    testcase.assertEqual(
        int(taskset_vcpu2_controller, 16), 0xC, "vCPU taskset should match"
    )


NESTED_CH_API_SOCKET = "/run/nested-chv.sock"
NESTED_CIRROS_DISK = "/root/nested-cirros.img"
NESTED_CIRROS_IP = "192.168.30.42"
NESTED_CIRROS_HOST_IP = "192.168.30.1"
NESTED_CIRROS_NETMASK = "255.255.255.0"
NESTED_CIRROS_TAP = "nestedtap0"
NESTED_CIRROS_MAC = "52:54:00:e5:b9:01"
NESTED_CIRROS_DNSMASQ_LEASEFILE = "/run/nested-cirros-dnsmasq.leases"


def setup_nested_cirros(machine: QemuMachine) -> None:
    """
    Setup a nested cirros VM inside of a already active guest VM running on the
    given 'machine'.
    Thus, we have following chain of virtual machines: machine -> guest ->
    cirros, where guest is our nixos image in the default scenario.
    We use Cirros as the image to boot because it is very small in size and we
    do not need to provide any additional disk space.
    """
    ssh(machine, f"cp /etc/cirros.img {NESTED_CIRROS_DISK}")
    ssh(machine, f"chmod +w {NESTED_CIRROS_DISK}")

    ssh(machine, f"tunctl -t {NESTED_CIRROS_TAP}")
    ssh(machine, f"ip link show dev {NESTED_CIRROS_TAP}")
    ssh(machine, f"ip addr add {NESTED_CIRROS_HOST_IP}/24 dev {NESTED_CIRROS_TAP}")
    ssh(machine, f"ip link set dev {NESTED_CIRROS_TAP} up")
    ssh(machine, f"ip link show dev {NESTED_CIRROS_TAP}")
    ssh(
        machine,
        "dnsmasq "
        f"--interface={NESTED_CIRROS_TAP} "
        "--bind-dynamic "
        "--port=0 "
        "--dhcp-authoritative "
        f"--dhcp-host={NESTED_CIRROS_MAC},{NESTED_CIRROS_IP} "
        f"--dhcp-range={NESTED_CIRROS_IP},{NESTED_CIRROS_IP},{NESTED_CIRROS_NETMASK},12h "
        f"--listen-address={NESTED_CIRROS_HOST_IP} "
        f"--dhcp-leasefile={NESTED_CIRROS_DNSMASQ_LEASEFILE} "
        ">/tmp/nested-cirros-dnsmasq.log 2>&1 &",
    )

    ssh(
        machine,
        "screen -dmS nested-ch cloud-hypervisor "
        f"--api-socket {NESTED_CH_API_SOCKET} "
        "--log-file /tmp/nested-cirros.log "
        "--memory size=256M "
        "--cpus boot=1 "
        "--kernel /etc/CLOUDHV.fd "
        f"--disk path={NESTED_CIRROS_DISK} "
        f"--net tap={NESTED_CIRROS_TAP},mac={NESTED_CIRROS_MAC} "
        "--console off "
        "--serial file=/tmp/nested-serial.log",
    )

    def nested_guest_available():
        try:
            ssh(machine, f"test -S {NESTED_CH_API_SOCKET}")
            ssh(
                machine,
                f"ch-remote --api-socket {NESTED_CH_API_SOCKET} ping",
            )
            return True
        except RuntimeError:
            pass
        return False

    try:
        wait_until_succeed(
            nested_guest_available,
            retries=100,
        )
    except RuntimeError as e:
        ch_log = ssh(machine, "cat /tmp/nested-cirros.log || true")
        dnsmasq_log = ssh(machine, "cat /tmp/nested-cirros-dnsmasq.log || true")
        raise RuntimeError(
            "nested cloud-hypervisor did not start successfully\n"
            f"cloud-hypervisor log:\n{ch_log}\n"
            f"dnsmasq log:\n{dnsmasq_log}\n"
        ) from e


def assert_nested_cirros_connectivity(machine: QemuMachine) -> None:
    """
    Test if the nested guest is alive and reachable over its DHCP-backed
    network. As the Cirros image takes very long to reach a state where SSH
    login is possible, we only use a ping to check the liveliness of the VM.
    """
    ssh(
        machine,
        f"ch-remote --api-socket {NESTED_CH_API_SOCKET} info >/dev/null",
    )

    def login():
        try:
            ssh(machine, f"ping -c 1 -W 1 {NESTED_CIRROS_IP}")
            return True
        except RuntimeError:
            pass
        return False

    wait_until_succeed(login)


def start_stress_in_vm(machine: QemuMachine):
    """
    Starts a memory-intensive stress instance in the VM in background, which
    increases the dirty rate and slows down the migration.

    :param machine: QemuMachine to run SSH on
    :return:
    """

    ssh(machine, "screen -dmS stress stress --vm 4 --vm-bytes 400M --vm-keep")
    # Give stress time to unfold its impact.
    time.sleep(1)


def stop_stress_in_vm(machine: QemuMachine, extra_ssh_params: str = ""):
    """
    Counterpart to `start_stress_in_vm`.

    :param machine: QemuMachine to run SSH on
    :param extra_ssh_params: extra SSH params
    :return:
    """

    # Ask the named screen session to exit first. This is the graceful path and
    # should remove both screen and its stress child.
    ssh(machine, "screen -S stress -X quit || true", extra_ssh_params=extra_ssh_params)

    # Give screen a short chance to reap stress before using SIGKILL. This
    # keeps the normal path quiet, but still bounded.
    time.sleep(1)

    # Guarantee that the workload is gone even if the graceful screen shutdown
    # did not terminate its child. Match the exact process name so unrelated
    # processes are left alone.
    ssh(machine, "pkill -9 -x stress || true", extra_ssh_params=extra_ssh_params)
