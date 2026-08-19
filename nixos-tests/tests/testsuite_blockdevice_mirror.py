import shlex
import textwrap
import time
import unittest

# Following import statement allows for proper python IDE support and proper
# nix build support. The duplicate listing of imported functions is a bit
# unfortunate, but it seems to be the best compromise. This way the python IDE
# support works out of the box in VSCode and IntelliJ without requiring
# additional IDE configuration.
try:
    from ..test_helper.test_helper import (  # type: ignore
        LibvirtTestsBase,
        hotplug,
        initialComputeVMSetup,
        initialControllerVMSetup,
        ssh,
        wait_for_ssh,
        wait_until_succeed,
    )
except Exception:
    from test_helper import (
        LibvirtTestsBase,
        hotplug,
        initialComputeVMSetup,
        initialControllerVMSetup,
        ssh,
        wait_for_ssh,
        wait_until_succeed,
    )

# pyright: reportPossiblyUnboundVariable=false

# Following is required to allow proper linting of the python code in IDEs.
# Because certain functions like start_all() and certain objects like computeVM
# or other machines are added by Nix, we need to provide certain stub objects
# in order to allow the IDE to lint the python code successfully.
if "start_all" not in globals():
    from ..test_helper.test_helper.nixos_test_stubs import (  # type: ignore
        computeVM,
        controllerVM,
        start_all,
    )


class LibvirtTests(LibvirtTestsBase):  # type: ignore
    def __init__(self, methodName):
        super().__init__(methodName, controllerVM, computeVM)

    @classmethod
    def setUpClass(cls):
        start_all()
        initialControllerVMSetup(controllerVM)
        initialComputeVMSetup(computeVM)

    def tearDown(self):
        self.cleanupStorageMigration()
        super().tearDown()

    def setupDevice(
        self, source_path: str = "/tmp/disk.img", target: str = "vdb", sizeMb: int = 200
    ):
        """Set up and hotplug a disk filled with random data to testvm."""
        controllerVM.succeed(f"qemu-img create -f raw {source_path} {sizeMb}M")
        controllerVM.succeed(f"dd if=/dev/random of={source_path} bs=1M count={sizeMb}")
        hotplug(
            controllerVM,
            f"virsh attach-disk --domain testvm --target {target} --persistent --source {source_path} --subdriver raw",
        )

    def startAndWaitForVM(self, domain_xml: str = "/etc/domain-chv.xml"):
        controllerVM.succeed(f"virsh define {domain_xml}")
        controllerVM.succeed("virsh start testvm")
        wait_for_ssh(controllerVM)

    def startStorageMigration(
        self,
        source: str = "/tmp/disk.img",
        dest: str = "/tmp/copy.img",
        target: str = "vdb",
        initializeSource: bool = True,
        initializeDest: bool = True,
        sizeMb: int = 200,
    ):
        if initializeSource:
            self.setupDevice(source, target, sizeMb)
        if initializeDest:
            controllerVM.succeed(f"qemu-img create -f raw {dest} {sizeMb!s}M")
        controllerVM.succeed(
            f"virsh blockcopy testvm --dest {dest} --path {target} --format raw --reuse-external"
        )

    def mirroringFinished(self, device: str) -> bool:
        status, out = controllerVM.execute(
            f"virsh blockjob --info testvm {device} 2>&1"
        )
        if status != 0:
            return False

        # If finished, output looks like: Block Copy: [100.00 %]
        return "100.00 %" in out

    def waitForMirrorReady(self, device: str = "vdb", copyPath: str = "/tmp/copy.img"):
        """
        Wait for a blockjob on a given device to reach the ready state, which
        indicates we can pivot.
        The copyPath is required to match in the domain XML.
        """

        def check_blockjob():
            if not self.mirroringFinished(device):
                return False
            out = controllerVM.succeed("virsh dumpxml testvm")
            return (
                f"<mirror type='file' file='{copyPath}' format='raw' job='copy' ready='yes'>"
                in out
            )

        wait_until_succeed(lambda: check_blockjob())

    def pivotAndVerifyMirror(
        self,
        device: str = "vdb",
        sourcePath: str = "/tmp/disk.img",
        copyPath: str = "/tmp/copy.img",
    ):
        """
        Pivot a current blockjob on the given device and verify the domain XML
        reflects that we have switched to the newly copied file path. Further,
        we compare the source and mirror file to ensure they are identical.
        """
        controllerVM.succeed(f"virsh blockjob --pivot testvm {device}")
        controllerVM.succeed(f"cmp {sourcePath} {copyPath}")
        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertNotIn(
            sourcePath[1:], out, f"{sourcePath} should be removed from XML"
        )
        self.assertIn(copyPath[1:], out, f"{copyPath} should be in XML")

    def cleanupStorageMigration(self):
        """
        Cleanup all possible artifacts of the storage migration so the next
        test case can run.
        """
        controllerVM.execute("virsh destroy testvm")
        controllerVM.execute("virsh undefine testvm")
        controllerVM.execute("rm -f /tmp/*.img")

    def start_fio(
        self,
        target: str = "vdb",
        flushes: bool = False,
        output: str | None = None,
        numjobs: int = 1,
    ):
        flush_option = " --fsync=1" if flushes else ""
        output_option = f" --status-interval=1 --output={output}" if output else ""
        numjobs_option = f" --numjobs={numjobs} --thread" if numjobs > 1 else ""
        ssh(
            controllerVM,
            f"screen -dmS disk fio --name=vdb-randwrite --filename=/dev/{target} --rw=randwrite --bs=1M --direct=1 --time_based --runtime=60{flush_option}{output_option}{numjobs_option}",
        )

    def stop_fio(self):
        ssh(controllerVM, "screen -S disk -X quit || true")
        ssh(controllerVM, "pkill -9 -x fio || true")

    def fioHasStarted(self, output: str) -> bool:
        """Return whether fio has reported completed I/O to its status log."""
        return (
            ssh(
                controllerVM, f"grep -q 'IOPS=' {output} && echo yes || echo no"
            ).strip()
            == "yes"
        )

    def requestGuestLifecycleAction(self, action: str):
        """Request a guest lifecycle action and verify the domain stays
        running, while the guest becomes unavailable. Thus, check that Libvirt
        notices the lifecycle event after the block mirroring finishes."""
        try:
            ssh(controllerVM, action)
        except RuntimeError:
            # The guest may stop SSH before the command receives its result.
            pass

        def guest_is_offline() -> bool:
            try:
                ssh(controllerVM, "true")
                return False
            except RuntimeError:
                return True

        wait_until_succeed(guest_is_offline)
        self.assertDomainRunning()

    def assertDomainRunning(self):
        self.assertIn("running", controllerVM.succeed("virsh domstate testvm"))

    def is_ofd_locked(
        machine=controllerVM,
        process: str = "cloud-hyperviso",
        path: str = "/tmp/copy.img",
    ):
        return (
            controllerVM.execute(
                f"lslocks --bytes --notruncate -o COMMAND,TYPE,PATH | grep '{process}.*OFDLCK.*{path}'"
            )[0]
            == 0
        )

    def test_block_copy(self):
        """A single block device can be migrated to a new backing file."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady("vdb", "/tmp/copy.img")
        self.pivotAndVerifyMirror("vdb", "/tmp/disk.img", "/tmp/copy.img")

    def test_block_copy_abort(self):
        """Aborting a storage migration persists the original backing file."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady("vdb", "/tmp/copy.img")

        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertIn(
            "tmp/disk.img", out, "disk.img should still be in XML after abort"
        )
        self.assertNotIn(
            "tmp/copy.img", out, "copy.img should not be in XML after abort"
        )

        controllerVM.succeed("systemctl restart virtchd")
        self.assertDomainRunning()

        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertIn(
            "tmp/disk.img", out, "disk.img should remain in XML after restart"
        )
        self.assertNotIn(
            "<mirror", out, "mirror should remain absent from XML after restart"
        )

    def test_block_copy_cancel_during_copy(self):
        """Cancels an in-progress copy and checks the original disk remains active.."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertIn(
            "tmp/disk.img", out, "disk.img should still be in XML after abort"
        )
        self.assertNotIn(
            "tmp/copy.img", out, "copy.img should not be in XML after abort"
        )

    def test_block_copy_cancel_during_guest_writes(self):
        """Writes during copying, aborts after synchronization, and confirms
        later writes affect only the original source.
        """
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.start_fio()
        self.waitForMirrorReady()
        self.stop_fio()
        disk_hash = controllerVM.succeed("md5sum /tmp/disk.img")
        copy_hash = controllerVM.succeed("md5sum /tmp/copy.img")
        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        self.start_fio()
        wait_until_succeed(
            lambda: controllerVM.execute("cmp /tmp/disk.img /tmp/copy.img")[0] == 1
        )
        self.stop_fio()
        self.assertEqual(copy_hash, controllerVM.succeed("md5sum /tmp/copy.img"))
        self.assertNotEqual(disk_hash, controllerVM.succeed("md5sum /tmp/disk.img"))

    def test_block_copy_destination_failure_keeps_source(self):
        """A failed copy cleans up and leaves the source usable for a retry."""
        destination_dir = "/tmp/blockcopy-destination"
        destination = f"{destination_dir}/copy.img"
        retry_destination = "/tmp/copy-retry.img"

        self.startAndWaitForVM()
        controllerVM.succeed(f"mkdir -p {destination_dir}")
        controllerVM.succeed(f"mount -t tmpfs -o size=64M tmpfs {destination_dir}")

        try:
            self.startStorageMigration(dest=destination)
            wait_until_succeed(
                lambda: (
                    "No current block job"
                    in controllerVM.execute("virsh blockjob --info testvm vdb")[1]
                )
            )
            controllerVM.succeed(
                f"df --output=avail {destination_dir} | grep -E '^[[:space:]]*0$'"
            )

            out = controllerVM.succeed("virsh dumpxml testvm")
            self.assertIn(
                "tmp/disk.img", out, "disk.img should remain in XML after failure"
            )
            self.assertNotIn(
                destination_dir, out, "failed destination should not be in XML"
            )
            wait_until_succeed(lambda: not self.is_ofd_locked(path=destination))

            # Check that the source disk is still usable in the guest
            ssh(
                controllerVM,
                "fio --name=vdb-source-write --filename=/dev/vdb --rw=write "
                "--bs=1M --size=1M --direct=1 --fsync=1",
            )

            # Remove the full temporary filesystem, then prove that the
            # asynchronous failure left no state that prevents another copy.
            controllerVM.succeed(f"umount {destination_dir}")
            controllerVM.succeed(f"rmdir {destination_dir}")

            self.startStorageMigration(dest=retry_destination, initializeSource=False)
            self.waitForMirrorReady("vdb", retry_destination)
            self.pivotAndVerifyMirror("vdb", "/tmp/disk.img", retry_destination)
        finally:
            controllerVM.execute(f"umount {destination_dir} || true")
            controllerVM.execute(f"rmdir {destination_dir}")

    def test_block_copy_rejects_locked_destination(self):
        """A writable lock prevents copying until its holder releases it."""
        destination = "/tmp/copy.img"
        lock_screen = "blockcopy-destination-lock"

        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed(f"qemu-img create -f raw {destination} 200M")

        try:
            # Create a QEMU like OFDLCK
            controllerVM.succeed(
                f"screen -dmS {lock_screen} flock --fcntl --start 100 --length 1 {destination} sleep 60"
            )
            wait_until_succeed(
                lambda: self.is_ofd_locked(process="flock", path=destination)
            )

            controllerVM.fail(
                f"virsh blockcopy testvm --dest {destination} --path vdb --format raw --reuse-external"
            )
            self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

            controllerVM.succeed(f"screen -S {lock_screen} -X quit")
            wait_until_succeed(
                lambda: not self.is_ofd_locked(process="flock", path=destination)
            )

            self.startStorageMigration(
                dest=destination, initializeSource=False, initializeDest=False
            )
            self.waitForMirrorReady("vdb", destination)
            self.pivotAndVerifyMirror("vdb", "/tmp/disk.img", destination)
        finally:
            controllerVM.execute(f"screen -S {lock_screen} -X quit || true")

    def test_block_copy_rejects_mismatched_destination_size(self):
        """An undersized destination is rejected and left unchanged."""
        invalid_destination = "/tmp/copy-too-small.img"

        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed(f"qemu-img create -f raw {invalid_destination} 100M")
        invalid_checksum = controllerVM.succeed(
            f"sha256sum {invalid_destination} | cut -d ' ' -f 1"
        ).strip()

        controllerVM.fail(
            f"virsh blockcopy testvm --dest {invalid_destination} --path vdb --format raw --reuse-external"
        )
        self.assertEqual(
            controllerVM.succeed(
                f"sha256sum {invalid_destination} | cut -d ' ' -f 1"
            ).strip(),
            invalid_checksum,
            "the rejected destination must not be modified",
        )
        self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

    def test_block_copy_rejects_source_as_destination(self):
        """Copying onto the source or an alias is rejected without changes."""
        source = "/tmp/disk.img"
        hardlink_destination = "/tmp/disk-hardlink.img"
        symlink_destination = "/tmp/disk-symlink.img"

        self.startAndWaitForVM()
        self.setupDevice(source_path=source)
        source_checksum = controllerVM.succeed(
            f"sha256sum {source} | cut -d ' ' -f 1"
        ).strip()
        controllerVM.succeed(f"ln {source} {hardlink_destination}")
        controllerVM.succeed(f"ln -s {source} {symlink_destination}")

        for destination in (source, hardlink_destination, symlink_destination):
            controllerVM.succeed(f"test {source} -ef {destination}")
            controllerVM.fail(
                f"virsh blockcopy testvm --dest {destination} --path vdb --format raw --reuse-external"
            )
            self.assertEqual(
                controllerVM.succeed(f"sha256sum {source} | cut -d ' ' -f 1").strip(),
                source_checksum,
                "the source must not be modified by a rejected self-copy",
            )
            self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

    def test_block_copy_rejects_unsupported_destinations(self):
        """Reject destination XML that are not supported by the CH mirror backend."""
        destination = "/tmp/copy.img"
        source = f"<source file='{destination}'/>"
        unsupported = (
            (
                f"<disk type='block'><source dev='{destination}'/></disk>",
                "block copy destination must use file storage",
            ),
            *(
                (
                    f"<disk>{source}<driver type='{fmt}'/></disk>",
                    "block copy destination must use raw format",
                )
                for fmt in ("qcow2", "vmdk", "vhd")
            ),
            *(
                (
                    f"<disk>{body}</disk>",
                    "unsupported block copy destination source options",
                )
                for body in (
                    f"{source}<readonly/>",
                    f"{source}<shareable/>",
                    f"{source}<encryption format='luks'><secret type='passphrase' uuid='11111111-2222-3333-4444-555555555555'/></encryption>",
                    f"<source file='{destination}'><reservations managed='yes'/></source>",
                    f"<source file='{destination}'><seclabel model='dac' relabel='no'/></source>",
                    f"<source file='{destination}' fdgroup='copy'/>",
                    f"<source file='{destination}'><slices><slice type='storage' offset='512' size='1024'/></slices></source>",
                    f"{source}<backingStore type='file'><format type='raw'/><source file='/tmp/backing.img'/></backingStore>",
                    f"{source}<driver type='raw'><metadata_cache><max_size unit='bytes'>4096</max_size></metadata_cache></driver>",
                )
            ),
        )

        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed(f"qemu-img create -f raw {destination} 200M")
        persistent_xml = controllerVM.succeed("virsh dumpxml testvm --inactive")

        for xml, error in unsupported:
            controllerVM.succeed(f"printf '%s' {shlex.quote(xml)} > /tmp/copy.xml")
            output = controllerVM.fail(
                "virsh blockcopy testvm --path vdb --xml /tmp/copy.xml --reuse-external 2>&1"
            )
            self.assertIn(error, output)
            self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))
            self.assertEqual(
                persistent_xml,
                controllerVM.succeed("virsh dumpxml testvm --inactive"),
            )

    def test_block_copy_defaults_destination_to_raw(self):
        """Omitting the destination format must still record a raw mirror."""
        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed("qemu-img create -f raw /tmp/copy.img 200M")
        # --xml selects virDomainBlockCopy; --dest without a format instead
        # selects virDomainBlockRebase, which requires COPY_RAW in this driver.
        xml = "<disk type='file'><source file='/tmp/copy.img'/></disk>"
        controllerVM.succeed(f"printf '%s' {shlex.quote(xml)} > /tmp/copy.xml")
        controllerVM.succeed(
            "virsh blockcopy testvm --path vdb --xml /tmp/copy.xml --reuse-external"
        )
        self.waitForMirrorReady()
        self.pivotAndVerifyMirror()

    def test_block_copy_rejects_unsupported_flags_and_parameters(self):
        """Unsupported blockcopy flags and parameters fail without a mirror."""
        destination = "/tmp/copy.img"
        unsupported_options = (
            "--shallow --format qcow2",
            "--transient-job --format raw",
            "--synchronous-writes --format raw",
            "--dest-is-zero --format raw",
            "--granularity 4096 --format raw",
            "--buf-size 4096 --format raw",
        )

        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed(f"qemu-img create -f raw {destination} 200M")

        for options in unsupported_options:
            controllerVM.fail(
                f"virsh blockcopy testvm --dest {destination} --path vdb "
                f"--reuse-external {options}"
            )
            self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

    def test_block_copy_releases_destination_lock_on_cancel(self):
        """Cancelling a copy releases the destination's exclusive lock."""
        fio_output = "/tmp/fio-cancel-lock.log"

        def is_locked():
            destination = "/tmp/copy.img"
            return (
                controllerVM.execute(
                    f"lslocks --bytes --notruncate -o COMMAND,TYPE,PATH | grep 'cloud-hyperviso OFDLCK {destination}'"
                )[0]
                == 0
            )

        self.startAndWaitForVM()
        self.setupDevice()
        self.start_fio(output=fio_output)

        wait_until_succeed(lambda: self.fioHasStarted(fio_output))
        self.startStorageMigration(initializeSource=False)
        wait_until_succeed(is_locked)

        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        wait_until_succeed(lambda: not is_locked())

    def test_block_copy_retains_destination_lock_after_pivot(self):
        """The pivoted destination stays exclusively locked as the active disk."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady()
        self.pivotAndVerifyMirror()

        self.assertDomainRunning()
        wait_until_succeed(lambda: self.is_ofd_locked(path="/tmp/copy.img"))

    def test_block_copy_guest_poweroff(self):
        """Guest poweroff is postponed until a block copy is pivoted."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.assertIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))
        self.requestGuestLifecycleAction("poweroff")
        self.waitForMirrorReady()
        self.assertDomainRunning()
        self.pivotAndVerifyMirror()
        wait_until_succeed(
            lambda: "shut off" in controllerVM.execute("virsh domstate testvm 2>&1")[1]
        )

    def test_block_copy_prevents_reboot(self):
        """Guest reboot is postponed until a block copy is pivoted."""
        self.startAndWaitForVM()
        boot_id = ssh(controllerVM, "cat /proc/sys/kernel/random/boot_id").strip()

        self.startStorageMigration()
        self.assertIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))
        self.requestGuestLifecycleAction("reboot")
        self.waitForMirrorReady()
        self.assertDomainRunning()
        self.pivotAndVerifyMirror()
        wait_for_ssh(controllerVM)

        new_boot_id = ssh(controllerVM, "cat /proc/sys/kernel/random/boot_id").strip()
        self.assertNotEqual(boot_id, new_boot_id)

    def test_block_copy_mirrors_writes_after_ready(self):
        """Writes after ready are mirrored before the disk is pivoted."""
        source = "/tmp/disk.img"
        destination = "/tmp/copy.img"
        marker = "block-copy-ready-write-marker"
        marker_offset = "16M"

        self.startAndWaitForVM()
        self.setupDevice(source_path=source)
        self.startStorageMigration(
            source=source, dest=destination, initializeSource=False
        )
        self.waitForMirrorReady("vdb", destination)

        ssh(
            controllerVM,
            f"\"sh -c 'printf %s {marker} | dd of=/dev/vdb bs=1 seek={marker_offset} conv=notrunc,fsync status=none'\"",
        )

        for path in (source, destination):
            marker_on_host = controllerVM.succeed(
                f"dd if={path} bs=1 skip={marker_offset} count={len(marker)} status=none"
            ).strip()
            self.assertEqual(marker_on_host, marker)

        self.pivotAndVerifyMirror("vdb", source, destination)
        guest_marker = ssh(
            controllerVM,
            f"dd if=/dev/vdb bs=1 skip={marker_offset} count={len(marker)} status=none",
        ).strip()
        self.assertEqual(guest_marker, marker)

    def test_block_copy_preserves_guest_writes(self):
        """A guest marker write is preserved by copy and pivot."""
        destination = "/tmp/copy.img"
        marker = "block-copy-guest-write-marker"
        marker_offset = 16 * 1024 * 1024

        self.startAndWaitForVM()
        self.setupDevice()
        self.startStorageMigration(initializeSource=False)

        ssh(
            controllerVM,
            f"\"sh -c 'printf %s {marker} | dd of=/dev/vdb bs=1 seek={marker_offset} conv=notrunc,fsync status=none'\"",
        )
        self.waitForMirrorReady(copyPath=destination)
        self.pivotAndVerifyMirror(copyPath=destination)

        # Retrieve the marker from within the guest
        guest_marker = ssh(
            controllerVM,
            f"dd if=/dev/vdb bs=1 skip={marker_offset} count={len(marker)} status=none",
        ).strip()

        # Retrieve the marker from outside the guest
        destination_marker = controllerVM.succeed(
            f"dd if={destination} bs=1 skip={marker_offset} count={len(marker)} status=none",
        ).strip()
        self.assertEqual(guest_marker, marker)
        self.assertEqual(destination_marker, marker)

    def test_block_copy_preserves_sparseness(self):
        """A sparse raw source remains sparse after copy and pivot."""
        source = "/tmp/sparse-disk.img"
        destination = "/tmp/sparse-copy.img"
        size_mb = 200
        logical_blocks = size_mb * 1024 * 1024 // 512

        self.startAndWaitForVM()
        controllerVM.succeed(f"truncate -s {size_mb}M {source}")
        controllerVM.succeed(
            f"dd if=/dev/random of={source} bs=1M count=1 conv=notrunc status=none"
        )
        controllerVM.succeed(
            f"dd if=/dev/random of={source} bs=1M count=1 seek={size_mb - 1} "
            "conv=notrunc status=none"
        )
        self.assertLess(
            int(controllerVM.succeed(f"stat --format=%b {source}").strip()),
            logical_blocks,
            "the source must be sparse",
        )
        hotplug(
            controllerVM,
            f"virsh attach-disk --domain testvm --target vdb --persistent "
            f"--source {source} --subdriver raw",
        )

        self.startStorageMigration(
            source=source, dest=destination, initializeSource=False, sizeMb=size_mb
        )
        self.waitForMirrorReady("vdb", destination)
        self.pivotAndVerifyMirror("vdb", source, destination)
        self.assertLess(
            int(controllerVM.succeed(f"stat --format=%b {destination}").strip()),
            logical_blocks,
            "the destination should not be fully allocated",
        )

    def test_block_copy_repeats_after_pivot(self):
        """The pivoted disk can be written and copied to a new destination."""
        source = "/tmp/disk.img"
        first_destination = "/tmp/copy.img"
        second_destination = "/tmp/copy-second.img"

        self.startAndWaitForVM()
        self.startStorageMigration(source=source, dest=first_destination)
        self.waitForMirrorReady("vdb", first_destination)
        self.pivotAndVerifyMirror("vdb", source, first_destination)

        controllerVM.succeed(f"rm -f {source}")
        ssh(
            controllerVM,
            "fio --name=vdb-after-pivot --filename=/dev/vdb --rw=write "
            "--bs=1M --size=1M --direct=1 --fsync=1",
        )

        self.startStorageMigration(
            source=first_destination,
            dest=second_destination,
            initializeSource=False,
        )
        self.waitForMirrorReady("vdb", second_destination)
        self.pivotAndVerifyMirror("vdb", first_destination, second_destination)

    def test_block_copy_root_disk(self):
        """The guest keeps running and cold-boots from a pivoted root disk."""
        source = "/var/lib/libvirt/storage-pools/nfs-share/nixos.img"
        destination = "/tmp/nixos-root-copy.img"
        hidden_source = f"{source}.hidden"

        controllerVM.succeed(
            "sed \"/<disk /,/<\\/disk>/s/<driver /<driver type='raw' /\" "
            "/etc/domain-chv.xml > /tmp/domain-root-raw.xml"
        )
        self.startAndWaitForVM("/tmp/domain-root-raw.xml")
        controllerVM.succeed(
            f"qemu-img create -f raw {destination} $(stat --format=%s {source})"
        )
        controllerVM.succeed(
            f"virsh blockcopy testvm --dest {destination} --path vda --format raw --reuse-external"
        )
        self.waitForMirrorReady("vda", destination)
        self.pivotAndVerifyMirror("vda", source, destination)
        wait_for_ssh(controllerVM)

        controllerVM.succeed("virsh shutdown testvm")
        wait_until_succeed(
            lambda: "shut off" in controllerVM.execute("virsh domstate testvm 2>&1")[1]
        )

        try:
            # We temporarily hide the source file to ensure we really boot from
            # the pivoted root disk.
            controllerVM.succeed(f"mv {source} {hidden_source}")
            controllerVM.succeed("virsh start testvm")
            wait_for_ssh(controllerVM)
        finally:
            controllerVM.execute(f"mv {hidden_source} {source}")

    def test_block_copy_survives_daemon_restart(self):
        """A running block copy survives a virtchd restart and can pivot."""
        self.startAndWaitForVM()
        self.startStorageMigration()

        self.assertIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

        controllerVM.succeed("systemctl restart virtchd")
        self.assertDomainRunning()
        out = controllerVM.succeed("virsh blockjob --info testvm vdb 2>&1")
        self.assertIn("Block Copy:", out, "copy must persist across restart")

        self.waitForMirrorReady()
        self.pivotAndVerifyMirror()

    def test_block_copy_recovers_lost_ready_after_restart(self):
        """Recover READY from CH when runtime XML predates the READY event."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady()

        # Model a crash after consuming READY but before saving it. Keep the
        # copy itself running in CH and restore only the stale cached state.
        controllerVM.succeed("systemctl stop virtchd")
        try:
            status_file = "/run/libvirt/ch/testvm.xml"
            controllerVM.succeed(f"grep \"<mirror .*ready='yes'\" {status_file}")
            controllerVM.succeed(f"sed -i \"/<mirror /s/ ready='yes'//\" {status_file}")
            controllerVM.fail(f"grep \"<mirror .*ready='yes'\" {status_file}")
        finally:
            controllerVM.succeed("systemctl start virtchd")

        self.assertDomainRunning()
        # Check reconnect itself, before blockjob --info could refresh READY.
        self.assertIn(
            "<mirror type='file' file='/tmp/copy.img' format='raw' job='copy' ready='yes'>",
            controllerVM.succeed("virsh dumpxml testvm"),
        )
        self.pivotAndVerifyMirror()

    def test_block_copy_rejects_early_pivot(self):
        """An early pivot is rejected while the block copy continues to ready."""
        fio_output = "/tmp/fio-early-pivot.log"
        # Use a slightly larger disk size for this test to make the mirroring
        # take longer
        diskSize = 500

        self.startAndWaitForVM()
        self.setupDevice(sizeMb=diskSize)
        ssh(controllerVM, f"rm -f {fio_output}")
        self.start_fio(output=fio_output)
        wait_until_succeed(lambda: self.fioHasStarted(fio_output))

        # Start the workload before blockcopy to slow down the copy process
        self.startStorageMigration(initializeSource=False, sizeMb=diskSize)
        controllerVM.fail("virsh blockjob --pivot testvm vdb")

        status, out = controllerVM.execute("virsh blockjob --info testvm vdb 2>&1")
        self.assertEqual(status, 0)
        self.assertIn("Block Copy:", out, "early pivot must not stop the copy")
        self.assertIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))

        self.waitForMirrorReady()
        self.stop_fio()
        self.pivotAndVerifyMirror()

    def test_block_copy_paused_domain(self):
        """A blockdev mirror job cannot be started on a paused domain but a
        running job stays intact when domain is paused in between."""
        paused_destination = "/tmp/copy.img"

        self.startAndWaitForVM()
        self.setupDevice()
        controllerVM.succeed(f"qemu-img create -f raw {paused_destination} 500M")

        controllerVM.succeed("virsh suspend testvm")
        controllerVM.fail(
            f"virsh blockcopy testvm --dest {paused_destination} --path vdb --format raw --reuse-external"
        )
        self.assertIn(
            "Mirror operation rejected: the device is paused",
            self.get_journal_current_test(controllerVM, self),
        )
        self.assertEqual(
            controllerVM.succeed("virsh domstate testvm").strip(), "paused"
        )
        self.assertNotIn("<mirror", controllerVM.succeed("virsh dumpxml testvm"))
        controllerVM.succeed("virsh resume testvm")

        self.startStorageMigration(initializeSource=False)
        self.waitForMirrorReady()
        controllerVM.succeed("virsh suspend testvm")
        self.assertEqual(
            controllerVM.succeed("virsh domstate testvm").strip(), "paused"
        )
        controllerVM.fail("virsh blockjob --pivot testvm vdb")

        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertIn("<mirror", out)
        self.assertIn("ready='yes'", out)

        controllerVM.succeed("virsh resume testvm")
        self.pivotAndVerifyMirror()

    def test_block_copy_persistent_configuration(self):
        """A persistent domain updates its inactive disk source."""
        source = "/tmp/disk.img"
        destination = "/tmp/copy.img"

        self.startAndWaitForVM()
        self.startStorageMigration(source=source, dest=destination)
        self.waitForMirrorReady("vdb", destination)
        self.pivotAndVerifyMirror("vdb", source, destination)
        inactive_xml = controllerVM.succeed("virsh dumpxml --inactive testvm")
        self.assertNotIn(source, inactive_xml)
        self.assertIn(destination, inactive_xml)

    def test_block_copy_cancel_during_flushes(self):
        """Flushes issued by the guest keep completing after a copy is aborted."""
        fio_output = "/tmp/fio-flushes.log"

        self.startAndWaitForVM()
        self.startStorageMigration()
        ssh(controllerVM, f"rm -f {fio_output}")
        self.start_fio(flushes=True, output=fio_output)

        # fio reports its progress every second.  With --fsync=1, each of the
        # reported writes has completed an fsync before the next write starts.
        wait_until_succeed(
            lambda: (
                ssh(
                    controllerVM, f"grep -q 'IOPS=' {fio_output} && echo yes || echo no"
                ).strip()
                == "yes"
            )
        )
        progress_before_abort = int(
            ssh(controllerVM, f"grep -c 'IOPS=' {fio_output}").strip()
        )

        controllerVM.succeed("virsh blockjob --abort testvm vdb")

        wait_until_succeed(
            lambda: (
                int(ssh(controllerVM, f"grep -c 'IOPS=' {fio_output}").strip())
                > progress_before_abort
            )
        )
        self.stop_fio()

    def test_block_copy_cancel_and_retry_works(self):
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady()
        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        controllerVM.succeed("rm -f /tmp/copy.img")
        self.startStorageMigration(initializeSource=False)
        self.waitForMirrorReady()
        self.pivotAndVerifyMirror()

    def test_block_copy_blocks_other_jobs(self):
        """Other conflicting domain jobs are rejected during a block copy."""
        self.startAndWaitForVM()
        self.startStorageMigration()
        self.waitForMirrorReady()

        controllerVM.fail(
            "virsh migrate --domain testvm --desturi ch+tcp://computeVM/session --live --p2p"
        )
        controllerVM.fail("virsh detach-disk testvm vdb")
        controllerVM.fail(
            "virsh blockcopy testvm --dest /tmp/copy.img --path vdb --format raw --reuse-external"
        )
        controllerVM.fail(
            "virsh blockresize --domain testvm --path /tmp/disk.img --size 10240"
        )
        controllerVM.fail("virsh save testvm /tmp/testvm")

        self.pivotAndVerifyMirror("vdb", "/tmp/disk.img", "/tmp/copy.img")

    def test_block_copy_parallel(self):
        """Aborting one of two parallel copies does not disturb the other."""
        self.startAndWaitForVM()
        self.startStorageMigration(
            source="/tmp/disk-vdc.img", dest="/tmp/copy-vdc.img", target="vdc"
        )
        self.startStorageMigration()
        self.waitForMirrorReady()

        controllerVM.succeed("virsh blockjob --abort testvm vdb")
        out = controllerVM.succeed("virsh dumpxml testvm")
        self.assertIn("<source file='/tmp/disk.img'", out)
        self.assertNotIn("<mirror type='file' file='/tmp/copy.img'", out)

        out = controllerVM.succeed("virsh blockjob --info testvm vdc 2>&1")
        self.assertIn("Block Copy:", out, "vdc copy must remain active")
        self.waitForMirrorReady("vdc", "/tmp/copy-vdc.img")
        self.pivotAndVerifyMirror("vdc", "/tmp/disk-vdc.img", "/tmp/copy-vdc.img")

    def test_block_copy_with_disk_writes(self):
        """Storage migration preserves data while the source disk is being written."""
        self.startAndWaitForVM()
        self.setupDevice()
        self.start_fio()
        # Give fio some time to warm up.
        time.sleep(5)
        self.startStorageMigration(initializeSource=False)
        self.waitForMirrorReady()

        # Stop disk accesses before pivoting so no writes are in flight when the
        # two images are compared. fio bypasses the page cache.
        self.stop_fio()

        self.pivotAndVerifyMirror()

    def test_block_copy_multiqueue_writes(self):
        """Concurrent writes through virtio-blk queues survive a copy and pivot."""
        source = "/tmp/disk.img"
        destination = "/tmp/copy.img"

        controllerVM.succeed(f"qemu-img create -f raw {source} 200M")
        controllerVM.succeed(f"dd if=/dev/random of={source} bs=1M count=200")
        self.startAndWaitForVM()
        controllerVM.succeed(
            textwrap.dedent(f"""
            cat >> multiqueue.xml << EOF
            <disk type='file' device='disk'>
                <source file='{source}'/>
                <target dev='vdb' bus='virtio'/>
                <driver type='raw' queues='2'/>
            </disk>
            EOF
            """).strip()
        )
        hotplug(
            controllerVM,
            "virsh attach-device testvm multiqueue.xml --persistent",
        )
        self.assertEqual(
            ssh(
                controllerVM,
                "find /sys/block/vdb/mq -mindepth 1 -maxdepth 1 -type d | wc -l",
            ).strip(),
            "2",
            "vdb should expose all four configured virtio-blk queues",
        )

        controllerVM.succeed(f"qemu-img create -f raw {destination} 200M")
        fio_output = "/tmp/fio-multiqueue.log"
        ssh(controllerVM, f"rm -f {fio_output}")
        self.start_fio(numjobs=4, output=fio_output)
        wait_until_succeed(lambda: self.fioHasStarted(fio_output))

        # Start copying only after all fio workers are issuing writes, so the
        # workload overlaps the copy rather than merely the ready state.
        controllerVM.succeed(
            f"virsh blockcopy testvm --dest {destination} --path vdb --format raw --reuse-external"
        )
        self.waitForMirrorReady("vdb", destination)

        # Quiesce the workload before pivoting so the checksum observes a
        # stable guest-visible device image.
        self.stop_fio()
        self.pivotAndVerifyMirror("vdb", source, destination)

        guest_checksum = ssh(
            controllerVM, "sha256sum /dev/vdb | cut -d ' ' -f 1"
        ).strip()
        destination_checksum = controllerVM.succeed(
            f"sha256sum {destination} | cut -d ' ' -f 1"
        ).strip()
        self.assertEqual(guest_checksum, destination_checksum)


def suite():
    # Test cases sorted in alphabetical order.
    testcases = [
        LibvirtTests.test_block_copy,
        LibvirtTests.test_block_copy_abort,
        LibvirtTests.test_block_copy_blocks_other_jobs,
        LibvirtTests.test_block_copy_cancel_and_retry_works,
        LibvirtTests.test_block_copy_cancel_during_copy,
        LibvirtTests.test_block_copy_cancel_during_flushes,
        LibvirtTests.test_block_copy_cancel_during_guest_writes,
        LibvirtTests.test_block_copy_defaults_destination_to_raw,
        LibvirtTests.test_block_copy_destination_failure_keeps_source,
        LibvirtTests.test_block_copy_guest_poweroff,
        LibvirtTests.test_block_copy_mirrors_writes_after_ready,
        LibvirtTests.test_block_copy_multiqueue_writes,
        LibvirtTests.test_block_copy_parallel,
        LibvirtTests.test_block_copy_paused_domain,
        LibvirtTests.test_block_copy_persistent_configuration,
        LibvirtTests.test_block_copy_preserves_guest_writes,
        LibvirtTests.test_block_copy_preserves_sparseness,
        LibvirtTests.test_block_copy_prevents_reboot,
        LibvirtTests.test_block_copy_recovers_lost_ready_after_restart,
        LibvirtTests.test_block_copy_rejects_early_pivot,
        LibvirtTests.test_block_copy_rejects_locked_destination,
        LibvirtTests.test_block_copy_rejects_mismatched_destination_size,
        LibvirtTests.test_block_copy_rejects_source_as_destination,
        LibvirtTests.test_block_copy_rejects_unsupported_destinations,
        LibvirtTests.test_block_copy_rejects_unsupported_flags_and_parameters,
        LibvirtTests.test_block_copy_releases_destination_lock_on_cancel,
        LibvirtTests.test_block_copy_repeats_after_pivot,
        LibvirtTests.test_block_copy_retains_destination_lock_after_pivot,
        LibvirtTests.test_block_copy_root_disk,
        LibvirtTests.test_block_copy_survives_daemon_restart,
        LibvirtTests.test_block_copy_with_disk_writes,
    ]

    suite = unittest.TestSuite()
    for testcaseMethod in testcases:
        suite.addTest(LibvirtTests(testcaseMethod.__name__))
    return suite


runner = unittest.TextTestRunner()
if not runner.run(suite()).wasSuccessful():
    raise Exception("Test Run unsuccessful")
