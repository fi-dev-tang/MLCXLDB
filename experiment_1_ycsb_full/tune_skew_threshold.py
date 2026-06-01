#!/usr/bin/env python3
"""Sweep skew_threshold_ratio at ws=4 GiB on YCSB-B for 3 thetas.

Goal: pick the value that maximises RecordCache hit rate (RC_HR) without
over-admitting to the point of SIEVE thrashing.

Run: ./tune_skew_threshold.py [--full]
  default: 4 skew values × 3 thetas = 12 runs (~25 min)
  --full : 5 skew values × 3 thetas = 15 runs (~30 min)

Output: per-run log under /tmp/tune_skew/, plus a summary table printed at
the end and a JSON file at /tmp/tune_skew/summary.json.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Sweep configuration
# ---------------------------------------------------------------------------
THETAS: list[float] = [0.90, 0.95, 0.99]
SKEW_VALUES_DEFAULT: list[float] = [0.10, 0.15, 0.20, 0.25]
SKEW_VALUES_FULL: list[float] = [0.08, 0.12, 0.18, 0.25, 0.32]

WS_GIB: float = 4.0
CXL_GIB: float = 2.5
WARMUP_LOOKUPS: int = 40_000_000     # 40 M — long enough to expose over-admission
MEASURE_LOOKUPS: int = 10_000_000    # 10 M — short but stable RC_HR
WORKER_THREADS: int = 8
PAYLOAD_BYTES: int = 100

# theta-specific tuning (everything except skew_threshold_ratio).
# trigger_visit_histogram_update_size locked at 1 M per user requirement.
THETA_FLAGS: dict[float, dict[str, float | int]] = {
    0.90: dict(uniform=0.45, max_per_page=8000, max_global=2_000_000),
    0.95: dict(uniform=0.55, max_per_page=6000, max_global=1_500_000),
    0.99: dict(uniform=0.65, max_per_page=4000, max_global=1_000_000),
}

# Paths
REPO_ROOT = Path("/home/zhizhi.tyf/cxl-recordcache-dev")
BUILD_DIR = REPO_ROOT / "build/frontend"
SSD_PATH = "/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
DAX_DEVICE = "/dev/dax0.2"
OUT_DIR = Path("/tmp/tune_skew")
OUT_DIR.mkdir(parents=True, exist_ok=True)

COOLDOWN_SEC: int = 10  # short cooldown between runs


def build_cmd(theta: float, skew: float) -> list[str]:
    flags = THETA_FLAGS[theta]
    return [
        str(BUILD_DIR / "experiment_1_ycsb_b"),
        "--test_admission_mode=two_level",
        f"--test_zipf_theta={theta}",
        f"--test_warmup_lookups={WARMUP_LOOKUPS}",
        f"--test_measure_lookups={MEASURE_LOOKUPS}",
        f"--test_working_set_gib={WS_GIB}",
        f"--test_payload_size_bytes={PAYLOAD_BYTES}",
        f"--worker_threads={WORKER_THREADS}",
        "--vi=true",
        "--cxl_tiering_enabled=true",
        f"--cxl_gib={CXL_GIB}",
        f"--cxl_dax_device_path={DAX_DEVICE}",
        "--pp_threads=1",
        "--cxl_pp_threads=1",
        "--two_level_admission_threads=2",
        "--delay_admission_recordcache_threads_start=true",
        "--dram_buffer_pool_gib=0.125",
        "--dram_recordcache_gib=0.500",
        "--forward_epoch_thread=1",
        "--sieve_eviction_thread=1",
        "--record_cache_promote_thread=4",
        f"--skew_threshold_ratio={skew}",
        f"--uniform_threshold_ratio={flags['uniform']}",
        f"--max_per_page_visits={flags['max_per_page']}",
        f"--max_global_requests_window={flags['max_global']}",
        "--trigger_visit_histogram_update_size=1000000",
        f"--ssd_path={SSD_PATH}",
        "--trunc=true",
        "--wal=true",
        "--test_progress_interval=2000000",
        "--test_warmup_progress_interval=5000000",
    ]


_LOG_PATTERNS: dict[str, re.Pattern[str]] = {
    "rc_hr":         re.compile(r"mode=two_level.*?RC_HR=([\d.]+)%", re.DOTALL),
    "mqps":          re.compile(r"mode=two_level.*?Mqps=([\d.]+)", re.DOTALL),
    "p99_us":        re.compile(r"mode=two_level.*?p99_us=([\d.]+)", re.DOTALL),
    "rc_entries":    re.compile(r"\[RC State\]\[Final\] entry_count=(\d+)"),
    "rc_capacity":   re.compile(r"\[RC State\]\[Final\] entry_count=\d+, capacity=(\d+)"),
    "rc_fill":       re.compile(r"\[RC State\]\[Final\] .*?fill=([\d.]+)%"),
    "warmup_secs":   re.compile(r"warmup_elapsed=([\d.]+)s"),
    "measure_secs":  re.compile(r"measure_elapsed=([\d.]+)s"),
    "evicted":       re.compile(r"RecordCache SIEVE evicted_entries=(\d+)"),
}

_SKEW_PROMOTE_FINAL = re.compile(
    r"\[L2\] skew_promote=(\d+)\s+uniform_promote=\d+\s+timeout_removed=\d+",
    re.DOTALL,
)


def parse_log(path: Path) -> dict[str, float | int]:
    text = path.read_text(errors="ignore")
    out: dict[str, float | int] = {}
    for key, pat in _LOG_PATTERNS.items():
        m = pat.search(text)
        if m:
            try:
                out[key] = float(m.group(1)) if "." in m.group(1) else int(m.group(1))
            except ValueError:
                out[key] = m.group(1)  # type: ignore[assignment]
    # last skew_promote = the [L2] line emitted by the *measure* progress line
    matches = _SKEW_PROMOTE_FINAL.findall(text)
    if matches:
        out["skew_promote_final"] = int(matches[-1])
    return out


def run_one(theta: float, skew: float, idx: int, total: int) -> dict:
    log_path = OUT_DIR / f"b_theta{theta}_skew{skew:.3f}.log"
    print(f"\n[{idx}/{total}] theta={theta} skew={skew}  -> {log_path}")
    t0 = time.time()
    with log_path.open("w") as f:
        proc = subprocess.run(
            build_cmd(theta, skew),
            stdout=f,
            stderr=subprocess.STDOUT,
            check=False,
        )
    dur = time.time() - t0
    parsed = parse_log(log_path)
    parsed.update({
        "theta": theta,
        "skew": skew,
        "exit_code": proc.returncode,
        "wall_secs": round(dur, 1),
        "log_path": str(log_path),
    })
    rc_hr = parsed.get("rc_hr", "?")
    fill = parsed.get("rc_fill", "?")
    sp = parsed.get("skew_promote_final", "?")
    mqps = parsed.get("mqps", "?")
    print(f"    RC_HR={rc_hr} fill={fill}% skew_promote={sp} Mqps={mqps} ({dur:.1f}s)")
    return parsed


def print_summary(results: list[dict]) -> None:
    print("\n" + "=" * 96)
    print(f"{'theta':>6}  {'skew':>6}  {'RC_HR%':>7}  {'fill%':>7}  {'Mqps':>7}  {'p99_us':>8}  "
          f"{'skew_promote':>13}  {'evicted':>10}")
    print("-" * 96)
    for r in sorted(results, key=lambda x: (x["theta"], x["skew"])):
        rc = r.get("rc_hr", "-")
        fl = r.get("rc_fill", "-")
        mq = r.get("mqps", "-")
        p9 = r.get("p99_us", "-")
        sp = r.get("skew_promote_final", "-")
        ev = r.get("evicted", "-")
        rc_s = f"{rc:.2f}" if isinstance(rc, float) else str(rc)
        fl_s = f"{fl:.2f}" if isinstance(fl, float) else str(fl)
        mq_s = f"{mq:.4f}" if isinstance(mq, float) else str(mq)
        p9_s = f"{p9:.1f}" if isinstance(p9, float) else str(p9)
        sp_s = f"{sp:,}" if isinstance(sp, int) else str(sp)
        ev_s = f"{ev:,}" if isinstance(ev, int) else str(ev)
        print(f"{r['theta']:>6.2f}  {r['skew']:>6.3f}  {rc_s:>7}  {fl_s:>7}  "
              f"{mq_s:>7}  {p9_s:>8}  {sp_s:>13}  {ev_s:>10}")
    print("=" * 96)


def recommend(results: list[dict]) -> dict[float, float]:
    """Pick the skew_threshold_ratio with the highest RC_HR per theta.
    Tie-break: prefer the value with higher Mqps."""
    out: dict[float, float] = {}
    for theta in THETAS:
        cands = [r for r in results if r["theta"] == theta and "rc_hr" in r]
        if not cands:
            continue
        best = max(cands, key=lambda r: (r["rc_hr"], r.get("mqps", 0)))
        out[theta] = best["skew"]
        print(f"theta={theta}: best skew_threshold_ratio = {best['skew']} "
              f"(RC_HR={best['rc_hr']:.2f}%, Mqps={best.get('mqps','?')})")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true",
                    help="use 5 skew values per theta (15 runs total)")
    args = ap.parse_args()

    skews = SKEW_VALUES_FULL if args.full else SKEW_VALUES_DEFAULT
    plan = [(t, s) for t in THETAS for s in skews]
    total = len(plan)

    print(f"Sweep plan: {total} runs ({len(THETAS)} thetas × {len(skews)} skew values)")
    print(f"  thetas        = {THETAS}")
    print(f"  skew_values   = {skews}")
    print(f"  ws            = {WS_GIB} GiB,  CXL = {CXL_GIB} GiB,  threads = {WORKER_THREADS}")
    print(f"  warmup        = {WARMUP_LOOKUPS:,}")
    print(f"  measure       = {MEASURE_LOOKUPS:,}")
    print(f"  log dir       = {OUT_DIR}")

    if not (BUILD_DIR / "experiment_1_ycsb_b").exists():
        print(f"ERROR: binary not found at {BUILD_DIR}/experiment_1_ycsb_b", file=sys.stderr)
        return 1

    results: list[dict] = []
    for idx, (theta, skew) in enumerate(plan, 1):
        r = run_one(theta, skew, idx, total)
        results.append(r)
        if idx < total:
            time.sleep(COOLDOWN_SEC)

    print_summary(results)

    print("\n=== Recommendation (max RC_HR per theta) ===")
    rec = recommend(results)

    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps({
        "config": {
            "ws_gib": WS_GIB, "cxl_gib": CXL_GIB,
            "warmup": WARMUP_LOOKUPS, "measure": MEASURE_LOOKUPS,
            "worker_threads": WORKER_THREADS,
        },
        "results": results,
        "recommendation": rec,
    }, indent=2))
    print(f"\nFull summary saved: {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
