#include <cassert>

#include "models/qwen3_5/program/storage/ring_lane_budget.h"

using namespace ninfer::models::qwen3_5::detail;

int main() {
    // pool_pages: at one lane the whole pool.
    assert(ring_lane_pool_pages(256, 1) == 256);
    assert(ring_lane_pool_pages(256, 2) == 128);
    assert(ring_lane_pool_pages(256, 0) == 256); // guard against divide-by-zero

    // need_pages: sink + budget + slack.
    assert(ring_lane_need_pages(4, 16) == 4 + 16 + 8);

    // dynamic ceiling: pool - slack - other lanes' resident.
    assert(ring_lane_dynamic_ceiling(256, 0) == 256 - 8);
    assert(ring_lane_dynamic_ceiling(256, 120) == 256 - 8 - 120);
    // If other lanes consume everything, ceiling is 0 (no room).
    assert(ring_lane_dynamic_ceiling(16, 16) == 0);
    assert(ring_lane_dynamic_ceiling(10, 100) == 0);

    // A lane holding few pages lets another grow: 128 pool, other holds 10 -> ceiling 110.
    assert(ring_lane_dynamic_ceiling(128, 10) == 110);

    const RingCapacityProjection reclaim_required = project_ring_capacity({
        .pool_pages           = 16,
        .allocated_pages      = 16,
        .lane_resident_pages  = 8,
        .other_resident_pages = 4,
        .required_pages       = 1,
        .preferred_restores   = 2,
    });
    assert(reclaim_required.free_pages == 0 && reclaim_required.ceiling_pages == 4 &&
           reclaim_required.projected_pages == 11 && reclaim_required.target_free_pages == 11 &&
           reclaim_required.status_after(0) == RingCapacityStatus::InsufficientDevicePages &&
           reclaim_required.status_after(1) == RingCapacityStatus::Ready);

    const RingCapacityProjection already_ready = project_ring_capacity({
        .pool_pages           = 16,
        .allocated_pages      = 12,
        .lane_resident_pages  = 4,
        .other_resident_pages = 2,
        .required_pages       = 1,
        .preferred_restores   = 0,
    });
    assert(already_ready.free_pages == 4 && already_ready.ceiling_pages == 6 &&
           already_ready.projected_pages == 5 && already_ready.target_free_pages == 0 &&
           already_ready.status_after(1) == RingCapacityStatus::Ready);

    return 0;
}
