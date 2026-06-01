#!/usr/bin/env python3
"""
分析 page 访问次数 和 record 访问次数 的对应关系，
排查 RecordCache promote 比例低的原因，并基于 Zipf 分布给出理论填充率上限。

字段语义（来自 backend/leanstore/storage/two-level-admission-control/SkewRecordAdmission.hpp）：
  PAGE\\t<page_id>\\t<page_cms_visit_count>\\t<bf_addr>\\t<is_in_dram>
       -- AddCandidate 时打印一次，page_cms_visit_count 是进入 candidate pool 时
          page 在全局 PageCountMinSketch 中的累计访问次数快照（=过去的 page 热度）。
  Slot\\t<page_id>\\t<per_page_visits>\\t<slot_id>:<cms_cnt>,...\\t<branch>
       -- 每轮 CheckAndPromote 对每个候选 page 打印一次，per_page_visits 是
          page 进入 candidate pool 之后的累加观察次数（不含进入前的历史）。
          slot 列表只包含 cms_cnt > 0 的槽。
"""
import sys
import re
from collections import defaultdict, Counter

CSV_FILE = sys.argv[1] if len(sys.argv) > 1 else \
    "result_ycsbb_two_level_theta0.90_small_20260505_215209.csv"

# ------------------------------------------------------------------
# Workload 常量（默认值，与日志 header 一致；下面会尝试从日志覆盖）
# ------------------------------------------------------------------
TOTAL_RECORDS   = 7340032
RECORDS_PER_PG  = 56
TOTAL_PAGES     = 131072
ZIPF_THETA      = 0.90
RC_GIB          = 0.5
RC_ENTRY_BYTES  = 124
RC_CAPACITY     = int(RC_GIB * (1 << 30) // RC_ENTRY_BYTES)

# ------------------------------------------------------------------
# 1. 解析 PAGE / Slot 行 + fine_threshold 轨迹 + 配置 header
# ------------------------------------------------------------------

branch_stats = defaultdict(lambda: {
    "slot_rows": 0,
    "ppv_sum": 0,
    "ppv_min": 10**18,
    "ppv_max": 0,
    "pages": set(),
    "ppv_bucket": Counter(),
    "record_max_bucket": Counter(),
    "distinct_slot_bucket": Counter(),
})

page_last_ppv           = {}
page_last_max_cnt       = {}
page_cms_at_admit       = {}
page_decision_seq       = defaultdict(list)
page_is_promoted        = defaultdict(bool)
page_is_entire_promoted = defaultdict(bool)

# (page, slot) 对的 max CMS（用于估算有多少槽历史上达到过 fine_threshold）
pair_max_cms            = defaultdict(int)

# fine_threshold 轨迹
fine_threshold_events   = []   # list of dict(req, threshold, promotions, cleared)

page_rows_total         = 0
slot_total_rows         = 0

HEADER_RES = {
    "theta":          re.compile(r"zipf_theta\s*=\s*([0-9.]+)"),
    "total_records":  re.compile(r"total_records\s*\(?[^=]*=\s*([0-9]+)"),
    "records_per_pg": re.compile(r"records_per_page\s*=\s*([0-9]+)"),
    "rc_gib":         re.compile(r"dram_recordcache_gib\s*=\s*([0-9.]+)"),
    "rc_entry":       re.compile(r"rc_entry_bytes\s*=\s*([0-9]+)"),
}
EPOCH_TICK_RE = re.compile(
    r"admission_epoch_tick\s+req=(\d+)\s+l1_fine_threshold=(\d+)\s+promotions=(\d+)\s+cleared_candidates=(\d+)"
)


def bucket_label(v: int) -> str:
    if v == 0: return "0"
    if v == 1: return "1"
    if v <= 3: return "2-3"
    if v <= 7: return "4-7"
    if v <= 15: return "8-15"
    if v <= 31: return "16-31"
    if v <= 63: return "32-63"
    if v <= 127: return "64-127"
    if v <= 255: return "128-255"
    if v <= 511: return "256-511"
    if v <= 1023: return "512-1023"
    if v <= 4095: return "1024-4095"
    if v <= 16383: return "4096-16383"
    return ">=16384"


def strip_ansi(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


BRANCH_PROMOTE_PARTIAL = {"C0_STRONG_SKEW_L1", "C3_SKEW_NEW_SLOT_PROMOTE"}
BRANCH_PROMOTE_ENTIRE  = {"C1_UNIFORM_BY_PAGE_VISITS", "C2_UNIFORM_BY_ACCESSED_SLOTS"}

with open(CSV_FILE, "r", errors="replace") as f:
    for raw in f:
        line = strip_ansi(raw)
        # -------- header 参数覆盖 --------
        if "zipf_theta" in line and "=" in line:
            m = HEADER_RES["theta"].search(line)
            if m: ZIPF_THETA = float(m.group(1))
        if "total_records" in line:
            m = HEADER_RES["total_records"].search(line)
            if m: TOTAL_RECORDS = int(m.group(1))
        if "records_per_page" in line:
            m = HEADER_RES["records_per_pg"].search(line)
            if m: RECORDS_PER_PG = int(m.group(1))
        if "dram_recordcache_gib" in line:
            m = HEADER_RES["rc_gib"].search(line)
            if m:
                RC_GIB = float(m.group(1))
                RC_CAPACITY = int(RC_GIB * (1 << 30) // RC_ENTRY_BYTES)
        if "rc_entry_bytes" in line:
            m = HEADER_RES["rc_entry"].search(line)
            if m:
                RC_ENTRY_BYTES = int(m.group(1))
                RC_CAPACITY = int(RC_GIB * (1 << 30) // RC_ENTRY_BYTES)

        # -------- fine_threshold 轨迹 --------
        if "admission_epoch_tick" in line:
            m = EPOCH_TICK_RE.search(line)
            if m:
                fine_threshold_events.append({
                    "req":        int(m.group(1)),
                    "threshold":  int(m.group(2)),
                    "promotions": int(m.group(3)),
                    "cleared":    int(m.group(4)),
                })

        # -------- PAGE / Slot 行 --------
        if line.startswith("PAGE\t"):
            page_rows_total += 1
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            try:
                page_id = int(parts[1])
                cms_at_admit = int(parts[2])
            except ValueError:
                continue
            # 首次 AddCandidate 的快照保留最大值（理论上每个 page 只打印一次）
            if page_id not in page_cms_at_admit or cms_at_admit > page_cms_at_admit[page_id]:
                page_cms_at_admit[page_id] = cms_at_admit

        elif line.startswith("Slot\t"):
            slot_total_rows += 1
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            try:
                page_id = int(parts[1])
                ppv = int(parts[2])
            except ValueError:
                continue
            slot_str = parts[3]
            branch = parts[4].strip()

            slot_counts = []
            for tok in slot_str.split(","):
                tok = tok.strip()
                if not tok or ":" not in tok:
                    continue
                try:
                    sid, cnt = tok.split(":")
                    slot_counts.append((int(sid), int(cnt)))
                except ValueError:
                    continue
            distinct_slot_num = len(slot_counts)
            record_max_cnt = max((c for _, c in slot_counts), default=0)

            st = branch_stats[branch]
            st["slot_rows"] += 1
            st["ppv_sum"] += ppv
            st["ppv_min"] = min(st["ppv_min"], ppv)
            st["ppv_max"] = max(st["ppv_max"], ppv)
            st["pages"].add(page_id)
            st["ppv_bucket"][bucket_label(ppv)] += 1
            st["record_max_bucket"][bucket_label(record_max_cnt)] += 1
            st["distinct_slot_bucket"][distinct_slot_num] += 1

            page_last_ppv[page_id] = max(page_last_ppv.get(page_id, 0), ppv)
            page_last_max_cnt[page_id] = max(page_last_max_cnt.get(page_id, 0), record_max_cnt)
            page_decision_seq[page_id].append(branch)

            # 更新 (page, slot) 对的 max CMS
            for sid, cnt in slot_counts:
                key = (page_id, sid)
                if cnt > pair_max_cms[key]:
                    pair_max_cms[key] = cnt

            if branch in BRANCH_PROMOTE_PARTIAL:
                page_is_promoted[page_id] = True
            elif branch in BRANCH_PROMOTE_ENTIRE:
                page_is_promoted[page_id] = True
                page_is_entire_promoted[page_id] = True


# ------------------------------------------------------------------
# 2. 打印关键配置 / 基础统计
# ------------------------------------------------------------------
print("=" * 80)
print("CSV 文件:", CSV_FILE)
print("=" * 80)

print("\n--- Workload 与 RC 容量 ---")
print(f"zipf_theta        = {ZIPF_THETA}")
print(f"total_records(N)  = {TOTAL_RECORDS}")
print(f"records_per_page  = {RECORDS_PER_PG}")
print(f"total_pages       = {TOTAL_PAGES}")
print(f"RC 容量           = {RC_GIB} GiB / {RC_ENTRY_BYTES} B = {RC_CAPACITY} entries "
      f"({RC_CAPACITY/TOTAL_RECORDS*100:.2f}% 的全部 record)")

# 注意：有些 page 只出现了 PAGE 行（AddCandidate 后就被 epoch timeout 清掉了，一次 CheckAndPromote 都没轮到）
# 也有一些 page 只出现在 Slot 行（PAGE 行可能因缓冲不同步丢失），取并集更稳妥
pages_in_page_line = set(page_cms_at_admit.keys())
pages_in_slot_line = set(page_last_ppv.keys())
pages_all          = pages_in_page_line | pages_in_slot_line

print(f"\n[基础] PAGE 行数 = {page_rows_total}, Slot 行数 = {slot_total_rows}")
print(f"[基础] 仅 PAGE 行见过的 page 数 = {len(pages_in_page_line)}"
      f" (= AddCandidate 次数，扣除重复)")
print(f"[基础] Slot 行见过的 distinct page 数 = {len(pages_in_slot_line)}"
      f" (= 至少经历过 1 次 CheckAndPromote)")
print(f"[基础] 两者并集 distinct page 数     = {len(pages_all)}")
print(f"[基础] 注：整个实验集共 {TOTAL_PAGES} 个 page，"
      f"candidate 覆盖率 = {len(pages_all)/TOTAL_PAGES*100:.2f}%")

# ------------------------------------------------------------------
# 2.1 fine_threshold 轨迹
# ------------------------------------------------------------------
print("\n--- fine_threshold 轨迹（epoch tick） ---")
if fine_threshold_events:
    print(f"{'req':>12}{'fine_th':>10}{'promotions':>14}{'cleared_candidates':>22}")
    for ev in fine_threshold_events:
        print(f"{ev['req']:>12}{ev['threshold']:>10}{ev['promotions']:>14}{ev['cleared']:>22}")
else:
    print("（未捕获 admission_epoch_tick 行）")

# ------------------------------------------------------------------
# 2.2 Branch 分布总览
# ------------------------------------------------------------------
print("\n--- Branch 分布（Slot 行判决结果） ---")
print(f"{'branch':<32}{'rows':>10}{'distinct_pages':>16}{'avg_ppv':>10}{'ppv_min':>10}{'ppv_max':>10}")
for branch, st in sorted(branch_stats.items(), key=lambda x: -x[1]["slot_rows"]):
    avg_ppv = st["ppv_sum"] / max(st["slot_rows"], 1)
    ppv_min = st["ppv_min"] if st["ppv_min"] < 10**18 else 0
    print(f"{branch:<32}{st['slot_rows']:>10}{len(st['pages']):>16}"
          f"{avg_ppv:>10.2f}{ppv_min:>10}{st['ppv_max']:>10}")


# ------------------------------------------------------------------
# 3. 各 branch 的 per_page_visits 分布
# ------------------------------------------------------------------
ORDER = ["0", "1", "2-3", "4-7", "8-15", "16-31", "32-63", "64-127",
         "128-255", "256-511", "512-1023", "1024-4095", "4096-16383", ">=16384"]

print("\n--- 每个 branch 的 per_page_visits 分布（Slot 行） ---")
header = "bucket".ljust(12) + "".join(b.ljust(18) for b in sorted(branch_stats.keys()))
print(header)
for bk in ORDER:
    row = bk.ljust(12)
    for br in sorted(branch_stats.keys()):
        row += str(branch_stats[br]["ppv_bucket"].get(bk, 0)).ljust(18)
    print(row)


# ------------------------------------------------------------------
# 4. 各 branch 的 max-slot CMS 分布（单槽 record 访问次数的上限）
# ------------------------------------------------------------------
print("\n--- 每个 branch 的 max-slot CMS 分布（单槽最大访问次数，反映 record 粒度） ---")
print(header)
for bk in ORDER:
    row = bk.ljust(12)
    for br in sorted(branch_stats.keys()):
        row += str(branch_stats[br]["record_max_bucket"].get(bk, 0)).ljust(18)
    print(row)


# ------------------------------------------------------------------
# 5. 各 branch 的 distinct-accessed-slot 数量分布
# ------------------------------------------------------------------
print("\n--- 每个 branch 的 distinct 被访问 slot 数分布 ---")
print(header)
max_d = 0
for br, st in branch_stats.items():
    if st["distinct_slot_bucket"]:
        max_d = max(max_d, max(st["distinct_slot_bucket"].keys()))
bins = [1, 2, 3, 4, 5, 6, 8, 12, 20, 40, max_d + 1]
def bucket_d(v):
    for b in bins:
        if v <= b:
            return f"<={b}"
    return f">{max_d}"

d_buckets_agg = defaultdict(lambda: Counter())
for br, st in branch_stats.items():
    for k, c in st["distinct_slot_bucket"].items():
        d_buckets_agg[br][bucket_d(k)] += c

d_order = [f"<={b}" for b in bins]
print("bucket".ljust(12) + "".join(b.ljust(18) for b in sorted(branch_stats.keys())))
for bk in d_order:
    row = bk.ljust(12)
    for br in sorted(branch_stats.keys()):
        row += str(d_buckets_agg[br].get(bk, 0)).ljust(18)
    print(row)


# ------------------------------------------------------------------
# 6. page-level 汇总：多少 candidate 真正被 promote？
# ------------------------------------------------------------------
page_seen_in_decision = pages_in_slot_line
page_promoted         = {p for p, v in page_is_promoted.items() if v}
page_entire_promoted  = {p for p, v in page_is_entire_promoted.items() if v}

print("\n--- Page 级 promote 情况 ---")
total_cand = len(page_seen_in_decision)
n_prom_all = len(page_promoted)
n_prom_ent = len(page_entire_promoted)
n_prom_par = n_prom_all - n_prom_ent
n_not      = total_cand - n_prom_all

def pct(n, d):
    return f"{n/d*100:.2f}%" if d else "-"

print(f"进入过 candidate pool 的 page 数（有 Slot 行）       = {total_cand}")
print(f"有过任意 promote 判决（C0/C3_NEW/C1/C2）的 page 数  = {n_prom_all}  ({pct(n_prom_all, total_cand)})")
print(f"    其中整页 promote（C1/C2）                         = {n_prom_ent}  ({pct(n_prom_ent, total_cand)})")
print(f"    仅部分 slot promote（C0/C3_NEW）                  = {n_prom_par}  ({pct(n_prom_par, total_cand)})")
print(f"从未被 promote 的 candidate page 数                  = {n_not}  ({pct(n_not, total_cand)})")

# ------------------------------------------------------------------
# 6.1 PAGE 行：进入 candidate 时的 page_cms_visit_count 分布
#      （= 进入 pool 前已被 global PageCMS 观察到的访问次数）
# ------------------------------------------------------------------
print("\n--- AddCandidate 时的 page_cms_visit_count 分布（PAGE 行） ---")
pcms_bucket = Counter()
for p, cms in page_cms_at_admit.items():
    pcms_bucket[bucket_label(cms)] += 1
ORDER = ["0", "1", "2-3", "4-7", "8-15", "16-31", "32-63", "64-127",
         "128-255", "256-511", "512-1023", "1024-4095", "4096-16383", ">=16384"]
print(f"{'bucket':<14}{'pages':>10}{'cumulative':>14}")
cum = 0
total_pcms = sum(pcms_bucket.values())
for bk in ORDER:
    c = pcms_bucket.get(bk, 0)
    if c == 0:
        continue
    cum += c
    print(f"{bk:<14}{c:>10}{cum:>14}  ({cum/total_pcms*100:.2f}%)")

# ------------------------------------------------------------------
# 7. 各 branch 的 per_page_visits 分布
# ------------------------------------------------------------------
br_keys_sorted = sorted(branch_stats.keys())
col_w = 20
print("\n--- 每个 branch 的 per_page_visits 分布（Slot 行） ---")
header = "bucket".ljust(12) + "".join(b.ljust(col_w) for b in br_keys_sorted)
print(header)
for bk in ORDER:
    row = bk.ljust(12)
    any_v = False
    for br in br_keys_sorted:
        v = branch_stats[br]["ppv_bucket"].get(bk, 0)
        if v: any_v = True
        row += str(v).ljust(col_w)
    if any_v:
        print(row)

# ------------------------------------------------------------------
# 8. 各 branch 的 max-slot-CMS 分布（record 粒度热度）
# ------------------------------------------------------------------
print("\n--- 每个 branch 的 max-slot-CMS 分布（单槽最大访问次数） ---")
print(header)
for bk in ORDER:
    row = bk.ljust(12)
    any_v = False
    for br in br_keys_sorted:
        v = branch_stats[br]["record_max_bucket"].get(bk, 0)
        if v: any_v = True
        row += str(v).ljust(col_w)
    if any_v:
        print(row)

# ------------------------------------------------------------------
# 9. 各 branch 的 distinct 被访问 slot 数分布
# ------------------------------------------------------------------
print("\n--- 每个 branch 的 distinct 被访问 slot 数分布 ---")
D_BINS = [1, 2, 3, 4, 6, 8, 12, 20, 40, 80, 10**9]
D_LABELS = ["1", "2", "3", "4", "5-6", "7-8", "9-12", "13-20", "21-40", "41-80", ">80"]
def bucket_d(v):
    for i, b in enumerate(D_BINS):
        if v <= b:
            return D_LABELS[i]
    return D_LABELS[-1]

print("bucket".ljust(12) + "".join(b.ljust(col_w) for b in br_keys_sorted))
for lab in D_LABELS:
    row = lab.ljust(12)
    any_v = False
    for br in br_keys_sorted:
        v = sum(c for k, c in branch_stats[br]["distinct_slot_bucket"].items() if bucket_d(k) == lab)
        if v: any_v = True
        row += str(v).ljust(col_w)
    if any_v:
        print(row)

# ------------------------------------------------------------------
# 10. page 级 per_page_visits vs 是否被 promote 的交叉表
# ------------------------------------------------------------------
print("\n--- Page 最大 per_page_visits vs 是否被 promote ---")
ppv_prom = Counter(); ppv_not = Counter()
for p, ppv in page_last_ppv.items():
    (ppv_prom if p in page_promoted else ppv_not)[bucket_label(ppv)] += 1
print(f"{'ppv_bucket':<14}{'promoted':>12}{'not_promoted':>16}{'total':>10}{'promote_rate':>15}")
for bk in ORDER:
    a = ppv_prom.get(bk, 0); b = ppv_not.get(bk, 0); t = a + b
    if t:
        print(f"{bk:<14}{a:>12}{b:>16}{t:>10}{pct(a, t):>15}")

# ------------------------------------------------------------------
# 11. page 级 max-slot-CMS vs 是否被 promote
# ------------------------------------------------------------------
print("\n--- Page 最大单槽 CMS vs 是否被 promote ---")
cms_prom = Counter(); cms_not = Counter()
for p, mc in page_last_max_cnt.items():
    (cms_prom if p in page_promoted else cms_not)[bucket_label(mc)] += 1
print(f"{'cms_bucket':<14}{'promoted':>12}{'not_promoted':>16}{'total':>10}{'promote_rate':>15}")
for bk in ORDER:
    a = cms_prom.get(bk, 0); b = cms_not.get(bk, 0); t = a + b
    if t:
        print(f"{bk:<14}{a:>12}{b:>16}{t:>10}{pct(a, t):>15}")

# ------------------------------------------------------------------
# 12. 未 promote 的 page：ppv vs max-slot-CMS 联合分布
# ------------------------------------------------------------------
print("\n--- 未被 promote 的 candidate page：ppv × max-slot-CMS 联合分布 ---")
joint = Counter()
for p in page_seen_in_decision - page_promoted:
    joint[(bucket_label(page_last_ppv.get(p, 0)),
           bucket_label(page_last_max_cnt.get(p, 0)))] += 1

cms_labels = [b for b in ORDER if any(k[1] == b for k in joint)]
ppv_labels = [b for b in ORDER if any(k[0] == b for k in joint)]
if joint:
    print("行 = ppv_bucket, 列 = max_slot_cms_bucket")
    hdr = "ppv\\cms".ljust(12) + "".join(c.ljust(12) for c in cms_labels) + "Row".rjust(10)
    print(hdr)
    for p in ppv_labels:
        row = p.ljust(12); rs = 0
        for c in cms_labels:
            v = joint.get((p, c), 0); rs += v
            row += str(v).ljust(12)
        row += str(rs).rjust(10)
        print(row)

# ------------------------------------------------------------------
# 13. 每轮判决机会分布（归因）
# ------------------------------------------------------------------
total_slot_rows = sum(st["slot_rows"] for st in branch_stats.values())
c0  = branch_stats.get("C0_STRONG_SKEW_L1", {}).get("slot_rows", 0)
c3n = branch_stats.get("C3_SKEW_NEW_SLOT_PROMOTE", {}).get("slot_rows", 0)
c3d = branch_stats.get("C3_SKEW_DUP_ONLY", {}).get("slot_rows", 0)
c12 = sum(branch_stats.get(b, {}).get("slot_rows", 0)
          for b in ("C1_UNIFORM_BY_PAGE_VISITS", "C2_UNIFORM_BY_ACCESSED_SLOTS"))
nd  = branch_stats.get("NO_DECISION", {}).get("slot_rows", 0)

print("\n--- 每轮判决的机会分布（归因） ---")
print(f"NO_DECISION（没达到任何阈值）         = {nd:>10}  ({pct(nd, total_slot_rows)})")
print(f"C0 + C3_NEW（真正产生新 promote）      = {c0+c3n:>10}  ({pct(c0+c3n, total_slot_rows)})")
print(f"C3_DUP_ONLY（skew 信号但无新可升槽）   = {c3d:>10}  ({pct(c3d, total_slot_rows)})")
print(f"C1/C2 整页 uniform promote             = {c12:>10}  ({pct(c12, total_slot_rows)})")

# ------------------------------------------------------------------
# 14. 基于 (page, slot) 对的 CMS 实测分布 —— 哪些槽能被 C0 promote
# ------------------------------------------------------------------
print("\n--- 实测 (page, slot) 对的 max-CMS 分布（来自 Slot 行累加） ---")
print(f"distinct (page, slot) 对数 = {len(pair_max_cms)}")
print(f"{'threshold':<12}{'pairs>=th':>14}{'占 RC 容量':>14}")
thresholds = sorted(set([2, 3, 4, 5, 8, 10, 15, 20, 25, 27,
                         *(ev["threshold"] for ev in fine_threshold_events)]))
for th in thresholds:
    n = sum(1 for v in pair_max_cms.values() if v >= th)
    print(f">={th:<10}{n:>14}{pct(n, RC_CAPACITY):>14}")

# ------------------------------------------------------------------
# 15. 理论 RecordCache 填充率上限（Zipf 分布精确计算）
# ------------------------------------------------------------------
# Zipf(theta) 下 rank-k 记录的访问概率 ∝ 1/k^theta，
# 理论 HR 上限 = H(RC_CAPACITY, theta) / H(N, theta)
# 理论"可填充 entries 数"上限 = min(N, RC_CAPACITY) 本身（容量是上界）
# 但本质问题：promote 机制能否识别出 top-K 记录？
# 下面给出若干阈值下，哪些 rank 的 record 在 M 次查询后期望计数 >= th。

print("\n--- 基于 Zipf(theta=%.2f) 的理论 RC 上限 ---" % ZIPF_THETA)

# 生成 harmonic 前缀和（只算一次，最多算到 N）
# 用 float64 足够：N=7.34M, theta=0.9，H(N) ~= sum(1/i^0.9)
# 为省时间，按对数采样构造累积分布查找表；这里 N 不大，直接线性即可
import math
def harmonic_prefix(N: int, theta: float):
    """返回 prefix[k] = sum_{i=1..k} 1/i^theta, 长度 N+1"""
    prefix = [0.0] * (N + 1)
    s = 0.0
    for i in range(1, N + 1):
        s += 1.0 / (i ** theta)
        prefix[i] = s
    return prefix

# N 很大（7.34M），prefix 数组会占约 60MB，可接受
print(f"[info] 正在计算 harmonic prefix(N={TOTAL_RECORDS}, theta={ZIPF_THETA})...", flush=True)
H = harmonic_prefix(TOTAL_RECORDS, ZIPF_THETA)
H_total = H[TOTAL_RECORDS]

# 15.1  RC 容量对应的 HR 上限
K = min(RC_CAPACITY, TOTAL_RECORDS)
rc_hr_upper = H[K] / H_total
print(f"[理论 RC 容量] 能容纳 top-{K} 条 record ({K/TOTAL_RECORDS*100:.2f}% 的全部 record)")
print(f"[理论上限]     若刚好装入 top-{K}，可吸收访问流量占比 = {rc_hr_upper*100:.4f}%")
print(f"               （与日志 header 打印的 theoretical_RC_HR_upper 应一致）")

# 15.2  实际 warmup+run 请求数下，rank-k 的期望访问次数 = M * (1/k^theta) / H_total
#       反解：给定阈值 th，哪个 rank 的 record 期望计数 >= th ?
#       k <= (M / (th * H_total))^(1/theta)

# 从日志抽取实际观察到的总 page 访问数（req 列最大值 + 可能的 warmup）
# 我们这里用 warmup=2M + run 运行到的 req 量
# 注意：Slot 行只在 run 阶段（warmup 可能也产生）。我们用 fine_threshold_events 的 req 最大值作下界
observed_M_lower = max((ev["req"] for ev in fine_threshold_events), default=0)
print(f"\n[观察] 日志中 admission_epoch_tick 最大 req = {observed_M_lower}  "
      f"(下界，实际总请求数可能更大)")

# 每个 record 期望被访问次数：M * p_k
# 对 rank=k, p_k = 1/k^theta / H_total
# 期望次数 >= th  ==>  k <= (M / (th * H_total))^(1/theta)
def max_rank_for_threshold(M: int, th: float, theta: float, H_total: float) -> int:
    if M <= 0 or th <= 0 or H_total <= 0:
        return 0
    rhs = M / (th * H_total)
    if rhs <= 0:
        return 0
    return max(0, min(TOTAL_RECORDS, int(rhs ** (1.0 / theta))))

# 15.3  当 fine_threshold=27 / 25 时，多少 record 在整个 run 里期望被观察到该次数？
print("\n[理论] 给定 total_requests M，rank <= K 的 record 期望被访问 >= fine_threshold")
print(f"{'M':>12}{'fine_th':>10}{'max_rank_K':>14}{'K/RC容量':>12}{'覆盖流量':>12}")
for M in (2_000_000, 2_000_000 + observed_M_lower, 2_000_000 + 5_000_000):
    for th in sorted(set([27, 25, 10, 5, 3, 2])):
        K_th = max_rank_for_threshold(M, th, ZIPF_THETA, H_total)
        cov = H[K_th] / H_total if K_th > 0 else 0.0
        print(f"{M:>12}{th:>10}{K_th:>14}"
              f"{pct(K_th, RC_CAPACITY):>12}{cov*100:>11.2f}%")

print("\n解读：")
print("  * RC 容量能容下 %d 条 record (= top-%.1f%%)，理论 HR 上限 %.2f%%；" %
      (RC_CAPACITY, RC_CAPACITY/TOTAL_RECORDS*100, rc_hr_upper*100))
print("  * 但在有限查询预算 M 下，能被 C0 (fine_threshold=27) 识别出的 record 数量"
      "等于 'rank≤K_th'，K_th 在上表中；")
print("  * 若 K_th 远小于 RC 容量，则真正的 promote 能力瓶颈是 *观察窗口不够长* 而非 *RC 容量不足*。")
