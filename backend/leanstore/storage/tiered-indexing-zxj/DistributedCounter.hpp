#pragma once
#include <cstdint>
#include <cstdlib>
#include <thread>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

namespace leanstore::storage::tiered_indexing_zxj {

struct PadInt {
   PadInt() { data = 0; }
   union {
      uint64_t data;
      char padding[CACHE_LINE_SIZE];
   };
};

template <int buckets = 32>
class DistributedCounter {
   static_assert(buckets == 0 || (buckets & (buckets - 1)) == 0);

public:
   DistributedCounter(int64_t init = 0) {
      for (int i = 0; i < buckets; ++i) arr_[i].data = 0;
      if (init != 0) arr_[0].data = init;
   }

   void operator+=(int64_t v) { increment(v); }
   void operator-=(int64_t v) { decrement(v); }
   void operator++() { increment(1); }
   void operator++(int) { increment(1); }
   void operator--() { decrement(1); }
   void operator--(int) { decrement(1); }

   int64_t load() const { return get(); }
   operator int64_t() const { return get(); }

   void store(int64_t v) {
      for (int i = 0; i < buckets; ++i) arr_[i].data = 0;
      arr_[0].data = v;
   }

   void increment(int64_t v = 1) {
      __atomic_add_fetch(&arr_[idx()].data, v, __ATOMIC_RELAXED);
   }

   void decrement(int64_t v = 1) {
      __atomic_sub_fetch(&arr_[idx()].data, v, __ATOMIC_RELAXED);
   }

   int64_t get() const {
      int64_t val = 0;
      for (int i = 0; i < buckets; ++i)
         val += __atomic_load_n(&arr_[i].data, __ATOMIC_RELAXED);
      return val;
   }

private:
   static uint64_t get_cpu_id() {
      static thread_local uint64_t id =
          static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
      return id;
   }
   int idx() const { return get_cpu_id() & (buckets - 1); }
   PadInt arr_[buckets];
};

}  // namespace leanstore::storage::tiered_indexing_zxj
