#!/usr/bin/env bash

## prepare Ubuntu root disk on shared NFS storage
## name of tap device is required as parameter to build a specific cloud-init image
## with a matching mac address which matches to the dhcp server setup
## please look into
##  https://gitlab.cyberus-technology.de/cyberus/cloud/libvirt/-/blob/gardenlinux/local_tests/helper_functions.sh#L70
##  https://gitlab.cyberus-technology.de/cyberus/cloud/hardware/-/blob/main/modules/host-services.nix?ref_type=heads#L43

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source ${SCRIPT_DIR}/helper_functions.sh

CI_JOB_ID=$1
TAPDEV=$2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}/.."
NFS_ROOT="/exports/gitlab"
WORK_DIR="${NFS_ROOT}/${CI_JOB_ID}"
VM_MAC=$(get_vm_mac_address $TAPDEV)
echo $VM_MAC > ${SCRIPT_DIR}/cloud-init/vm_mac

cd ${BASE_DIR}

nix build .\#packages.x86_64-linux.prepare-images
${BASE_DIR}/result ${WORK_DIR}

echo "Build CHV firmware"
cd ${BASE_DIR}
nix build .\#packages.x86_64-linux.chv-ovmf
cp result ${WORK_DIR}/CLOUDHV.fd

ls -lha ${WORK_DIR}
