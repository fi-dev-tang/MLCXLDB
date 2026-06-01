# MLCXLDB

Cloud-native OLTP databases rely on local DRAM buffer pools to absorb remote storage latency, yet DRAM capacity is physically and economically constrained. CXL-attached memory offers byte-addressable expansion at roughly half the cost of local DRAM and 4x its latency---two orders of magnitude faster than SSD---but simply extending the buffer pool into CXL at page granularity is insufficient: the top 1--2% hottest record slots within a page account for 69.1% of intra-page accesses, so page-level tiering wastes most of the precious DRAM tier on cold records.

Exploiting this observation requires solving three problems simultaneously: (i) *adaptive placement* across a 1x--4x--508x latency hierarchy without excessive migration overhead, (ii) *bridging page- and record-granularity* caching so that uniformly hot pages and skew-hot pages receive different promotion treatment, and (iii) *low-overhead concurrency control* on CXL 2.0 memory, where traditional per-access locking erodes the latency benefit that tiering is designed to provide.

Built on top of [LeanStore](https://github.com/leanstore/leanstore).

## Branch Description

| Branch | Description |
|--------|-------------|
| `ReadOnly` | MLCXLDB ReadOnly mode base implementation |
| `WriteThrough` | MLCXLDB WriteThrough mode base implementation |
| `ReadOnly_comparison` | ReadOnly mode with comparison baselines interface integrated |
| `WriteThrough_comparison` | WriteThrough mode with comparison baselines interface integrated |
| `experiments` | Experiment scripts for reproducibility |

## Compiling

Install dependencies:

`sudo apt-get install cmake libtbb2-dev libaio-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev librocksdb-dev liblmdb-dev libwiredtiger-dev liburing-dev`

`mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. && make -j`

## Benchmark Examples

### Exp1 & Exp2: End-to-End YCSB (ReadOnly / WriteThrough)

Sweeps working-set sizes (4/8/16 GiB) at theta=0.90 across YCSB A–F with 5 admission variants.

```bash
# Example: YCSB-A, two_level variant, WS=4G, ReadOnly mode
build/frontend/experiment_1_ycsb_a \
    --test_admission_mode=two_level \
    --test_zipf_theta=0.90 \
    --test_working_set_gib=4.0 \
    --test_warmup_lookups=100000000 \
    --test_measure_lookups=100000000 \
    --cxl_tiering_enabled=true \
    --cxl_gib=2.5 \
    --cxl_dax_device_path=/dev/dax0.2 \
    --dram_buffer_pool_gib=0.10 \
    --dram_recordcache_gib=0.50 \
    --worker_threads=8 \
    --pp_threads=1 \
    --cxl_pp_threads=1 \
    --two_level_admission_threads=2 \
    --forward_epoch_thread=1 \
    --sieve_eviction_thread=1 \
    --record_cache_promote_thread=4 \
    --delay_admission_recordcache_threads_start=true \
    --skew_threshold_ratio=0.08 \
    --uniform_threshold_ratio=0.45 \
    --max_per_page_visits=8000 \
    --max_global_requests_window=2000000 \
    --trigger_visit_histogram_update_size=1000000 \
    --test_payload_size_bytes=100 \
    --ssd_path=<SSD_PATH> \
    --trunc=true --wal=true --vi=true
```

**Memory configuration (per WS):**

| WS | DRAM_tier | CXL | BP (ABCF) | RC (ABCF) | BP (DE) | RC (DE) |
|----|-----------|-----|-----------|-----------|---------|---------|
| 4G | 0.6 GiB | 2.5 GiB | 0.10 | 0.50 | 0.40 | 0.20 |
| 8G | 1.2 GiB | 5.0 GiB | 0.20 | 1.00 | 0.80 | 0.40 |
| 16G | 2.5 GiB | 10.0 GiB | 0.50 | 2.00 | 1.50 | 1.00 |

**Admission variants:**
- `two_level` — CXL tiering + RecordCache (our approach)
- `page_only` — CXL tiering, page-level only (BP = DRAM_tier total)
- `lru` — CXL tiering with LRU eviction (BP = DRAM_tier total)
- `dram_ssd` — DRAM+SSD only, no CXL (BP = DRAM_tier total)
- `dram_ssd_unconstrained` — DRAM+SSD, BP = DRAM_tier + CXL (upper bound)

### Exp7: Comparison Experiment (5 systems)

Compares our system against 3 baselines on YCSB A–F × theta 0.90/0.95/0.99 + TPC-C.

```bash
# Example: YCSB-B, bf-tree baseline, theta=0.95, WS=23G
build/frontend/experiment_1_ycsb_b \
    --test_admission_mode=bf-tree \
    --test_zipf_theta=0.95 \
    --test_working_set_gib=23.0 \
    --test_warmup_lookups=800000000 \
    --test_measure_lookups=200000000 \
    --cxl_tiering_enabled=true \
    --cxl_gib=18.0 \
    --cxl_dax_device_path=/dev/dax0.6 \
    --dram_buffer_pool_gib=3.0 \
    --dram_recordcache_gib=0.0 \
    --worker_threads=20 \
    --pp_threads=1 \
    --cxl_pp_threads=1 \
    --test_payload_size_bytes=100 \
    --ssd_path=<SSD_PATH> \
    --trunc=true --wal=true --vi=true

# Example: TPC-C, two_level_readonly
build/frontend/tpcc_compare_test \
    --test_admission_mode=two_level \
    --tpcc_warehouse_count=100 \
    --test_warmup_seconds=120 \
    --test_measure_seconds=180 \
    --test_load_data=true \
    --cxl_tiering_enabled=true \
    --cxl_gib=18.0 \
    --cxl_dax_device_path=/dev/dax0.6 \
    --dram_buffer_pool_gib=0.43 \
    --dram_recordcache_gib=2.57 \
    --worker_threads=20 \
    --pp_threads=1 \
    --cxl_pp_threads=1 \
    --two_level_admission_threads=2 \
    --forward_epoch_thread=1 \
    --sieve_eviction_thread=1 \
    --record_cache_promote_thread=4 \
    --delay_admission_recordcache_threads_start=true \
    --ssd_path=<SSD_PATH> \
    --trunc=true --wal=true --vi=true
```

**Exp7 memory config:** WS=23G, CXL=18G, DRAM=3G

**Comparison modes:**
| Mode | Repo | Description |
|------|------|-------------|
| `two_level_readonly` | ReadOnly_comparison | Our system (ReadOnly) |
| `two_level_wt` | WriteThrough_comparison | Our system (WriteThrough) |
| `bf-tree` | WriteThrough_comparison | BF-Tree baseline |
| `tiered-indexing-zxj` | WriteThrough_comparison | Tiered Indexing baseline (+ `--vi_fremove=true`) |
| `hybried-tier-asplos2025` | WriteThrough_comparison | HybridTier ASPLOS'25 baseline |

**two_level memory split (Exp7):**
- ABCF/TPC-C: BP=0.43, RC=2.57 (RC:BP ≈ 6:1)
- DE: BP=2.57, RC=0.43 (BP:RC ≈ 6:1)
- Baselines: BP=3.0, RC=0.0

## Implement Your Workload

LeanStore offers a flexible transactional Key/Value interface similar to WiredTiger and RocksDB.
A table is a B-Tree index where keys and values are stored in a normalized format, i.e., lexicographically ordered strings.
For convenience, frontend/shared offers templates that take care of (un)folding common types.
The best starting points are frontend/minimal-example and frontend/ycsb.
The required parameters at runtime are: `--ssd_path=/block_device/or/filesystem --dram_gib=fixed_in_gib`.
The default transaction isolation level is `--isolation_level=si`. You can lower it to Read Committed or Read Uncommitted by replacing `si` with `rc` or `ru`, respectively.
You can set the transaction isolation level using `--isolation_level=si` and enable the B-Tree techniques from CIDR202 with `--contention_split --xmerge`.

### Metrics Reporting
LeanStore emits several metrics per second in CSV files: `log_bm.csv, log_configs.csv, log_cpu.csv, log_cr.csv, log_dt.csv`.
Each row has a c_hash value, which is calculated by chaining and hashing all the configurations that you passed to the binary at runtime.
This gives you an easy way to identify your run and join all relevant information from the different CSV files using SQLite, for example."

## Implemented Features

- [x] Lightweight buffer manager with pointer swizzling [ICDE'18]
- [x] Optimstic Lock Coupling with Hybrid Page Guard to synchronize paged data structures [IEEE'19]
- [x] Contention and Space Management in B-Trees [CIDR'21]
- [x] Variable-length key/values B-Tree with prefix compression and hints  [BTW'23]
- [x] Scalable and robust out-of-memory Snapshot Isolation (OSIC protocol, Graveyard, and FatTuple) [VLDB'23]
- [x] Distributed Logging with remote flush avoidance [SIGMOD'20, BTW'23]
- [ ] Recovery [SIGMOD'20]
- [ ] What Modern NVMe Storage Can Do, And How To Exploit It: High-Performance {I/O} for High-Performance Storage Engines [VLDB'23] [branch](https://github.com/leanstore/leanstore/tree/io)
- [ ] Why Files If You Have a DBMS? [ICDE'24] [branch](https://github.com/leanstore/leanstore/tree/blob)
- [ ] Moving on From Group Commit: Autonomous Commit Enables High Throughput and Low Latency on NVMe SSDs [SIGMOD'25] [branch](https://github.com/leanstore/leanstore/tree/latency)

## Cite

LeanStore was originally implemented using Pointer Swizzling ([paper](https://15721.courses.cs.cmu.edu/spring2020/papers/23-largethanmemory/leis-icde2018.pdf)).
More recently, LeanStore was also completely rewritten from scratch, replacing Pointer Swizzling with Virtual-Memory Assisted Buffer Pool ([Paper](https://www.cs.cit.tum.de/fileadmin/w00cfj/dis/_my_direct_uploads/vmcache.pdf)).
You can find the citations for different versions (which include different features) in the following.

### VMCache version

[Branch `blob`](https://github.com/leanstore/leanstore/tree/blob) covers the high-performance variable-sized objects, which we used for our ICDE 2024 paper.

```BibTeX
@inproceedings{DBLP:conf/icde/NguyenL24,
  author       = {Lam{-}Duy Nguyen and
                  Viktor Leis},
  title        = {Why Files If You Have a DBMS?},
  booktitle    = {{ICDE}},
  pages        = {3878--3892},
  publisher    = {{IEEE}},
  year         = {2024}
}
```

[Branch `latency`](https://github.com/leanstore/leanstore/tree/latency) covers the autonomous commit technique that delivers high-throughput, low-latency commits.

```BibTeX
@inproceedings{DBLP:journals/pacmmod/NguyenL25,
  author       = {Lam{-}Duy Nguyen and
                  Adnan Alhomssi and
                  Tobias Ziegler and
                  Viktor Leis},
  title        = {Moving on From Group Commit: Autonomous Commit Enables High Throughput and Low Latency on NVMe SSDs},
  journal      = {Proc. {ACM} Manag. Data},
  volume       = {3},
  year         = {2025}
}
```

### Pointer Swizzling version

The code we used for our VLDB 2023 that covers alternative SI commit protocols is in a different [branch](https://github.com/leanstore/leanstore/tree/mvcc).

```BibTeX
@inproceedings{alhomssi23,
    author    = {Adnan Alhomssi and Viktor Leis},
    title     = {Scalable and Robust Snapshot Isolation for High-Performance Storage Engines},
    booktitle = {VLDB},
    year      = {2023}
}
```

The code we used for our VLDB 2023 that covers the fast I/O implementation is in a different [branch](https://github.com/leanstore/leanstore/tree/io).

```BibTeX
@article{haas23,
  author       = {Gabriel Haas and Viktor Leis},
  title        = {What Modern NVMe Storage Can Do, And How To Exploit It: High-Performance {I/O} for High-Performance Storage Engines},
  journal      = {Proc. {VLDB} Endow.},
  year         = {2023}
}
```

BTW 2023 [branch](https://github.com/leanstore/leanstore/tree/btw) that covers alternative dependency tracking.

```BibTeX
@inproceedings{leanstore23,
    author    = {Adnan Alhomssi, Michael Haubenschild and Viktor Leis},
    title     = {The Evolution of LeanStore},
    booktitle = {BTW},
    year      = {2023}
}
```

CIDR 2021 [branch](https://github.com/leanstore/leanstore/tree/cidr) (outdated).

```BibTeX
@inproceedings{alhomssi21,
    author    = {Adnan Alhomssi and Viktor Leis},
    title     = {Contention and Space Management in B-Trees},
    booktitle = {CIDR},
    year      = {2021}
}
```
# gcc-13.2.0 Enable Environment Variable

Prerequisite: c++20 (for `std::atomic::wait`). Make sure gcc-13.2.0 or later is available in your `PATH`.

# cxl usage
Replace daxctl with ali_pack (a self-developed tool),
Topology:

[cxl memory pool] <-------> [cxl switch] <---------> [host node (We are here!)]

![Topology](/hardware_topology.png)

Usage:
```bash
# Old Open-Source Tools Usage:
daxctl reconfigure-device --mode=system-ram -f dax0.1
daxctl reconfigure-device --mode=devdax -f dax0.1

# 1.help
cxlmgmt help
cxlmgmt alloc -h

# 2. version check
# [2.1] version check for host node
cxlmgmt -v
/opt/fma/fmagent -v

# 2. version check
# [2.2] version check for mcpu
/opt/fm/cxlfm -v

# 3. Memory Allocation
cxlmgmt alloc --socket=0 --size=128

# 4. Memory Information Check
cxlmgmt query
```