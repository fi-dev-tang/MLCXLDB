#include "DTRegistry.hpp"

#include "leanstore/profiling/counters/WorkerCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace storage
{
// -------------------------------------------------------------------------------------
DTRegistry DTRegistry::global_dt_registry;
// -------------------------------------------------------------------------------------
void DTRegistry::iterateChildrenSwips(DTID dtid, BufferFrame& bf, std::function<bool(Swip<BufferFrame>&)> callback)
{
   auto dt_meta = dt_instances_ht[dtid];
   dt_types_ht[std::get<0>(dt_meta)].iterate_children(std::get<1>(dt_meta), bf, callback);
}
// -------------------------------------------------------------------------------------
ParentSwipHandler DTRegistry::findParent(DTID dtid, BufferFrame& bf)
{
   auto dt_meta = dt_instances_ht[dtid];
   auto name = std::get<2>(dt_meta);
   return dt_types_ht[std::get<0>(dt_meta)].find_parent(std::get<1>(dt_meta), bf);
}
// -------------------------------------------------------------------------------------
SpaceCheckResult DTRegistry::checkSpaceUtilization(DTID dtid, BufferFrame& bf)
{
   auto dt_meta = dt_instances_ht[dtid];
   return dt_types_ht[std::get<0>(dt_meta)].check_space_utilization(std::get<1>(dt_meta), bf);
}
// -------------------------------------------------------------------------------------
void DTRegistry::checkpoint(DTID dtid, BufferFrame& bf, u8* dest)
{
   auto dt_meta = dt_instances_ht[dtid];
   return dt_types_ht[std::get<0>(dt_meta)].checkpoint(std::get<1>(dt_meta), bf, dest);
}
// -------------------------------------------------------------------------------------
// Datastructures management
// -------------------------------------------------------------------------------------
void DTRegistry::registerDatastructureType(DTType type, DTRegistry::DTMeta dt_meta)
{
   std::unique_lock guard(mutex);
   dt_types_ht[type] = dt_meta;
}
// -------------------------------------------------------------------------------------
void DTRegistry::registerDatastructureInstance(DTType type, void* root_object, string name, DTID dt_id)
{
   std::unique_lock guard(mutex);
   dt_instances_ht.insert({dt_id, {type, root_object, name}});
   if (dt_id >= instances_counter) {
      instances_counter = dt_id + 1;
   }
}
// -------------------------------------------------------------------------------------
DTID DTRegistry::registerDatastructureInstance(DTType type, void* root_object, string name)
{
   std::unique_lock guard(mutex);
   DTID new_instance_id = instances_counter++;
   dt_instances_ht.insert({new_instance_id, {type, root_object, name}});
   return new_instance_id;
}
// -------------------------------------------------------------------------------------
void DTRegistry::undo(DTID dt_id, const u8* wal_entry, u64 tts)
{
   auto dt_meta = dt_instances_ht[dt_id];
   return dt_types_ht[std::get<0>(dt_meta)].undo(std::get<1>(dt_meta), wal_entry, tts);
}

// -------------------------------------------------------------------------------------
void DTRegistry::todo(DTID dt_id, const u8* entry, const u64 version_worker_id, u64 version_tx_id, const bool called_before)
{
   auto dt_meta = dt_instances_ht[dt_id];
   return dt_types_ht[std::get<0>(dt_meta)].todo(std::get<1>(dt_meta), entry, version_worker_id, version_tx_id, called_before);
}
// -------------------------------------------------------------------------------------
void DTRegistry::unlock(DTID dt_id, const u8* entry)
{
   auto dt_meta = dt_instances_ht[dt_id];
   return dt_types_ht[std::get<0>(dt_meta)].unlock(std::get<1>(dt_meta), entry);
}
// -------------------------------------------------------------------------------------
std::unordered_map<std::string, std::string> DTRegistry::serialize(DTID dt_id)
{
   auto dt_meta = dt_instances_ht[dt_id];
   return dt_types_ht[std::get<0>(dt_meta)].serialize(std::get<1>(dt_meta));
}
// -------------------------------------------------------------------------------------
void DTRegistry::deserialize(DTID dt_id, std::unordered_map<std::string, std::string> map)
{
   auto dt_meta = dt_instances_ht[dt_id];
   return dt_types_ht[std::get<0>(dt_meta)].deserialize(std::get<1>(dt_meta), map);
}
// -------------------------------------------------------------------------------------
// [Added]. 
// 1. dt_types_ht: (key: data_structure, value: registered_callback_function)
// 2. dt_instance_ht: (key: data_instance_id, value: (data_structure, data_instance_object_pointer, instance_name))
//
// The function's logic:
// 1. use data_instance_id to traverse current dt_instance_ht, find the corresponding entry
// 2. if exist, find it data_structre, use dt_types_ht[data_structure] to find callback function
// data_structure = entry -> second.get<0>
// 3. if should_promote_to_dram callback function exists, call it with
// should_promote_to_dram(data_instance_object_pointer, bufferFrame)
// data_instance_obeject_pointer = entry -> second.get<1>
// -------------------------------------------------------------------------------------
bool DTRegistry::shouldDirectlyPromoteToDRAM(DTID data_instance_id, const BufferFrame& bf)
{
   auto current_instance_id_index = dt_instances_ht.find(data_instance_id);
   if (current_instance_id_index == dt_instances_ht.end()) {
      return false;  // Not Found!
   }
   const auto& callback = dt_types_ht[std::get<0>(current_instance_id_index ->second)].should_directly_promote_to_dram;
   if (!callback) {
      return false;  // DT did not register a callback → no preference
   }
   return callback(std::get<1>(current_instance_id_index ->second), bf);
}
// -------------------------------------------------------------------------------------
}  // namespace storage
}  // namespace leanstore
