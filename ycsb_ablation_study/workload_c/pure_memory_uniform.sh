#!/usr/bin/env bash
# =============================================================================
# pure_memory_uniform.sh
#
# Modes:
#   1. dram_ssd  — pure DRAM + SSD (系统默认内存 node 0)
#   2. pure_cxl  — pure CXL  + SSD (numactl --membind=1, 相同内存总量)
#
# Sweeps:
#   working_set_gib : 4, 8, 16, 32
#   distribution    : uniform (全内存随机访问，不测倾斜)
#
# Results are written to: pure_memory_uniform/
# =============================================================================

set -euo pipefail

# =============================================================================
# ★ Tune these before each experiment run ★
# =============================================================================

# --- Sweep dimensions --------------------------------------------------------
WORKING_SET_GIB_LIST=(4 8 16 32 64 128)

# --- Fixed parameters --------------------------------------------------------
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0

WORKER_THREADS=4
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=1
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=1

# 全内存测试，数据在 Bulk Load 阶段已经全部在内存中，不需要漫长的预热
WARMUP_LOOKUPS=0
MEASURE_LOOKUPS=20000000
PROGRESS_INTERVAL=1000000

# CXL DAX 设备名（不带 /dev/ 前缀，供 daxctl 使用）
CXL_DAX_DEVICE="dax0.3"
# CXL 对应的 NUMA node（dax0.3 转为 system-ram 后的 node）
CXL_NUMA_NODE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BINARY="$REPO_ROOT/build/frontend/ycsb_c_compatile_test"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
RESULT_DIR="$SCRIPT_DIR/pure_memory_uniform/"

# =============================================================================
# Helpers
# =============================================================================

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

log_phase() {
    echo ""
    echo "========================================"
    echo "  $1"
    echo "========================================"
}

check_binary() {
    if [[ ! -x "$BINARY" ]]; then
        echo "[ERROR] Binary not found or not executable: $BINARY"
        echo "        Please build first: cmake --build build --target ycsb_c_compatile_test"
        exit 1
    fi
}

check_ssd_path() {
    mkdir -p "$(dirname "$SSD_PATH")"
    echo "[INFO] SSD path parent dir ready: $(dirname "$SSD_PATH")"
}

# 根据 working set 大小动态算 DRAM baseline
# 为了保证全内存，分配 1.5 倍的内存给 Buffer Pool，确保没有驱逐
compute_dram_baseline() {
    local ws_gib="$1"
    awk "BEGIN { printf \"%.3f\", $ws_gib * 1.50}"
}

# 根据 working set 大小决定预分配 SSD 空间
compute_falloc_gib() {
    local ws_gib="$1"
    awk "BEGIN { v = $ws_gib * 1.5; printf \"%d\", (v == int(v)) ? v : int(v)+1 }"
}

# =============================================================================
# CXL lifecycle helpers
# =============================================================================

cxl_to_system_ram() {
    log_phase "Converting ${CXL_DAX_DEVICE} → system-ram (NUMA node ${CXL_NUMA_NODE})"

    sudo daxctl reconfigure-device \
        --mode=system-ram "${CXL_DAX_DEVICE}" --force

    sleep 3

    local mem_mb
    mem_mb=$(numactl --hardware \
        | awk "/^node ${CXL_NUMA_NODE} size:/ {print \$4}")

    if [[ -z "$mem_mb" || "$mem_mb" -eq 0 ]]; then
        echo "[ERROR] NUMA node ${CXL_NUMA_NODE} has no memory after conversion!"
        exit 1
    fi
    echo "[INFO] NUMA node ${CXL_NUMA_NODE} memory: ${mem_mb} MB ✓"
    numactl --hardware
}

cxl_to_devdax() {
    log_phase "Restoring ${CXL_DAX_DEVICE} → devdax"

    echo "[INFO] Offlining memory sections first..."
    sudo daxctl offline-memory "${CXL_DAX_DEVICE}" || true

    sleep 2

    sudo daxctl reconfigure-device \
        --mode=devdax "${CXL_DAX_DEVICE}" --force

    echo "[INFO] ${CXL_DAX_DEVICE} restored to devdax mode ✓"
    daxctl list -d "${CXL_DAX_DEVICE}"
}

# =============================================================================
# run_mode <label> <admission_mode> [extra_flags...]
# =============================================================================
run_mode() {
    local label="$1"
    local mode="$2"
    local result_file
    result_file="$RESULT_DIR/pure_memory_uniform_${label}_ws${CURRENT_WS_GIB}gib_${TIMESTAMP}.csv"
    shift 2
    local extra_flags=("$@")

    local numa_prefix=()
    if [[ -n "${NUMA_MEMBIND_NODE:-}" ]]; then
        numa_prefix=(numactl --membind="${NUMA_MEMBIND_NODE}")
        echo "[INFO] NUMA membind: node ${NUMA_MEMBIND_NODE} (CXL memory)"
    else
        echo "[INFO] NUMA membind: disabled (system default DRAM)"
    fi

    log_phase "Starting mode: ${label} (ws=${CURRENT_WS_GIB} GiB, uniform, admission=${mode})"
    echo "[INFO] Result file : $result_file"
    echo "[INFO] Working set : ${CURRENT_WS_GIB} GiB | distribution: uniform"
    echo "[INFO] DRAM baseline: ${CURRENT_DRAM_BASELINE} GiB"
    echo "[INFO] Lookups     : warmup=${WARMUP_LOOKUPS}  measure=${MEASURE_LOOKUPS}"

    local start_ts
    start_ts=$(date +%s)

    local cmd_header
    cmd_header="$(
        echo ""
        echo "[CMD] ${numa_prefix[*]:-} $BINARY \\"
        echo "    --test_admission_mode=$mode \\"
        for flag in "${extra_flags[@]}"; do echo "    $flag \\"; done
        for flag in "${COMMON_FLAGS[@]}";  do echo "    $flag \\"; done
        echo ""
    )"
    echo "$cmd_header"

    local exit_code=0
    {
        echo "$cmd_header"
        "${numa_prefix[@]}" "$BINARY" \
            --test_admission_mode="$mode" \
            "${extra_flags[@]}" \
            "${COMMON_FLAGS[@]}" \
            2>&1
    } | tee "$result_file" || exit_code=$?

    if [[ $exit_code -ne 0 ]]; then
        echo ""
        echo "[WARN] Binary exited with code $exit_code (likely teardown segfault)."
        echo "[WARN] Measured data already written — $result_file is valid."
    fi

    echo "[INFO] Waiting 20s for full process teardown..."
    sleep 20

    local end_ts elapsed
    end_ts=$(date +%s)
    elapsed=$((end_ts - start_ts))
    echo "[INFO] Mode '${label}' (ws=${CURRENT_WS_GIB}, uniform) finished in ${elapsed}s."
}

# =============================================================================
# run_one_combination <ws_gib>
# =============================================================================
run_one_combination() {
    local ws_gib="$1"

    CURRENT_WS_GIB="$ws_gib"

    local dram_baseline
    dram_baseline=$(compute_dram_baseline "$ws_gib")
    CURRENT_DRAM_BASELINE="$dram_baseline"

    local falloc_gib
    falloc_gib=$(compute_falloc_gib "$ws_gib")

    COMMON_FLAGS=(
        --test_distribution="uniform"
        --test_working_set_gib="$ws_gib"
        --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
        --worker_threads="$WORKER_THREADS"
        --vi=true
        --test_warmup_lookups="$WARMUP_LOOKUPS"
        --test_measure_lookups="$MEASURE_LOOKUPS"
        --test_progress_interval="$PROGRESS_INTERVAL"
        --ssd_path="$SSD_PATH"
        --trunc=true
        --wal=true
        --falloc="$falloc_gib"
    )

    log_phase "=== Combination: ws=${ws_gib} GiB | uniform | dram_baseline=${dram_baseline} GiB ==="

    # ------------------------------------------------------------------
    # Run A: pure DRAM + SSD
    # ------------------------------------------------------------------
    log_phase "Run A: pure DRAM + SSD  (ws=${ws_gib}, uniform)"

    NUMA_MEMBIND_NODE="" \
    run_mode "dram_ssd" "page_only" \
        --cxl_tiering_enabled=false \
        --dram_buffer_pool_gib="$dram_baseline" \
        --pp_threads="$PP_THREADS"

    # ------------------------------------------------------------------
    # Run B: pure CXL + SSD
    # ------------------------------------------------------------------
    log_phase "Run B: pure CXL + SSD  (ws=${ws_gib}, uniform)"

    cxl_to_system_ram

    trap 'echo ""; echo "[TRAP] Caught signal — restoring ${CXL_DAX_DEVICE} to devdax..."; cxl_to_devdax' \
        EXIT INT TERM ERR

    NUMA_MEMBIND_NODE="${CXL_NUMA_NODE}" \
    run_mode "pure_cxl" "page_only" \
        --cxl_tiering_enabled=false \
        --dram_buffer_pool_gib="$dram_baseline" \
        --pp_threads="$PP_THREADS"

    trap - EXIT INT TERM ERR
    cxl_to_devdax
}

# =============================================================================
# Pre-flight checks
# =============================================================================
check_binary
check_ssd_path
mkdir -p "$RESULT_DIR"

# =============================================================================
# 汇总打印本次实验规模
# =============================================================================
total_combinations=${#WORKING_SET_GIB_LIST[@]}
total_runs=$(( total_combinations * 2 ))

echo ""
echo "============================================================"
echo "  YCSB-C  pure DRAM  vs  pure CXL  — Uniform Random"
echo "  working_set_gib_list = ${WORKING_SET_GIB_LIST[*]}"
echo "  distribution         = uniform"
echo "  payload_size_bytes   = $PAYLOAD_SIZE_BYTES B"
echo "  cxl_numa_node        = $CXL_NUMA_NODE"
echo "  warmup_lookups       = $WARMUP_LOOKUPS"
echo "  measure_lookups      = $MEASURE_LOOKUPS"
echo "  combinations         = $total_combinations"
echo "  total runs           = $total_runs  (× 2 modes)"
echo "  timestamp            = $TIMESTAMP"
echo "============================================================"

# =============================================================================
# 循环：working_set
# =============================================================================
combo_idx=0
for ws_gib in "${WORKING_SET_GIB_LIST[@]}"; do
    combo_idx=$(( combo_idx + 1 ))
    log_phase "Progress: combination ${combo_idx}/${total_combinations}  (ws=${ws_gib} GiB)"
    run_one_combination "$ws_gib"
done

# =============================================================================
# Done
# =============================================================================
log_phase "All combinations completed"
echo "[INFO] Results saved in: $RESULT_DIR"
ls -lh "$RESULT_DIR"/pure_memory_uniform_*_"${TIMESTAMP}".csv \
    2>/dev/null || true
