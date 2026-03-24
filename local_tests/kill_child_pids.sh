#!/usr/bin/env bash

BDIR=$(dirname $0)

PIDS=$(cat ${BDIR}/../../child_pids.txt)
for P in ${PIDS}; do
  kill ${P} || true
done
