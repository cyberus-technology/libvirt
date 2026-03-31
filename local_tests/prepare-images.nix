{
  pkgs,
}:
let
  ubuntu_img = pkgs.fetchurl {
    url = "https://cloud-images.ubuntu.com/jammy/20260320/jammy-server-cloudimg-amd64.img";
    hash = "sha256-6oWxb4Gz9qpToSYJEtP5kfwz4OD8HXPwuMnJYkfkL9s=";
  };

  ubuntu_raw = pkgs.runCommand "ubuntu_raw" { } ''
    ${pkgs.qemu-utils}/bin/qemu-img convert -f qcow2 -O raw ${ubuntu_img} $out
  '';

  # Seed image for the Ubuntu cloud image used in the tests. It creates an
  # `ubuntu` user with password `ubuntu` and the fixed guest network expected
  # by the test topology.
  ubuntu_cloud_init =
    pkgs.runCommand "ubuntu_cloud_init"
      {
        nativeBuildInputs = with pkgs; [
          cloud-utils
          cloud-init
          mkpasswd
        ];
      }
      ''
        password_hash="$(mkpasswd --method=SHA-512 ubuntu)"

        substitute ${./cloud-init/user-data.yaml.in} user-data \
          --subst-var-by password_hash "$password_hash"

        cp ${./cloud-init/meta-data.yaml} meta-data
        cp ${./cloud-init/network-config.yaml} network-config

        cloud-init schema --config-file user-data
        cloud-localds -N network-config "$out" user-data meta-data
      '';

in
pkgs.writeShellScript "prepare-images.sh" ''
  set -euo pipefail

  WORK_DIR=$1
  echo "Prepare ubuntu root disk and cloud-init disk in pipeline working directory: $WORK_DIR"
  mkdir $WORK_DIR
  cp ${ubuntu_cloud_init} $WORK_DIR/cloud-init.raw
  cp ${ubuntu_raw} $WORK_DIR/jammy-server-cloudimg-amd64.raw
  chmod 666 $WORK_DIR/cloud-init.raw
  chmod 666 $WORK_DIR/jammy-server-cloudimg-amd64.raw
''
