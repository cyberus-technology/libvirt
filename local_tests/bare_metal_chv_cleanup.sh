#!/usr/bin/env bash

# This script is called by gitlab runner in the after_script section of each bare metal job.
# This script does the hole process cleanup on both hosts.
#   shutdown the pipeline vm
#   shutdown the pipeline vmm
#   send TERM to CHV

HOST1="ferona-granite-rapids"   # 172.16.0.159
HOST2="ferona-sapphire-rapids"  # 172.16.0.82

# this should only be run in the post stage of gitlab
# to cleanup all jobs in this pipeline run
get_all_jobs_ids() {
  echo "Start get_all_jobs_ids"

  API_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/pipelines/${CI_PIPELINE_ID}/jobs"
  IDS=$(curl --silent --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${API_URL}" | jq -r '.[].id')
  for i in ${IDS}; do
    echo "Cleanup all hosts for JOB_ID: ${i}"
    shutdown_vm ${HOST1} ${i}
    shutdown_vm ${HOST2} ${i}
  done
  echo "Finish get_all_jobs_ids"
}

shutdown_vm() {
  local HOST=$1
  local JOB_ID=$2
  # set -x
  echo "Debug running CHV processes on host ${HOST}"
  ssh -F ~/.ssh/config ${HOST} "ps axu | grep cloud-hypervisor | grep ${JOB_ID}" || true

  echo "Send shutdown to CHV on host ${HOST}"
  ssh -F ~/.ssh/config ${HOST} \
  /home/benchmark/tmp-${JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${JOB_ID}.sock shutdown || true

  echo "Send shutdown-vmm to CHV on host ${HOST}"
  ssh -F ~/.ssh/config ${HOST} \
  /home/benchmark/tmp-${JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${JOB_ID}.sock shutdown-vmm || true

  echo "Send TERM signal to CHV on host ${HOST}"
  ssh -F ~/.ssh/config ${HOST} \
  pkill -f "/home/benchmark/tmp-${JOB_ID}/${CI_PROJECT_NAME}/result/bin/cloud-hypervisor" || true

  echo "Remove CHV API socket: /tmp/chv.${JOB_ID}.sock on host ${HOST}"
  ssh -F ~/.ssh/config ${HOST} \
  rm -f /tmp/chv.${JOB_ID}.sock || true
  # set +x
}

if [ "$1" == "all" ]; then
  echo "Cleanup all JOB_IDs of this CI_PIPELINE_ID: $CI_PIPELINE_ID"
  get_all_jobs_ids
else
  echo "Cleanup only this CI_JOB_ID: $CI_JOB_ID"
  shutdown_vm ${HOST1} ${CI_JOB_ID}
  shutdown_vm ${HOST2} ${CI_JOB_ID}
fi
