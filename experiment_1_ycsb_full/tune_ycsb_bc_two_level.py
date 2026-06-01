#!/usr/bin/env python3
import argparse
import csv
import datetime
import os
import random
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class Params:
    skew_threshold_ratio: float
    uniform_threshold_ratio: float
    max_per_page_visits: int
    max_global_requests_window: int
    admission_aging_interval: int
    trigger_visit_histogram_update_size: int

    def as_flags(self) -> List[str]:
        return [
            f"--skew_threshold_ratio={self.skew_threshold_ratio:.4f}",
            f"--uniform_threshold_ratio={self.uniform_threshold_ratio:.4f}",
            f"--max_per_page_visits={self.max_per_page_visits}",
            f"--max_global_requests_window={self.max_global_requests_window}",
            f"--admission_aging_interval={self.admission_aging_interval}",
            f"--trigger_visit_histogram_update_size={self.trigger_visit_histogram_update_size}",
        ]

    def as_dict(self) -> Dict[str, float]:
        return {
            "skew_threshold_ratio": self.skew_threshold_ratio,
            "uniform_threshold_ratio": self.uniform_threshold_ratio,
            "max_per_page_visits": self.max_per_page_visits,
            "max_global_requests_window": self.max_global_requests_window,
            "admission_aging_interval": self.admission_aging_interval,
            "trigger_visit_histogram_update_size": self.trigger_visit_histogram_update_size,
        }


def clamp_float(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def clamp_int(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


def mutate(base: Params, scale: float) -> Params:
    # Multiplicative perturbation around best-so-far.
    def mfloat(v: float, lo: float, hi: float, max_pct: float) -> float:
        ratio = 1.0 + random.uniform(-max_pct, max_pct) * scale
        return clamp_float(v * ratio, lo, hi)

    def mint(v: int, lo: int, hi: int, max_pct: float, quantum: int = 1000) -> int:
        ratio = 1.0 + random.uniform(-max_pct, max_pct) * scale
        nv = int(round(v * ratio / quantum) * quantum)
        return clamp_int(nv, lo, hi)

    return Params(
        skew_threshold_ratio=round(mfloat(base.skew_threshold_ratio, 0.05, 0.25, 0.45), 4),
        uniform_threshold_ratio=round(mfloat(base.uniform_threshold_ratio, 0.20, 0.90, 0.35), 4),
        max_per_page_visits=mint(base.max_per_page_visits, 1000, 20000, 0.80, 500),
        max_global_requests_window=mint(base.max_global_requests_window, 200000, 5000000, 0.90, 10000),
        admission_aging_interval=mint(base.admission_aging_interval, 5000, 200000, 0.90, 1000),
        trigger_visit_histogram_update_size=mint(
            base.trigger_visit_histogram_update_size, 10000, 500000, 0.90, 1000
        ),
    )


def parse_rc_lookup_hr(text: str) -> float:
    # Accept both:
    # "RC_lookup_HR=11.054330%"
    # "RC_lookup_HR     = 11.054330%"
    m_lookup = re.findall(r"RC_lookup_HR\s*[=:]\s*([0-9]+(?:\.[0-9]+)?)%", text)
    if m_lookup:
        return float(m_lookup[-1])
    # YCSB-C (100% read) may only print RC_HR in measured summary.
    m_rc = re.findall(r"\bRC_HR\s*[=:]\s*([0-9]+(?:\.[0-9]+)?)%", text)
    if m_rc:
        return float(m_rc[-1])
    raise RuntimeError("Failed to parse RC_lookup_HR/RC_HR from output")


def run_one_workload(
    binary: Path,
    theta: float,
    params: Params,
    args: argparse.Namespace,
    workload: str,
    run_tag: str,
    out_dir: Path,
) -> Tuple[float, Path]:
    log_file = out_dir / f"tune_{run_tag}_ycsb{workload}_theta{theta:.2f}.log"
    cmd = [
        str(binary),
        "--test_admission_mode=two_level",
        f"--test_zipf_theta={theta:.2f}",
        "--cxl_tiering_enabled=true",
        f"--cxl_gib={args.cxl_gib}",
        f"--cxl_dax_device_path={args.cxl_dax_device}",
        f"--pp_threads={args.pp_threads}",
        f"--cxl_pp_threads={args.cxl_pp_threads}",
        f"--two_level_admission_threads={args.two_level_admission_threads}",
        "--delay_admission_recordcache_threads_start=true",
        f"--dram_buffer_pool_gib={args.dram_bp_gib}",
        f"--dram_recordcache_gib={args.dram_rc_gib}",
        f"--forward_epoch_thread={args.forward_epoch_thread}",
        f"--sieve_eviction_thread={args.sieve_eviction_thread}",
        f"--record_cache_promote_thread={args.record_cache_promote_thread}",
        f"--test_working_set_gib={args.working_set_gib}",
        f"--test_payload_size_bytes={args.payload_size_bytes}",
        f"--worker_threads={args.worker_threads}",
        "--vi=true",
        f"--test_warmup_lookups={args.warmup_lookups}",
        f"--test_measure_lookups={args.measure_lookups}",
        f"--test_warmup_progress_interval={args.warmup_progress_interval}",
        f"--test_progress_interval={args.progress_interval}",
        f"--ssd_path={args.ssd_path}",
        "--trunc=true",
        "--wal=true",
        *params.as_flags(),
    ]

    print(f"[RUN] {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log_file.write_text(result.stdout)
    if result.returncode != 0:
        print(
            f"[WARN] ycsb_{workload} returned {result.returncode}; continue and parse log (teardown segfault may happen).",
            flush=True,
        )
    hr = parse_rc_lookup_hr(result.stdout)
    return hr, log_file


def seed_by_theta(theta: float) -> Params:
    # Initial seed from previous manual tuning experience.
    if abs(theta - 0.80) < 1e-9:
        return Params(0.08, 0.35, 7000, 2000000, 60000, 120000)
    if abs(theta - 0.85) < 1e-9:
        return Params(0.09, 0.40, 6000, 1500000, 50000, 100000)
    if abs(theta - 0.90) < 1e-9:
        return Params(0.10, 0.50, 5000, 1000000, 40000, 80000)
    if abs(theta - 0.95) < 1e-9:
        return Params(0.12, 0.60, 4000, 700000, 30000, 60000)
    return Params(0.15, 0.70, 3000, 500000, 20000, 50000)


def evaluate_candidate(
    theta: float,
    params: Params,
    args: argparse.Namespace,
    out_dir: Path,
    run_tag: str,
) -> Tuple[float, float, float, List[Path]]:
    bin_b = Path(args.build_dir) / "experiment_1_ycsb_b"
    bin_c = Path(args.build_dir) / "experiment_1_ycsb_c"
    hr_b, log_b = run_one_workload(bin_b, theta, params, args, "b", run_tag, out_dir)
    hr_c, log_c = run_one_workload(bin_c, theta, params, args, "c", run_tag, out_dir)
    err_b = abs(args.target_hr - hr_b)
    err_c = abs(args.target_hr - hr_c)
    max_err = max(err_b, err_c)
    return hr_b, hr_c, max_err, [log_b, log_c]


def ensure_binaries(build_dir: Path) -> None:
    need = [build_dir / "experiment_1_ycsb_b", build_dir / "experiment_1_ycsb_c"]
    missing = [str(p) for p in need if not p.exists()]
    if missing:
        raise FileNotFoundError("Missing binaries:\n" + "\n".join(missing))


def main() -> int:
    parser = argparse.ArgumentParser(description="Auto tune two-level admission params for YCSB-B/C.")
    parser.add_argument("--repo-root", default="/home/zhizhi.tyf/cxl-recordcache-dev")
    parser.add_argument("--build-dir", default="/home/zhizhi.tyf/cxl-recordcache-dev/build/frontend")
    parser.add_argument("--result-dir", default="")
    parser.add_argument("--ssd-path", default="/nvme0n1/data/cxl_test_tmp/cxl_test_ssd")
    parser.add_argument("--cxl-dax-device", default="/dev/dax0.2")
    parser.add_argument("--thetas", default="0.80,0.85,0.90,0.95,0.99")

    # Scaled memory setup requested by user.
    parser.add_argument("--working-set-gib", type=float, default=8.0)
    parser.add_argument("--dram-bp-gib", type=float, default=0.5)
    parser.add_argument("--dram-rc-gib", type=float, default=1.0)
    parser.add_argument("--cxl-gib", type=float, default=6.0)

    parser.add_argument("--payload-size-bytes", type=int, default=100)
    parser.add_argument("--worker-threads", type=int, default=4)
    parser.add_argument("--pp-threads", type=int, default=1)
    parser.add_argument("--cxl-pp-threads", type=int, default=1)
    parser.add_argument("--two-level-admission-threads", type=int, default=1)
    parser.add_argument("--forward-epoch-thread", type=int, default=1)
    parser.add_argument("--sieve-eviction-thread", type=int, default=1)
    parser.add_argument("--record-cache-promote-thread", type=int, default=1)

    # Keep defaults small enough for iterative tuning.
    parser.add_argument("--warmup-lookups", type=int, default=200000)
    parser.add_argument("--measure-lookups", type=int, default=400000)
    parser.add_argument("--warmup-progress-interval", type=int, default=50000)
    parser.add_argument("--progress-interval", type=int, default=100000)

    parser.add_argument("--target-hr", type=float, default=90.0)
    parser.add_argument("--tolerance", type=float, default=5.0)
    parser.add_argument("--max-rounds", type=int, default=6)
    parser.add_argument("--candidates-per-round", type=int, default=5)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)
    build_dir = Path(args.build_dir)
    ensure_binaries(build_dir)

    now = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    if args.result_dir:
        out_dir = Path(args.result_dir)
    else:
        out_dir = Path(args.repo_root) / "experiment_1_ycsb_full" / "ablation_results" / f"tune_bc_two_level_{now}"
    out_dir.mkdir(parents=True, exist_ok=True)

    thetas = [float(t.strip()) for t in args.thetas.split(",") if t.strip()]
    rounds_csv = out_dir / "round_results.csv"
    best_csv = out_dir / "best_params_by_theta.csv"
    report_md = out_dir / "report.md"

    with rounds_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "theta",
                "round",
                "candidate_id",
                "hr_b",
                "hr_c",
                "max_error_vs_target",
                "skew_threshold_ratio",
                "uniform_threshold_ratio",
                "max_per_page_visits",
                "max_global_requests_window",
                "admission_aging_interval",
                "trigger_visit_histogram_update_size",
                "log_b",
                "log_c",
            ]
        )

    best_rows = []
    report_lines = ["# Two-Level Admission Auto Tuning Report", ""]
    report_lines.append(f"- Target RC_lookup_HR: {args.target_hr}%")
    report_lines.append(f"- Convergence tolerance: {args.tolerance}%")
    report_lines.append(f"- Working set: {args.working_set_gib} GiB (scaled 1/2)")
    report_lines.append(
        f"- Memory: DRAM BP={args.dram_bp_gib} GiB, RecordCache={args.dram_rc_gib} GiB, CXL BP={args.cxl_gib} GiB"
    )
    report_lines.append("")

    for theta in thetas:
        print(f"\n===== Tuning theta={theta:.2f} =====", flush=True)
        best = seed_by_theta(theta)
        best_hr_b = 0.0
        best_hr_c = 0.0
        best_err = float("inf")
        converged = False

        for rd in range(1, args.max_rounds + 1):
            # Search radius shrinks round by round.
            scale = 1.0 / rd
            candidates: List[Params] = [best]
            for _ in range(args.candidates_per_round - 1):
                candidates.append(mutate(best, scale))

            for cid, cand in enumerate(candidates):
                run_tag = f"theta{theta:.2f}_r{rd:02d}_c{cid:02d}"
                hr_b, hr_c, max_err, logs = evaluate_candidate(theta, cand, args, out_dir, run_tag)
                print(
                    f"[theta={theta:.2f}] round={rd} cand={cid} hr_b={hr_b:.3f}% hr_c={hr_c:.3f}% max_err={max_err:.3f}% params={cand.as_dict()}",
                    flush=True,
                )

                with rounds_csv.open("a", newline="") as f:
                    writer = csv.writer(f)
                    writer.writerow(
                        [
                            f"{theta:.2f}",
                            rd,
                            cid,
                            f"{hr_b:.6f}",
                            f"{hr_c:.6f}",
                            f"{max_err:.6f}",
                            cand.skew_threshold_ratio,
                            cand.uniform_threshold_ratio,
                            cand.max_per_page_visits,
                            cand.max_global_requests_window,
                            cand.admission_aging_interval,
                            cand.trigger_visit_histogram_update_size,
                            str(logs[0]),
                            str(logs[1]),
                        ]
                    )

                if max_err < best_err:
                    best = cand
                    best_hr_b = hr_b
                    best_hr_c = hr_c
                    best_err = max_err

            print(
                f"[BEST@theta={theta:.2f}] after round {rd}: hr_b={best_hr_b:.3f}% hr_c={best_hr_c:.3f}% max_err={best_err:.3f}% params={best.as_dict()}",
                flush=True,
            )
            if best_err <= args.tolerance:
                converged = True
                break

        best_rows.append(
            {
                "theta": f"{theta:.2f}",
                "converged": converged,
                "hr_b": best_hr_b,
                "hr_c": best_hr_c,
                "max_error_vs_target": best_err,
                **best.as_dict(),
            }
        )

    with best_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "theta",
                "converged",
                "hr_b",
                "hr_c",
                "max_error_vs_target",
                "skew_threshold_ratio",
                "uniform_threshold_ratio",
                "max_per_page_visits",
                "max_global_requests_window",
                "admission_aging_interval",
                "trigger_visit_histogram_update_size",
            ]
        )
        for row in best_rows:
            writer.writerow(
                [
                    row["theta"],
                    row["converged"],
                    f"{row['hr_b']:.6f}",
                    f"{row['hr_c']:.6f}",
                    f"{row['max_error_vs_target']:.6f}",
                    row["skew_threshold_ratio"],
                    row["uniform_threshold_ratio"],
                    row["max_per_page_visits"],
                    row["max_global_requests_window"],
                    row["admission_aging_interval"],
                    row["trigger_visit_histogram_update_size"],
                ]
            )

    report_lines.append("## Best Params By Theta")
    for row in best_rows:
        report_lines.append(
            (
                f"- theta={row['theta']}: converged={row['converged']}, "
                f"hr_b={row['hr_b']:.3f}%, hr_c={row['hr_c']:.3f}%, max_error={row['max_error_vs_target']:.3f}% | "
                f"skew_threshold_ratio={row['skew_threshold_ratio']}, "
                f"uniform_threshold_ratio={row['uniform_threshold_ratio']}, "
                f"max_per_page_visits={row['max_per_page_visits']}, "
                f"max_global_requests_window={row['max_global_requests_window']}, "
                f"admission_aging_interval={row['admission_aging_interval']}, "
                f"trigger_visit_histogram_update_size={row['trigger_visit_histogram_update_size']}"
            )
        )
    report_lines.append("")
    report_lines.append(f"- round details: `{rounds_csv}`")
    report_lines.append(f"- best summary: `{best_csv}`")
    report_lines.append("")
    report_md.write_text("\n".join(report_lines))

    print("\n===== Tuning Completed =====", flush=True)
    print(f"Round report: {rounds_csv}", flush=True)
    print(f"Best report : {best_csv}", flush=True)
    print(f"Markdown    : {report_md}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"[FATAL] {e}", file=sys.stderr)
        sys.exit(1)
