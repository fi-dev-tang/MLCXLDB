#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Extract key metrics from CMS parameter sensitivity experiment results.
# Produces a tab-separated summary table for easy comparison.
#
# Usage:
#   ./extract_metrics.sh                    # Process all results in ./results/
#   ./extract_metrics.sh ./results/         # Specify results directory
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULT_DIR="${1:-$SCRIPT_DIR/results}"

if [[ ! -d "$RESULT_DIR" ]]; then
  echo "[ERROR] Results directory not found: $RESULT_DIR"
  echo "Run ./run_parameter_sensitivity.sh first."
  exit 1
fi

# Strip ANSI escape codes
strip_ansi() {
  sed 's/\x1b\[[0-9;]*m//g'
}

echo "============================================================================"
echo "CMS Parameter Sensitivity — Results Summary"
echo "============================================================================"
echo ""

# Header
printf "%-8s %-8s %-12s %-12s %-10s %-10s %-10s %-10s %-10s %-12s %-12s %-10s\n" \
  "Group" "WL" "CMS_cols" "Window" "RC_fill%" "RC_HR%" "Mqps" "p95_us" "p99_us" "skew_prom" "unif_prom" "sieve_evict"
printf "%-8s %-8s %-12s %-12s %-10s %-10s %-10s %-10s %-10s %-12s %-12s %-10s\n" \
  "--------" "--------" "------------" "------------" "----------" "----------" "----------" "----------" "----------" "------------" "------------" "----------"

for result_file in "$RESULT_DIR"/result_*.csv; do
  [[ -f "$result_file" ]] || continue

  fname="$(basename "$result_file")"

  # Parse group and workload from filename: result_A1_ycsbc_cols64K_TIMESTAMP.csv
  group=$(echo "$fname" | sed -n 's/^result_\([A-Z][0-9]\)_ycsb.*/\1/p')
  wl=$(echo "$fname" | sed -n 's/^result_[A-Z][0-9]_ycsb\([bc]\)_.*/\1/p')

  [[ -z "$group" || -z "$wl" ]] && continue

  # Extract CMS cols from filename (e.g., cols64K, cols256K, cols1M, cols2M)
  cms_cols=$(echo "$fname" | grep -oP 'cols\K[0-9]+[KMG]?' || true)
  [[ -z "$cms_cols" ]] && cms_cols="-"

  # Extract trigger_window from DEBUG line (e.g., trigger_window=1000000)
  trigger_win=$(grep "trigger_window=" "$result_file" 2>/dev/null | strip_ansi | grep -oP 'trigger_window=\K[0-9]+' | tail -1 || true)
  # Also try extracting from filename for window groups (e.g., win500K, win1M)
  if [[ -z "$trigger_win" ]]; then
    trigger_win=$(echo "$fname" | grep -oP 'win\K[0-9]+[KMG]?' || true)
  fi
  [[ -z "$trigger_win" ]] && trigger_win="-"

  # Extract RC fill from final state
  rc_fill=$(grep "RC State.*Final" "$result_file" 2>/dev/null | strip_ansi | grep -oP 'fill=\K[0-9.]+' | tail -1 || true)
  [[ -z "$rc_fill" ]] && rc_fill="-"

  # Extract final metrics from mode=two_level line
  final_line=$(grep "mode=two_level" "$result_file" 2>/dev/null | strip_ansi | tail -1 || true)

  if [[ -n "$final_line" ]]; then
    rc_hr=$(echo "$final_line" | grep -oP 'RC_HR=\K[0-9.]+' || true)
    mqps=$(echo "$final_line" | grep -oP 'Mqps=\K[0-9.]+' || true)
    p95=$(echo "$final_line" | grep -oP 'p95_us=\K[0-9.]+' || true)
    p99=$(echo "$final_line" | grep -oP 'p99_us=\K[0-9.]+' || true)
  else
    rc_hr="-"; mqps="-"; p95="-"; p99="-"
  fi

  # Extract promotion counts from last progress line
  last_progress=$(grep "skew_promote=" "$result_file" 2>/dev/null | strip_ansi | tail -1 || true)
  if [[ -n "$last_progress" ]]; then
    skew_prom=$(echo "$last_progress" | grep -oP 'skew_promote=\K[0-9]+' || true)
    unif_prom=$(echo "$last_progress" | grep -oP 'uniform_promote=\K[0-9]+' || true)
  else
    skew_prom="-"; unif_prom="-"
  fi

  # Extract SIEVE evictions
  sieve_evict=$(grep "SIEVE evicted_entries" "$result_file" 2>/dev/null | strip_ansi | grep -oP '\d+' | tail -1 || true)
  [[ -z "$sieve_evict" ]] && sieve_evict="-"

  printf "%-8s %-8s %-12s %-12s %-10s %-10s %-10s %-10s %-10s %-12s %-12s %-10s\n" \
    "$group" "YCSB-${wl^^}" "$cms_cols" "$trigger_win" \
    "$rc_fill" "${rc_hr:-"-"}" "${mqps:-"-"}" "${p95:-"-"}" "${p99:-"-"}" \
    "${skew_prom:-"-"}" "${unif_prom:-"-"}" "$sieve_evict"

done | sort

echo ""
echo "============================================================================"
echo "Key: RC_fill = RecordCache fill ratio at end"
echo "     RC_HR   = RecordCache hit rate during measurement"
echo "     Mqps    = Million queries per second"
echo "     skew/unif_prom = cumulative skew/uniform promotions"
echo "     sieve_evict = SIEVE evicted entries (high = thrashing)"
echo "============================================================================"
