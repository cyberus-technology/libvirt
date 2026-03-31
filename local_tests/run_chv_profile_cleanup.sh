#!/usr/bin/env bash

CI_PIPELINE_ID=$1
CI_PROJECT_NAME=$2

HOST1="ferona-granite-rapids"   # 172.16.0.159
HOST2="ferona-sapphire-rapids"  # 172.16.0.82

shutdown_vm() {
  local HOST=$1
  ssh -F ~/.ssh/config ${HOST} \
  ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock shutdown || true

  ssh -F ~/.ssh/config ${HOST} \
  ./tmp-${CI_PIPELINE_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_PIPELINE_ID}.sock shutdown-vmm || true
}

shutdown_vm ${HOST1}
shutdown_vm ${HOST2}
