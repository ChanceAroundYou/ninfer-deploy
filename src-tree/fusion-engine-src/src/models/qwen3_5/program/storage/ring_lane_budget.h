#pragma once

#include <algorithm>
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

enum class RingCapacityStatus : std::uint8_t {
    Ready,
    InsufficientDevicePages,
};

// Read-only capacity accounting for one ring mapping request. Callers may use target_free_pages to
// attempt reclamation, then classify the known hard mapping requirement with status_after().
struct RingCapacityProjectionInput {
    std::uint32_t pool_pages           = 0;
    std::uint32_t allocated_pages      = 0;
    std::uint32_t lane_resident_pages  = 0;
    std::uint32_t other_resident_pages = 0;
    std::uint32_t required_pages       = 0;
    std::uint32_t preferred_restores   = 0;
};

struct RingCapacityProjection {
    std::uint32_t free_pages        = 0;
    std::uint32_t ceiling_pages     = 0;
    std::uint32_t projected_pages   = 0;
    std::uint32_t share_shortfall   = 0;
    std::uint32_t target_free_pages = 0;
    std::uint32_t required_pages    = 0;

    [[nodiscard]] constexpr RingCapacityStatus status_after(std::uint32_t freed_pages) const noexcept {
        return free_pages + freed_pages >= required_pages
                   ? RingCapacityStatus::Ready
                   : RingCapacityStatus::InsufficientDevicePages;
    }
};

[[nodiscard]] constexpr RingCapacityProjection
project_ring_capacity(RingCapacityProjectionInput input) noexcept {
    const std::uint32_t free_pages =
        input.pool_pages > input.allocated_pages ? input.pool_pages - input.allocated_pages : 0U;
    const std::uint32_t want_free =
        input.required_pages + kRingSlackPages + input.preferred_restores;
    const std::uint32_t ceiling =
        input.pool_pages > kRingSlackPages + input.other_resident_pages
            ? input.pool_pages - kRingSlackPages - input.other_resident_pages
            : 0U;
    const std::uint32_t projected =
        input.lane_resident_pages + input.required_pages + input.preferred_restores;
    const std::uint32_t global_shortfall = want_free > free_pages ? want_free - free_pages : 0U;
    const std::uint32_t share_shortfall = projected > ceiling ? projected - ceiling : 0U;
    return {
        .free_pages        = free_pages,
        .ceiling_pages     = ceiling,
        .projected_pages   = projected,
        .share_shortfall   = share_shortfall,
        .target_free_pages = std::max(global_shortfall, share_shortfall),
        .required_pages    = input.required_pages,
    };
}

}  // namespace ninfer::models::qwen3_5::detail
