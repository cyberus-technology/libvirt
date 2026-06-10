#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source ${SCRIPT_DIR}/process_handling.sh
source ${SCRIPT_DIR}/helper_functions.sh

CI_JOB_ID=$1
CI_PROJECT_NAME=$2

WORKDIR="$HOME/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}"

if [ ! -d "$WORKDIR" ]; then
  echo "error: directory $WORKDIR didn't exists"
  exit 1
fi

# save main pid for cleanup_remote.sh (triggered by gitlab after_script)
echo $$ > $WORKDIR/.main.pid

cd $WORKDIR

########################
# build test suite
########################
echo ">>> Build test suite"
start_sync nix build .\#tests.x86_64-linux.cpu_profiles.passthru.no_port_forwarding.driver

########################
# run test suite
########################
XDG_RUNTIME_DIR=$(mktemp -d)
export XDG_RUNTIME_DIR
# save XDG_RUNTIME_DIR for cleanup_remote.sh (triggered by gitlab after_script)
echo $XDG_RUNTIME_DIR > $WORKDIR/.xdg_runtime_dir

echo ">>> Run test suite"
start_sync nix run .\#tests.x86_64-linux.cpu_profiles.passthru.no_port_forwarding.driver
