#include "leanstore/storage/buffer-manager/BufferManager.hpp"

namespace leanstore::storage::two_level_admission_control {

void DramHotPageCandidates::ClearPromotedSlot(u64 page_id, u16 slot_id) {
    const u16 shard_idx = getShardIndex(page_id);
    std::shared_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
    auto it = shards[shard_idx].map.find(page_id);
    if (it != shards[shard_idx].map.end()) {
        it->second.ClearPromotedSlotBit(slot_id);
    }
}

} // namespace leanstore::storage::two_level_admission_control
