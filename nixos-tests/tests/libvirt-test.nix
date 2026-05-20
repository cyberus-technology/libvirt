# Sets up a NixOS integration test with two VMs running NixOS.
#
# This will run our test suite.

{
  pkgs,
  nixos-image,
  chv-ovmf,
  testScriptFile,
  enablePortForwarding,
  extraControllerConfig ? [ ],
  extraComputeConfig ? [ ],
}:
let
  common = import ./common.nix { inherit nixos-image chv-ovmf; };
  tls =
    let
      c = pkgs.callPackage ./certificates.nix { };
    in
    {
      ca = c.tlsCA;
      controller = c.mkHostCert "controllerVM" "192.168.100.1";
      compute = c.mkHostCert "computeVM" "192.168.100.2";
    };
  hostVMHosts = {
    "192.168.100.1" = [
      "controllerVM"
      "controllerVM.local"
    ];
    "192.168.100.2" = [
      "computeVM"
      "computeVM.local"
    ];
  };
in
pkgs.testers.nixosTest {
  name = "Libvirt test suite for Cloud Hypervisor";

  extraPythonPackages =
    p: with p; [
      pytest
      test-helper
    ];

  nodes.controllerVM =
    { lib, ... }:
    {
      imports = [
        common
        ../modules/nfs-host.nix
      ]
      ++ extraControllerConfig;

      virtualisation = {
        cores = 4;
        memorySize = 4096;
        # Deactivate default eth1 network setup as we configure it manually
        vlans = [ ];
        diskSize = 28672;
        forwardPorts =
          # Port forwarding prevents us from executing the nixos tests in
          # parallel in the CI, as they run in the same context and ports are
          # already occupied then.
          (
            lib.optionals enablePortForwarding [
              {
                from = "host";
                host.port = 2222;
                guest.port = 22;
              }
            ]
          );
        qemu.networkingOptions = lib.mkAfter [
          "-device virtio-net-pci,netdev=hostvm,mac=52:54:00:12:01:02"
          ''-netdev stream,id=hostvm,server=off,addr.type=unix,addr.path="$SHARED_DIR"/hostvm-net.sock''
        ];
      };

      boot.initrd.services.udev.rules = ''
        SUBSYSTEM=="net",ACTION=="add",ATTR{address}=="52:54:00:12:01:02",NAME="eth1"
      '';

      networking.hosts = hostVMHosts;

      systemd.network = {
        enable = true;
        wait-online.enable = false;
        networks = {
          eth0 = {
            matchConfig.Name = [ "eth0" ];
            networkConfig.DHCP = "yes";
          };
          eth1 = {
            matchConfig.Name = [ "eth1" ];
            networkConfig = {
              Address = "192.168.100.1/24";
              DHCP = "no";
              IPv6AcceptRA = false;
              LinkLocalAddressing = "no";
            };
          };
        };
      };

      systemd.tmpfiles.settings."11-certs" = {
        "/var/lib/libvirt/ch/pki/ca-cert.pem"."C+".argument = "${tls.ca}/ca-cert.pem";
        "/var/lib/libvirt/ch/pki/server-cert.pem"."C+".argument = "${tls.controller}/server-cert.pem";
        "/var/lib/libvirt/ch/pki/server-key.pem"."C+".argument = "${tls.controller}/server-key.pem";
      };
    };

  nodes.computeVM =
    { lib, ... }:
    {
      imports = [
        common
        ../modules/nfs-client.nix
      ]
      ++ extraComputeConfig;

      networking.hosts = hostVMHosts;

      virtualisation = {
        cores = 4;
        memorySize = 4096;
        # Deactivate default eth1 network setup as we configure it manually
        vlans = [ ];
        diskSize = 2048;
        forwardPorts =
          # Port forwarding prevents us from executing the nixos tests in
          # parallel in the CI, as they run in the same context and ports are
          # already occupied then.
          (
            lib.optionals enablePortForwarding [
              {
                from = "host";
                host.port = 3333;
                guest.port = 22;
              }
            ]
          );
        qemu.networkingOptions = lib.mkAfter [
          "-device virtio-net-pci,netdev=hostvm,mac=52:54:00:12:01:01"
          # The test driver starts computeVM before controllerVM, so computeVM
          # owns the listening end of the host-VM socket.
          ''-netdev stream,id=hostvm,server=on,addr.type=unix,addr.path="$SHARED_DIR"/hostvm-net.sock''
        ];
      };

      boot.initrd.services.udev.rules = ''
        SUBSYSTEM=="net",ACTION=="add",ATTR{address}=="52:54:00:12:01:01",NAME="eth1"
      '';

      livemig.nfs.host = "192.168.100.1";

      systemd.network = {
        enable = true;
        wait-online.enable = false;
        networks = {
          eth0 = {
            matchConfig.Name = [ "eth0" ];
            networkConfig.DHCP = "yes";
          };
          eth1 = {
            matchConfig.Name = [ "eth1" ];
            networkConfig = {
              Address = "192.168.100.2/24";
              DHCP = "no";
              IPv6AcceptRA = false;
              LinkLocalAddressing = "no";
            };
          };
        };
      };

      systemd.tmpfiles.settings."11-certs" = {
        "/var/lib/libvirt/ch/pki/ca-cert.pem"."C+".argument = "${tls.ca}/ca-cert.pem";
        "/var/lib/libvirt/ch/pki/server-cert.pem"."C+".argument = "${tls.compute}/server-cert.pem";
        "/var/lib/libvirt/ch/pki/server-key.pem"."C+".argument = "${tls.compute}/server-key.pem";
      };
    };

  testScript = { ... }: builtins.readFile testScriptFile;
}
