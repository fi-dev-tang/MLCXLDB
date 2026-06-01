#!/usr/bin/env bash
set -euo pipefail

# Small-scale smoke/debug runner for YCSB-B/C on two-level mode only.
# Defaults are chosen to be quick while trying to reduce CPU-cache-only artifacts.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/frontend}"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/optimization_step5/small_bc_two_level}"

# -------- Workload scope --------
WORKLOADS=(b c)
THETAS=(${THETAS:-0.90})

# -------- Scale knobs (override via env) --------
WORKING_SET_GIB="${WORKING_SET_GIB:-2.0}"
PAYLOAD_SIZE_BYTES="${PAYLOAD_SIZE_BYTES:-100}"
WARMUP_LOOKUPS="${WARMUP_LOOKUPS:-2000000}"
MEASURE_LOOKUPS="${MEASURE_LOOKUPS:-5000000}"
WARMUP_PROGRESS_INTERVAL="${WARMUP_PROGRESS_INTERVAL:-50000}"
PROGRESS_INTERVAL="${PROGRESS_INTERVAL:-100000}"

# -------- Two-level memory config --------
DRAM_BP_TWO_LEVEL="${DRAM_BP_TWO_LEVEL:-0.25}"
DRAM_RC_TWO_LEVEL="${DRAM_RC_TWO_LEVEL:-0.5}"
CXL_GIB="${CXL_GIB:-3.0}"

# -------- Threading --------
WORKER_THREADS="${WORKER_THREADS:-4}"
PP_THREADS="${PP_THREADS:-1}"
CXL_PP_THREADS="${CXL_PP_THREADS:-1}"
TWO_LEVEL_ADMISSION_THREADS="${TWO_LEVEL_ADMISSION_THREADS:-1}"
FORWARD_EPOCH_THREAD="${FORWARD_EPOCH_THREAD:-1}"
SIEVE_EVICTION_THREAD="${SIEVE_EVICTION_THREAD:-1}"
RECORD_CACHE_PROMOTE_THREAD="${RECORD_CACHE_PROMOTE_THREAD:-1}"

# -------- Paths --------
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd}"
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.2}"

mkdir -p "$RESULT_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

log() {
  echo "[$(date '+%F %T')] $*"
}

get_binary() {
  local wl="$1"
  echo "$BUILD_DIR/experiment_1_ycsb_${wl}"
}

get_theta_admission_tuning_flags() {
  local theta="$1"
  case "$theta" in
    0.80)
      cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.35
--max_per_page_visits=7000
--max_global_requests_window=2000000
--trigger_visit_histogram_update_size=1200000
EOF
      ;;
    0.85)
      cat <<'EOF'
--skew_threshold_ratio=0.09
--uniform_threshold_ratio=0.40
--max_per_page_visits=6000
--max_global_requests_window=1500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.90)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=5000
--max_global_requests_window=1000000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.95)
      cat <<'EOF'
--skew_threshold_ratio=0.12
--uniform_threshold_ratio=0.60
--max_per_page_visits=4000
--max_global_requests_window=700000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.99)
      cat <<'EOF'
--skew_threshold_ratio=0.15
--uniform_threshold_ratio=0.70
--max_per_page_visits=3000
--max_global_requests_window=500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    *)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=3000
--max_global_requests_window=500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
  esac
}

# Preflight
for wl in "${WORKLOADS[@]}"; do
  b="$(get_binary "$wl")"
  if [[ ! -x "$b" ]]; then
    echo "[ERROR] Missing binary: $b"
    echo "[HINT] cmake --build build -- experiment_1_ycsb_b experiment_1_ycsb_c"
    exit 1
  fi
done

log "Start small BC two-level run."
log "workloads=${WORKLOADS[*]} thetas=${THETAS[*]} ws=${WORKING_SET_GIB}GiB warmup=${WARMUP_LOOKUPS} measure=${MEASURE_LOOKUPS}"

for theta in "${THETAS[@]}"; do
  for wl in "${WORKLOADS[@]}"; do
    bin="$(get_binary "$wl")"
    result_file="$RESULT_DIR/result_ycsb${wl}_two_level_theta${theta}_small_${TIMESTAMP}.csv"

    theta_tuning_flags=()
    while IFS= read -r line; do
      [[ -n "$line" ]] && theta_tuning_flags+=("$line")
    done < <(get_theta_admission_tuning_flags "$theta")

    log "RUN wl=$wl theta=$theta -> $result_file"

    exit_code=0
    {
      "$bin" \
        --test_admission_mode=two_level \
        --test_zipf_theta="$theta" \
        --cxl_tiering_enabled=true \
        --cxl_gib="$CXL_GIB" \
        --cxl_dax_device_path="$CXL_DAX_DEVICE" \
        --pp_threads="$PP_THREADS" \
        --cxl_pp_threads="$CXL_PP_THREADS" \
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
        --delay_admission_recordcache_threads_start=true \
        --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
        --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
        --test_working_set_gib="$WORKING_SET_GIB" \
        --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
        --worker_threads="$WORKER_THREADS" \
        --vi=true \
        --test_warmup_lookups="$WARMUP_LOOKUPS" \
        --test_measure_lookups="$MEASURE_LOOKUPS" \
        --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
        --test_progress_interval="$PROGRESS_INTERVAL" \
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        "${theta_tuning_flags[@]}" \
        2>&1
    } | tee "$result_file" || exit_code=$?

    if [[ "$exit_code" -ne 0 ]]; then
      log "WARN: wl=$wl theta=$theta exit_code=$exit_code (teardown non-zero may happen)."
    fi

    log "Cooldown 10s..."
    sleep 10
  done
done

log "Done. Results in: $RESULT_DIR"
