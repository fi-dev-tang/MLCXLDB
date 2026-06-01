#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Wrapper: Run Exp7 Group1 (B/C/D + TPC-C on dax0.6)
#
# Exp5 已独占 dax0.8, 不再串联。
# Group2 (A/E/F on dax0.7) 由另一个 nohup 独立启动。
#
# Usage:
#   nohup bash exp7_scripts/run_exp7_then_exp5.sh &
#   nohup bash exp7_scripts/exp7_group2_AEF.sh &       # 并行
#   nohup bash exp5_profile_scrips/exp5_perf_profiling.sh &  # 独立并行
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================================"
echo "  Exp7-Group1 Runner (dax0.6)"
echo "  Started: $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"

# --- Exp7 Group1: B/C/D + TPC-C ---
echo ""
echo "[CHAIN] Starting Exp7 Group1 (B/C/D + TPC-C, dax0.6)..."
echo "[CHAIN] $(date '+%Y-%m-%d %H:%M:%S')"
bash "$SCRIPT_DIR/exp7_group1_BCD_tpcc.sh"
EXP7_EXIT=$?
echo "[CHAIN] Exp7 Group1 finished with exit code $EXP7_EXIT at $(date '+%Y-%m-%d %H:%M:%S')"

echo ""
echo "============================================================"
echo "  Exp7-Group1 Complete"
echo "  exit=$EXP7_EXIT"
echo "  Finished: $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"
