{
  description = "libvirt";

  inputs = {
    cloud-hypervisor-src.url = "github:cyberus-technology/cloud-hypervisor?ref=gardenlinux";
    cloud-hypervisor-src.flake = false;
    # We follow the latest stable release of nixpkgs
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.05";
    libvirt-tests = {
      url = "github:cyberus-technology/libvirt-tests";
      inputs.cloud-hypervisor-src.follows = "cloud-hypervisor-src";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    keycodemapdb = {
      url = "git+https://gitlab.com/keycodemap/keycodemapdb.git";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, libvirt-tests, keycodemapdb, ... }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;

      # libvirt requires a populated submodule to build successfully. As we
      # cannot reference a local git directory as a flake input to let nix
      # handle the submodule population, we assemble it ourself.
      sourceWithSubmodules = pkgs.runCommand "source-with-submodules" {} ''
        mkdir -p $out

        cp -r ${self}/* $out/

        mkdir -p $out/subprojects/keycodemapdb
        cp -r ${keycodemapdb}/* $out/subprojects/keycodemapdb/
      '';

      # We just use "every system" here to not restrict any user. However, it
      # likely happens that certain packages don't build for/under certain
      # systems.
      systems = nixpkgs.lib.systems.flakeExposed;
      forAllSystems =
        function: nixpkgs.lib.genAttrs systems (system: function nixpkgs.legacyPackages.${system});
    in
    {
      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ pkgs.libvirt ];
        };
      });
      tests = libvirt-tests.tests.x86_64-linux.override {libvirt-src = sourceWithSubmodules; };
    };
}
