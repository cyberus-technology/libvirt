{
  description = "libvirt with Cloud Hypervisor patches by Cyberus Technology";

  inputs = {
    # A local path can be used for developing or testing local changes.
    # cloud-hypervisor.url = "git+file:<path/to/cloud-hypervisor>";
    cloud-hypervisor.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux";
    cloud-hypervisor.inputs.nixpkgs.follows = "nixpkgs";
    # Previous release of cloud-hypervisor for migration testing with different versions.
    cloud-hypervisor-prev.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux-release-26-04-20";
    cloud-hypervisor-prev.inputs.nixpkgs.follows = "nixpkgs";
    # Previous release of libvirt for migration testing with different versions.
    # Using the shorthand notation of "github:user/repo..." may lead to build errors
    # like "source-with-submodules> cp: cannot create regular file '[...]': Permission denied".
    libvirt-prev.url = "git+https://github.com/cyberus-technology/libvirt?ref=refs/tags/gardenlinux-release-26-04-20&submodules=1";
    libvirt-prev.flake = false;

    keycodemapdb.url = "git+https://gitlab.com/keycodemap/keycodemapdb.git";
    keycodemapdb.flake = false;
    # We follow the latest stable release of nixpkgs
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
    # We use our custom firmware
    edk2-src.url = "git+https://github.com/cyberus-technology/edk2?ref=gardenlinux&submodules=1";
    edk2-src.flake = false;
    fcntl-tool.url = "github:phip1611/fcntl-tool";
    fcntl-tool.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      keycodemapdb,
      edk2-src,
      cloud-hypervisor,
      cloud-hypervisor-prev,
      fcntl-tool,
      libvirt-prev,
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
            # Exclude our own additional files that are irrelevant for the
            # libvirt package.
            !(
              lib.hasSuffix ".nix" baseName
              || baseName == "flake.lock"
              || baseName == ".gitlab-ci.yml"
              || baseName == "nixos-tests"
              || baseName == "local_tests"
            )
          );
      };

      # libvirt requires a populated submodule to build successfully. As we
      # cannot rely on every source input to already contain the populated
      # submodule checkout, we assemble it ourselves.
      withKeycodemapdbSubmodule =
        name: src:
        pkgs.runCommand name { } ''
          mkdir -p $out

          cp -r ${src}/* $out/

          # If someone fetched this source with `submodules=1`, then we are
          # good to go. If not, we populate the submodule from our explicit
          # input to keep the source self-contained.
          if [ ! -f $out/subprojects/keycodemapdb/meson.build ]; then
            echo "Was fetched without submodules: populating ..."
            mkdir -p $out/subprojects/keycodemapdb
            cp -r ${keycodemapdb}/* $out/subprojects/keycodemapdb/
          else
            echo "Was fetched with 'submodules=1'"
          fi
        '';

      cleanSourceWithSubmodules = withKeycodemapdbSubmodule "source-with-submodules" cleanSource;
      libvirtPrevSourceWithSubmodules = withKeycodemapdbSubmodule "libvirt-prev-source-with-submodules" libvirt-prev;

      # Build the libvirt package variants exported by this flake from a given
      # source tree. The returned attrset currently contains `libvirt` and
      # `libvirt-debugoptimized`.
      mkLibvirtPackageSet =
        {
          pkgs,
          src,
          name,
          commitHash ? null,
        }:
        let
          mesonBuild = builtins.readFile "${src}/meson.build";
          fallback = builtins.trace "WARN: cannot obtain version from libvirt fork" "0.0.0-unknown";
          # Searches for the line `version: '11.3.0'` and captures the version.
          matches = builtins.match ".*[[:space:]]*version:[[:space:]]'([0-9]+.[0-9]+.[0-9]+)'.*" mesonBuild;
          version = builtins.elemAt matches 0;
          libvirt = pkgs.libvirt.overrideAttrs (old: {
            inherit name src;
            version = if matches != null then version else fallback;
            doInstallCheck = false;
            doCheck = false;
            patches = [
              ./patches/libvirt/0001-meson-patch-in-an-install-prefix-for-building-on-nix.patch
              ./patches/libvirt/0002-substitute-zfs-and-zpool-commands.patch
            ];
            mesonFlags =
              (old.mesonFlags or [ ]) ++ lib.optional (commitHash != null) "-Dcommit_hash=${commitHash}";
          });
          libvirt-debugoptimized = libvirt.overrideAttrs (_old: {
            mesonBuildType = "debugoptimized";
            # IMPORTANT: donStrip is required because otherwise, nix will strip
            # all debug info from the binaries in its fixupPhase. Having the
            # debug info is crucial for getting source code info from the
            # sanitizers, as well as when using GDB.
            dontStrip = true;
          });
        in
        {
          inherit libvirt libvirt-debugoptimized;
        };

      libvirtPackageSet = mkLibvirtPackageSet {
        inherit pkgs;
        src = cleanSourceWithSubmodules;
        name = "libvirt-chv";
        # Helps to keep track of the commit hash in the libvirt log.
        # Nix strips all `.git`, so we need to be explicit here. This
        # is a non-standard functionality of our own libvirt fork.
        commitHash = if self ? rev then self.rev else "local-dirty";
      };

      libvirtPrevPackageSet = mkLibvirtPackageSet {
        inherit pkgs;
        src = libvirtPrevSourceWithSubmodules;
        name = "libvirt-prev-chv";
      };

      # Flake-like output structure for the NixOS test environment.
      nixosTestsOutputs = import ./nixos-tests/outputs.nix {
        inherit
          pkgs
          nixpkgs
          cloud-hypervisor
          cloud-hypervisor-prev
          edk2-src
          fcntl-tool
          ;
        libvirt = libvirtPackageSet.libvirt-debugoptimized;
        libvirt-prev = libvirtPrevPackageSet.libvirt-debugoptimized;
      };

      libvirtPackages = libvirtPackageSet // {
        libvirt-prev = libvirtPrevPackageSet.libvirt;
        libvirt-prev-debugoptimized = libvirtPrevPackageSet.libvirt-debugoptimized;
        default = libvirtPackageSet.libvirt;
        prepare-images = import ./local_tests/prepare-images.nix { inherit pkgs; };
        prepare-windows-image = import ./local_tests/prepare-windows-image.nix { inherit pkgs; };
      };

      # Flake outputs of libvirt itself.
      libvirtOutputs = {
        devShells."x86_64-linux".default = pkgs.mkShell {
          inputsFrom = builtins.attrValues libvirtPackageSet;
        };
        packages."x86_64-linux" = libvirtPackages;
      };
    in
    # Gracefully aggregated flake outputs of libvirt and the test suite.
    {
      checks = nixosTestsOutputs.checks;
      formatter."x86_64-linux" = pkgs.nixfmt-tree;
      tests = nixosTestsOutputs.tests;
      devShells."x86_64-linux" =
        nixosTestsOutputs.devShells."x86_64-linux" // libvirtOutputs.devShells."x86_64-linux";
      packages."x86_64-linux" =
        nixosTestsOutputs.packages."x86_64-linux" // libvirtOutputs.packages."x86_64-linux";
    };
}
