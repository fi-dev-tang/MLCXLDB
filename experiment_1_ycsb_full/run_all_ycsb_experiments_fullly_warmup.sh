#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Full warmup ablation:
#   Workloads: YCSB-A/B/C/D/E/F
#   Variants:
#     1) two_level                (Full-System)
#     2) page_only                (only first-level RecordCache admission)
#     3) lru
#     4) dram_ssd                 (memory constrained)
#     5) dram_ssd_unconstrained   (memory unconstrained)
#
# Config requested:
#   working_set_gib = 4.0
#   cxl_buffer_pool = 2.0 GiB (for CXL-enabled variants)
#   two_level:
#     - A/B/C/F: dram_bp=0.125, dram_rc=0.5
#     - D/E    : dram_bp=0.5,   dram_rc=0.125
#   page_only / lru:
#     - dram_bp=0.625
#   dram_ssd (constrained):
#     - dram_bp=0.625, no cxl
#   dram_ssd_unconstrained:
#     - dram_bp=2.625, no cxl
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
CXL_DAX_DEVICE="/dev/dax0.2"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/result_${TIMESTAMP}_fullly_warmup_ablation"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Workload / theta / variant matrix
# -----------------------------------------------------------------------------
WORKLOADS=(a b c d e f)
THETAS=(0.85 0.90 0.95 0.99)
VARIANTS=(two_level page_only lru dram_ssd dram_ssd_unconstrained)

# -----------------------------------------------------------------------------
# Global run config
# -----------------------------------------------------------------------------
WORKING_SET_GIB=4.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0

WORKER_THREADS=4
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

WARMUP_LOOKUPS=300000000
MEASURE_LOOKUPS=100000000
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=2000000

COOLDOWN_SECONDS=30

# -----------------------------------------------------------------------------
# Variant memory config
# -----------------------------------------------------------------------------
DRAM_BP_TWO_LEVEL_LIGHT=0.125    # for A/B/C/F
DRAM_RC_TWO_LEVEL_LIGHT=0.5
DRAM_BP_TWO_LEVEL_HEAVY=0.5      # for D/E
DRAM_RC_TWO_LEVEL_HEAVY=0.125

DRAM_BP_PAGE_ONLY=0.625
DRAM_BP_LRU=0.625
DRAM_BP_DRAM_SSD=0.625
DRAM_BP_DRAM_SSD_UNCONSTRAINED=2.625

TOTAL_EXPERIMENTS=$(( ${#WORKLOADS[@]} * ${#THETAS[@]} * ${#VARIANTS[@]} ))
CURRENT_EXPERIMENT=0

log_phase() {
  echo ""
  echo "============================================================"
  echo "$1"
  echo "============================================================"
}

get_theta_tuning_flags() {
  local theta="$1"
  case "$theta" in
    0.85)
      cat <<'EOF'
--skew_threshold_ratio=0.09
--uniform_threshold_ratio=0.40
--max_per_page_visits=6000
--max_global_requests_window=1500000
--admission_aging_interval=50000
--trigger_visit_histogram_update_size=100000
EOF
      ;;
    0.90)
      cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.45
--max_per_page_visits=8000
--max_global_requests_window=2000000
--admission_aging_interval=80000
--trigger_visit_histogram_update_size=160000
EOF
      ;;
    0.95)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.55
--max_per_page_visits=6000
--max_global_requests_window=1500000
--admission_aging_interval=60000
--trigger_visit_histogram_update_size=120000
EOF
      ;;
    0.99)
      cat <<'EOF'
--skew_threshold_ratio=0.12
--uniform_threshold_ratio=0.65
--max_per_page_visits=4000
--max_global_requests_window=1000000
--admission_aging_interval=40000
--trigger_visit_histogram_update_size=80000
EOF
      ;;
    *)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=5000
--max_global_requests_window=1000000
--admission_aging_interval=40000
--trigger_visit_histogram_update_size=80000
EOF
      ;;
  esac
}

get_two_level_dram_bp() {
  local wl="$1"
  case "$wl" in
    d|e) echo "$DRAM_BP_TWO_LEVEL_HEAVY" ;;
    *)   echo "$DRAM_BP_TWO_LEVEL_LIGHT" ;;
  esac
}

get_two_level_dram_rc() {
  local wl="$1"
  case "$wl" in
    d|e) echo "$DRAM_RC_TWO_LEVEL_HEAVY" ;;
    *)   echo "$DRAM_RC_TWO_LEVEL_LIGHT" ;;
  esac
}

COMMON_FLAGS=(
  --test_working_set_gib="$WORKING_SET_GIB"
  --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
  --worker_threads="$WORKER_THREADS"
  --vi=true
  --test_warmup_lookups="$WARMUP_LOOKUPS"
  --test_measure_lookups="$MEASURE_LOOKUPS"
  --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL"
  --test_progress_interval="$PROGRESS_INTERVAL"
  --ssd_path="$SSD_PATH"
  --trunc=true
  --wal=true
)

run_one() {
  local wl="$1"
  local theta="$2"
  local variant="$3"

  CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
  local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
  local result_file="$RESULT_DIR/result_ycsb${wl}_${variant}_theta${theta}_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.log"

  if [[ ! -x "$binary" ]]; then
    echo "[ERROR] binary not found: $binary"
    return 1
  fi

  local theta_flags=()
  while IFS= read -r line; do
    [[ -n "$line" ]] && theta_flags+=("$line")
  done < <(get_theta_tuning_flags "$theta")

  local admission_mode=""
  local extra_flags=()

  case "$variant" in
    two_level)
      admission_mode="two_level"
      local dram_bp
      local dram_rc
      dram_bp="$(get_two_level_dram_bp "$wl")"
      dram_rc="$(get_two_level_dram_rc "$wl")"

      extra_flags=(
        --cxl_tiering_enabled=true
        --cxl_gib="$CXL_GIB"
        --cxl_dax_device_path="$CXL_DAX_DEVICE"
        --pp_threads="$PP_THREADS"
        --cxl_pp_threads="$CXL_PP_THREADS"
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
        --delay_admission_recordcache_threads_start=true
        --dram_buffer_pool_gib="$dram_bp"
        --dram_recordcache_gib="$dram_rc"
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
        "${theta_flags[@]}"
      )
      ;;
    page_only)
      admission_mode="page_only"
      extra_flags=(
        --cxl_tiering_enabled=true
        --cxl_gib="$CXL_GIB"
        --cxl_dax_device_path="$CXL_DAX_DEVICE"
        --pp_threads="$PP_THREADS"
        --cxl_pp_threads="$CXL_PP_THREADS"
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
        --delay_admission_recordcache_threads_start=true
        --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"
        "${theta_flags[@]}"
      )
      ;;
    lru)
      admission_mode="lru"
      extra_flags=(
        --cxl_tiering_enabled=true
        --cxl_gib="$CXL_GIB"
        --cxl_dax_device_path="$CXL_DAX_DEVICE"
        --pp_threads="$PP_THREADS"
        --cxl_pp_threads="$CXL_PP_THREADS"
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
        --delay_admission_recordcache_threads_start=true
        --dram_buffer_pool_gib="$DRAM_BP_LRU"
      )
      ;;
    dram_ssd)
      admission_mode="lru"
      extra_flags=(
        --cxl_tiering_enabled=false
        --dram_buffer_pool_gib="$DRAM_BP_DRAM_SSD"
        --pp_threads="$PP_THREADS"
      )
      ;;
    dram_ssd_unconstrained)
      admission_mode="lru"
      extra_flags=(
        --cxl_tiering_enabled=false
        --dram_buffer_pool_gib="$DRAM_BP_DRAM_SSD_UNCONSTRAINED"
        --pp_threads="$PP_THREADS"
      )
      ;;
    *)
      echo "[ERROR] unknown variant: $variant"
      return 1
      ;;
  esac

  echo ""
  echo "[RUN][$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] wl=$wl variant=$variant theta=$theta"
  echo "[RUN] result -> $result_file"
  local exit_code=0

  "$binary" \
    --test_admission_mode="$admission_mode" \
    --test_zipf_theta="$theta" \
    "${extra_flags[@]}" \
    "${COMMON_FLAGS[@]}" \
    2>&1 | tee "$result_file" || exit_code=$?

  if [[ "$exit_code" -ne 0 ]]; then
    echo "[WARN] wl=$wl variant=$variant theta=$theta exit code $exit_code (continuing)"
  fi

  echo "[INFO] cooldown ${COOLDOWN_SECONDS}s"
  sleep "$COOLDOWN_SECONDS"
}

log_phase "YCSB full warmup ablation start"
echo "[INFO] timestamp=${TIMESTAMP}"
echo "[INFO] result_dir=${RESULT_DIR}"
echo "[INFO] workloads=${WORKLOADS[*]}"
echo "[INFO] thetas=${THETAS[*]}"
echo "[INFO] variants=${VARIANTS[*]}"
echo "[INFO] total_experiments=${TOTAL_EXPERIMENTS}"

for theta in "${THETAS[@]}"; do
  log_phase "Theta=${theta}"
  for wl in "${WORKLOADS[@]}"; do
    for variant in "${VARIANTS[@]}"; do
      run_one "$wl" "$theta" "$variant"
    done
  done
done

log_phase "All experiments completed"
echo "[DONE] all runs complete in ${RESULT_DIR}"
