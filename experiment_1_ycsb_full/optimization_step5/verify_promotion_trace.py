#!/usr/bin/env python3
"""
Analyze promotion trace from stress_high_concurrency logs.
Determine whether hot records are being blocked at L1 (page) or L2 (record/skew detection).
"""
import re
import sys
from collections import Counter, defaultdict

RESULT_FILE = "stress_high_concurrency/result_ycsbc_w128_theta0.90_20260512_152248.csv"

def parse_promote_lines(filepath):
    """Parse [PROMOTE] lines."""
    promotes = []
    pattern = re.compile(
        r'\[PROMOTE\] round=(\d+) skew_records=(\d+) uniform_pages=(\d+) '
        r'decisions=(\d+) global_requests=(\d+) '
        r'l1_orig=(\d+) l1_dynamic=(\d+) rc_fill=([\d.]+)'
    )
    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                promotes.append({
                    'round': int(m.group(1)),
                    'skew_records': int(m.group(2)),
                    'uniform_pages': int(m.group(3)),
                    'decisions': int(m.group(4)),
                    'global_requests': int(m.group(5)),
                    'l1_orig': int(m.group(6)),
                    'l1_dynamic': int(m.group(7)),
                    'rc_fill': float(m.group(8)),
                })
    return promotes

def parse_epoch_ticks(filepath):
    """Parse [DEBUG] admission_epoch_tick lines."""
    epochs = []
    pattern = re.compile(
        r'admission_epoch_tick req=(\d+) l1_fine_threshold=(\d+) '
        r'promotions=(\d+) cleared_candidates=(\d+)'
    )
    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                epochs.append({
                    'req': int(m.group(1)),
                    'l1_fine_threshold': int(m.group(2)),
                    'promotions': int(m.group(3)),
                    'cleared_candidates': int(m.group(4)),
                })
    return epochs

def parse_warmup_lines(filepath):
    """Parse [Warmup] progress lines."""
    warmups = []
    pattern = re.compile(
        r'\[Warmup\].*?elapsed=([\d.]+)s.*?throughput=([\d.]+) Mqps.*?'
        r'threshold=(\d+) candidates_total=(\d+).*?'
        r'skew_promote=(\d+) uniform_promote=(\d+) timeout_removed=(\d+)'
    )
    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                warmups.append({
                    'elapsed': float(m.group(1)),
                    'throughput': float(m.group(2)),
                    'threshold': int(m.group(3)),
                    'candidates_total': int(m.group(4)),
                    'skew_promote': int(m.group(5)),
                    'uniform_promote': int(m.group(6)),
                    'timeout_removed': int(m.group(7)),
                })
    return warmups

def analyze():
    print("=" * 70)
    print("Promotion Trace Analysis: YCSB-C w128 theta=0.90 (10 GiB)")
    print("=" * 70)

    promotes = parse_promote_lines(RESULT_FILE)
    epochs = parse_epoch_ticks(RESULT_FILE)
    warmups = parse_warmup_lines(RESULT_FILE)

    print(f"\n  Total [PROMOTE] lines: {len(promotes):,}")
    print(f"  Total [epoch_tick] lines: {len(epochs):,}")
    print(f"  Total [Warmup] progress lines: {len(warmups):,}")

    # ====== Summary Statistics ======
    print("\n" + "=" * 70)
    print("Part A: Overall Promotion Statistics")
    print("=" * 70)

    total_skew = sum(p['skew_records'] for p in promotes)
    total_uniform = sum(p['uniform_pages'] for p in promotes)
    total_decisions = sum(p['decisions'] for p in promotes)

    print(f"\n  Total skew record promotes:   {total_skew:,}")
    print(f"  Total uniform page promotes:  {total_uniform:,}")
    print(f"  Total promotion decisions:    {total_decisions:,}")
    print(f"  Uniform / Total ratio:        {total_uniform}/{total_decisions} = {100*total_uniform/max(total_decisions,1):.2f}%")

    # RC fill progression
    if promotes:
        fill_at_start = promotes[0]['rc_fill']
        fill_at_end = promotes[-1]['rc_fill']
        print(f"\n  RC fill at start: {fill_at_start:.3f}")
        print(f"  RC fill at end:   {fill_at_end:.3f}")

    # ====== Epoch Analysis ======
    print("\n" + "=" * 70)
    print("Part B: Epoch Tick Analysis")
    print("=" * 70)

    if epochs:
        total_epoch_promotions = sum(e['promotions'] for e in epochs)
        total_epoch_cleared = sum(e['cleared_candidates'] for e in epochs)
        avg_promotions = total_epoch_promotions / len(epochs)
        avg_cleared = total_epoch_cleared / len(epochs)

        print(f"\n  Total epochs: {len(epochs)}")
        print(f"  Total promotions across epochs: {total_epoch_promotions:,}")
        print(f"  Total cleared candidates: {total_epoch_cleared:,}")
        print(f"  Avg promotions/epoch: {avg_promotions:.1f}")
        print(f"  Avg cleared/epoch: {avg_cleared:.1f}")
        print(f"  Promotion/cleared ratio: {total_epoch_promotions/max(total_epoch_cleared,1):.2%}")

        # L1 threshold distribution
        threshold_counts = Counter(e['l1_fine_threshold'] for e in epochs)
        print(f"\n  L1 fine threshold distribution:")
        for t, count in sorted(threshold_counts.items()):
            print(f"    threshold={t}: {count} epochs ({100*count/len(epochs):.1f}%)")

        # Epochs with 0 promotions
        zero_promo_epochs = sum(1 for e in epochs if e['promotions'] == 0)
        print(f"\n  Epochs with 0 promotions: {zero_promo_epochs}/{len(epochs)} ({100*zero_promo_epochs/len(epochs):.1f}%)")

    # ====== The Critical Question ======
    print("\n" + "=" * 70)
    print("Part C: Why is RC Fill Only 8%?")
    print("=" * 70)

    # Calculate: how many records SHOULD be in RC?
    # RC capacity = 34,636,833. Target = top Zipf records
    # With theta=0.9 and 655K pages * 56 records = 36.7M records
    # Top 34.6M / 36.7M = 94.4% of records → RC_HR_upper = 99.3%
    # But we only have 8.3% fill = 2,864,847 entries

    rc_capacity = 34_636_833
    final_entries = 2_864_847  # from RC State Final
    target_fill = rc_capacity

    print(f"\n  RC capacity: {rc_capacity:,}")
    print(f"  Final RC entries: {final_entries:,}")
    print(f"  Fill ratio: {100*final_entries/rc_capacity:.1f}%")
    print(f"  Deficit: {rc_capacity - final_entries:,} entries not filled")

    # How many records promoted per second?
    # Total runtime: warmup 41.76s + measure 1B/3.6Mqps ≈ 277s = ~319s total
    total_runtime_s = 41.76 + 1_000_000_000 / 3_608_394
    records_per_second = final_entries / total_runtime_s
    print(f"\n  Total runtime: ~{total_runtime_s:.0f}s")
    print(f"  Effective promotion rate: {records_per_second:.0f} records/s")
    print(f"  To fill RC: need {rc_capacity / total_runtime_s:.0f} records/s")
    print(f"  Actual vs needed: {100*records_per_second/(rc_capacity/total_runtime_s):.1f}%")

    # ====== Rate-limiting analysis ======
    print("\n" + "=" * 70)
    print("Part D: Bottleneck Identification")
    print("=" * 70)

    # Each epoch is 1M global_requests. With 128 workers, how often does epoch tick?
    # From epochs data, calculate interval between epochs
    if len(epochs) >= 2:
        epoch_intervals = [epochs[i+1]['req'] - epochs[i]['req'] for i in range(len(epochs)-1)]
        avg_epoch_interval = sum(epoch_intervals) / len(epoch_intervals)
        print(f"\n  Average requests between epoch ticks: {avg_epoch_interval:,.0f}")

        # With throughput 3.6 Mqps, how many epochs per second?
        throughput = 3_608_394  # from final summary
        epochs_per_second = throughput / avg_epoch_interval
        print(f"  Epochs per second (at 3.6 Mqps): {epochs_per_second:.1f}")

        # Each epoch: promotes ~avg_promotions records, then clears all candidates
        print(f"  Records promoted per epoch: {avg_promotions:.1f}")
        print(f"  Records promoted per second: {avg_promotions * epochs_per_second:.0f}")
        print(f"  Candidates cleared per epoch: {avg_cleared:.1f}")

        # KEY: cleared > promoted means we're throwing away candidates
        # that could have been promoted if given more time
        print(f"\n  *** CRITICAL: cleared_candidates ({avg_cleared:.0f}) > promotions ({avg_promotions:.0f})")
        print(f"  *** {100*(avg_cleared - avg_promotions)/max(avg_cleared,1):.1f}% of candidates are wasted (cleared before promotion)")

    # ====== Condition 0 vs Condition 1/2/3 ======
    print("\n" + "=" * 70)
    print("Part E: Why Condition 1 (Uniform Promote) Never Fires")
    print("=" * 70)

    print(f"\n  effective_max_per_page_visits (dynamic) = 3")
    print(f"  uniform_threshold_ratio = 0.50 (50% of 56 slots = 28 slots)")
    print(f"  skew_threshold_ratio = 0.10")
    print(f"")
    print(f"  For Condition 1 to fire: per_page_visits >= 3 AND !has_ratio_skew_slot")
    print(f"  For Condition 2 to fire: accessed_slots >= 28 AND !has_ratio_skew_slot")
    print(f"")
    print(f"  BUT: has_ratio_skew_slot is determined by RecordCMS counts.")
    print(f"  Since RecordCMS noise floor = 7.7 (from CMS analysis),")
    print(f"  and per_page_visits after 3 accesses is tiny,")
    print(f"  ppv_skew_threshold = ceil(3 * 0.10) = 1")
    print(f"  cms_total ≈ noise * accessed_slots ≈ 7.7 * accessed_slots")
    print(f"  cms_skew_threshold = ceil(cms_total * 0.10)")
    print(f"")
    print(f"  For a page with per_page_visits=3:")
    print(f"    - ppv_skew_threshold = ceil(3 * 0.10) = 1")
    print(f"    - ANY slot with CMS count > 1 triggers has_ratio_skew_slot")
    print(f"    - Since ALL slots have CMS noise ≥ 7.7, has_ratio_skew_slot is ALWAYS true")
    print(f"    - Therefore Condition 1 and 2 can NEVER fire")
    print(f"")
    print(f"  ==> Root cause: CMS noise makes skew detection trigger on ALL pages,")
    print(f"      blocking the uniform promotion path entirely.")
    print(f"")
    print(f"  Meanwhile, Condition 0 fires when slot_count ≥ l1_dynamic (=2),")
    print(f"  which is also always true due to noise. But promoted_bits prevent")
    print(f"  re-promoting the same slot. So promotion rate = new slots appearing")
    print(f"  in candidates each epoch, which is very slow due to epoch clearing.")

    # ====== What SHOULD happen ======
    print("\n" + "=" * 70)
    print("Part F: Expected Behavior With Correct CMS")
    print("=" * 70)

    # Under Zipf(0.9), page rank-1 gets ~0.3% of traffic
    # In 1M requests window: top page gets ~3000 visits
    # With 56 records per page, each record on top page gets ~54 visits
    # For a "truly uniform" hot page, all 56 slots get ~equal visits
    # This SHOULD trigger Condition 2 (accessed_slots ≥ 28)

    # But in reality, with epoch clearing every 1M requests:
    # A page needs to enter candidates, accumulate 28+ distinct slot accesses,
    # AND have CheckAndPromote run before the epoch clears it.

    # Expected visits to top page in 1M window:
    import numpy as np
    total_pages = 655360
    zipf_theta = 0.9
    # Harmonic number approximation
    h_total = sum(1.0/i**zipf_theta for i in range(1, min(total_pages+1, 100001)))
    if total_pages > 100000:
        # Approximate remainder
        h_total += (total_pages**(1-zipf_theta) - 100000**(1-zipf_theta)) / (1-zipf_theta)

    top_page_prob = (1.0 / 1**zipf_theta) / h_total
    page_100_prob = (1.0 / 100**zipf_theta) / h_total
    page_1000_prob = (1.0 / 1000**zipf_theta) / h_total
    page_10000_prob = (1.0 / 10000**zipf_theta) / h_total

    window = 1_000_000
    print(f"\n  Harmonic number H({total_pages}, {zipf_theta}) ≈ {h_total:.1f}")
    print(f"\n  Expected page visits in one epoch (1M requests):")
    print(f"    Page rank-1:     {window * top_page_prob:.1f} visits → {window * top_page_prob / 56:.1f} per record")
    print(f"    Page rank-100:   {window * page_100_prob:.1f} visits → {window * page_100_prob / 56:.1f} per record")
    print(f"    Page rank-1000:  {window * page_1000_prob:.1f} visits → {window * page_1000_prob / 56:.1f} per record")
    print(f"    Page rank-10000: {window * page_10000_prob:.1f} visits → {window * page_10000_prob / 56:.1f} per record")

    # How many pages get ≥ 3 visits (= effective_max_per_page_visits)?
    # P(page gets ≥ k visits in window) depends on its probability
    # For Poisson approx: P(X ≥ 3) where X ~ Poisson(window * p_i)
    from math import exp, factorial
    pages_with_3_visits = 0
    for rank in range(1, total_pages + 1):
        p_i = (1.0 / rank**zipf_theta) / h_total
        lam = window * p_i
        if lam > 20:  # definitely ≥ 3
            pages_with_3_visits += 1
        elif lam > 0.01:
            # P(X >= 3) = 1 - P(X=0) - P(X=1) - P(X=2)
            p_less_3 = exp(-lam) * (1 + lam + lam**2/2)
            pages_with_3_visits += (1 - p_less_3)

    print(f"\n  Pages with ≥ 3 expected visits per epoch: ~{pages_with_3_visits:.0f}")
    print(f"  These pages SHOULD be Condition-1 candidates (if no skew misdetection)")
    print(f"  That's {pages_with_3_visits * 56:.0f} records that could be promoted per epoch")
    print(f"  But we only promote ~{avg_promotions:.0f} per epoch due to CMS noise → skew misdetection")

    print("\n" + "=" * 70)
    print("CONCLUSION")
    print("=" * 70)
    print("""
  The Record CMS noise (mean=7.7, 96% of cold records ≥ threshold=4) causes:

  1. Condition 0 ALWAYS fires (any slot with CMS ≥ 4 → promote as "strong-skew")
     BUT: already_promoted_bits prevent re-promotion → only NEW candidates' slots
     get promoted, yielding ~300 records/epoch

  2. Condition 1/2 (uniform page promote) NEVER fires because:
     - ppv_skew_threshold = ceil(per_page_visits * 0.10) is tiny (=1 when visits=3)
     - ALL slots show CMS count >> 1 due to noise
     - Therefore has_ratio_skew_slot is ALWAYS true
     - Condition 1/2 require !has_ratio_skew_slot → blocked

  3. ClearAllCandidatesByEpoch() wipes all accumulated progress every 1M requests
     (≈0.28s at 3.6 Mqps), so candidates never accumulate enough visits

  Fix priorities:
  (A) Record CMS cols: 64K → 2M+ (noise must be < threshold)
  (B) Remove or lengthen epoch-timeout for candidates (5-10M, not 1M)
  (C) OR: don't use RecordCMS for Condition 0/skew detection when fill < 50%
      (during warmup, promote entire pages unconditionally if they pass L1)
""")

if __name__ == "__main__":
    analyze()
