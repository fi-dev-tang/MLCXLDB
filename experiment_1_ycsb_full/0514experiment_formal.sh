#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Master orchestrator: formal ablation experiment
#
# Calls three working_set scripts strictly in series so that /dev/dax0.2 and
# the SSD test file are never touched by two YCSB binaries simultaneously.
# Between each sub-script we cool down for COOLDOWN_BETWEEN_SCRIPTS seconds
# to let the dax mapping fully release and the kernel page cache settle.
#
# Run order (chosen by user — ws=4 is priority for the 8AM deadline):
#   1) ws=4   (priority,  ~14h with 8 worker threads)
#   2) ws=10  (extension, ~35h)
#   3) ws=8   (extension, ~26h)
#
# Output layout (under this script's directory):
#   experiment_formal_<TS>/
#     ws4/   <- result_*.log files from ws=4 sub-run
#     ws10/  <- result_*.log files from ws=10 sub-run
#     ws8/   <- result_*.log files from ws=8 sub-run
#     master.log  <- this script's stdout/stderr (if invoked with tee)
#
# Sub-scripts honor the EXPERIMENT_ROOT env var we export here, so each one
# writes into a clean per-ws sub-directory beneath the single timestamped root.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Single timestamped root that groups all 3 sub-runs together.
export EXPERIMENT_ROOT="$SCRIPT_DIR/experiment_formal_${TIMESTAMP}"
mkdir -p "$EXPERIMENT_ROOT"

# Mirror the master log into the experiment dir.
MASTER_LOG="$EXPERIMENT_ROOT/master.log"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
DAX_DEVICE="/dev/dax0.2"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
COOLDOWN_BETWEEN_SCRIPTS=120        # seconds — lets dax mapping fully release

# Sub-script execution order (do NOT change without re-checking deadline plan).
SUB_SCRIPTS=(
   "run_ablation_ws4.sh"
   "run_ablation_ws10.sh"
   "run_ablation_ws8.sh"
)

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "############################################################"
   echo "# $1"
   echo "############################################################"
}

# Fail fast if any other YCSB binary is still using the dax device.
# Uses lsof when available; falls back to pgrep on the binary name pattern.
assert_no_running_ycsb() {
   local stale=""
   if command -v lsof >/dev/null 2>&1; then
      stale="$(lsof -t "$DAX_DEVICE" 2>/dev/null || true)"
   fi
   if [[ -z "$stale" ]]; then
      stale="$(pgrep -f 'experiment_1_ycsb_[abcdef]( |$)' || true)"
   fi
   if [[ -n "$stale" ]]; then
      echo "[ERROR] another YCSB process is still alive (pids: $stale)"
      echo "[ERROR] aborting to avoid /dev/dax0.2 contention"
      echo "[HINT]  kill the stale process first, then re-run this script"
      return 1
   fi
}

# -----------------------------------------------------------------------------
# Pre-flight checks
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $DAX_DEVICE not found"
      return 1
   fi
   for s in "${SUB_SCRIPTS[@]}"; do
      if [[ ! -x "$SCRIPT_DIR/$s" ]]; then
         echo "[ERROR] sub-script not executable: $SCRIPT_DIR/$s"
         return 1
      fi
   done
   assert_no_running_ycsb
}

# -----------------------------------------------------------------------------
# Main flow — wrapped so we can tee everything to master.log
# -----------------------------------------------------------------------------
main() {
   log_phase "FORMAL EXPERIMENT START"
   echo "[INFO] timestamp                = $TIMESTAMP"
   echo "[INFO] experiment_root          = $EXPERIMENT_ROOT"
   echo "[INFO] sub-scripts (in order)   = ${SUB_SCRIPTS[*]}"
   echo "[INFO] dax_device               = $DAX_DEVICE"
   echo "[INFO] ssd_path                 = $SSD_PATH"
   echo "[INFO] cooldown_between_scripts = ${COOLDOWN_BETWEEN_SCRIPTS}s"

   preflight

   local overall_start
   overall_start=$(date +%s)

   local n=${#SUB_SCRIPTS[@]}
   for idx in "${!SUB_SCRIPTS[@]}"; do
      local script="${SUB_SCRIPTS[$idx]}"
      local step=$((idx + 1))

      log_phase "[$step/$n] starting sub-script: $script"
      echo "[INFO] start_time = $(date '+%Y-%m-%d %H:%M:%S')"

      # Ensure no leftover process before launching the next sub-script
      # (defensive — should already be true after wait + cooldown).
      assert_no_running_ycsb || {
         echo "[ERROR] aborting before $script"
         return 1
      }

      local sub_start sub_end sub_min sub_exit=0
      sub_start=$(date +%s)

      # Run the sub-script. EXPERIMENT_ROOT is exported above so it inherits.
      "$SCRIPT_DIR/$script" || sub_exit=$?

      sub_end=$(date +%s)
      sub_min=$(awk "BEGIN {printf \"%.1f\", ($sub_end - $sub_start) / 60.0}")

      if [[ "$sub_exit" -ne 0 ]]; then
         echo "[WARN] $script exited with code $sub_exit (continuing to next)"
      fi
      echo "[DONE] $script elapsed: ${sub_min} min"
      echo "[INFO] end_time   = $(date '+%Y-%m-%d %H:%M:%S')"

      # Cooldown — but skip after the final sub-script.
      if (( step < n )); then
         log_phase "cooldown ${COOLDOWN_BETWEEN_SCRIPTS}s before next sub-script"
         sleep "$COOLDOWN_BETWEEN_SCRIPTS"
      fi
   done

   local overall_end overall_sec overall_hr
   overall_end=$(date +%s)
   overall_sec=$((overall_end - overall_start))
   overall_hr=$(awk "BEGIN {printf \"%.2f\", $overall_sec / 3600.0}")

   log_phase "FORMAL EXPERIMENT COMPLETE"
   echo "[DONE] experiment_root = $EXPERIMENT_ROOT"
   echo "[TIME] total elapsed   = ${overall_sec}s (${overall_hr} hr)"
   echo ""
   echo "Result tree:"
   ls -la "$EXPERIMENT_ROOT"
   echo ""
   for sub in ws4 ws10 ws8; do
      if [[ -d "$EXPERIMENT_ROOT/$sub" ]]; then
         local n
         n=$(find "$EXPERIMENT_ROOT/$sub" -maxdepth 1 -name 'result_ycsb*.log' | wc -l)
         echo "  $sub : $n result files"
      fi
   done
}

# -----------------------------------------------------------------------------
# Entry — tee everything to master.log so a remote disconnect doesn't lose it.
# -----------------------------------------------------------------------------
main 2>&1 | tee "$MASTER_LOG"
