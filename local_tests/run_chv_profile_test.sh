#!/usr/bin/env bash

set -euo pipefail

CI_PIPELINE_ID=$1
CI_PROJECT_NAME=$2

HOST1="ferona-granite-rapids"   # 172.16.0.159
HOST2="ferona-sapphire-rapids"  # 172.16.0.82
VM_IP="192.168.100.70"

NFS_ROOT="/shared/ferona-turin/gitlab/${CI_PIPELINE_ID}"

shutdown_vm() {
  local HOST=$1
  ssh -F ~/.ssh/config ${HOST} \
  ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock shutdown

  sleep 3

  ssh -F ~/.ssh/config ${HOST} \
  ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock shutdown-vmm
}

collect_logs() {
  echo "Collect log files from bare metal hosts"
  scp -F ~/.ssh/config ${HOST1}:/home/benchmark/tmp-${CI_PIPELINE_ID}/*.log ./logs
  scp -F ~/.ssh/config ${HOST2}:/home/benchmark/tmp-${CI_PIPELINE_ID}/*.log ./logs
}

check_vm() {
  local SSH_HOST=$1
  local TARGET=$2
  local tries=$3

  echo "Check Host: ${TARGET} from: ${SSH_HOST}"

  for i in $(seq 1 ${tries}); do
    set +e
    ssh -F ~/.ssh/config ${SSH_HOST} ping -c 1 ${TARGET}
    if [ $? -eq 0 ]; then
      echo "Host: ${TARGET} are reachable from: ${SSH_HOST}"
      set -e
      return 0
    else
      echo "Host: ${TARGET} are not reachable from: ${SSH_HOST} - iteration $i / ${tries}"
    fi
  done
  set -e
  return 1
}

# run vm on host1
# keep mac address in sync with dnsmask configuration on each host (static dhcp configuration of test-vm ip: 192.168.100.70)
#   look here: https://gitlab.cyberus-technology.de/cyberus/cloud/hardware/-/blob/main/modules/host-services.nix?ref_type=heads#L43
echo "Start test-vm on: ${HOST1}"
ssh -F ~/.ssh/config ${HOST1} \
  "nohup ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/cloud-hypervisor \
  --net tap=tap10,mac=be:e3:00:00:00:01 \
  --kernel ${NFS_ROOT}/CLOUDHV.fd \
  --disk path=${NFS_ROOT}/jammy-server-cloudimg-amd64.raw,image_type=raw,sparse=off \
    path=${NFS_ROOT}/cloud-init.raw,image_type=raw,sparse=off \
  --memory size=2G --cpus boot=2,profile=sapphire-rapids \
  --serial tty \
  --console off \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock | tee -a ./tmp-${CI_PIPELINE_ID}/${HOST1}.chv.log" &

check_vm ${HOST1} ${VM_IP} 15

echo "Run CHV only with api socket on host2: ${HOST2}"
ssh -F ~/.ssh/config ${HOST2} \
  "nohup ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/cloud-hypervisor \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock | tee -a ./tmp-${CI_PIPELINE_ID}/${HOST2}.chv.log" &

sleep 2

echo "Debug Host2 socket"
ssh -F ~/.ssh/config ${HOST2} 'ls -l /tmp/chv*'

echo "Run ch-remote receive-migration on host2: ${HOST2}"
ssh -F ~/.ssh/config ${HOST2} \
  "nohup ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock receive-migration tcp:0.0.0.0:4711 \
  | tee -a ./tmp-${CI_PIPELINE_ID}/${HOST2}.ch-remote.log" &

echo "Debug processes on host2"
ssh -F ~/.ssh/config ${HOST2} "ps axu | grep tmp-${CI_PIPELINE_ID}"

echo "Run ch-remote send-migration on host1: ${HOST1}"
ssh -F ~/.ssh/config ${HOST1} \
  "./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock send-migration tcp:172.16.0.82:4711 \
  | tee -a ./tmp-${CI_PIPELINE_ID}/${HOST1}.ch-remote.log"

echo "Migration complete from ${HOST1} to ${HOST2}"
check_vm ${HOST2} ${VM_IP} 15
collect_logs

# cleanup
set +e
shutdown_vm ${HOST2}
