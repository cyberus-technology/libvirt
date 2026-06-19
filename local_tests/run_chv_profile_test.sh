#!/usr/bin/env bash

# all cleanup tasks are done by generic script: bare_metal_chv_cleanup.sh
#
# DON'T use any pipes in a `start_async` remote command

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source ${SCRIPT_DIR}/process_handling.sh
source ${SCRIPT_DIR}/helper_functions.sh

# seconds
NETWORK_TIMEOUT="60"

CI_JOB_ID=$1
CI_PROJECT_NAME=$2
TAPDEV=$3
CHV_VERSION="${4:-current}"

# please look into helper_functions.sh
VM_IP=$(get_vm_ip_address $TAPDEV)
VM_MAC=$(get_vm_mac_address $TAPDEV)
VMM_PORT=$(get_vmm_port $TAPDEV)

HOST1="ferona-granite-rapids"   # 172.16.0.159
HOST2="ferona-sapphire-rapids"  # 172.16.0.82

NFS_ROOT="/shared/ferona-turin/gitlab/${CI_JOB_ID}"

# Perform specific tasks when we have received the INT / TERM / EXIT signals.
# This function is called in process_handling.sh
# This function is can be specific for each test script.
forward_signal_local() {
  logging "Running forward_signal_local"
  collect_logs
}

# Start real tests

# run vm on host1
logging "Start test-vm on host1: ${HOST1}"
start_async ssh -F ~/.ssh/config ${HOST1} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/cloud-hypervisor \
  --net tap=${TAPDEV},mac=${VM_MAC} \
  --firmware ${NFS_ROOT}/CLOUDHV.fd \
  --disk path=${NFS_ROOT}/jammy-server-cloudimg-amd64.raw,image_type=raw,sparse=off \
    path=${NFS_ROOT}/cloud-init.raw,image_type=raw,sparse=off \
  --memory size=2G --cpus boot=2,profile=sapphire-rapids \
  --serial file=/shared/ferona-turin/gitlab/${CI_JOB_ID}/serial.log \
  --console off \
  --no-shutdown \
  --log-file /home/benchmark/tmp-${CI_JOB_ID}/${HOST1}.chv.log \
  --api-socket /tmp/chv.${CI_JOB_ID}.sock \
  -v"

# if the check fails collect all logs for further debugging
check_vm ${HOST1} ${VM_IP} ${NETWORK_TIMEOUT}

logging "Run CHV only with api socket on host2: ${HOST2}"
start_async ssh -F ~/.ssh/config ${HOST2} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/cloud-hypervisor \
  --log-file /home/benchmark/tmp-${CI_JOB_ID}/${HOST2}.chv.log \
  --no-shutdown \
  --api-socket /tmp/chv.${CI_JOB_ID}.sock \
  -v"

sleep 2

logging "Debug Host2 socket"
ssh -F ~/.ssh/config ${HOST2} 'ls -l /tmp/chv*'

logging "Run ch-remote info on host: ${HOST1}"
start_sync ssh -F ~/.ssh/config ${HOST1} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket \
  /tmp/chv.${CI_JOB_ID}.sock info" || collect_logs_exit_error

logging "Run ch-remote ping on host: ${HOST1}"
start_sync ssh -F ~/.ssh/config ${HOST1} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket \
  /tmp/chv.${CI_JOB_ID}.sock ping" || collect_logs_exit_error

logging "Run ch-remote ping on host: ${HOST2}"
start_sync ssh -F ~/.ssh/config ${HOST2} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket \
  /tmp/chv.${CI_JOB_ID}.sock ping" || collect_logs_exit_error

# prepare host2
logging "Run ch-remote receive-migration on host2: ${HOST2}"
start_async ssh -F ~/.ssh/config ${HOST2} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket \
  /tmp/chv.${CI_JOB_ID}.sock receive-migration tcp:0.0.0.0:${VMM_PORT}"

logging "Debug processes on host2"
ssh -F ~/.ssh/config ${HOST2} "ps axu | grep tmp-${CI_JOB_ID}"

# send vm
logging "Run ch-remote send-migration on host: ${HOST1}"
if [ "$CHV_VERSION" == "v51" ]; then
  # use old chv v51 syntax
  start_sync ssh -F ~/.ssh/config ${HOST1} \
    "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
    --api-socket \
    /tmp/chv.${CI_JOB_ID}.sock send-migration tcp:172.16.0.82:${VMM_PORT}" || collect_logs_exit_error
else
  start_sync ssh -F ~/.ssh/config ${HOST1} \
    "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
    --api-socket \
    /tmp/chv.${CI_JOB_ID}.sock send-migration destination_url=tcp:172.16.0.82:${VMM_PORT},connections=8,keep_alive=true" || collect_logs_exit_error
fi

logging "Run ch-remote info on host: ${HOST2}"
start_sync ssh -F ~/.ssh/config ${HOST2} \
  "/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket \
  /tmp/chv.${CI_JOB_ID}.sock info" || collect_logs_exit_error

logging "Migration complete from ${HOST1} to ${HOST2} - check vm on ${HOST2} now"
check_vm ${HOST2} ${VM_IP} ${NETWORK_TIMEOUT}
collect_logs
cleanup
