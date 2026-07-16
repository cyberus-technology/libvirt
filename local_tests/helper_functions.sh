# This file only contains functions and will be sourced by other scripts.

shutdown_vm() {
  local HOST=$1
  logging "Start shutdown_vm ${HOST}"
  ssh -F ~/.ssh/config ${HOST} \
  /home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_JOB_ID}.sock shutdown || true

  sleep 3

  ssh -F ~/.ssh/config ${HOST} \
  /home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}/result/bin/ch-remote \
  --api-socket /tmp/chv.${CI_JOB_ID}.sock shutdown-vmm || true
  logging "Finished shutdown_vm ${HOST}"
}

collect_logs() {
  logging "Start collect_logs: copy files from bare metal hosts into local ./logs directory"
  ssh -F ~/.ssh/config ${HOST1} sync
  ssh -F ~/.ssh/config ${HOST2} sync
  ssh -F ~/.ssh/config ferona-turin sync
  scp -F ~/.ssh/config "${HOST1}:/home/benchmark/tmp-${CI_JOB_ID}/*.log" ./logs || true
  scp -F ~/.ssh/config "${HOST2}:/home/benchmark/tmp-${CI_JOB_ID}/*.log" ./logs || true
  scp -F ~/.ssh/config "ferona-turin:/shared/ferona-turin/gitlab/${CI_JOB_ID}/serial.log" ./logs || true
  ls -la ./logs/*
  logging "Finished collect_logs"
}

collect_logs_exit_error() {
  collect_logs
  logging "Run now: 'exit 1'"
  exit 1
}

wait_for_migration_receiver() {
  local SSH_HOST=$1
  local PORT=$2
  local tries=$3

  logging "Wait for migration receiver on ${SSH_HOST}:${PORT}"

  for i in $(seq 1 ${tries}); do
    set +e
    ssh -F ~/.ssh/config ${SSH_HOST} \
      "ss -H -ltn 'sport = :${PORT}' | grep -q LISTEN"
    if [ $? -eq 0 ]; then
      logging "Migration receiver is ready on ${SSH_HOST}:${PORT}"
      set -e
      return 0
    fi
    set -e
    logging "Migration receiver is not ready on ${SSH_HOST}:${PORT} - iteration $i / ${tries}"
    sleep 1
  done

  logging "Migration receiver is not ready on ${SSH_HOST}:${PORT}"
  return 1
}

check_vm() {
  local SSH_HOST=$1
  local TARGET=$2
  local tries=$3

  logging "Check Host: ${TARGET} from: ${SSH_HOST}"

  for i in $(seq 1 ${tries}); do
    set +e
    ssh -F ~/.ssh/config ${SSH_HOST} ping -c 1 ${TARGET}
    if [ $? -eq 0 ]; then
      logging "Host: ${TARGET} are reachable from: ${SSH_HOST}"
      set -e
      return 0
    else
      logging "Host: ${TARGET} are not reachable from: ${SSH_HOST} - iteration $i / ${tries}"
    fi
  done
  set -e
  # VM not reachable - collect all logs
  logging "Host: ${TARGET} are not reachable from: ${SSH_HOST} and run now: collect_logs_exit_error"
  collect_logs_exit_error
}

cleanup() {
  logging "Start cleanup"
  shutdown_vm ${HOST1} || true
  shutdown_vm ${HOST2} || true
  logging "Finished cleanup"
}


# keep mac address in sync with dnsmask configuration on each host
#   look here: https://gitlab.cyberus-technology.de/cyberus/cloud/hardware/-/blob/main/modules/host-services.nix?ref_type=heads#L43
get_vm_config() {
  local tapdev=$1
  local out=()
  case "$tapdev" in
    "tap10")
      out="192.168.100.60#02:50:F2:00:01:81#4711"
      ;;
    "tap11")
      out="192.168.100.61#02:50:F2:00:01:82#4712"
      ;;
    "tap12")
      out="192.168.100.62#02:50:F2:00:01:83#4713"
      ;;

    "tap15")
      out="192.168.100.70#be:e3:00:00:00:01#4715"
      ;;
    "tap16")
      out="192.168.100.71#be:e3:00:00:00:02#4716"
      ;;
    "tap17")
      out="192.168.100.72#be:e3:00:00:00:03#4717"
      ;;
    # tap20 - tap29 multi queue devices
    "tap20")
      out="192.168.100.90#02:50:F2:00:01:90#4790"
      ;;
    "tap21")
      out="192.168.100.91#02:50:F2:00:01:91#4791"
      ;;
    "tap22")
      out="192.168.100.92#02:50:F2:00:01:92#4792"
      ;;
    "tap23")
      out="192.168.100.93#02:50:F2:00:01:93#4793"
      ;;
    "tap24")
      out="192.168.100.94#02:50:F2:00:01:94#4794"
      ;;
    "tap25")
      out="192.168.100.95#02:50:F2:00:01:95#4795"
      ;;
    "tap26")
      out="192.168.100.96#02:50:F2:00:01:96#4796"
      ;;
    "tap27")
      out="192.168.100.97#02:50:F2:00:01:97#4797"
      ;;
    "tap28")
      out="192.168.100.98#02:50:F2:00:01:98#4798"
      ;;
    "tap29")
      out="192.168.100.99#02:50:F2:00:01:99#4799"
      ;;
    *)
      logging "Error: for tap device: $tapdev has no valid configuration"
      exit 5
  esac
  echo $out
}

get_vm_ip_address() {
  local tapdev=$1
  get_vm_config $tapdev | cut -f 1 -d '#'
}

get_vm_mac_address() {
  local tapdev=$1
  get_vm_config $tapdev | cut -f 2 -d '#'
}

get_vmm_port() {
  local tapdev=$1
  get_vm_config $tapdev | cut -f 3 -d '#'
}
