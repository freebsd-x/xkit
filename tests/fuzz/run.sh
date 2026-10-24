#!/bin/sh
# Build and run the XBus fuzz harnesses with clang/libFuzzer + ASan + UBSan.
# Usage:  sh sdk/xkit/tests/fuzz/run.sh [max_total_time_seconds]
# From the repo root. Default runs 60s; CI can pass a short budget.
set -e

ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)   # repo root
OUT="${ROOT}/out/fuzz"
BUDGET="${1:-60}"
mkdir -p "${OUT}" "${OUT}/corpus_xbus"

CXX="${CXX:-clang++}"
FLAGS="-std=c++23 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
INC="-I${ROOT}/core/xserver/include -I${ROOT}/include -I${ROOT}/sdk/xkit/include"

echo "=== building xbus_handleincoming_fuzz ==="
${CXX} ${FLAGS} ${INC} \
  "${ROOT}/sdk/xkit/tests/fuzz/xbus_handleincoming_fuzz.cc" \
  "${ROOT}/sdk/xkit/src/xbus_client.cc" \
  "${ROOT}/core/xserver/src/ipc_socket.cc" \
  "${ROOT}/core/xserver/src/xbus_message_pool.cc" \
  -lpthread -o "${OUT}/xbus_handleincoming_fuzz"

echo "=== fuzzing HandleIncoming for ${BUDGET}s ==="
"${OUT}/xbus_handleincoming_fuzz" \
  -max_total_time="${BUDGET}" -print_final_stats=1 \
  "${OUT}/corpus_xbus"
