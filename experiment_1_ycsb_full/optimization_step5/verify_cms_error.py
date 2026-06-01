#!/usr/bin/env python3
"""
Verify CMS structural error for 10 GiB dataset.
Simulates Record CMS (4 rows x 64K cols) and Page CMS (12 rows x 1M cols)
under Zipf(0.9) access to measure the noise floor vs true access counts.
"""
import numpy as np
from collections import Counter
import hashlib
import struct

# ====== Configuration matching stress_high_concurrency ======
TOTAL_PAGES = 655360          # 10 GiB / 16 KiB
RECORDS_PER_PAGE = 56
TOTAL_RECORDS = TOTAL_PAGES * RECORDS_PER_PAGE  # 36,700,160

# CMS dimensions from Config.cpp
PAGE_CMS_ROWS = 12
PAGE_CMS_COLS = 1024 * 1024   # 1M
RECORD_CMS_ROWS = 4
RECORD_CMS_COLS = 1024 * 64   # 64K

# Simulation parameters
ZIPF_THETA = 0.90
NUM_ACCESSES = 1_000_000      # one epoch window
SEED = 42

def generate_zipf_ranks(n_items, theta, n_samples, rng):
    """Generate Zipf-distributed samples (0-indexed ranks)."""
    weights = np.array([1.0 / (i ** theta) for i in range(1, n_items + 1)])
    weights /= weights.sum()
    # For large n_items, use inverse CDF sampling
    # but numpy's choice is fine for our sample count
    if n_items > 10_000_000:
        # Use approximate method for very large populations
        # Sample from truncated Zipf
        cdf = np.cumsum(weights)
        u = rng.random(n_samples)
        ranks = np.searchsorted(cdf, u)
        return ranks
    else:
        return rng.choice(n_items, size=n_samples, p=weights)

def murmur_hash(key_bytes, seed):
    """Simple hash mixing similar to the C++ code."""
    h = int.from_bytes(hashlib.md5(key_bytes + struct.pack('<Q', seed)).digest()[:8], 'little')
    h ^= (h >> 33)
    h = (h * 0xff51afd7e558ccd) & 0xFFFFFFFFFFFFFFFF
    h ^= (h >> 33)
    h = (h * 0xc4ceb9fe1a85ec53) & 0xFFFFFFFFFFFFFFFF
    h ^= (h >> 33)
    return h

class SimpleCMS:
    def __init__(self, rows, cols):
        self.rows = rows
        self.cols = cols
        self.matrix = np.zeros((rows, cols), dtype=np.int64)
        rng = np.random.default_rng(0x9e3779b9)
        self.seeds = rng.integers(0, 2**63, size=rows)

    def _hash(self, key, row):
        # Simplified hash that's still reasonable for collision analysis
        h = hash((key, int(self.seeds[row])))
        # Mix
        h = ((h ^ (h >> 33)) * 0xff51afd7e558ccd) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53) & 0xFFFFFFFFFFFFFFFF
        h ^= (h >> 33)
        return h % self.cols

    def update(self, key):
        for r in range(self.rows):
            col = self._hash(key, r)
            self.matrix[r, col] += 1

    def query(self, key):
        min_val = self.matrix[0, self._hash(key, 0)]
        for r in range(1, self.rows):
            val = self.matrix[r, self._hash(key, r)]
            if val < min_val:
                min_val = val
        return min_val

    def total_updates(self):
        return self.matrix[0].sum()

def analyze_cms_error():
    print("=" * 70)
    print("CMS Structural Error Analysis")
    print("=" * 70)
    print(f"\nConfiguration:")
    print(f"  Total pages:    {TOTAL_PAGES:,}")
    print(f"  Total records:  {TOTAL_RECORDS:,}")
    print(f"  Page CMS:       {PAGE_CMS_ROWS} rows x {PAGE_CMS_COLS:,} cols")
    print(f"  Record CMS:     {RECORD_CMS_ROWS} rows x {RECORD_CMS_COLS:,} cols")
    print(f"  Zipf theta:     {ZIPF_THETA}")
    print(f"  Accesses/epoch: {NUM_ACCESSES:,}")

    # ============================================================
    # Theoretical Analysis (no simulation needed for bounds)
    # ============================================================
    print("\n" + "=" * 70)
    print("Part A: Theoretical CMS Error Bounds")
    print("=" * 70)

    # For CMS with d rows and w columns, after N insertions of M distinct items:
    # Expected noise per bucket = N / w
    # P(overestimate > epsilon * N) <= (1/e)^d  where epsilon = e/w

    # Record CMS after one epoch (1M accesses):
    record_noise_per_bucket = NUM_ACCESSES / RECORD_CMS_COLS
    print(f"\n  Record CMS noise floor (1 epoch = {NUM_ACCESSES:,} accesses):")
    print(f"    Expected noise per bucket per row = {NUM_ACCESSES:,} / {RECORD_CMS_COLS:,} = {record_noise_per_bucket:.1f}")
    print(f"    With min-of-{RECORD_CMS_ROWS} rows, expected noise ≈ {record_noise_per_bucket:.1f}")
    print(f"    (min-of-d only helps probabilistically, not expected-value)")

    # After CMS aging (halving), the noise accumulates across epochs.
    # Steady-state noise with halving every epoch:
    # noise = N/w * (1 + 1/2 + 1/4 + ...) = N/w * 2 = 2 * N/w
    steady_noise = 2 * record_noise_per_bucket
    print(f"\n    Steady-state noise (halving each epoch) = 2 × {record_noise_per_bucket:.1f} = {steady_noise:.1f}")
    print(f"\n    l1_fine_threshold (stable) = 4")
    print(f"    ==> ANY record with CMS query ≥ 4 will trigger Condition 0")
    print(f"    ==> Noise floor ({steady_noise:.0f}) >> threshold (4)")
    print(f"    ==> CONCLUSION: Record CMS is USELESS for discrimination at this scale")

    # Page CMS analysis
    page_noise_per_bucket = NUM_ACCESSES / PAGE_CMS_COLS
    page_steady_noise = 2 * page_noise_per_bucket
    print(f"\n  Page CMS noise floor (1 epoch):")
    print(f"    Expected noise per bucket per row = {NUM_ACCESSES:,} / {PAGE_CMS_COLS:,} = {page_noise_per_bucket:.2f}")
    print(f"    Steady-state noise = {page_steady_noise:.2f}")
    print(f"    With min-of-{PAGE_CMS_ROWS} rows: effective noise ≈ {page_noise_per_bucket:.2f} (very low)")
    print(f"    ==> Page CMS is fine (noise ~1 vs threshold ~4)")

    # ============================================================
    # Simulation: verify with actual Zipf access pattern
    # ============================================================
    print("\n" + "=" * 70)
    print("Part B: Simulation with Zipf(0.9) Access Pattern")
    print("=" * 70)

    rng = np.random.default_rng(SEED)

    # For tractability, simulate record accesses within a subset
    # Key insight: with 36.7M records, even at 1M accesses per epoch,
    # most records have 0 TRUE accesses. Let's see how CMS reports them.

    # Generate page-level Zipf access
    print("\n  Generating 1M Zipf-distributed page accesses...")
    page_ranks = generate_zipf_ranks(TOTAL_PAGES, ZIPF_THETA, NUM_ACCESSES, rng)
    page_true_counts = Counter(page_ranks)

    # For each page access, pick a uniform random slot
    print("  Generating record-level accesses (uniform within page)...")
    slot_ids = rng.integers(0, RECORDS_PER_PAGE, size=NUM_ACCESSES)

    # Build record keys: (page_rank, slot_id)
    record_keys = list(zip(page_ranks.tolist(), slot_ids.tolist()))
    record_true_counts = Counter(record_keys)

    # Simulate Record CMS
    print(f"  Simulating Record CMS ({RECORD_CMS_ROWS}x{RECORD_CMS_COLS:,})...")
    record_cms = SimpleCMS(RECORD_CMS_ROWS, RECORD_CMS_COLS)
    for key in record_keys:
        record_cms.update(key)

    # Query records that were NEVER accessed (should return 0 but won't due to collisions)
    print("\n  Querying NEVER-accessed records (expect 0, measure noise)...")
    never_accessed_pages = [p for p in range(TOTAL_PAGES) if p not in page_true_counts]
    # Sample 10000 never-accessed records
    noise_samples = []
    sample_size = min(10000, len(never_accessed_pages) * RECORDS_PER_PAGE)
    for i in range(sample_size):
        page = never_accessed_pages[i % len(never_accessed_pages)]
        slot = i % RECORDS_PER_PAGE
        noise_samples.append(record_cms.query((page, slot)))

    noise_arr = np.array(noise_samples)
    print(f"    Samples: {len(noise_samples)}")
    print(f"    Noise min/mean/max: {noise_arr.min()}/{noise_arr.mean():.1f}/{noise_arr.max()}")
    print(f"    Noise > 4 (L1 threshold): {(noise_arr >= 4).sum()}/{len(noise_arr)} = {100*(noise_arr >= 4).mean():.1f}%")
    print(f"    Noise > 2: {(noise_arr >= 2).sum()}/{len(noise_arr)} = {100*(noise_arr >= 2).mean():.1f}%")

    # Query records that WERE accessed - compare CMS vs true count
    print("\n  Querying accessed records (CMS vs true count)...")
    # Take top-100 most accessed records
    top_records = record_true_counts.most_common(100)
    print(f"    {'Record (page,slot)':<25} {'True':>6} {'CMS':>6} {'Error':>6} {'Relative':>10}")
    print(f"    {'-'*25} {'-'*6} {'-'*6} {'-'*6} {'-'*10}")
    errors = []
    for (page, slot), true_count in top_records[:20]:
        cms_count = record_cms.query((page, slot))
        error = cms_count - true_count
        rel_error = error / max(true_count, 1)
        errors.append(error)
        print(f"    ({page:>6},{slot:>3}){'':<13} {true_count:>6} {cms_count:>6} {error:>+6} {rel_error:>+9.1%}")

    # Summary statistics for all accessed records
    all_errors = []
    for (page, slot), true_count in record_true_counts.items():
        cms_count = record_cms.query((page, slot))
        all_errors.append(cms_count - true_count)

    all_errors_arr = np.array(all_errors)
    print(f"\n  Error statistics for ALL {len(all_errors)} accessed records:")
    print(f"    Mean overestimate: {all_errors_arr.mean():.1f}")
    print(f"    Median overestimate: {np.median(all_errors_arr):.1f}")
    print(f"    P95 overestimate: {np.percentile(all_errors_arr, 95):.1f}")
    print(f"    P99 overestimate: {np.percentile(all_errors_arr, 99):.1f}")
    print(f"    Max overestimate: {all_errors_arr.max()}")

    # ============================================================
    # Key question: how does this affect Condition 0 decisions?
    # ============================================================
    print("\n" + "=" * 70)
    print("Part C: Impact on Admission Control Decisions")
    print("=" * 70)

    # With l1_fine_threshold = 4 and steady-state noise, what fraction
    # of records would pass Condition 0?
    print(f"\n  With l1_fine_threshold = 4:")
    print(f"    Noise floor (never-accessed) mean = {noise_arr.mean():.1f}")
    print(f"    → {100*(noise_arr >= 4).mean():.1f}% of COLD records falsely pass Condition 0")

    # After one aging cycle (halve), what does the noise look like?
    # Simulate: do aging (halve all), then add another epoch of accesses
    print(f"\n  After aging + 2nd epoch:")
    record_cms_aged = SimpleCMS(RECORD_CMS_ROWS, RECORD_CMS_COLS)
    # First epoch
    for key in record_keys:
        record_cms_aged.update(key)
    # Age (halve) - simulate by reducing all values
    record_cms_aged.matrix //= 2
    # Second epoch (same pattern for simplicity)
    record_keys_2 = list(zip(
        generate_zipf_ranks(TOTAL_PAGES, ZIPF_THETA, NUM_ACCESSES, rng).tolist(),
        rng.integers(0, RECORDS_PER_PAGE, size=NUM_ACCESSES).tolist()
    ))
    for key in record_keys_2:
        record_cms_aged.update(key)

    noise_after_aging = []
    for i in range(sample_size):
        page = never_accessed_pages[i % len(never_accessed_pages)]
        slot = i % RECORDS_PER_PAGE
        noise_after_aging.append(record_cms_aged.query((page, slot)))

    noise_aged_arr = np.array(noise_after_aging)
    print(f"    Noise min/mean/max: {noise_aged_arr.min()}/{noise_aged_arr.mean():.1f}/{noise_aged_arr.max()}")
    print(f"    Noise ≥ 4: {100*(noise_aged_arr >= 4).mean():.1f}%")

    # ============================================================
    # What CMS size would be needed?
    # ============================================================
    print("\n" + "=" * 70)
    print("Part D: Required CMS Size for 10 GiB Working Set")
    print("=" * 70)

    # To keep noise below threshold T after steady-state:
    # Need: 2 * N_epoch / cols < T
    # cols > 2 * N_epoch / T = 2 * 1M / 4 = 500K
    # But this is per-row expected noise. With min-of-d:
    # Actually need: noise * (1/e)^(d-1) factor
    # More practically: cols should be >= N_distinct_items / target_noise_ratio

    print(f"\n  Target: noise floor < l1_fine_threshold = 4")
    print(f"  Formula: steady_noise = 2 * N_accesses / cols < threshold")
    print(f"  Minimum cols = 2 * {NUM_ACCESSES:,} / 4 = {2 * NUM_ACCESSES // 4:,}")
    print(f"  With safety margin (4x): cols = {4 * 2 * NUM_ACCESSES // 4:,}")
    print(f"")
    print(f"  Alternative: increase rows to leverage min-of-d probability reduction")
    print(f"  With 8 rows x 256K cols:")
    print(f"    Per-row noise = 2 * 1M / 256K = {2*1_000_000/262144:.1f}")
    print(f"    P(all 8 rows collide) ≈ (noise/totalInBucket)^8 → much lower")
    print(f"")
    print(f"  Recommended for 10 GiB / 36.7M records:")
    print(f"    Record CMS: 8 rows x 1M cols (64 MB) — matches Page CMS scale")
    print(f"    OR: 4 rows x 4M cols (128 MB)")

    return noise_arr.mean(), steady_noise

if __name__ == "__main__":
    analyze_cms_error()
