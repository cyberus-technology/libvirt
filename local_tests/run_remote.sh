#!/usr/bin/env bash

# this script handles signals from the gitlab runner
# if the script receives a TERM or INT signal
# this will be forwarded to the remote process

set -Eeuo pipefail

REMOTE_HOST=$1
REMOTE_CMD=$2

if [ "$REMOTE_HOST" == "" ]; then
  echo "Error: usage $0 <remote_host> <remote_cmd>"
  exit 1
fi

if [ "$REMOTE_CMD" == "" ]; then
  echo "Error: usage $0 <remote_host> <remote_cmd>"
  exit 1
fi

cleanup() {
    echo "Send TERM signal to host: $REMOTE_HOST to cmd: $REMOTE_CMD"
    ssh -F ~/.ssh/config $REMOTE_HOST "pkill -TERM -f 'bash $REMOTE_CMD'"
    kill -TERM $SSH_PID 2>/dev/null
}

trap cleanup TERM INT

echo "Run cmd: $REMOTE_CMD on remote host: $REMOTE_HOST"
ssh -F ~/.ssh/config $REMOTE_HOST "$REMOTE_CMD" &
SSH_PID=$!

wait $SSH_PID
