// Online cache release: scoped-release result vocabulary (isolation, batch-a).
// Per-layer released/skipped counters ride the existing CacheClearResult.detail
// as JSON; the struct below is the worker-side accumulator, not a wire type.
#pragma once
#include <array>
#include <cstdint>

namespace ninfer::runtime {

// One entry per CacheScope bit 0..6, in bit order:
// 0 device_main, 1 backend, 2 host_kv, 3 host_state, 4 shared_prefix,
// 5 continuation, 6 anchors.
struct ScopedReleaseStats {
    std::array<std::uint32_t, 7> released{};  // owners/pages/units actually freed
    std::array<std::uint32_t, 7> skipped{};   // no releasable content in that layer
    bool all_empty = true;                    // every requested layer skipped
};

}  // namespace ninfer::runtime
