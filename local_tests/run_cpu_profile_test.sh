#!/usr/bin/env bash

set -euo pipefail

if [ "$1" == "" ]; then
  echo "error: usage $0 <workdir>"
  exit 1
fi

if [ ! -d "$1" ]; then
  echo "error: directory $1 didn't exists"
  exit 1
fi

# if [ "$2" == "" ]; then
#   echo "error: usage $0 <workdir> <CHV nix flake url>"
#   exit 1
# fi

cd $1
# echo ">>> Update CHV flake input url to: $2"
# nix flake lock --override-input cloud-hypervisor "$2"
# git status

########################
# build test suite
########################
echo ">>> Build test suite"
nix build .\#tests.x86_64-linux.cpu_profiles.driver

########################
# run test suite
########################
./local_tests/get_child_pids.sh & # run in background
echo ">>> Run test suite"
nix run .\#tests.x86_64-linux.cpu_profiles.driver
