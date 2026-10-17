#pragma once

#include <cstdint>

#include "core/paged_kv_cache.h"   // kRingSlackPages

namespace ninfer::models::qwen3_5::detail {

// Per-lane share of the Device page pool: N lanes each staying inside pool/N never need to take
// a page from each other (a lane may only reclaim its own pages). At one lane this is the whole pool.
[[nodiscard]] std::uint32_t ring_lane_pool_pages(std::uint32_t pool, std::uint32_t lanes);

// What one lane must be able to hold for the configuration to be servable: its attention-sink pin,
// its retrieval budget, and the restore slack. `budget` is passed in already capped by the lane's share.
[[nodiscard]] std::uint32_t ring_lane_need_pages(std::uint32_t sink_pages, std::uint32_t budget);

// A lane's resident ceiling given what OTHER lanes currently hold on the Device: the pool minus the
// restore slack minus the other lanes' resident pages. This is the dynamic form of the old static
// `pool / lanes - slack`: a short request that holds few pages lets another lane grow into the
// remainder, so the sum may exceed a fixed 1/N split while never exceeding the pool.
[[nodiscard]] std::uint32_t ring_lane_dynamic_ceiling(std::uint32_t pool,
                                                      std::uint32_t other_lanes_resident);

}  // namespace ninfer::models::qwen3_5::detail