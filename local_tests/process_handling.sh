# This file only contains functions and will be sourced by other scripts.

# Array of child PIDs started by this script
children=()
handler_running=0

logging() {
    local message="$@"
    local d=$(date +%F-%N)
    echo "$$ $d $message"
}

# Start a process and remember its PID and wait for completion
start_sync() {
    logging "start sync process: $@"
    "$@" &
    local pid="$!"
    logging "add pid: $pid to list of child processes"
    children+=("$pid")
    wait $pid
    local ec="$?"
    logging "end sync process: $@"
    logging "end sync process with exit code: $ec"
    return $ec
}

# Start a process and remember its PID
# DON'T use any pipes in a `start_async` remote command
start_async() {
    logging "start async process: $@"
    "$@" &
    local pid="$!"
    logging "add pid: $pid to list of child processes"
    children+=("$pid")
}

# Forward a signal to all tracked children
forward_signal() {
    if [ $handler_running -ne 0 ]; then
        logging "Handler already running. skip it now"
        return 0
    fi
    handler_running=1
    logging "forward_signal"
    local sig="$1"

    logging "Received ${sig}, forwarding to children..."

    for pid in "${children[@]}"; do
        logging "11 handle PID:  $pid"
        if [ "$pid" == "$$" ]; then
          # don't kill myself
          continue
        fi
        logging "22 handle PID:  $pid"
        if kill -0 "$pid" 2>/dev/null; then
            logging "Send ${sig} to pid: $pid"
            kill "-${sig}" "$pid" 2>/dev/null || true
        fi
    done

    cleanup_processes ${sig}

    if [[ $(type -t forward_signal_local) == function ]]; then
        logging "local funtion forward_signal_local is defined. I'll call it"
        forward_signal_local
    fi
}

cleanup_processes() {
    logging "start cleanup_processes"
    local HOSTNAME=$(hostname)
    if [[ ! "$HOSTNAME" =~ ^ferona-.* ]]; then
      logging "Hostname is $HOSTNAME. I don't run on real hardware. No further process cleanup."
      logging "stop cleanup_processes"
      return
    fi
    local sig="$1"
    local BASE_DIR="/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}"

    local targets=()
    targets+=("${BASE_DIR}")
    # add BASE_DIR also in case if the directory is already deleted by an other process
    targets+=("${BASE_DIR} (deleted)")

    # if run on real hardware with nixos test driver
    # in run_cpu_profile_test.sh XDG_RUNTIME_DIR will be set to a temporary directory in: /tmp
    # XDG_RUNTIME_DIR=$(mktemp -d)
    # all processes in this directory must be removed
    if [ "${XDG_RUNTIME_DIR:-}" != "" ]; then
        if [[ "${XDG_RUNTIME_DIR}" != "/run/user/"* ]]; then
            logging "XDG_RUNTIME_DIR is set to a non default directory - add this directory into process check list"
            targets+=("${XDG_RUNTIME_DIR}")
            # add XDG_RUNTIME_DIR also in case if the directory is already deleted by an other process
            targets+=("${XDG_RUNTIME_DIR} (deleted)")
        fi
    fi

    logging "Cleanup all processes with current working directory in: ${targets[*]}"

    for proc in /proc/[0-9]*; do
        local pid=${proc#/proc/}

        if [ "$pid" == "$$" ]; then
          continue
        fi

        # Some processes may disappear while iterating
        [ -e "$proc/cwd" ] || continue

        cwd=$(readlink -f "$proc/cwd" 2>/dev/null) || continue
        exe=$(readlink -f "$proc/exe" 2>/dev/null) || exe="can-not-read-executeable-path-from-proc"

        for target in "${targets[@]}"; do
            case "$cwd" in
                "$target")
                    if kill -0 "$pid" 2>/dev/null; then
                        logging "Send ${sig} to pid: $pid (cwd: $cwd - exe: $exe)"
                        logging "DEBUG: target: $target"
                        kill "-${sig}" "$pid" 2>/dev/null || true
                    fi
                    break
                    ;;
            esac
        done
    done
    logging "stop cleanup_processes"
}

# Signal handlers
handle_sigint() {
    logging "Received INT send INT to children"
    forward_signal INT
}

handle_sigterm() {
    logging "Received TERM send TERM to children"
    forward_signal TERM
}

handle_sighup() {
    logging "Received HUP send TERM to children"
    forward_signal TERM
}

handle_exit() {
    logging "Received EXIT send TERM to children"
    forward_signal TERM
}

trap handle_sigint INT
trap handle_sigterm TERM
trap handle_sighup HUP
trap handle_exit EXIT
