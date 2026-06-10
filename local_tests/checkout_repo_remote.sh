#!/usr/bin/env bash

# this script should run on a gitlab-runner itself
# we have to decide what is the source repo for a pipeline run (the repo itself or a fork)

SRC_PATH=""
REMOTE_HOST=$1
REMOTE_DIR="/home/benchmark/tmp-${CI_JOB_ID}"

if [ -n "${CI_MERGE_REQUEST_SOURCE_PROJECT_PATH}" ]; then
  SRC_PATH=${CI_MERGE_REQUEST_SOURCE_PROJECT_PATH}
else
  SRC_PATH=${CI_PROJECT_PATH}
fi

if [ -z "${REMOTE_HOST}" ]; then
  echo "Error: Usage $0 <remote host>"
  exit 1
fi

if [ -z "${CI_JOB_ID}" ]; then
  echo "Error: environment variable CI_JOB_ID is not set"
  exit 1
fi

echo "checkout repo: ${SRC_PATH} on remote host: ${REMOTE_HOST} into directory: ${REMOTE_DIR}"
ssh -F ~/.ssh/config ${REMOTE_HOST} rm -rf ${REMOTE_DIR}
ssh -F ~/.ssh/config ${REMOTE_HOST} mkdir ${REMOTE_DIR}
ssh -F ~/.ssh/config ${REMOTE_HOST} git -C ${REMOTE_DIR} clone ${CI_SERVER_PROTOCOL}://gitlab-ci-token:${CI_JOB_TOKEN}@${CI_SERVER_HOST}/${SRC_PATH}.git
echo "checkout branch: ${CI_COMMIT_REF_NAME}"
ssh -F ~/.ssh/config ${REMOTE_HOST} git -C ${REMOTE_DIR}/${CI_PROJECT_NAME} checkout ${CI_COMMIT_REF_NAME}
