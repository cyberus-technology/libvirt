"""Disk alias checks shared by live and cross-version migration suites."""

import json
import shlex
import xml.etree.ElementTree as ET

from .test_helper import (
    LibvirtTestsBase,
    QemuMachine,
    assert_domain_domstate,
    hotplug,
    number_of_storage_devices,
    ssh,
    wait_for_ssh,
    wait_until_succeed,
)


def guest_boot_id(machine: QemuMachine) -> str:
    return ssh(machine, "cat /proc/sys/kernel/random/boot_id").strip()


def check_omitted_disk_alias_is_restored(
    test: LibvirtTestsBase, expect_cookie: bool = False
) -> None:
    """Restore omitted aliases independently of XML disk ordering."""
    controllerVM = test.controllerVM
    computeVM = test.computeVM
    assert controllerVM is not None and computeVM is not None
    controllerVM.succeed("virsh define /etc/domain-chv.xml")
    controllerVM.succeed("virsh start testvm")
    wait_for_ssh(controllerVM)
    controllerVM.succeed("qemu-img create -f raw /nfs-root/disk.img 100M")
    controllerVM.succeed("chmod 0666 /nfs-root/disk.img")
    # Without a serial, only the cookie can restore this custom alias.
    serial_option = "" if expect_cookie else " --serial omitted-alias-volume"
    hotplug(
        controllerVM,
        "virsh attach-disk testvm /var/lib/libvirt/storage-pools/nfs-share/disk.img vdb "
        f"--alias ua-migration-volume{serial_option}",
    )

    xml = ET.fromstring(controllerVM.succeed("virsh dumpxml testvm"))
    devices = xml.find("devices")
    assert devices is not None
    disks = devices.findall("disk")
    for disk in disks:
        alias = disk.find("alias")
        assert alias is not None
        disk.remove(alias)
        devices.remove(disk)
    # Neither cookie application nor reconciliation may depend on XML order.
    devices.extend(reversed(disks))
    controllerVM.succeed(
        f"printf %s {shlex.quote(ET.tostring(xml, encoding='unicode'))} > /tmp/alias-migration.xml"
    )
    controllerVM.succeed(
        "virsh migrate testvm ch+tcp://computeVM/session --persistent --live --p2p "
        "--xml /tmp/alias-migration.xml"
    )
    wait_for_ssh(computeVM)
    computeVM.succeed("systemctl restart virtchd")
    xml = ET.fromstring(computeVM.succeed("virsh dumpxml testvm"))
    info = json.loads(
        computeVM.succeed("ch-remote --api-socket /run/libvirt/ch/testvm-socket info")
    )
    ch_disks = {disk["path"]: disk["id"] for disk in info["config"]["disks"]}
    for disk in xml.findall("./devices/disk"):
        source = disk.find("source")
        alias = disk.find("alias")
        assert source is not None and alias is not None
        test.assertEqual(alias.get("name"), ch_disks[source.get("file")])
    test.assertIn("ua-migration-volume", ch_disks.values())
    hotplug(computeVM, "virsh detach-disk testvm vdb")
    test.assertEqual(number_of_storage_devices(computeVM), 1)
    computeVM.succeed(
        "ch-remote --api-socket /run/libvirt/ch/testvm-socket info "
        "| jq -e 'all(.config.disks[]; .id != \"ua-migration-volume\")'"
    )


def check_live_migration_disk_alias(
    test: LibvirtTestsBase, change_alias: bool, expect_cookie: bool = False
) -> None:
    controllerVM = test.controllerVM
    computeVM = test.computeVM
    assert controllerVM is not None and computeVM is not None
    serial = "01234567-89ab-cdef-0123-456789abcdef"
    new_alias = "ua-nova-migrated-disk"
    disk_path = "/var/lib/libvirt/storage-pools/nfs-share/alias-disk.img"
    disk_xpath = "devices/disk/target[@dev='vdb']/.."
    info_command = "ch-remote --api-socket /run/libvirt/ch/testvm-socket info"

    controllerVM.succeed("virsh define /etc/domain-chv.xml")
    controllerVM.succeed("virsh start testvm")
    wait_for_ssh(controllerVM)
    original_disk_count = number_of_storage_devices(controllerVM)

    controllerVM.succeed("qemu-img create -f raw /nfs-root/alias-disk.img 10M")
    controllerVM.succeed("chmod 0666 /nfs-root/alias-disk.img")
    hotplug(
        controllerVM,
        "virsh attach-disk --domain testvm --target vdb --persistent "
        f"--source {disk_path} --serial {serial}",
    )

    xml = ET.fromstring(controllerVM.succeed("virsh dumpxml testvm"))
    alias = xml.find(f"{disk_xpath}/alias")
    assert alias is not None
    old_alias = alias.attrib["name"]
    test.assertTrue(old_alias.startswith("virtio-disk"))
    test.assertEqual(xml.findtext(f"{disk_xpath}/serial"), serial)

    boot_id = guest_boot_id(controllerVM)
    # Nova supplies a replacement alias, which must be ignored because CH
    # restores the source ID from its migration stream.
    if change_alias:
        alias.set("name", new_alias)
    destination_xml = shlex.quote(ET.tostring(xml, encoding="unicode"))
    controllerVM.succeed(
        f"printf '%s' {destination_xml} > /tmp/domain-changed-disk-alias.xml"
    )
    controllerVM.succeed(
        "virsh migrate --domain testvm --desturi ch+tcp://computeVM/session "
        "--persistent --live --p2p --xml /tmp/domain-changed-disk-alias.xml"
    )
    wait_for_ssh(computeVM)

    migrated_xml = ET.fromstring(computeVM.succeed("virsh dumpxml testvm"))
    migrated_alias = migrated_xml.find(f"{disk_xpath}/alias")
    assert migrated_alias is not None
    test.assertEqual(migrated_alias.attrib["name"], old_alias)
    test.assertEqual(migrated_xml.findtext(f"{disk_xpath}/serial"), serial)
    disks = json.loads(computeVM.succeed(info_command))["config"]["disks"]
    migrated_disks = [disk for disk in disks if disk["path"] == disk_path]
    test.assertEqual(len(migrated_disks), 1)
    test.assertEqual(migrated_disks[0]["id"], old_alias)
    test.assertEqual(migrated_disks[0]["serial"], serial[:20])
    test.assertEqual(number_of_storage_devices(computeVM), original_disk_count + 1)

    if change_alias:
        if expect_cookie:
            warning = (
                "DISK ALIAS OVERRIDE: domain 'testvm', disk 'vdb': ignoring "
                f"requested migration alias '{new_alias}'; preserving source alias '{old_alias}'"
            )
        else:
            warning = (
                "DISK ALIAS REPAIRED: domain 'testvm', disk 'vdb': replacing "
                f"incorrect libvirt alias '{new_alias}' with Cloud Hypervisor disk ID '{old_alias}'"
            )
        wait_until_succeed(
            lambda: warning in test.get_journal_current_test(computeVM, test)
        )
    if expect_cookie or not change_alias:
        test.assertNotIn(
            "DISK ALIAS REPAIRED", test.get_journal_current_test(computeVM, test)
        )
    if not expect_cookie or not change_alias:
        test.assertNotIn(
            "DISK ALIAS OVERRIDE", test.get_journal_current_test(computeVM, test)
        )
    test.assertNotIn(
        "DISK ALIAS REPAIR FAILED", test.get_journal_current_test(computeVM, test)
    )
    # Repair must be complete and durable before any destination restart.
    for path in ("/run/libvirt/ch/testvm.xml", "/var/lib/libvirt/ch/testvm.xml"):
        saved = ET.fromstring(computeVM.succeed(f"cat {path}"))
        saved_alias = saved.find(f".//{disk_xpath}/alias")
        assert saved_alias is not None
        test.assertEqual(saved_alias.get("name"), old_alias)
    computeVM.succeed("virsh blockresize testvm vdb 20480")
    wait_until_succeed(
        lambda: (
            ssh(computeVM, "blockdev --getsize64 /dev/vdb").strip()
            == str(20 * 1024 * 1024)
        )
    )
    computeVM.succeed("systemctl restart virtchd")
    restarted_xml = ET.fromstring(computeVM.succeed("virsh dumpxml testvm"))
    restarted_alias = restarted_xml.find(f"{disk_xpath}/alias")
    assert restarted_alias is not None
    test.assertEqual(restarted_alias.attrib["name"], old_alias)
    test.assertEqual(guest_boot_id(computeVM), boot_id)

    hotplug(
        computeVM,
        "virsh detach-disk --domain testvm --target vdb --persistent",
    )
    test.assertEqual(number_of_storage_devices(computeVM), original_disk_count)
    for flags in ("", "--inactive"):
        remaining_xml = ET.fromstring(
            computeVM.succeed(f"virsh dumpxml testvm {flags}")
        )
        test.assertIsNone(remaining_xml.find(disk_xpath))

    remaining_disks = json.loads(computeVM.succeed(info_command))["config"]["disks"]
    test.assertEqual(
        {disk["id"] for disk in remaining_disks},
        {disk["id"] for disk in disks if disk["id"] != old_alias},
    )
    wait_for_ssh(computeVM)


def check_disk_alias_recovery(
    test: LibvirtTestsBase,
    ambiguous_serials: bool = False,
    expect_cookie: bool = False,
) -> None:
    controllerVM = test.controllerVM
    computeVM = test.computeVM
    assert controllerVM is not None and computeVM is not None
    serials = [
        "01234567-89ab-cdef-0123-456789abcdef",
        "fedcba98-7654-3210-fedc-ba9876543210",
    ]
    if ambiguous_serials:
        serials[1] = serials[0][:20] + "different-suffix"
    targets = ["vdb", "vdc"]
    wrong_aliases = ["ua-stale-disk-b", "ua-stale-disk-c"]
    info_command = "ch-remote --api-socket /run/libvirt/ch/testvm-socket info"

    controllerVM.succeed("virsh define /etc/domain-chv.xml")
    controllerVM.succeed("virsh start testvm")
    wait_for_ssh(controllerVM)
    boot_id = guest_boot_id(controllerVM)
    for target, serial in zip(targets, serials):
        controllerVM.succeed(f"qemu-img create -f raw /nfs-root/{target}.img 10M")
        controllerVM.succeed(f"chmod 0666 /nfs-root/{target}.img")
        hotplug(
            controllerVM,
            f"virsh attach-disk testvm /var/lib/libvirt/storage-pools/nfs-share/{target}.img "
            f"{target} --serial {serial} --persistent",
        )
    ch_disks = json.loads(controllerVM.succeed(info_command))["config"]["disks"]
    expected_aliases = {disk["path"]: disk["id"] for disk in ch_disks}

    # Model the state left by the old migration code: only libvirt's live
    # and persistent XML have the new aliases; the running CH IDs stay put.
    state_files = ["/run/libvirt/ch/testvm.xml", "/var/lib/libvirt/ch/testvm.xml"]
    controllerVM.succeed("systemctl stop virtchd")
    try:
        for path in state_files:
            xml = ET.fromstring(controllerVM.succeed(f"cat {path}"))
            for target, wrong_alias in zip(targets, wrong_aliases):
                disk = xml.find(f".//devices/disk/target[@dev='{target}']/..")
                assert disk is not None
                alias = disk.find("alias")
                if alias is None:
                    alias = ET.SubElement(disk, "alias")
                alias.set("name", wrong_alias)
            payload = shlex.quote(ET.tostring(xml, encoding="unicode"))
            controllerVM.succeed(f"printf '%s' {payload} > {path}")
    finally:
        controllerVM.succeed("systemctl start virtchd")

    # Ensure the source still has stale aliases before migration.
    xml = ET.fromstring(controllerVM.succeed("virsh dumpxml testvm"))
    for target, wrong_alias in zip(targets, wrong_aliases):
        alias = xml.find(f".//devices/disk/target[@dev='{target}']/../alias")
        assert alias is not None
        test.assertEqual(alias.get("name"), wrong_alias)
    migration_xml_option = ""
    if expect_cookie:
        # Force cookie application first, then require vm.info to correct
        # the stale aliases that the updated source put in that cookie.
        for target in targets:
            alias = xml.find(f".//devices/disk/target[@dev='{target}']/../alias")
            assert alias is not None
            alias.set("name", f"ua-requested-{target}")
        payload = shlex.quote(ET.tostring(xml, encoding="unicode"))
        controllerVM.succeed(f"printf '%s' {payload} > /tmp/corrupted-alias.xml")
        migration_xml_option = " --xml /tmp/corrupted-alias.xml"
    controllerVM.succeed(
        "virsh migrate testvm ch+tcp://computeVM/session --persistent --live --p2p"
        + migration_xml_option
    )
    wait_for_ssh(computeVM)
    controllerVM.fail("virsh domstate testvm | grep -q running")
    journal = test.get_journal_current_test(computeVM, test)
    if expect_cookie:
        for target, wrong_alias in zip(targets, wrong_aliases):
            test.assertIn(
                f"requested migration alias 'ua-requested-{target}'; "
                f"preserving source alias '{wrong_alias}'",
                journal,
            )
    else:
        test.assertNotIn("DISK ALIAS OVERRIDE", journal)

    assert_domain_domstate(computeVM, "running")
    test.assertEqual(guest_boot_id(computeVM), boot_id)
    received_disks = json.loads(computeVM.succeed(info_command))["config"]["disks"]
    test.assertEqual(len(received_disks), len(ch_disks))
    for received, original in zip(received_disks, ch_disks):
        # New CH versions can add vm.info fields (for example bootindex).
        # Every non-null configuration field from the source must survive.
        test.assertEqual({key: received.get(key) for key in original}, original)

    if ambiguous_serials:
        xml = ET.fromstring(computeVM.succeed("virsh dumpxml testvm"))
        for target, wrong_alias in zip(targets, wrong_aliases):
            alias = xml.find(f".//devices/disk/target[@dev='{target}']/../alias")
            assert alias is not None
            test.assertEqual(alias.get("name"), wrong_alias)
        journal = test.get_journal_current_test(computeVM, test)
        test.assertIn("DISK ALIAS REPAIR FAILED", journal)
        test.assertIn("after truncation, found 2", journal)
        test.assertNotIn("DISK ALIAS REPAIRED", journal)
        return

    # Check saved and live aliases before any destination daemon restart.
    for command in ["virsh dumpxml testvm"] + [f"cat {path}" for path in state_files]:
        xml = ET.fromstring(computeVM.succeed(command))
        for disk in xml.findall(".//devices/disk"):
            source = disk.find("source")
            alias = disk.find("alias")
            assert source is not None and alias is not None
            test.assertEqual(alias.get("name"), expected_aliases[source.get("file")])
    test.assertEqual(guest_boot_id(computeVM), boot_id)

    journal = test.get_journal_current_test(computeVM, test)
    test.assertIn("DISK ALIAS REPAIRED", journal)
    test.assertIn("incorrect libvirt alias 'ua-stale-disk-b'", journal)

    # Resize uses the alias directly; successful detach alone would not
    # distinguish a durable repair from the old serial-based fallback.
    computeVM.succeed("virsh blockresize testvm vdb 20480")
    wait_until_succeed(
        lambda: (
            ssh(computeVM, "blockdev --getsize64 /dev/vdb").strip()
            == str(20 * 1024 * 1024)
        )
    )
    test.assertEqual(guest_boot_id(computeVM), boot_id)
    for target in targets:
        hotplug(computeVM, f"virsh detach-disk testvm {target} --persistent")
    test.assertEqual(number_of_storage_devices(computeVM), 1)
