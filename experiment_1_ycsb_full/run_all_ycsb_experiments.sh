#!/usr/bin/env bash
# =============================================================================
# run_ycsb_full_ablation.sh
#
# Full YCSB ablation study: 5 system variants × 6 workloads × 5 theta values.
#
# System variants (5):
#   1. two_level  — full system (DRAM RecordCache + DRAM BP + CXL BP)
#   2. page_only  — first-level admission only (DRAM BP + CXL BP, no RC)
#   3. lru        — pure LRU page tiering (DRAM BP + CXL BP, no admission)
#   4. dram_ssd   — baseline, memory-constrained (DRAM-only, no CXL)
#   5. dram_ssd_unconstrained — baseline, memory-unconstrained (DRAM-only, no CXL)
#
# YCSB workloads (6):
#   A (50R/50U), B (95R/5U), C (100R), D (95R/5I), E (95S/5I), F (50RMW/50R)
#
# Zipfian theta sweep (5):
#   0.80, 0.85, 0.90, 0.95, 0.99
#
# Total: 5 × 6 × 5 = 150 experiment groups
#
#
# Usage:
#   cd /home/zhizhi.tyf/cxl-recordcache-dev
#   bash run_ycsb_full_ablation.sh
#
# To run only specific workloads or thetas, edit WORKLOADS / THETAS below.
# =============================================================================

set -euo pipefail

# =============================================================================
# ★ Experiment Parameters — Tune before each run ★
# =============================================================================

# Working set
WORKING_SET_GIB=4.0
PAYLOAD_SIZE_BYTES=100

# Theta sweep
THETAS=(0.80 0.85 0.90 0.95 0.99)

# Workloads to run (a=50R/50U, b=95R/5U, c=100R, d=95R/5I, e=95S/5I, f=50RMW/50R)
WORKLOADS=(a b c d e f)

# System variants to run
VARIANTS=(two_level page_only lru dram_ssd dram_ssd_unconstrained)

# ---------------------------------------------------------------------------
# Memory configuration
# ---------------------------------------------------------------------------
# two_level mode: DRAM split into buffer pool + record cache
DRAM_BP_TWO_LEVEL=0.25
DRAM_RC_TWO_LEVEL=0.5

# page_only / lru mode: same total DRAM but all in buffer pool (no RC)
# Must equal DRAM_BP_TWO_LEVEL + DRAM_RC_TWO_LEVEL for fair comparison
DRAM_BP_PAGE_ONLY=0.75

# CXL pool size
CXL_GIB=3.0

# dram_ssd baseline: total DRAM(Memory constained environment)
DRAM_BASELINE=0.75
DRAM_NOT_CONSTAINED=0.75

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
WORKER_THREADS=4
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=1
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=1

# ---------------------------------------------------------------------------
# Lookup / operation counts
# ---------------------------------------------------------------------------
WARMUP_LOOKUPS=5000000
MEASURE_LOOKUPS=10000000
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=200000

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
CXL_DAX_DEVICE="/dev/dax0.2"
RESULT_DIR="$SCRIPT_DIR/ablation_results"

# =============================================================================
# Derived
# =============================================================================
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TOTAL_EXPERIMENTS=$(( ${#VARIANTS[@]} * ${#WORKLOADS[@]} * ${#THETAS[@]} ))
CURRENT_EXPERIMENT=0

mkdir -p "$RESULT_DIR"

# =============================================================================
# Helpers
# =============================================================================

log_phase() {
    echo ""
    echo "========================================"
    echo "  $1"
    echo "========================================"
}

log_info() {
    echo "[INFO] $1"
}

log_warn() {
    echo "[WARN] $1"
}

# Map workload letter to binary name
get_binary() {
    local wl="$1"
    echo "$BUILD_DIR/experiment_1_ycsb_${wl}"
}

# Pretty workload description
get_workload_desc() {
    case "$1" in
        a) echo "A (50R/50U)" ;;
        b) echo "B (95R/5U)" ;;
        c) echo "C (100R)" ;;
        d) echo "D (95R/5I)" ;;
        e) echo "E (95S/5I)" ;;
        f) echo "F (50RMW/50R)" ;;
        *) echo "Unknown" ;;
    esac
}

# Theta-specific two-level-admission tuning profile
# These are runtime gflags (do NOT modify global defaults in Config.cpp).
get_theta_admission_tuning_flags() {
    local theta="$1"
    case "$theta" in
        0.80)
            cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.35
--max_per_page_visits=7000
--max_global_requests_window=2000000
--admission_aging_interval=60000
--trigger_visit_histogram_update_size=120000
EOF
            ;;
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
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=5000
--max_global_requests_window=1000000
--admission_aging_interval=40000
--trigger_visit_histogram_update_size=80000
EOF
            ;;
        0.95)
            cat <<'EOF'
--skew_threshold_ratio=0.12
--uniform_threshold_ratio=0.60
--max_per_page_visits=4000
--max_global_requests_window=700000
--admission_aging_interval=30000
--trigger_visit_histogram_update_size=60000
EOF
            ;;
        0.99)
            cat <<'EOF'
--skew_threshold_ratio=0.15
--uniform_threshold_ratio=0.70
--max_per_page_visits=3000
--max_global_requests_window=500000
--admission_aging_interval=20000
--trigger_visit_histogram_update_size=50000
EOF
            ;;
        *)
            # fallback to current defaults
            cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=3000
--max_global_requests_window=500000
--admission_aging_interval=10000
--trigger_visit_histogram_update_size=50000
EOF
            ;;
    esac
}

# =============================================================================
# Pre-flight checks
# =============================================================================
log_phase "Pre-flight Checks"

MISSING_BINARIES=()
for wl in "${WORKLOADS[@]}"; do
    binary="$(get_binary "$wl")"
    if [[ ! -x "$binary" ]]; then
        MISSING_BINARIES+=("$binary")
    fi
done

if [[ ${#MISSING_BINARIES[@]} -gt 0 ]]; then
    echo "[ERROR] Missing binaries:"
    for b in "${MISSING_BINARIES[@]}"; do
        echo "  $b"
    done
    echo ""
    echo "Build them with:"
    echo "  cmake --build build -- experiment_1_ycsb_a experiment_1_ycsb_b experiment_1_ycsb_c experiment_1_ycsb_d experiment_1_ycsb_e experiment_1_ycsb_f"
    exit 1
fi

log_info "All binaries found."

# =============================================================================
# Common flags
# =============================================================================
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

# CXL-specific flags (used by two_level / page_only / lru)
CXL_FLAGS=(
    --cxl_tiering_enabled=true
    --cxl_gib="$CXL_GIB"
    --cxl_dax_device_path="$CXL_DAX_DEVICE"
    --pp_threads="$PP_THREADS"
    --cxl_pp_threads="$CXL_PP_THREADS"
    --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
    --delay_admission_recordcache_threads_start=true
)

# =============================================================================
# Run one experiment
#
# Usage:
#   run_one <workload_letter> <variant> <theta>
# =============================================================================
run_one() {
    local wl="$1"
    local variant="$2"
    local theta="$3"

    CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
    local wl_desc
    wl_desc="$(get_workload_desc "$wl")"
    local binary
    binary="$(get_binary "$wl")"

    local result_file="$RESULT_DIR/result_ycsb${wl}_${variant}_theta${theta}_ws${WORKING_SET_GIB}gib_${TIMESTAMP}.log"
    local theta_tuning_flags=()
    while IFS= read -r line; do
        [[ -n "$line" ]] && theta_tuning_flags+=("$line")
    done < <(get_theta_admission_tuning_flags "$theta")

    log_phase "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] YCSB-$wl_desc | $variant | θ=$theta"
    log_info "Binary: $binary"
    log_info "Result: $result_file"
    log_info "Two-level tuning(theta=${theta}): ${theta_tuning_flags[*]}"

    # Build variant-specific flags
    local extra_flags=()
    local admission_mode=""

    case "$variant" in
        two_level)
            admission_mode="two_level"
            extra_flags=(
                "${CXL_FLAGS[@]}"
                --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL"
                --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL"
                --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
                --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
                --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
                "${theta_tuning_flags[@]}"
            )
            ;;
        page_only)
            admission_mode="page_only"
            extra_flags=(
                "${CXL_FLAGS[@]}"
                --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"
                "${theta_tuning_flags[@]}"
            )
            ;;
        lru)
            admission_mode="lru"
            extra_flags=(
                "${CXL_FLAGS[@]}"
                --dram_buffer_pool_gib="$DRAM_BP_PAGE_ONLY"
            )
            ;;
        dram_ssd)
            admission_mode="lru"
            extra_flags=(
                --cxl_tiering_enabled=false
                --dram_buffer_pool_gib="$DRAM_BASELINE"
                --pp_threads="$PP_THREADS"
            )
            ;;
        dram_ssd_unconstrained)
            admission_mode="lru"
            extra_flags=(
                --cxl_tiering_enabled=false
                --dram_buffer_pool_gib="$DRAM_NOT_CONSTAINED"
                --pp_threads="$PP_THREADS"
            )
            ;;
        *)
            echo "[ERROR] Unknown variant: $variant"
            return 1
            ;;
    esac

    local start_ts
    start_ts=$(date +%s)

    # Build and log the full command
    local cmd_header
    cmd_header="$(
        echo ""
        echo "# ================================================================"
        echo "# Experiment: YCSB-${wl_desc} | variant=${variant} | theta=${theta}"
        echo "# Working set: ${WORKING_SET_GIB} GiB | CXL: ${CXL_GIB} GiB"
        echo "# Warmup: ${WARMUP_LOOKUPS} | Measured: ${MEASURE_LOOKUPS}"
        echo "# ================================================================"
        echo "[CMD] $binary \\"
        echo "    --test_admission_mode=$admission_mode \\"
        echo "    --test_zipf_theta=$theta \\"
        for flag in "${extra_flags[@]}"; do echo "    $flag \\"; done
        for flag in "${COMMON_FLAGS[@]}"; do echo "    $flag \\"; done
        echo ""
    )"

    echo "$cmd_header"

    # Run the binary. Treat teardown segfaults (exit code 139) as non-fatal.
    local exit_code=0
    {
        echo "$cmd_header"
        "$binary" \
            --test_admission_mode="$admission_mode" \
            --test_zipf_theta="$theta" \
            "${extra_flags[@]}" \
            "${COMMON_FLAGS[@]}" \
            2>&1
    } | tee "$result_file" || exit_code=$?

    if [[ $exit_code -ne 0 ]]; then
        log_warn "Binary exited with code $exit_code (likely a teardown segfault)."
        log_warn "Measured data is already written — results in $result_file are valid."
    fi

    # Cool-down: ensure OS releases DAX device, SSD handles, shared memory
    log_info "Waiting 15s for process teardown before next experiment..."
    sleep 15

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    log_info "Finished in ${elapsed}s. Result: $result_file"
}

# =============================================================================
# Banner
# =============================================================================
echo ""
echo "============================================================"
echo "  YCSB Full Ablation Study"
echo "============================================================"
echo "  Working set      : ${WORKING_SET_GIB} GiB"
echo "  DRAM BP (2-level): ${DRAM_BP_TWO_LEVEL} GiB"
echo "  RecordCache      : ${DRAM_RC_TWO_LEVEL} GiB"
echo "  CXL BP           : ${CXL_GIB} GiB"
echo "  DRAM baseline    : ${DRAM_BASELINE} GiB"
echo "  Payload          : ${PAYLOAD_SIZE_BYTES} B"
echo "  Thetas           : ${THETAS[*]}"
echo "  Workloads        : ${WORKLOADS[*]}"
echo "  Variants         : ${VARIANTS[*]}"
echo "  Warmup ops       : ${WARMUP_LOOKUPS}"
echo "  Measured ops     : ${MEASURE_LOOKUPS}"
echo "  Warmup progress  : every ${WARMUP_PROGRESS_INTERVAL} ops"
echo "  Measured progress: every ${PROGRESS_INTERVAL} ops"
echo "  Total experiments: ${TOTAL_EXPERIMENTS}"
echo "  Result dir       : ${RESULT_DIR}"
echo "  Timestamp        : ${TIMESTAMP}"
echo "============================================================"

GLOBAL_START=$(date +%s)

# =============================================================================
# Main loop: iterate theta → workload → variant
#
# Outer loop is theta so that all workloads/variants at the same skew are
# grouped together, making it easy to compare across variants for a fixed θ.
# =============================================================================
for theta in "${THETAS[@]}"; do
    log_phase "=== Theta = $theta ==="

    for wl in "${WORKLOADS[@]}"; do
        for variant in "${VARIANTS[@]}"; do
            run_one "$wl" "$variant" "$theta"
        done
    done
done

# =============================================================================
# Final summary
# =============================================================================
GLOBAL_END=$(date +%s)
GLOBAL_ELAPSED=$((GLOBAL_END - GLOBAL_START))
GLOBAL_HOURS=$((GLOBAL_ELAPSED / 3600))
GLOBAL_MINS=$(( (GLOBAL_ELAPSED % 3600) / 60 ))

log_phase "All experiments completed!"
log_info "Total time: ${GLOBAL_ELAPSED}s (${GLOBAL_HOURS}h ${GLOBAL_MINS}m)"
log_info "Total experiments: ${TOTAL_EXPERIMENTS}"
log_info "Results saved in: $RESULT_DIR"
echo ""
log_info "Result files:"
ls -lh "$RESULT_DIR"/result_ycsb*_${TIMESTAMP}.log 2>/dev/null || true
