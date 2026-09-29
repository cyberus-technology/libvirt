import unittest

# Keep both import forms for Nix execution and editor support.
try:
    from ..test_helper.test_helper import (  # type: ignore
        LibvirtTestsBase,
        assert_domain_domstate,
        initialComputeVMSetup,
        initialControllerVMSetup,
        wait_for_ssh,
    )
except Exception:
    from test_helper import (
        LibvirtTestsBase,
        assert_domain_domstate,
        initialComputeVMSetup,
        initialControllerVMSetup,
        wait_for_ssh,
    )

# pyright: reportPossiblyUnboundVariable=false
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

    def test_migration_survives_long_receiver_prefault(self):
        """
        This test migrates a VM to a patched version of CHV simulating a long prefault time.
        CHV must keep the migration connection alive for the duration of the prefaulting.
        """
        controllerVM.succeed("virsh define /etc/domain-chv-prefault.xml")
        controllerVM.succeed("virsh start testvm")
        wait_for_ssh(controllerVM)

        controllerVM.succeed(
            "virsh migrate --domain testvm --desturi ch+tcp://computeVM/session --persistent --live --p2p"
        )

        wait_for_ssh(computeVM)
        assert_domain_domstate(controllerVM, "shut off")


def suite():
    # Test cases sorted in alphabetical order.
    testcases = [LibvirtTests.test_migration_survives_long_receiver_prefault]

    suite = unittest.TestSuite()
    for testcaseMethod in testcases:
        suite.addTest(LibvirtTests(testcaseMethod.__name__))
    return suite


runner = unittest.TextTestRunner()
if not runner.run(suite()).wasSuccessful():
    raise Exception("Test Run unsuccessful")
