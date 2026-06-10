#!/usr/bin/env bash

CI_JOB_ID=$1
CI_PROJECT_NAME=$2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source ${SCRIPT_DIR}/process_handling.sh
source ${SCRIPT_DIR}/helper_functions.sh

if [ -z "${CI_JOB_ID}" ]; then
  echo "Environment variable CI_JOB_ID is empty."
  exit 1
fi

if [ -z "${CI_PROJECT_NAME}" ]; then
  echo "Environment variable CI_PROJECT_NAME is empty."
  exit 1
fi

PID=$(cat tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/.main.pid)
export XDG_RUNTIME_DIR=$(cat tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/.xdg_runtime_dir)
cleanup_processes TERM
kill $PID

# cleanup working directory
rm -rf tmp-${CI_JOB_ID}
