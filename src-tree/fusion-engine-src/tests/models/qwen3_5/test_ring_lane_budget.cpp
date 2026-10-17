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

    return 0;
}
