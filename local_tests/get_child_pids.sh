#!/usr/bin/env bash

set -euo pipefail

# wait until all processes are started
sleep 5

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR=$(dirname $SCRIPT_DIR)
TEST_DRIVER_PIDS=$(pgrep -f .nixos-test-driver-wrapped)

for P in ${TEST_DRIVER_PIDS}; do
  PROD_CWD=$(readlink -f /proc/${P}/cwd)
  if [ "$PROD_CWD" == "$REPO_DIR" ]; then
    pgrep -P ${P} > ${SCRIPT_DIR}/../../child_pids.txt  # the tmp-${CI_PIPELINE_ID} directory
    echo ${P} >> ${SCRIPT_DIR}/../../child_pids.txt
  fi
done
