#pragma once
// Online cache release, design v1 (2026-10-14, isolation only).
// HTTP threads never touch the cache: they enqueue a CacheClearCmd and poll.
// The worker executes at most one op per boundary, while holding execution_mutex_.
// No production wiring yet: this header only defines the command vocabulary.
#include <chrono>
#include <cstdint>
#include <string>
namespace ninfer::runtime {
// Bitmask so a caller can scope which layers to release.
enum class CacheScope : std::uint32_t {
    DeviceMainKV = 1u << 0,
    BackendKV    = 1u << 1,
    HostKV       = 1u << 2,
    HostState    = 1u << 3,
    SharedPrefix = 1u << 4,
    Continuation = 1u << 5,
    Anchors      = 1u << 6,
    All          = 0x7Fu,
};
constexpr CacheScope operator|(CacheScope a, CacheScope b) noexcept {
    return static_cast<CacheScope>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr CacheScope operator&(CacheScope a, CacheScope b) noexcept {
    return static_cast<CacheScope>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool any_scope(CacheScope s) noexcept { return static_cast<std::uint32_t>(s) != 0; }
// Three roads, never mixed: idle-only refuse / drain-and-fail / abort-and-clear.
enum class ClearMode : std::uint8_t { TryIdle, DrainAndFail, AbortAndClear };
enum class CacheClearState : std::uint8_t { Queued, Running, Done, RefusedBusy, Failed };
struct CacheClearCmd {
    std::uint64_t id = 0;
    CacheScope scope = CacheScope::All;
    ClearMode mode = ClearMode::TryIdle;
    bool dry_run = false;
    std::string requested_by;
    std::chrono::steady_clock::time_point enqueued_at{};
};
struct CacheClearResult {
    std::uint64_t id = 0;
    CacheClearState state = CacheClearState::Queued;
    std::string detail;
    std::uint32_t in_flight = 0;
    bool materializing = false;
    bool has_transaction = false;
    std::uint32_t failed_requests = 0;
    std::uint64_t kv_tokens_before = 0;
    std::uint64_t kv_tokens_after = 0;
    std::uint64_t host_bytes_before = 0;
    std::uint64_t host_bytes_after = 0;
    std::uint64_t duration_ms = 0;
};
}  // namespace ninfer::runtime
