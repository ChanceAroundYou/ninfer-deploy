#pragma once

// Program owns retrieval rounds; checkpoints share immutable accepted history.
// Query coordinates describe evidence coverage, never round identity. Scores stay
// independent of the instantaneous selector and retain raw softmax-mass units.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ninfer::ops::detail {

inline bool kvmem_lrfu_enabled() noexcept {
    const char* v = std::getenv("NINFER_TERNARY_KVMEM_LRFU");
    return v != nullptr && v[0] != '\0' && std::strtol(v, nullptr, 10) != 0;
}

inline double kvmem_lrfu_env_real(const char* name, double fallback, bool allow_zero) noexcept {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') { return fallback; }
    char* end = nullptr;
    const double parsed = std::strtod(v, &end);
    if (end == v || *end != '\0' || !std::isfinite(parsed) || parsed > 1.0 ||
        (allow_zero ? parsed < 0.0 : parsed <= 0.0)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "kvmem_lrfu: invalid %s, falling back to %.4f\n", name, fallback);
        }
        return fallback;
    }
    return parsed;
}

inline double kvmem_lrfu_lambda() noexcept {
    return kvmem_lrfu_env_real("NINFER_TERNARY_KVMEM_LRFU_LAMBDA", 0.5, true);
}
inline double kvmem_lrfu_top_frac() noexcept {
    return kvmem_lrfu_env_real("NINFER_TERNARY_KVMEM_LRFU_TOP_FRAC", 0.9, false);
}

struct KvMemLrfuState {
    std::vector<float> ks;
    std::vector<float> ks_candidate;
    std::uint64_t ks_round = 0;
    bool has_candidate = false;
};

// Every preview derives from committed history, not the previous preview.
// A score vector covers logical blocks [0,n). Blocks beyond n were not observed
// and preserve their old value; newly observed blocks start with zero history.
inline void kvmem_lrfu_preview(KvMemLrfuState& st, const float* scores, std::size_t n,
                               double lambda, double top_frac) {
    st.ks_candidate.clear();
    st.has_candidate = false;
    if (scores == nullptr || n == 0 || !std::isfinite(lambda) || lambda < 0 || lambda > 1 ||
        !std::isfinite(top_frac) || top_frac <= 0 || top_frac > 1) { return; }
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(scores[i]) || scores[i] < 0) { return; }
    }
    double threshold = -INFINITY;
    if (top_frac < 1.0) {
        std::vector<float> ranked(scores, scores + n);
        const auto rank = std::min(n - 1, static_cast<std::size_t>((1.0 - top_frac) * n));
        std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(rank),
                         ranked.end());
        threshold = ranked[rank];
    }
    auto candidate = st.ks;
    candidate.resize(std::max(n, candidate.size()), 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double aged = i < st.ks.size() ? lambda * st.ks[i] : 0.0;
        const double next = aged + (scores[i] >= threshold ? scores[i] : 0.0);
        if (!std::isfinite(next) || next > std::numeric_limits<float>::max()) { return; }
        candidate[i] = static_cast<float>(next);
    }
    st.ks_candidate = std::move(candidate);
    st.has_candidate = true;
}

inline float kvmem_lrfu_score(const KvMemLrfuState& st, std::size_t block) noexcept {
    const auto& scores = st.has_candidate ? st.ks_candidate : st.ks;
    return block < scores.size() ? scores[block] : 0.0f;
}

inline bool kvmem_lrfu_coverage_eligible(std::int32_t begin, std::int32_t end,
                                         std::int32_t query_begin, std::int32_t query_end) noexcept {
    return query_begin >= 0 && query_end > query_begin && begin >= query_begin &&
           end > begin && end <= query_end;
}

struct KvMemLrfuHistory {
    std::vector<float> ks;
    std::uint64_t round = 0;
};

struct KvMemLrfuRound {
    std::shared_ptr<const KvMemLrfuHistory> history;
    KvMemLrfuState preview;
    std::vector<float> evidence;
    std::vector<std::pair<std::int32_t, std::int32_t>> coverage;
    std::uint64_t token = 0;
    bool active = false;
};

inline void kvmem_lrfu_round_begin(KvMemLrfuRound& round, std::uint64_t token,
                                   std::shared_ptr<const KvMemLrfuHistory> history) {
    round = {};
    round.history = std::move(history);
    round.token = token;
    round.active = token != 0;
    if (round.history) {
        round.preview.ks = round.history->ks;
        round.preview.ks_round = round.history->round;
    }
}

// Aggregated vectors cannot be split after scoring: reject partial overlap as
// well as exact duplicates rather than count any query token twice.
inline bool kvmem_lrfu_round_add(KvMemLrfuRound& round, std::uint64_t token,
                                 std::int32_t begin, std::int32_t end, const float* scores,
                                 std::size_t n, double lambda, double top_frac) {
    if (!round.active || token != round.token || begin < 0 || end <= begin ||
        scores == nullptr || n == 0) { return false; }
    for (const auto& span : round.coverage) {
        if (begin < span.second && end > span.first) { return false; }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const double old = i < round.evidence.size() ? round.evidence[i] : 0.0;
        if (!std::isfinite(scores[i]) || scores[i] < 0 ||
            old + scores[i] > std::numeric_limits<float>::max()) { return false; }
    }
    auto evidence = round.evidence;
    evidence.resize(std::max(n, evidence.size()), 0.0f);
    for (std::size_t i = 0; i < n; ++i) { evidence[i] += scores[i]; }
    auto preview = round.preview;
    kvmem_lrfu_preview(preview, evidence.data(), evidence.size(), lambda, top_frac);
    if (!preview.has_candidate) { return false; }
    round.coverage.emplace_back(begin, end);
    round.evidence = std::move(evidence);
    round.preview = std::move(preview);
    return true;
}

inline std::shared_ptr<const KvMemLrfuHistory>
kvmem_lrfu_round_accept(KvMemLrfuRound& round, std::uint64_t token) {
    if (!round.active || token != round.token) { return round.history; }
    if (round.preview.has_candidate) {
        auto next = std::make_shared<KvMemLrfuHistory>();
        next->ks = round.preview.ks_candidate;
        next->round = (round.history ? round.history->round : 0) + 1;
        round.preview.ks = next->ks;
        round.preview.ks_round = next->round;
        round.preview.ks_candidate.clear();
        round.preview.has_candidate = false;
        round.history = std::move(next);
    }
    round.active = false;
    round.evidence.clear();
    round.coverage.clear();
    return round.history;
}

// A checkpoint can be captured while its request's prefill round is still in flight.
// It records that round token and settles exactly once when the matching Begin commit
// accepts the immutable history; other requests cannot overwrite it.
inline std::shared_ptr<const KvMemLrfuHistory>
kvmem_lrfu_checkpoint_settle(std::shared_ptr<const KvMemLrfuHistory>& history,
                              std::optional<std::uint64_t>& pending_token,
                              std::uint64_t accepted_token,
                              std::shared_ptr<const KvMemLrfuHistory> accepted_history) {
    if (!pending_token || *pending_token != accepted_token) { return history; }
    history = std::move(accepted_history);
    pending_token.reset();
    return history;
}

inline void kvmem_lrfu_round_abort(KvMemLrfuRound& round) {
    const auto history = round.history;
    const auto token = round.token;
    kvmem_lrfu_round_begin(round, token, history);
    round.active = false;
}

} // namespace ninfer::ops::detail
