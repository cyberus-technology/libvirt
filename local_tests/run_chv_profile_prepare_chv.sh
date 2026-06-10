#!/usr/bin/env bash

## build chv on target host

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source ${SCRIPT_DIR}/process_handling.sh

CI_JOB_ID=$1
CI_PROJECT_NAME=$2
CMD="${3:-current}"

BASE_DIR="/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}"
HOSTNAME=$(hostname)

cd ${BASE_DIR}

if [ "$CMD" == "prev" ]; then
  echo "Build cloud hypervisor in the previous release version on ${HOSTNAME}"
  start_sync nix build .\#packages.x86_64-linux.cloud-hypervisor-prev
else
  echo "Build cloud hypervisor in the current version on ${HOSTNAME}"
  start_sync nix build .\#packages.x86_64-linux.cloud-hypervisor
fi

# print current version of CHV
${BASE_DIR}/result/bin/cloud-hypervisor --version
