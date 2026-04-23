#!/usr/bin/env bash

## build chv on target host

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}/.."
HOSTNAME=$(hostname)
CMD="${1:-current}"

cd ${BASE_DIR}

if [ "$CMD" == "prev" ]; then
  echo "Build cloud hypervisor in the previous release version on ${HOSTNAME}"
  nix build .\#packages.x86_64-linux.cloud-hypervisor-prev
else
  echo "Build cloud hypervisor in the current version on ${HOSTNAME}"
  nix build .\#packages.x86_64-linux.cloud-hypervisor
fi

${BASE_DIR}/result/bin/cloud-hypervisor --version
