#!/usr/bin/env bash
set -euo pipefail

workspace=/mnt/c/Desktop/BYTESE~1
spark=/root/bigdata/spark
java_home=/usr/lib/jvm/java-17-openjdk-amd64
trace=${1:-$workspace/datasets/spark-flink/traces/spark-kmeans.trace}
points=${2:-5000000}
blocks=${3:-200000}
log=${trace%.trace}.workload.log

export JAVA_HOME=$java_home
"$spark/bin/spark-submit" \
  --master 'local[4]' \
  --conf spark.ui.enabled=false \
  --conf spark.driver.memory=4g \
  --class org.apache.spark.examples.SparkKMeans \
  "$spark/examples/jars/spark-examples_2.13-4.2.0.jar" \
  8 "$points" 10 4 >"$log" 2>&1 &
workload_pid=$!

cleanup() {
  kill "$workload_pid" 2>/dev/null || true
  wait "$workload_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 120); do
  if grep -q 'Running Spark version' "$log" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$workload_pid" 2>/dev/null; then
    cat "$log"
    exit 1
  fi
  sleep 1
done

sleep 3
cd "$workspace"
timeout 240 ./pin-external-4.3-99850-gce5652921-gcc-linux/pin \
  -pid "$workload_pid" \
  -t ./tools/pin/obj-intel64/cacheline_trace.so \
  -o "$trace" -cache_mb 16 -ways 16 -warmup_misses 100000 \
  -max_blocks "$blocks" || status=$?

if [[ ! -s "$trace" ]]; then
  cat "$log"
  exit "${status:-1}"
fi

echo "trace=$trace bytes=$(stat -c %s "$trace") workload_pid=$workload_pid"
