{
  self,
  src,
  pkgs,
  nixpkgs,
  cloud-hypervisor,
  cloud-hypervisor-prev,
  edk2-src,
  fcntl-tool,
  libvirt,
  libvirt-prev,
}:
# Shared flake outputs for the NixOS test environment. The root flake imports
# this file and recursively augments the exported attrsets as needed. The
# exported attrsets mirror the usual attrsets of a flake.
let
  # Debug optimized Cloud Hypervisor build, suited for quicker rebuilds
  # with reasonable good performance.
  toDebugOptimizedChv =
    drv:
    drv.overrideAttrs (old: {
      env = (old.env or { }) // {
        CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS = "true";
        CARGO_PROFILE_RELEASE_OPT_LEVEL = 2;
        CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS = "true";
        CARGO_PROFILE_RELEASE_LTO = "thin";
      };
    });

  # The nixos python test-driver is currently not exported, but we require it
  # for our test helper lib to get all required type information.
  nixos-test-driver = pkgs.callPackage "${pkgs.path}/nixos/lib/test-driver/default.nix" { };

  test-helper = pkgs.callPackage ./test_helper.nix {
    inherit nixos-test-driver;
    inherit (pkgs.python3Packages) buildPythonPackage setuptools;
  };

  chv-ovmf = pkgs.OVMF-cloud-hypervisor.overrideAttrs (_: {
    version = "cbs";
    src = edk2-src;
  });

  testPkgs = pkgs.appendOverlays [
    (_: prev: {
      fcntl-tool = fcntl-tool.packages."x86_64-linux".default;
      cloud-hypervisor = toDebugOptimizedChv cloud-hypervisor.packages."x86_64-linux".default;
      cloud-hypervisor-prev = toDebugOptimizedChv cloud-hypervisor-prev.packages."x86_64-linux".default;
      libvirt = libvirt.packages."x86_64-linux".libvirt-debugoptimized;
      libvirt-prev = libvirt-prev.packages."x86_64-linux".libvirt-debugoptimized;
      python3Packages = prev.python3Packages.overrideScope (
        _: _: {
          inherit test-helper;
        }
      );
    })
  ];

  nixos-image =
    let
      nixos-system = pkgs.callPackage ./images/nixos-image.nix { inherit nixpkgs chv-ovmf; };
      iso = nixos-system.config.system.build.isoImage;
    in
    pkgs.runCommand "nixos.iso"
      {
        nativeBuildInputs = [ pkgs.coreutils ];
      }
      ''
        # The image has a non deterministic name, so we make it
        # deterministic.
        cp ${iso}/iso/*.iso $out
      '';
in
{
  checks."x86_64-linux" =
    let
      fs = pkgs.lib.fileset;
      cleanSrc = fs.toSource {
        root = src;
        fileset = fs.gitTracked src;
      };
      deadnix =
        pkgs.runCommand "deadnix"
          {
            nativeBuildInputs = [ pkgs.deadnix ];
          }
          ''
            deadnix -L ${cleanSrc} --fail
            mkdir $out
          '';
      pythonFormat =
        pkgs.runCommand "python-format"
          {
            nativeBuildInputs = with pkgs; [ ruff ];
          }
          ''
            cp -r ${cleanSrc}/. .
            ruff format --check .
            mkdir $out
          '';
      pythonLint =
        pkgs.runCommand "python-lint"
          {
            nativeBuildInputs = with pkgs; [ ruff ];
          }
          ''
            cp -r ${cleanSrc}/. .
            ruff check ${cleanSrc}/test_helper
            ruff check ${cleanSrc}/tests
            mkdir $out
          '';
      pythonTypes =
        pkgs.runCommand "python-types"
          {
            nativeBuildInputs = with pkgs; [
              pyright
              test-helper
            ];
          }
          ''
            pyright ${cleanSrc}/tests
            mkdir $out
          '';
      typos =
        pkgs.runCommand "spellcheck"
          {
            nativeBuildInputs = [ pkgs.typos ];
          }
          ''
            # By cd'ing first, we prevent that typos complains about
            # weird path names (Nix store).
            cd ${cleanSrc}
            typos .
            mkdir $out
          '';
      all = pkgs.symlinkJoin {
        name = "combined-checks";
        paths = [
          deadnix
          pythonFormat
          pythonLint
          typos
        ];
      };
    in
    {
      inherit
        all
        deadnix
        pythonFormat
        pythonLint
        pythonTypes
        typos
        ;
      default = all;
    };

  formatter."x86_64-linux" = pkgs.nixfmt-tree;

  devShells."x86_64-linux".default = pkgs.mkShellNoCC {
    inputsFrom = builtins.attrValues self.checks."x86_64-linux";
    packages = with pkgs; [
      gitlint
    ];
    shellHook =
      # We need our `test_helper` Python library for the NixOS integration
      # tests, which are Python projects themselves. For this reason, we
      # assemble a Python toolchain that includes this package. We however
      # also want full convenience for local development flows.
      #
      # The very same Nix Python toolchain is also used by the Nix
      # development shell. When developers run `nix run #<test>.driver`,
      # the Python process executes in the host environment of the caller
      # and resolves modules via `PYTHONPATH` - from the caller who might
      # have opened a Nix development shell. In that situation,
      # `test_helper` will be imported from the likely outdated location
      # in PYTHONPATH rather than from the local (potentially modified)
      # files.
      #
      # This results in a confusing and very poor developer experience
      # where an outdated version of `test_helper` is used even though
      # local changes exist. To ensure the local version always takes
      # precedence, we explicitly prepend the local path to `PYTHONPATH`.
      ''
        export PYTHONPATH=$PWD/test_helper/test_helper:$PYTHONPATH
      '';
  };

  packages."x86_64-linux" = {
    # Export of the overlay'ed package
    inherit (testPkgs)
      cloud-hypervisor
      cloud-hypervisor-prev
      libvirt
      libvirt-prev
      ;
    inherit nixos-image;
    chv-ovmf = pkgs.runCommand "OVMF-CLOUHDHV.fd" { } ''
      cp ${chv-ovmf.fd}/FV/CLOUDHV.fd $out
    '';
  };

  tests."x86_64-linux" = import (src + "/tests/default.nix") {
    inherit
      nixos-image
      chv-ovmf
      ;
    pkgs = testPkgs;
  };
}
