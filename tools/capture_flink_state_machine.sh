#!/usr/bin/env bash
set -euo pipefail

workspace=/mnt/c/Desktop/BYTESE~1
flink=/root/bigdata/flink
trace=${1:-$workspace/datasets/spark-flink/traces/flink-state-machine.trace}
blocks=${2:-200000}
log=${trace%.trace}.workload.log
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64

cleanup() {
  "$flink/bin/stop-cluster.sh" >/dev/null 2>&1 || true
}
trap cleanup EXIT
"$flink/bin/start-cluster.sh" >"$log" 2>&1
sleep 4
task_pid=$(jps -l | awk '/TaskManagerRunner/ {print $1; exit}')
if [[ -z "$task_pid" ]]; then
  cat "$log"
  exit 1
fi

cd "$workspace"
timeout 240 ./pin-external-4.3-99850-gce5652921-gcc-linux/pin \
  -pid "$task_pid" \
  -t ./tools/pin/obj-intel64/cacheline_trace.so \
  -o "$trace" -cache_mb 16 -ways 16 -warmup_misses 100000 \
  -max_blocks "$blocks" >"${trace%.trace}.pin.log" 2>&1 &
pin_pid=$!
sleep 2

"$flink/bin/flink" run "$flink/examples/streaming/StateMachineExample.jar" \
  >>"$log" 2>&1 || true
wait "$pin_pid" || true

if [[ ! -s "$trace" ]]; then
  cat "$log"
  cat "${trace%.trace}.pin.log"
  exit 1
fi
echo "trace=$trace bytes=$(stat -c %s "$trace") taskmanager_pid=$task_pid"
