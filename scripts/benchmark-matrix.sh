#!/usr/bin/env bash
# Same-source farm relay and reconnect comparison; never a 100k-capacity claim.
set -euo pipefail
cd "$(dirname "$0")/.."

RESULT_ROOT="${BENCH_MATRIX_RESULT_DIR:-benchmark-results/matrix-$(date -u +%Y%m%dT%H%M%SZ)}"
read -ra WORKERS <<<"${BENCH_MATRIX_WORKERS:-1 2 4 8}"
read -ra TRANSPORTS <<<"${BENCH_MATRIX_TLS:-0 1}"
read -ra SCENARIOS <<<"${BENCH_MATRIX_SCENARIOS:-relay reconnect}"
export BENCH_CONNECTIONS="${BENCH_CONNECTIONS:-1000}"
export BENCH_DURATION_SECONDS="${BENCH_DURATION_SECONDS:-15}"
export BENCH_RAMP_PER_SECOND="${BENCH_RAMP_PER_SECOND:-100}"

mkdir -p "$RESULT_ROOT"
status=0
for workers in "${WORKERS[@]}"; do
  for tls in "${TRANSPORTS[@]}"; do
    for scenario in "${SCENARIOS[@]}"; do
      case_status=0
      rate="${BENCH_RATE_PER_SECOND:-1000}"
      if [[ "$scenario" == "reconnect" ]]; then
        rate="${BENCH_RECONNECT_RATE_PER_SECOND:-100}"
      fi
      BENCH_IO_WORKERS="$workers" BENCH_TLS="$tls" BENCH_SCENARIO="$scenario" \
        BENCH_RATE_PER_SECOND="$rate" \
        BENCH_RESULT_DIR="$RESULT_ROOT/workers-$workers-tls-$tls-$scenario" \
        bash scripts/benchmark.sh || case_status=$?
      if [[ "$case_status" -ne 0 ]]; then
        if [[ "$case_status" -ne 3 ]]; then
          status=1
        elif [[ "$status" -eq 0 ]]; then
          status=3
        fi
      fi
    done
  done
done
echo "==> Worker matrix exit=$status; artifacts: $RESULT_ROOT"
exit "$status"
