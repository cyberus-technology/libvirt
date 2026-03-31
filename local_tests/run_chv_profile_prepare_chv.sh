#!/usr/bin/env bash

## build chv on target host

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}/.."

cd ${BASE_DIR}
nix build .\#packages.x86_64-linux.cloud-hypervisor

${BASE_DIR}/result/bin/cloud-hypervisor --version
