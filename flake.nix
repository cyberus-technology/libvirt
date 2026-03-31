{
  description = "libvirt with Cloud Hypervisor patches by Cyberus Technology";

  inputs = {
    cloud-hypervisor.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux";
    cloud-hypervisor.inputs.nixpkgs.follows = "nixpkgs";
    # Previous release of cloud-hypervisor for migration testing with different versions.
    cloud-hypervisor-prev.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux-release-26-03-31";
    cloud-hypervisor-prev.inputs.nixpkgs.follows = "nixpkgs";
    # Previous release of libvirt for migration testing with different versions.
    # Using the shorthand notation of "github:user/repo..." may lead to build errors
    # like "source-with-submodules> cp: cannot create regular file '[...]': Permission denied".
    libvirt-prev.url = "git+https://github.com/cyberus-technology/libvirt?ref=refs/tags/gardenlinux-release-26-03-31&submodules=1";
    libvirt-prev.inputs.nixpkgs.follows = "nixpkgs";

    keycodemapdb.url = "git+https://gitlab.com/keycodemap/keycodemapdb.git";
    keycodemapdb.flake = false;
    libvirt-tests.url = "github:cyberus-technology/libvirt-tests?ref=main";
    libvirt-tests.inputs.cloud-hypervisor.follows = "cloud-hypervisor";
    libvirt-tests.inputs.cloud-hypervisor-prev.follows = "cloud-hypervisor-prev";
    # Breaking the chain of cyclic dependencies:
    libvirt-tests.inputs.edk2-src.follows = "edk2-src";
    libvirt-tests.inputs.libvirt.inputs.libvirt-tests.follows = "libvirt-tests";
    libvirt-tests.inputs.libvirt-prev.inputs.libvirt-tests.follows = "libvirt-tests";
    libvirt-tests.inputs.nixpkgs.follows = "nixpkgs";
    # We follow the latest stable release of nixpkgs
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
    # We use our custom firmware
    edk2-src.url = "git+https://github.com/cyberus-technology/edk2?ref=gardenlinux&submodules=1";
    edk2-src.flake = false;
  };

  outputs =
    {
      self,
      nixpkgs,
      libvirt-tests,
      keycodemapdb,
      edk2-src,
      cloud-hypervisor,
      ...
    }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      lib = pkgs.lib;

      # Clean source for libvirt. No GitLab CI, no Nix
      cleanSource = lib.cleanSourceWith {
        src = self;
        filter =
          path: type:
          let
            baseName = baseNameOf path;
          in
          # Exclude .git and other VCS artifacts
          lib.cleanSourceFilter path type
          && (
            # Exclude our own additional files
            !(lib.hasSuffix ".nix" baseName || baseName == "flake.lock" || baseName == ".gitlab-ci.yml")
          );
      };

      # libvirt requires a populated submodule to build successfully. As we
      # cannot reference a local git directory as a flake input to let nix
      # handle the submodule population, we assemble it ourself.
      cleanSourceWithSubmodules = pkgs.runCommand "source-with-submodules" { } ''
        mkdir -p $out

        cp -r ${cleanSource}/* $out/

        # If someone fetched this flake with `submodules=1`, then we are good
        # to go. If not, we populate the submodules.
        if [ ! -f $out/subprojects/keycodemapdb/meson.build ]; then
          echo "Was fetched without submodules: populating ..."
          mkdir -p $out/subprojects/keycodemapdb
          cp -r ${keycodemapdb}/* $out/subprojects/keycodemapdb/
        else
          echo "Was fetched with 'submodules=1'"
        fi
      '';

      systems = [ "x86_64-linux" ];
      forAllSystems =
        function: nixpkgs.lib.genAttrs systems (system: function nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ pkgs.libvirt ];
        };
      });
      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
      # We export all tests from `libvirt-tests.tests` following the typical
      # Nix flake attribute structure (tests.<system>.<test-name>). For each
      # test, we override the source of libvirt with the source from this
      # repository.
      packages = nixpkgs.lib.recursiveUpdate cloud-hypervisor.packages (
        forAllSystems (
          pkgs:
          let
            # This builds libvirt with our own sources and the normal upstream libvirt configuration.
            libvirt = pkgs.libvirt.overrideAttrs (old: {
              name = "libvirt-chv";
              src = cleanSourceWithSubmodules;
              version =
                # We fetch the version from `meson.build`.
                let
                  fallback = builtins.trace "WARN: cannot obtain version from libvirt fork" "0.0.0-unknown";
                  mesonBuild = builtins.readFile "${cleanSourceWithSubmodules}/meson.build";
                  # Searches for the line `version: '11.3.0'` and captures the version.
                  matches = builtins.match ".*[[:space:]]*version:[[:space:]]'([0-9]+.[0-9]+.[0-9]+)'.*" mesonBuild;
                  version = builtins.elemAt matches 0;
                in
                if matches != null then version else fallback;
              doInstallCheck = false;
              doCheck = false;
              patches = [
                ./patches/libvirt/0001-meson-patch-in-an-install-prefix-for-building-on-nix.patch
                ./patches/libvirt/0002-substitute-zfs-and-zpool-commands.patch
              ];
              mesonFlags = (old.mesonFlags or [ ]) ++ [
                # Helps to keep track of the commit hash in the libvirt log. Nix strips
                # all `.git`, so we need to be explicit here.
                #
                # This is a non-standard functionality of our own libvirt fork.
                "-Dcommit_hash=${if self ? rev then self.rev else self.dirtyRev}"
              ];
            });
            # This builds libvirt with our own sources and:
            # - support for debugging (optimized debug build with symbols)
            # - support for address sanitizers
            libvirt-debugoptimized = libvirt.overrideAttrs (_old: {
              mesonBuildType = "debugoptimized";
              # IMPORTANT: donStrip is required because otherwise, nix will strip all
              # debug info from the binaries in its fixupPhase. Having the debug info
              # is crucial for getting source code info from the sanitizers, as well as
              # when using GDB.
              dontStrip = true;
            });

            chv-ovmf = pkgs.OVMF-cloud-hypervisor.overrideAttrs (_old: {
              version = "cbs";
              src = edk2-src;
            });

          in
          {
            inherit libvirt libvirt-debugoptimized;
            default = libvirt;
            chv-ovmf = pkgs.runCommand "OVMF-CLOUHDHV.fd" { } ''
              cp ${chv-ovmf.fd}/FV/CLOUDHV.fd $out
            '';
            prepare-images = import ./local_tests/prepare-images.nix { inherit pkgs; };
          }
        )
      );
      # We export all tests from `libvirt-tests.tests` but override the libvirt
      # input with libvirt from this repository.
      tests = forAllSystems (
        pkgs:
        let
          lib = pkgs.lib;
          system = pkgs.stdenv.hostPlatform.system;

          # New attribute set with updated `libvirt-src` input for each test.
          tests = lib.concatMapAttrs (name: value: {
            ${name} =
              value.override (old: {
                pkgs = old.pkgs.extend (
                  _final: _prev: {
                    libvirt = self.packages.${system}.libvirt-debugoptimized;
                  }
                );
              })
              // {
                passthru.no_port_forwarding = value.passthru.no_port_forwarding.override (old: {
                  pkgs = old.pkgs.extend (
                    _final: _prev: {
                      libvirt = self.packages.${system}.libvirt-debugoptimized;
                    }
                  );
                });
              };
          }) libvirt-tests.tests.${system};
        in
        tests
        // {
          all_drivers = pkgs.symlinkJoin {
            name = "all-test-drivers";
            paths = with tests; [
              # Sorted alphabetically.
              default.driver
              hugepage.driver
              live_migration.driver
              long_migration_with_load.driver
              numa_hosts.driver
              version_migration.driver
            ];
          };
        }
      );
    };
}
