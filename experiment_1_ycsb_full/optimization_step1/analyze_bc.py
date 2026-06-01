#!/usr/bin/env python3
"""
批量分析 optimization_step1/small_bc_two_level 下的 ycsb_b / ycsb_c 实验日志，
提取:
  - 关键配置参数 (admission tuning flags / memory / threads / workload)
  - 测量阶段最终吞吐 (Measure Progress 最后一行)
  - 测量阶段最终 RC / DRAM / CXL / SSD 命中率
  - RecordCache 填充率 (rc entries / record_cache_capacity)
  - 总吞吐 (measure_lookups / measure_total_elapsed)
输出一份对比表格 (按 workload 分组, 按吞吐排序)。
"""

import os
import re
import sys
import glob
from typing import Dict, List, Optional

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

RESULT_DIR = os.path.dirname(os.path.abspath(__file__)) + "/small_bc_two_level"


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


# ---- 关键正则 ----
RE_KV_CONFIG_LINE = re.compile(r"\s*([a-zA-Z_]+)\s*=\s*([^\s]+)")
# Measure progress 行:
#   Progress 5000000/5000000, throughput=0.123456 Mqps, RC_HR=10.12%, ..., DRAM_HR=9.88%, ..., CXL_HR=79.33%, SSD_miss=0.00% [rc=xxx dram=yyy cxl=zzz ssd=www total=ttt], fine_threshold=4, dram_hot_candidates=..., [L1] threshold=... [L2] skew_promote=... uniform_promote=... timeout_removed=...
RE_MEASURE_PROGRESS = re.compile(
    r"Progress\s+(\d+)/(\d+),\s*throughput=([\d.]+)\s*Mqps,\s*"
    r"RC_HR=([\d.]+)%.*?DRAM_HR=([\d.]+)%.*?CXL_HR=([\d.]+)%.*?SSD_miss=([\d.]+)%"
    r"\s*\[rc=(\d+)\s+dram=(\d+)\s+cxl=(\d+)\s+ssd=(\d+)\s+total=(\d+)\]"
)
# Config 行（INFO 打印）
RE_CONF_LINE = re.compile(r"^\s*(\w[\w_]*)\s+=\s+(.+?)\s*$")

# 命令行 flag（脚本把 gflags 也打印出来了，但我们主要从脚本推断）。
# 配置行里包含的内容比如 working_set_gib / record_cache_gib / zipf_theta / warmup_lookups / measure_lookups
CONFIG_KEYS_WANTED = {
    "admission_mode",
    "working_set_gib",
    "record_cache_gib",
    "zipf_theta",
    "payload_size_bytes",
    "records_per_page",
    "total_pages",
    "total_records",
    "record_cache_capacity",
    "warmup_lookups",
    "measure_lookups",
    "theoretical_RC_HR_upper",
    "theoretical_DRAM_HR_upper",
    "dram_buffer_pool_gib",
    "dram_recordcache_gib",
}

# admission tuning flags (在脚本里用参数传入, 但日志里不一定直接出现)
# 我们额外从 Workload Sanity Report 之前的 "Configuration" block 中收集能找到的值。


def parse_file(path: str) -> Dict:
    wl = "b" if "ycsbb" in os.path.basename(path) else "c"
    conf: Dict[str, str] = {}
    last_progress: Optional[re.Match] = None
    last_measure_throughput_line = ""
    # 用于粗略计算总耗时的 elapsed
    last_elapsed = None

    # 部分日志非常大, 流式读
    with open(path, "r", errors="ignore") as f:
        for raw in f:
            line = strip_ansi(raw).rstrip("\n")

            # 配置 block 里的键值对 (只在 Phase 3 之前出现)
            m = RE_CONF_LINE.match(line.lstrip("[INFO] "))
            if m:
                key = m.group(1)
                if key in CONFIG_KEYS_WANTED and key not in conf:
                    conf[key] = m.group(2)

            # Measure 阶段
            # 用 "Progress " 来识别, 但 Warmup 也有 "Progress"? 其实 warmup 是 "[Warmup] Progress"-like
            # 实际上看到的是 "[Warmup] 50000/2000000" 和 "Progress 100000/5000000" 两种格式
            # Measure 阶段是 "Progress xxx/measure_lookups"
            if "Progress " in line and "/5000000" in line and "RC_HR=" in line:
                mp = RE_MEASURE_PROGRESS.search(line)
                if mp:
                    last_progress = mp
                    last_measure_throughput_line = line

            # 抓取 elapsed 信息, 在 Warmup 里会有, 这里忽略

    rc_capacity = int(float(conf.get("record_cache_capacity", "0"))) if conf.get("record_cache_capacity") else 0

    result = {
        "file": os.path.basename(path),
        "workload": wl,
        "config": conf,
        "rc_capacity": rc_capacity,
        "progress_line": last_measure_throughput_line,
    }

    if last_progress:
        done, total, tput, rc_hr, dram_hr, cxl_hr, ssd_miss, rc_n, dram_n, cxl_n, ssd_n, total_n = last_progress.groups()
        rc_n_i = int(rc_n)
        result.update({
            "measure_done": int(done),
            "measure_total": int(total),
            "throughput_mqps": float(tput),
            "rc_hr": float(rc_hr),
            "dram_hr": float(dram_hr),
            "cxl_hr": float(cxl_hr),
            "ssd_miss": float(ssd_miss),
            "rc_hits": rc_n_i,
            "dram_hits": int(dram_n),
            "cxl_hits": int(cxl_n),
            "ssd_hits": int(ssd_n),
            "total_reqs": int(total_n),
            # RC 总命中率 + DRAM 页面命中率之和视作 "整体 DRAM 命中"
            "overall_dram_hit_rate": float(rc_hr) + float(dram_hr),
        })
    else:
        result.update({
            "throughput_mqps": None,
            "rc_hr": None,
            "dram_hr": None,
            "cxl_hr": None,
            "ssd_miss": None,
            "rc_hits": None,
        })

    return result


def parse_fill_rate_from_progress(line: str, rc_capacity: int) -> Optional[float]:
    """
    填充率 = 测量阶段末尾 RecordCache 中实际容纳的条目 / 容量
    日志里没有直接打印 entries 数, 但 skew_promote 约等于被提升的记录数;
    更准确的做法: 解析 [L2] skew_promote=N。同时注意 timeout_removed 会减掉。
    这里我们使用: filled ~ skew_promote + uniform_promote - timeout_removed
    """
    m = re.search(r"skew_promote=(\d+)\s+uniform_promote=(\d+)\s+timeout_removed=(\d+)", line)
    if not m or rc_capacity <= 0:
        return None
    skew, uni, tout = int(m.group(1)), int(m.group(2)), int(m.group(3))
    filled = max(skew + uni - tout, 0)
    return 100.0 * filled / rc_capacity


def main():
    files = sorted(glob.glob(os.path.join(RESULT_DIR, "*.csv")))
    if not files:
        print(f"No CSV under {RESULT_DIR}")
        sys.exit(1)

    rows: List[Dict] = []
    for fp in files:
        try:
            r = parse_file(fp)
        except Exception as e:
            print(f"[WARN] parse failed {fp}: {e}")
            continue
        # 填充率(近似)
        r["rc_fill_rate_pct"] = parse_fill_rate_from_progress(r.get("progress_line", ""), r["rc_capacity"])
        # 同时基于 rc_hits 推断填充率太不准(跟访问有关), 这里不使用
        rows.append(r)

    # 打印
    def fmt(x, nd=3):
        return "--" if x is None else f"{x:.{nd}f}"

    print("=" * 130)
    print(f"RESULT_DIR = {RESULT_DIR}")
    print(f"文件数量   = {len(rows)}")
    print("=" * 130)

    for wl in ("b", "c"):
        sub = [r for r in rows if r["workload"] == wl and r.get("throughput_mqps") is not None]
        sub.sort(key=lambda r: r["throughput_mqps"], reverse=True)
        print(f"\n=== YCSB-{wl.upper()}  (按 throughput 降序) ===")
        header = (
            f"{'文件 (时间戳)':<48}  {'Tput':>7}  {'RC_HR%':>7}  {'DRAM_HR%':>8}  "
            f"{'CXL_HR%':>8}  {'SSD%':>6}  {'RC_fill%':>8}  {'RC_cap':>8}  {'ws_gib':>6}  {'rc_gib':>6}  {'theta':>5}"
        )
        print(header)
        print("-" * len(header))
        for r in sub:
            ts = r["file"].split("_")[-2] + "_" + r["file"].split("_")[-1].replace(".csv", "")
            cfg = r["config"]
            print(
                f"{ts:<48}  "
                f"{r['throughput_mqps']:>7.4f}  "
                f"{r['rc_hr']:>7.2f}  "
                f"{r['dram_hr']:>8.2f}  "
                f"{r['cxl_hr']:>8.2f}  "
                f"{r['ssd_miss']:>6.2f}  "
                f"{fmt(r['rc_fill_rate_pct'],2):>8}  "
                f"{r['rc_capacity']:>8}  "
                f"{cfg.get('working_set_gib','--'):>6}  "
                f"{cfg.get('record_cache_gib','--'):>6}  "
                f"{cfg.get('zipf_theta','--'):>5}"
            )

    # 最佳组
    print("\n" + "=" * 130)
    print("TOP-1 per workload:")
    for wl in ("b", "c"):
        sub = [r for r in rows if r["workload"] == wl and r.get("throughput_mqps") is not None]
        if not sub:
            continue
        best = max(sub, key=lambda r: r["throughput_mqps"])
        print(f"\n--- YCSB-{wl.upper()} best ---")
        print(f"  file          = {best['file']}")
        print(f"  throughput    = {best['throughput_mqps']:.4f} Mqps")
        print(f"  RC_HR         = {best['rc_hr']:.2f}%")
        print(f"  DRAM_HR       = {best['dram_hr']:.2f}%")
        print(f"  CXL_HR        = {best['cxl_hr']:.2f}%")
        print(f"  SSD_miss      = {best['ssd_miss']:.2f}%")
        print(f"  RC_fill(~)    = {fmt(best['rc_fill_rate_pct'],2)}%  "
              f"(cap={best['rc_capacity']})")
        print(f"  config        = {best['config']}")
        print(f"  progress_line = {best['progress_line'][:260]}")


if __name__ == "__main__":
    main()
