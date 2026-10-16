#include "ops/kvmem/kvmem_lrfu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
using namespace ninfer::ops::detail;
namespace {
void require(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a-b) < 1e-6f; }
}
int main() {
    require(kvmem_lrfu_coverage_eligible(100, 200, 0, 300), "declared query tail eligible");
    require(!kvmem_lrfu_coverage_eligible(300, 303, 0, 300), "auxiliary tail excluded");
    require(!kvmem_lrfu_coverage_eligible(290, 310, 0, 300), "partial query coverage excluded");
    require(!kvmem_lrfu_coverage_eligible(100, 200, -1, -1), "unknown query excluded");
    const float hot[] = {0.9f, 0.f}, cold[] = {0.f, 0.f};
    KvMemLrfuRound a;
    kvmem_lrfu_round_begin(a, 1, {});
    require(kvmem_lrfu_round_add(a, 1, 10, 11, hot, 2, .5, 1.), "first evidence");
    auto checkpoint = kvmem_lrfu_round_accept(a, 1);
    require(checkpoint && near(checkpoint->ks[0], .9f), "checkpoint value");
    kvmem_lrfu_round_begin(a, 2, checkpoint);
    require(kvmem_lrfu_round_add(a, 2, 10, 11, cold, 2, .5, 1.), "same coords different round");
    require(!kvmem_lrfu_round_add(a, 2, 10, 11, hot, 2, .5, 1.), "duplicate rejected");
    require(kvmem_lrfu_round_add(a, 2, 11, 12, cold, 2, .5, 1.), "second chunk");
    auto second = kvmem_lrfu_round_accept(a, 2);
    require(near(second->ks[0], .45f) && second->round == 2, "one decay per accepted round");
    require(kvmem_lrfu_round_accept(a, 2) == second, "accept idempotent");
    KvMemLrfuRound fork;
    kvmem_lrfu_round_begin(fork, 7, checkpoint);
    kvmem_lrfu_round_add(fork, 7, 20, 21, cold, 2, .5, 1.);
    require(near(kvmem_lrfu_round_accept(fork, 7)->ks[0], .45f), "rollback excludes future history");
    require(near(checkpoint->ks[0], .9f), "immutable checkpoint");
    kvmem_lrfu_round_begin(a, 3, second);
    kvmem_lrfu_round_add(a, 3, 30, 31, hot, 2, .5, 1.);
    kvmem_lrfu_round_abort(a);
    require(a.history == second, "abort preserves base");
    kvmem_lrfu_round_begin(a, 4, second);
    require(kvmem_lrfu_round_accept(a, 4) == second, "no evidence no decay");
    kvmem_lrfu_round_begin(a, 5, {});
    require(!kvmem_lrfu_round_add(a, 5, -1, -1, hot, 2, .5, 1.), "invalid coverage rejected");
    require(!kvmem_lrfu_round_accept(a, 5), "fresh owner no inherited heat");
    // A capture may be published before its prefill round commits. It must retain the
    // pre-round snapshot until the matching round accepts, then publish that accepted
    // immutable history; a different request must never settle the checkpoint.
    std::optional<std::uint64_t> pending_capture = 6;
    std::shared_ptr<const KvMemLrfuHistory> captured_before_accept;
    require(kvmem_lrfu_checkpoint_settle(captured_before_accept, pending_capture, 5, checkpoint) ==
                nullptr &&
                pending_capture && *pending_capture == 6,
            "unrelated round cannot settle capture history");
    require(kvmem_lrfu_checkpoint_settle(captured_before_accept, pending_capture, 6, checkpoint) ==
                checkpoint &&
                !pending_capture,
            "matching accepted round settles capture history");
    // Independent summation oracle: [0.2,0.8] + [0.3,0.7] = [0.5,1.5].
    const float x[] = {.2f,.8f}, y[] = {.3f,.7f};
    kvmem_lrfu_round_begin(a, 6, {});
    kvmem_lrfu_round_add(a, 6, 40, 41, x, 2, .5, 1.);
    kvmem_lrfu_round_add(a, 6, 41, 42, y, 2, .5, 1.);
    auto total = kvmem_lrfu_round_accept(a, 6);
    require(near(total->ks[0], .5f) && near(total->ks[1], 1.5f), "chunks accumulate not overwrite");
    require(near(second->ks[0], .45f), "independent branches");
    // Missing KV blocks are unobserved, not zero-score evidence.
    kvmem_lrfu_round_begin(a, 8, total);
    const float short_scores[] = {0.f};
    require(kvmem_lrfu_round_add(a, 8, 50, 51, short_scores, 1, .5, 1.), "short coverage");
    auto short_history = kvmem_lrfu_round_accept(a, 8);
    require(short_history->ks.size() == 2 && near(short_history->ks[1], 1.5f),
            "unobserved blocks preserve history");
    kvmem_lrfu_round_begin(a, 9, {});
    const float huge[] = {3e38f};
    require(kvmem_lrfu_round_add(a, 9, 60, 61, huge, 1, .5, 1.), "finite large score");
    require(!kvmem_lrfu_round_add(a, 9, 61, 62, huge, 1, .5, 1.), "overflow rejected atomically");
    require(near(kvmem_lrfu_round_accept(a, 9)->ks[0] / huge[0], 1.f), "valid evidence retained");
    // Independent FP64 oracle at a realistic history size, two chunk partitions.
    constexpr std::size_t blocks = 4096;
    std::vector<float> first(blocks), last(blocks), whole(blocks);
    for (std::size_t i = 0; i < blocks; ++i) {
        first[i] = static_cast<float>(i % 31) / 128.f;
        last[i] = static_cast<float>(i % 19) / 256.f;
        whole[i] = first[i] + last[i];
    }
    KvMemLrfuRound split, unsplit;
    kvmem_lrfu_round_begin(split, 10, {});
    kvmem_lrfu_round_begin(unsplit, 11, {});
    require(kvmem_lrfu_round_add(split, 10, 100, 120, first.data(), blocks, .5, .9), "oracle first");
    require(kvmem_lrfu_round_add(split, 10, 120, 140, last.data(), blocks, .5, .9), "oracle last");
    require(kvmem_lrfu_round_add(unsplit, 11, 100, 140, whole.data(), blocks, .5, .9), "oracle whole");
    auto split_history = kvmem_lrfu_round_accept(split, 10);
    auto whole_history = kvmem_lrfu_round_accept(unsplit, 11);
    std::vector<double> oracle(blocks);
    for (std::size_t i = 0; i < blocks; ++i) { oracle[i] = double(first[i]) + double(last[i]); }
    auto sorted = oracle;
    std::sort(sorted.begin(), sorted.end());
    const double threshold = sorted[static_cast<std::size_t>((1. - .9) * blocks)];
    for (std::size_t i = 0; i < blocks; ++i) {
        const double expected = oracle[i] >= threshold ? oracle[i] : 0.;
        require(std::fabs(split_history->ks[i] - expected) < 1e-6, "FP64 sum/top-fraction oracle");
        require(near(split_history->ks[i], whole_history->ks[i]), "partition invariance");
    }
    require(!kvmem_lrfu_round_add(split, 10, 140, 141, hot, 2, .5, 1.), "late evidence rejected");
    kvmem_lrfu_round_begin(a, 12, second);
    require(!kvmem_lrfu_round_add(a, 13, 1, 2, hot, 2, .5, 1.), "stale token rejected");
    require(kvmem_lrfu_round_add(a, 12, 1, 3, cold, 2, .5, 1.), "valid token");
    require(!kvmem_lrfu_round_add(a, 12, 2, 4, hot, 2, .5, 1.), "partial overlap rejected");
    require(near(kvmem_lrfu_round_accept(a, 12)->ks[0], .225f), "third round decay");
    std::puts("PASS explicit rounds, isolation, snapshots, aggregation, FP64 oracle and abort");
    return 0;
}
