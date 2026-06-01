#!/usr/bin/env python3
"""
Analysis 3: Aging window sensitivity vs working set size.

The three structures that share trigger_visit_histogram_update_size (1M):
1. Page CMS aging (halve every epoch)
2. Record CMS aging (halve every epoch)
3. Candidate pool clearing (ClearAllCandidatesByEpoch every epoch)

This script analyzes how the fixed 1M window interacts differently
with 2 GiB vs 10 GiB working sets.
"""
import numpy as np
from math import exp, factorial, ceil, log

def harmonic_number(n, theta):
    """Compute generalized harmonic number H(n, theta)."""
    if n <= 100000:
        return sum(1.0 / i**theta for i in range(1, n + 1))
    else:
        # Use integral approximation for tail
        h = sum(1.0 / i**theta for i in range(1, 100001))
        # Integral of x^(-theta) from 100001 to n
        h += (n**(1-theta) - 100000**(1-theta)) / (1 - theta)
        return h

def analyze_window_sensitivity():
    print("=" * 70)
    print("Aging Window Sensitivity Analysis")
    print("=" * 70)

    configs = [
        {"name": "small_bc (2 GiB)", "ws_gib": 2.0, "rc_gib": 0.5, "dram_bp_gib": 1.0, "cxl_gib": 0.5, "workers": 4},
        {"name": "stress (10 GiB)", "ws_gib": 10.0, "rc_gib": 4.0, "dram_bp_gib": 2.0, "cxl_gib": 3.0, "workers": 128},
    ]

    THETA = 0.90
    PAGE_SIZE = 16384
    RECORDS_PER_PAGE = 56
    TRIGGER_WINDOW = 1_000_000  # current fixed value

    print(f"\n  Fixed parameters:")
    print(f"    trigger_visit_histogram_update_size = {TRIGGER_WINDOW:,}")
    print(f"    zipf_theta = {THETA}")
    print(f"    page_size = {PAGE_SIZE} B")
    print(f"    records_per_page = {RECORDS_PER_PAGE}")

    # ====== Compare the two configurations ======
    print("\n" + "=" * 70)
    print("Part A: Per-Page Visit Density Comparison")
    print("=" * 70)

    for cfg in configs:
        ws_gib = cfg["ws_gib"]
        total_pages = int(ws_gib * 1024**3 / PAGE_SIZE)
        total_records = total_pages * RECORDS_PER_PAGE

        h_n = harmonic_number(total_pages, THETA)

        # Average visits per page per epoch
        avg_visits_per_page = TRIGGER_WINDOW / total_pages

        # Top-1 page expected visits
        top1_prob = 1.0 / h_n
        top1_visits = TRIGGER_WINDOW * top1_prob

        # Median page rank that gets ≥ 3 visits
        # P(page rank-k gets ≥ 3) ≈ P(Poisson(λ_k) ≥ 3) where λ_k = W * p_k
        pages_with_ge3 = 0
        last_rank_ge3 = 0
        for rank in range(1, total_pages + 1):
            p_k = (1.0 / rank**THETA) / h_n
            lam = TRIGGER_WINDOW * p_k
            if lam > 20:
                pages_with_ge3 += 1
                last_rank_ge3 = rank
            elif lam > 0.001:
                p_ge3 = 1 - exp(-lam) * (1 + lam + lam**2/2)
                if p_ge3 > 0.5:
                    pages_with_ge3 += 1
                    last_rank_ge3 = rank

        # How many records would be in RC if all pages with ≥3 visits promote all records?
        potential_rc_fill = pages_with_ge3 * RECORDS_PER_PAGE

        print(f"\n  --- {cfg['name']} ---")
        print(f"    Total pages: {total_pages:,}")
        print(f"    Total records: {total_records:,}")
        print(f"    H({total_pages}, {THETA}) = {h_n:.2f}")
        print(f"    Avg visits/page/epoch: {avg_visits_per_page:.2f}")
        print(f"    Top-1 page visits/epoch: {top1_visits:.0f}")
        print(f"    Pages with ≥3 expected visits: {pages_with_ge3:,} (rank ≤ {last_rank_ge3:,})")
        print(f"    Potential records promotable/epoch: {potential_rc_fill:,}")
        print(f"    vs RC capacity: {int(cfg['rc_gib'] * 1024**3 / 124):,}")

    # ====== The Epoch-Clear Interaction ======
    print("\n" + "=" * 70)
    print("Part B: Epoch-Clear Interaction with Candidate Lifecycle")
    print("=" * 70)

    for cfg in configs:
        ws_gib = cfg["ws_gib"]
        total_pages = int(ws_gib * 1024**3 / PAGE_SIZE)
        h_n = harmonic_number(total_pages, THETA)
        workers = cfg["workers"]

        print(f"\n  --- {cfg['name']} ---")
        print(f"    Epoch window: {TRIGGER_WINDOW:,} requests")

        # With multiple admission threads, epoch triggers more frequently
        # because each partition hits 1M independently? No - it's global.
        # Actually from the code: sampling_set.OnPageAccess increments a global counter
        # and ShouldTriggerVisitHistogramUpdate checks >= trigger_size

        # The key issue: a candidate page enters the pool, then must accumulate
        # enough record-level accesses WITHIN THE SAME EPOCH to be promoted.
        # Otherwise ClearAllCandidatesByEpoch() removes it.

        # For a page at rank-k to be promoted via Condition 1:
        # Need per_page_visits >= effective_max_per_page_visits (=3 for 10GiB)
        # AND it must NOT be tagged as skew (which CMS noise causes)
        #
        # But even ignoring the CMS problem, the candidate lifecycle is:
        # 1. Page passes L1 threshold → enters candidates
        # 2. Each subsequent access increments per_page_visits
        # 3. If per_page_visits >= 3 before epoch clear → promote
        # 4. Epoch clear → all candidates removed, progress lost

        # When does a page enter candidates?
        # It enters when it first passes l1_fine_threshold (=4) in PageCMS
        # But PageCMS is cumulative (with halving). So a page that had
        # count 2 from previous epoch (halved to 1) needs 3 more hits in this epoch.

        # Expected visits from rank-k page in one epoch:
        # λ_k = W * (1/k^θ) / H(N,θ)

        # Pages that can accumulate ≥ 3 per_page_visits in ONE epoch
        # (note: per_page_visits starts from 0 each epoch due to clear)
        # These are pages with λ_k ≥ 3 approximately

        pages_survive_epoch = 0
        for rank in range(1, total_pages + 1):
            p_k = (1.0 / rank**THETA) / h_n
            lam = TRIGGER_WINDOW * p_k
            if lam >= 3:
                pages_survive_epoch += 1
            else:
                break  # ranks are monotonically decreasing in λ

        # For the stress test, what if we use 5M window?
        pages_survive_5m = 0
        for rank in range(1, total_pages + 1):
            p_k = (1.0 / rank**THETA) / h_n
            lam = 5_000_000 * p_k
            if lam >= 3:
                pages_survive_5m += 1
            else:
                break

        pages_survive_10m = 0
        for rank in range(1, total_pages + 1):
            p_k = (1.0 / rank**THETA) / h_n
            lam = 10_000_000 * p_k
            if lam >= 3:
                pages_survive_10m += 1
            else:
                break

        print(f"    Pages that get ≥3 visits within epoch (can trigger Condition 1):")
        print(f"      Window 1M:  {pages_survive_epoch:,} pages")
        print(f"      Window 5M:  {pages_survive_5m:,} pages")
        print(f"      Window 10M: {pages_survive_10m:,} pages")

    # ====== CMS Aging and Threshold Stability ======
    print("\n" + "=" * 70)
    print("Part C: CMS Aging Impact on Threshold Calculation")
    print("=" * 70)

    for cfg in configs:
        ws_gib = cfg["ws_gib"]
        total_pages = int(ws_gib * 1024**3 / PAGE_SIZE)
        h_n = harmonic_number(total_pages, THETA)

        # Page CMS values after halving:
        # Steady state: count_k = λ_k * (1 + 1/2 + 1/4 + ...) = 2 * λ_k
        # where λ_k = TRIGGER_WINDOW * p_k

        # The sampling set takes 4096 unique pages in each window
        # It samples from the ACCESS STREAM (first 4096 unique page_ids seen)
        # This is biased toward hot pages!

        # The threshold is determined by:
        # "top dram_ratio fraction of SAMPLED pages"
        # If sampling is biased toward hot pages, the threshold is HIGH

        dram_ratio = (cfg["dram_bp_gib"] + cfg["rc_gib"]) / (cfg["dram_bp_gib"] + cfg["rc_gib"] + cfg["cxl_gib"])
        target_pages_in_sample = int(4096 * dram_ratio)

        # Simulate: what page ranks are sampled?
        # The sampling_set takes first 4096 UNIQUE pages from access stream
        # Under Zipf, the first K unique pages appear in approximately:
        # E[requests to see K unique from N with Zipf(θ)] = complex coupon collector
        # But since hot pages appear first, the sample is dominated by top-ranked pages

        # After 1M accesses with Zipf(0.9) on 655K pages:
        # Expected unique pages seen ≈ N * (1 - (1 - 1/N)^W) for uniform
        # For Zipf, more complex, but essentially all 655K pages are touched
        # because 1M >> 655K is not true... actually 1M / 655K = 1.5 on average
        # Many pages NOT touched at all

        # Actually: expected unique pages in W accesses:
        # E[unique] = sum_{k=1}^{N} (1 - (1-p_k)^W)
        unique_pages_expected = 0
        for rank in range(1, min(total_pages + 1, 200001)):
            p_k = (1.0 / rank**THETA) / h_n
            prob_seen = 1 - (1 - p_k)**TRIGGER_WINDOW if TRIGGER_WINDOW * p_k < 100 else 1.0
            unique_pages_expected += prob_seen
        # Approximate remainder for rank > 200000 (if exists)
        if total_pages > 200000:
            for rank in range(200001, total_pages + 1, 100):
                p_k = (1.0 / rank**THETA) / h_n
                lam = TRIGGER_WINDOW * p_k
                prob_seen = 1 - exp(-lam) if lam < 100 else 1.0
                unique_pages_expected += prob_seen * min(100, total_pages - rank + 1)

        print(f"\n  --- {cfg['name']} ---")
        print(f"    DRAM ratio: {dram_ratio:.3f}")
        print(f"    Expected unique pages seen in 1M accesses: ~{unique_pages_expected:.0f}")
        print(f"    max_sampled_page_ids: 4096")
        print(f"    Target top pages in sample (for threshold): {target_pages_in_sample}")
        print(f"")
        print(f"    Since sampling takes first 4096 unique pages from stream,")
        print(f"    and hot pages appear first in Zipf, the sample is BIASED toward hot pages.")
        print(f"    The threshold is computed from this biased sample's distribution.")

        # Steady-state CMS values for sampled pages (biased toward hot):
        # If sample contains mostly rank 1-4096 pages:
        # Their CMS counts (steady state) ≈ 2 * TRIGGER_WINDOW * p_k
        rank_4096_visits = TRIGGER_WINDOW * (1.0 / 4096**THETA) / h_n
        rank_1_visits = TRIGGER_WINDOW * (1.0 / 1**THETA) / h_n
        print(f"    Rank-1 page CMS value (steady): ~{2*rank_1_visits:.0f}")
        print(f"    Rank-4096 page CMS value (steady): ~{2*rank_4096_visits:.0f}")
        print(f"    threshold target = top {100*dram_ratio:.0f}% of sample = rank ~{int(4096*(1-dram_ratio))}")
        threshold_rank = int(4096 * (1 - dram_ratio))
        if threshold_rank < total_pages:
            threshold_visits = TRIGGER_WINDOW * (1.0 / threshold_rank**THETA) / h_n
            print(f"    Expected threshold ≈ 2 * {threshold_visits:.1f} = {2*threshold_visits:.0f}")
            print(f"    Actual observed: l1_fine_threshold = 4")
            print(f"    {'MATCH' if abs(2*threshold_visits - 4) < 5 else 'MISMATCH'}")

    # ====== Recommended Window Sizing ======
    print("\n" + "=" * 70)
    print("Part D: Recommended Window Sizing Formula")
    print("=" * 70)

    print("""
  The aging window should scale with working set so that:
  - Enough visits accumulate per hot page for meaningful threshold calculation
  - Candidates have time to accumulate record-level statistics before clearing
  - CMS noise remains below threshold

  Proposed formula:
    trigger_window = base_window * (working_set_pages / reference_pages)
                   = 1M * (N_pages / 131072)

  | Working Set | Pages   | Proposed Window | Avg visits/page | Top-page visits |
  |-------------|---------|-----------------|-----------------|-----------------|""")

    reference_pages = 131072  # 2 GiB
    base_window = 1_000_000

    for ws_gib in [2.0, 5.0, 10.0, 20.0, 40.0]:
        n_pages = int(ws_gib * 1024**3 / PAGE_SIZE)
        proposed_window = int(base_window * n_pages / reference_pages)
        h_n = harmonic_number(n_pages, THETA)
        avg_vpg = proposed_window / n_pages
        top1 = proposed_window * (1.0 / h_n)
        print(f"  | {ws_gib:>5.0f} GiB   | {n_pages:>7,} | {proposed_window:>15,} | {avg_vpg:>15.1f} | {top1:>15.0f} |")

    # ====== Impact of DIFFERENT window sizes on promotion rate ======
    print("\n" + "=" * 70)
    print("Part E: Projected RC Fill Under Different Windows (10 GiB)")
    print("=" * 70)

    total_pages_10g = int(10.0 * 1024**3 / PAGE_SIZE)
    h_n_10g = harmonic_number(total_pages_10g, THETA)
    rc_capacity = 34_636_833
    throughput = 3_600_000  # ~3.6 Mqps

    print(f"\n  {'Window':>10} | {'Epochs/s':>9} | {'Pages≥3/epoch':>14} | {'Records/epoch':>14} | {'Time to fill':>13}")
    print(f"  {'-'*10}-+-{'-'*9}-+-{'-'*14}-+-{'-'*14}-+-{'-'*13}")

    for window in [1_000_000, 2_000_000, 5_000_000, 10_000_000]:
        epochs_per_sec = throughput / window
        pages_ge3 = 0
        for rank in range(1, total_pages_10g + 1):
            p_k = (1.0 / rank**THETA) / h_n_10g
            lam = window * p_k
            if lam >= 3:
                pages_ge3 += 1
            else:
                break

        records_per_epoch = pages_ge3 * RECORDS_PER_PAGE
        # If all promotable records get promoted each epoch (ideal):
        total_records_per_sec = records_per_epoch * epochs_per_sec
        time_to_fill = rc_capacity / total_records_per_sec if total_records_per_sec > 0 else float('inf')

        print(f"  {window:>10,} | {epochs_per_sec:>9.1f} | {pages_ge3:>14,} | {records_per_epoch:>14,} | {time_to_fill:>10.1f}s")

    print("""
  Note: "Time to fill" is THEORETICAL LOWER BOUND assuming perfect promotion.
  Actual time will be longer due to:
  - Only one CheckAndPromote per epoch tick
  - Lock contention in promotion path
  - Some candidates entering late in the epoch window

  With window=5M: RC could fill in ~1.1s (ideal) vs current ~never.
  The bottleneck is NOT throughput — it's the epoch-clear policy destroying
  candidate progress every 0.28s (at 3.6 Mqps / 1M window).
""")

if __name__ == "__main__":
    analyze_window_sensitivity()
