#!/bin/bash
# Benchmark: alag-alag concurrency par scan speed
HOST=${1:-127.0.0.1}
echo "Benchmark on $HOST (ports 1-10000)" | tee benchmark_result.txt
for c in 10 50 100 500 900; do
  echo -n "concurrency=$c  ->  " | tee -a benchmark_result.txt
  ./scanner $HOST -p 1-10000 -c $c -t 300 | tail -1 | tee -a benchmark_result.txt
done
echo "Result saved in benchmark_result.txt"
