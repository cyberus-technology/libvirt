{
  pkgs,
}:
let
  # The image size is currently 20 GiB
  windows_img = pkgs.fetchurl {
    url = "https://nexus.vpn.cyberus-technology.de/repository/vm-test-images/server-2025-root-small-ssh-enabled.raw";
    hash = "sha256-Afc5ectMbmVxch8ivflQ4G27CcpKhCFsLPf5J9I+1KE=";
  };

in
pkgs.writeShellScript "prepare-windows-image.sh" ''
  set -euo pipefail

  WORK_DIR=$1
  echo "Prepare windows root disk in pipeline working directory: $WORK_DIR"
  mkdir $WORK_DIR
  cp ${windows_img} $WORK_DIR/windows-root.raw
  chmod 666 $WORK_DIR/windows-root.raw
''
