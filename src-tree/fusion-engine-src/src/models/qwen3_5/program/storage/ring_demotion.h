#pragma once

#include "models/qwen3_5/program/storage/host_kv_store.h"

#include <cuda_runtime.h>

#include <cstdint>

namespace ninfer::models::qwen3_5::detail {

// Demote inactive, fully committed addresses only after their pages have current Host backing.
// This storage operation is independent of Program state, which keeps lifecycle regressions
// testable without constructing a model-backed Program.
std::uint32_t demote_inactive_addresses_to_host(
    HostKVExtentStore& host_kv_extents, cudaStream_t stream, KVAddressSpaceStore& addresses,
    LogicalKVPageStore& pages, const KVAddressSpaceHandle& skip, std::uint32_t target_free);

} // namespace ninfer::models::qwen3_5::detail
