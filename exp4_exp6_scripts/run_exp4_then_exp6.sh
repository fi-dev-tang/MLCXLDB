#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Wrapper: Run Exp4 (parameter sensitivity) then Exp6 (convergence time)
#
# Both share DAX=dax0.5 and SSD=exp4_exp6_readonly, so they run sequentially.
#
# Usage:
#   nohup bash exp4_exp6_scripts/run_exp4_then_exp6.sh &
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================================"
echo "  Exp4 + Exp6 Sequential Runner"
echo "  Started: $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"

# --- Exp4: Parameter Sensitivity ---
echo ""
echo "[CHAIN] Starting Exp4 (parameter sensitivity)..."
echo "[CHAIN] $(date '+%Y-%m-%d %H:%M:%S')"
bash "$SCRIPT_DIR/exp4_parameter_sensitivity.sh"
EXP4_EXIT=$?
echo "[CHAIN] Exp4 finished with exit code $EXP4_EXIT at $(date '+%Y-%m-%d %H:%M:%S')"

# Brief cooldown between experiments
echo "[CHAIN] Cooldown 60s before Exp6..."
sleep 60

# --- Exp6: Convergence Time ---
echo ""
echo "[CHAIN] Starting Exp6 (convergence time)..."
echo "[CHAIN] $(date '+%Y-%m-%d %H:%M:%S')"
bash "$SCRIPT_DIR/exp6_convergence_time.sh"
EXP6_EXIT=$?
echo "[CHAIN] Exp6 finished with exit code $EXP6_EXIT at $(date '+%Y-%m-%d %H:%M:%S')"

echo ""
echo "============================================================"
echo "  Exp4 + Exp6 Complete"
echo "  Exp4 exit=$EXP4_EXIT, Exp6 exit=$EXP6_EXIT"
echo "  Finished: $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"
