#!/usr/bin/env bash

## prepare Ubuntu root disk on shared NFS storage

set -euo pipefail

CI_PIPELINE_ID=$1
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}/.."
NFS_ROOT="/exports/gitlab"
WORK_DIR="${NFS_ROOT}/${CI_PIPELINE_ID}"
cd ${BASE_DIR}

nix build .\#packages.x86_64-linux.prepare-images
${BASE_DIR}/result ${WORK_DIR}

echo "Build CHV firmware"
cd ${BASE_DIR}
nix build .\#packages.x86_64-linux.chv-ovmf
cp result ${WORK_DIR}/CLOUDHV.fd

ls -lha ${WORK_DIR}
