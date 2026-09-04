{
  lib,
  runCommand,
  runtimeShell,
  python3,
  coreutils,
  iproute2,
  iputils,
  openssh,
  cloud-hypervisor-tdx,
  sshpass,
  test-helper,
  nixos-test-driver,
  testScriptFile,
  guestImage,
}:

let
  python = python3.withPackages (_: nixos-test-driver.dependencies ++ [ test-helper ]);

  # The bare-metal suite drives Cloud Hypervisor on this host, not in a NixOS
  # test VM. This wrapper supplies what the NixOS test runner normally gives
  # it: the Python test driver, a PATH with the helper commands and the TDX
  # firmware through the environment.
  drv =
    runCommand "tdx-bare-metal-test"
      {
        # Expose the same `<test>.driver` attribute as the nested suites.
        # This keeps the documented `nix run .#tests.x86_64-linux.tdx.driver`
        # invocation working.
        passthru.driver = drv;
        meta.mainProgram = "tdx-bare-metal-test";
      }
      ''
        mkdir -p $out/bin
        cat > $out/bin/tdx-bare-metal-test <<'EOF'
        #!${runtimeShell}
        set -euo pipefail

        export PATH="${
          lib.makeBinPath [
            coreutils
            iproute2
            iputils
            openssh
            sshpass
          ]
        }:''${PATH:-}"
        export TDX_CLOUD_HYPERVISOR="${cloud-hypervisor-tdx}/bin/cloud-hypervisor"

        # TODO(@Leander): nixify TDVF and build it automatically. Use the
        # Config-B target from OvmfPkg/IntelTdx/IntelTdxX64.dsc. Until then
        # the runner has to provide TDX_FIRMWARE.
        # https://github.com/tianocore/edk2/tree/master/OvmfPkg/IntelTdx
        if [[ -z "''${TDX_FIRMWARE:-}" ]]; then
          echo "TDX_FIRMWARE must point to an Intel TDX guest firmware image" >&2
          exit 2
        fi

        export TDX_IMAGE="''${TDX_IMAGE:-${guestImage}}"
        # LocalMachine imports test_driver. Only the test driver's Python
        # environment provides that module.
        export PYTHONPATH="${python}/${python3.sitePackages}:${nixos-test-driver}/${python3.sitePackages}"
        exec ${python}/bin/python ${testScriptFile} "$@"
        EOF
        chmod +x $out/bin/tdx-bare-metal-test
      '';
in
drv
