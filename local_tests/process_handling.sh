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
    return $?
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
      logging "I don't run on real hardware. No further process cleanup."
      logging "stop cleanup_processes"
      return
    fi
    local sig="$1"
    local BASE_DIR="/home/benchmark/tmp-${CI_JOB_ID}/${CI_PROJECT_NAME}"

    local targets=()
    targets+=("$(realpath "${BASE_DIR}")")

    # if run on real hardware with nixos test driver
    if [ "${XDG_RUNTIME_DIR:-}" != "" ]; then
        logging "XDG_RUNTIME_DIR is set - add this directory into process check list"
        targets+=("$(realpath "${XDG_RUNTIME_DIR}")")
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

        for target in "${targets[@]}"; do
            case "$cwd" in
                "$target"|"$target"/*)
                    if kill -0 "$pid" 2>/dev/null; then
                        logging "Send ${sig} to pid: $pid (cwd: $cwd)"
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
