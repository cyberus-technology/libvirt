{
  description = "libvirt";

  inputs = {
    cloud-hypervisor-src.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux";
    cloud-hypervisor-src.flake = false;
    keycodemapdb.url = "git+https://gitlab.com/keycodemap/keycodemapdb.git";
    keycodemapdb.flake = false;
    libvirt-tests.url = "github:cyberus-technology/libvirt-tests";
    libvirt-tests.inputs.cloud-hypervisor-src.follows = "cloud-hypervisor-src";
    libvirt-tests.inputs.nixpkgs.follows = "nixpkgs";
    # We follow the latest stable release of nixpkgs
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
  };

  outputs =
    {
      self,
      nixpkgs,
      libvirt-tests,
      keycodemapdb,
      ...
    }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;

      # libvirt requires a populated submodule to build successfully. As we
      # cannot reference a local git directory as a flake input to let nix
      # handle the submodule population, we assemble it ourself.
      sourceWithSubmodules =
        (pkgs.runCommand "source-with-submodules" { } ''
          mkdir -p $out

          cp -r ${self}/* $out/

          mkdir -p $out/subprojects/keycodemapdb
          cp -r ${keycodemapdb}/* $out/subprojects/keycodemapdb/
        '')
        # We make the source behave somewhat like a real flake input.
        .overrideAttrs
          {
            rev = if self ? rev then self.rev else self.dirtyRev;
          };

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
      tests = forAllSystems (
        pkgs:
        let
          lib = pkgs.lib;
          system = pkgs.stdenv.hostPlatform.system;
        in
        # New attribute set with updated `libvirt-src` input for each test.
        lib.concatMapAttrs (name: value: {
          ${name} = value.override {
            libvirt-src = sourceWithSubmodules;
          };
        }) libvirt-tests.tests.${system}
      );
    };
}
