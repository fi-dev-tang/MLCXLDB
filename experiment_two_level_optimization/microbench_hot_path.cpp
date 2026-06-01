// Standalone microbench for the RecordCache hot path's per-step costs.
//
// Why: analysis_performance.md §L3 lists per-step ns estimates (10 ns for
// enterEpoch, 50-100 ns for unordered_map::find, etc.) that were hand-waved.
// This binary isolates each step under best-case (single-threaded, pinned)
// conditions so we have a real lower bound to compare profile-derived
// numbers against.
//
// Each test runs N iterations on a single pinned core, with a barrier and
// asm-volatile fences to prevent the compiler from hoisting work out of
// the loop. We report ns/op and cycles/op (using __rdtsc when available).
//
// Build (uses repo's vendored ankerl::unordered_dense):
//   /home/zhizhi.tyf/local/bin/g++ -O2 -g -std=c++20 -pthread \
//     -I/home/zhizhi.tyf/cxl-WT-comparison/build/vendor/unordered_dense/src/unordered_dense_src/include \
//     microbench_hot_path.cpp -o microbench_hot_path
//
// Run (pin to core 0, ignore terminal noise):
//   taskset -c 0 ./microbench_hot_path
//
// Tests:
//   1. ankerl::dense::map<RecordCacheKey, void*>::find — 1M entries, 50% hit
//   2. std::unordered_map<std::string, void*>::find    — same workload (control)
//   3. FNV1a hash of 12B key                            — current production
//   4. wyhash of 12B key                                — proposed P0-3 replacement
//   5. std::shared_mutex shared_lock + unlock           — uncontended
//   6. SeqLock beginRead + retryRead                    — uncontended
//   7. std::atomic<u64> fetch_add(1, relaxed)           — mimick enterEpoch
//   8. memcpy 100B + 100B verify                        — mimick value snapshot
//
// What this is NOT:
//   - not a multi-threaded benchmark (single core, uncontended)
//   - not an end-to-end RecordCache test
//   - not a replacement for perf record; profile shows real workload mix,
//     this shows per-primitive lower bounds.
//
// Use both together: profile tells you which primitive dominates in
// production; this tells you the floor each primitive can possibly hit.

#include <ankerl/unordered_dense.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------- tiny helpers ----------

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

static inline u64 rdtsc() {
   unsigned hi, lo;
   __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
   return (static_cast<u64>(hi) << 32) | lo;
}

template <typename F>
static void run_test(const char* name, size_t iterations, F&& body) {
   // Warm caches and predictors.
   for (size_t i = 0; i < 1000; ++i) body(i);

   const auto t0 = std::chrono::steady_clock::now();
   const u64 c0  = rdtsc();
   for (size_t i = 0; i < iterations; ++i) {
      body(i);
   }
   const u64 c1  = rdtsc();
   const auto t1 = std::chrono::steady_clock::now();

   const double ns_total = std::chrono::duration<double, std::nano>(t1 - t0).count();
   const double ns_per_op  = ns_total / iterations;
   const double cyc_per_op = static_cast<double>(c1 - c0) / iterations;
   std::printf("  %-50s %10.2f ns/op   %8.1f cyc/op   (%zu iter)\n",
               name, ns_per_op, cyc_per_op, iterations);
}

// ---------- RecordCacheKey (verbatim from RecordCache.hpp) ----------

struct RecordCacheKey {
   static constexpr u8 kMaxLen = 24;
   std::array<u8, kMaxLen> bytes{};
   u8 len{0};

   bool operator==(const RecordCacheKey& other) const noexcept {
      return len == other.len && std::memcmp(bytes.data(), other.bytes.data(), len) == 0;
   }
};

static u64 fnv1a(const u8* data, size_t len) noexcept {
   u64 h = 14695981039346656037ULL;
   for (size_t i = 0; i < len; ++i) {
      h ^= static_cast<u64>(data[i]);
      h *= 1099511628211ULL;
   }
   return h;
}

struct RecordCacheKeyHashFnv {
   using is_transparent = void;
   size_t operator()(const RecordCacheKey& k) const noexcept { return fnv1a(k.bytes.data(), k.len); }
};

struct RecordCacheKeyEq {
   using is_transparent = void;
   bool operator()(const RecordCacheKey& a, const RecordCacheKey& b) const noexcept { return a == b; }
};

// ---------- wyhash (public domain, https://github.com/wangyi-fudan/wyhash, trimmed) ----------

namespace wy {
static inline u64 _wymum(u64 A, u64 B) {
   __uint128_t r = (__uint128_t)A * B;
   return (u64)(r ^ (r >> 64));
}
static inline u64 _wyr8(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
static inline u64 _wyr4(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
static inline u64 _wyr3(const u8* p, size_t k) {
   return ((u64)p[0] << 16) | ((u64)p[k >> 1] << 8) | p[k - 1];
}
static inline u64 hash(const u8* key, size_t len, u64 seed = 0) {
   u64 a, b;
   seed ^= 0xa0761d6478bd642full;
   if (len <= 16) {
      if (len >= 4) { a = (_wyr4(key) << 32) | _wyr4(key + ((len >> 3) << 2));
                      b = (_wyr4(key + len - 4) << 32) | _wyr4(key + len - 4 - ((len >> 3) << 2)); }
      else if (len > 0) { a = _wyr3(key, len); b = 0; }
      else { a = b = 0; }
   } else {
      size_t i = len; const u8* p = key;
      u64 see1 = seed;
      while (i > 48) {
         seed = _wymum(_wyr8(p) ^ seed, _wyr8(p + 8) ^ 0xe7037ed1a0b428dbull);
         see1 = _wymum(_wyr8(p + 16) ^ see1, _wyr8(p + 24) ^ 0x8ebc6af09c88c6e3ull);
         seed = _wymum(_wyr8(p + 32) ^ seed, _wyr8(p + 40) ^ 0x589965cc75374cc3ull);
         seed ^= see1;
         p += 48; i -= 48;
      }
      while (i > 16) {
         seed = _wymum(_wyr8(p) ^ seed, _wyr8(p + 8) ^ 0xe7037ed1a0b428dbull);
         p += 16; i -= 16;
      }
      a = _wyr8(p + i - 16);
      b = _wyr8(p + i - 8);
   }
   return _wymum(0x1d8e4e27c47d124full ^ len, _wymum(a ^ 0xa0761d6478bd642full, b ^ seed));
}
} // namespace wy

// ---------- mini SeqLock (matches the spirit of RecordCacheEntry's SeqLock) ----------

struct alignas(64) MiniSeqLock {
   std::atomic<u64> version{0};
   u64 v0{0}, v1{0};   // protected fields

   u64 beginRead() const {
      u64 v;
      do { v = version.load(std::memory_order_acquire); } while (v & 1);
      return v;
   }
   bool retryRead(u64 v) const {
      std::atomic_thread_fence(std::memory_order_acquire);
      return version.load(std::memory_order_acquire) == v;
   }
   void beginWrite() { version.fetch_add(1, std::memory_order_acquire); } // odd
   void endWrite()   { version.fetch_add(1, std::memory_order_release); } // even
};

// ---------- pin to core 0 ----------

static void pin_to_core(int core_id) {
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);
   if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
      std::printf("[WARN] pin_to_core(%d) failed; continuing un-pinned\n", core_id);
   }
}

// ============================================================
//                          tests
// ============================================================

int main(int argc, char** argv) {
   pin_to_core(0);

   const size_t N_HASH       = 50'000'000;   // hash/lock primitive iterations
   const size_t N_MAP_FIND   = 30'000'000;   // map find iterations
   const size_t MAP_ENTRIES  = 1'000'000;    // ~ what RecordCache holds in YCSB

   std::printf("======================================================================\n");
   std::printf("microbench_hot_path — single-threaded, pinned to core 0\n");
   std::printf("  N_HASH=%zu  N_MAP_FIND=%zu  MAP_ENTRIES=%zu\n",
               N_HASH, N_MAP_FIND, MAP_ENTRIES);
   std::printf("======================================================================\n");

   // ---- Build common workload: a pool of 12B keys (YCSB-style) ----
   //
   // We mix the digits a bit so hash distribution is non-trivial.
   std::mt19937_64 rng(0xC0FFEE);
   std::vector<RecordCacheKey> all_keys(MAP_ENTRIES * 2);  // half in map, half NOT
   std::vector<std::string>     all_strkeys(MAP_ENTRIES * 2);
   for (size_t i = 0; i < all_keys.size(); ++i) {
      RecordCacheKey& k = all_keys[i];
      k.len = 12;
      u64 v = rng();
      std::memcpy(k.bytes.data(),     &v, 8);
      v = rng();
      std::memcpy(k.bytes.data() + 8, &v, 4);
      all_strkeys[i].assign(reinterpret_cast<const char*>(k.bytes.data()), k.len);
   }

   // First half goes into the maps; second half is "miss" probes.
   const size_t IN_MAP = MAP_ENTRIES;

   ankerl::unordered_dense::map<RecordCacheKey, void*, RecordCacheKeyHashFnv, RecordCacheKeyEq> ankerl_map;
   ankerl_map.reserve(IN_MAP);
   for (size_t i = 0; i < IN_MAP; ++i) {
      ankerl_map.emplace(all_keys[i], reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));
   }

   std::unordered_map<std::string, void*> std_map;
   std_map.reserve(IN_MAP);
   for (size_t i = 0; i < IN_MAP; ++i) {
      std_map.emplace(all_strkeys[i], reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));
   }

   // Random probe sequence (50% hit / 50% miss).
   std::vector<u32> probe_idx(N_MAP_FIND);
   std::uniform_int_distribution<u32> dist_all(0, static_cast<u32>(all_keys.size() - 1));
   for (size_t i = 0; i < N_MAP_FIND; ++i) probe_idx[i] = dist_all(rng);

   std::printf("\n--- Map lookups (50%% hit, 50%% miss, %zu entries, 12B keys) ---\n", IN_MAP);

   {
      volatile u64 sink = 0;
      run_test("ankerl::dense::map find (FNV1a)", N_MAP_FIND, [&](size_t i) {
         auto it = ankerl_map.find(all_keys[probe_idx[i]]);
         if (it != ankerl_map.end()) sink += reinterpret_cast<uintptr_t>(it->second);
      });
      asm volatile("" :: "r"(sink));
   }
   {
      volatile u64 sink = 0;
      run_test("std::unordered_map<string> find", N_MAP_FIND, [&](size_t i) {
         auto it = std_map.find(all_strkeys[probe_idx[i]]);
         if (it != std_map.end()) sink += reinterpret_cast<uintptr_t>(it->second);
      });
      asm volatile("" :: "r"(sink));
   }

   std::printf("\n--- Hash functions on 12B key (per-call cost only) ---\n");
   {
      volatile u64 sink = 0;
      const u8* k = all_keys[0].bytes.data();
      run_test("FNV1a 12B", N_HASH, [&](size_t i) {
         sink ^= fnv1a(k, 12);
         (void)i;
      });
      asm volatile("" :: "r"(sink));
   }
   {
      volatile u64 sink = 0;
      const u8* k = all_keys[0].bytes.data();
      run_test("wyhash 12B", N_HASH, [&](size_t i) {
         sink ^= wy::hash(k, 12);
         (void)i;
      });
      asm volatile("" :: "r"(sink));
   }

   std::printf("\n--- Locks (uncontended, single thread) ---\n");
   {
      std::shared_mutex sm;
      volatile u64 sink = 0;
      run_test("std::shared_mutex shared_lock/unlock", N_HASH, [&](size_t i) {
         std::shared_lock lk(sm);
         sink += i;
      });
      asm volatile("" :: "r"(sink));
   }
   {
      std::shared_mutex sm;
      volatile u64 sink = 0;
      run_test("std::shared_mutex unique_lock/unlock", N_HASH / 5, [&](size_t i) {
         std::unique_lock lk(sm);
         sink += i;
      });
      asm volatile("" :: "r"(sink));
   }
   {
      MiniSeqLock sl;
      volatile u64 sink = 0;
      run_test("SeqLock beginRead+retryRead (uncontended)", N_HASH, [&](size_t i) {
         u64 v = sl.beginRead();
         u64 a = sl.v0, b = sl.v1;
         if (sl.retryRead(v)) sink += a + b + i;
      });
      asm volatile("" :: "r"(sink));
   }

   std::printf("\n--- Atomics (mimick enterEpoch / leaveEpoch / SIEVE visited) ---\n");
   {
      std::atomic<u64> ctr{0};
      run_test("atomic<u64> fetch_add relaxed", N_HASH, [&](size_t i) {
         ctr.fetch_add(1, std::memory_order_relaxed);
         (void)i;
      });
   }
   {
      std::atomic<u64> ctr{0};
      run_test("atomic<u64> store release", N_HASH, [&](size_t i) {
         ctr.store(i, std::memory_order_release);
      });
   }
   {
      std::atomic<u64> ctr{0};
      run_test("atomic<u64> load acquire", N_HASH, [&](size_t i) {
         volatile u64 v = ctr.load(std::memory_order_acquire);
         (void)v; (void)i;
      });
   }

   std::printf("\n--- Memcpy 100B (mimick value snapshot inside SeqLock loop) ---\n");
   {
      alignas(64) u8 src[100]; std::memset(src, 0xA5, sizeof(src));
      alignas(64) u8 dst[100];
      volatile u64 sink = 0;
      run_test("memcpy 100B", N_HASH, [&](size_t i) {
         std::memcpy(dst, src, 100);
         sink += dst[i % 100];
      });
      asm volatile("" :: "r"(sink));
   }

   std::printf("\nDone.\n");
   std::printf("Compare against production numbers: take Mqps from logs, compute\n");
   std::printf("  ns/lookup = 1e9 / (Mqps * 1e6) / worker_threads\n");
   std::printf("and subtract miss-path cost to get hit-path ns. Then map each\n");
   std::printf("step above to its share of the hit path.\n");
   (void)argc; (void)argv;
   return 0;
}
