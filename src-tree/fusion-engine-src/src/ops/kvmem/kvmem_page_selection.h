#pragma once

// Page-set completion and budget-margin measurement for the KVMem ring: the two closed computations
// that decide WHICH pages a reselection keeps, and whether the budget line is what dropped the rest.
//
// WHY THEY LIVE HERE (2026-10-16)
//   Both were file-local to ProgramImpl::select_retrieval_pages (models/qwen3_5/program/storage/
//   context.cpp), which put a pure page-set transformation behind a translation unit that pulls the
//   whole CUDA engine -- so neither had a single test, while both are load-bearing: add_seam_neighbors
//   is the D-5 seam-completeness fix and the margin is what authorises G4's budget growth. They own no
//   sequence state, no device work and no policy beyond what is written below, which is an Op, so they
//   belong with the other kvmem Ops and are qualified from tests/host/test_kvmem_page_selection.cpp.
//
//   The move is verbatim: same bodies, same order of operations, same literals. The float and double
//   overloads of kvmem_measure_budget_margin are deliberately NOT collapsed into one template -- the
//   double one is the lexical fallback's own path (the negative control) and its 1e-12 floor is a
//   double literal where the float one is 1e-12F; a shared definition would have to pick one and would
//   silently move a guard constant on one of the two paths.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <functional>
#include <vector>

namespace ninfer::ops::detail {

// LOCAL FIX (D-5 seam completeness, 2026-10-04): visibility is PAGE granularity, so evidence that
// straddles a page boundary is cut in half -- and the measured failure mode at a small pool is exactly
// that: the model answers with a TRUNCATED needle (`ZX-7001` -> `ZX-700`, `ARCH-4201` -> `ARCH-420`,
// 4/4 deterministic in the ledger's D-5 note; the pool-4000 multi-needle arm loses turn 2 on 4 of 6
// trials precisely because the needle's own page was brought back without its neighbour).
//
// That is what "a small window cannot hold a big question" looks like once it is measured: it is not
// that 63 pages cannot hold one needle -- it is that the resident set holds HALF of it. The cure does
// not need a bigger pool, it needs complete evidence for the pages the scorer picked.
//
// Policy: walk the ranked pages best-first and complete each one's neighbourhood while the budget
// lasts. Breadth is traded for completeness on purpose: 31 isolated pages become ~10 fully covered
// ones, and a needle needs 3 of them at most (its page plus the two the seam can be on). The budget
// stays the hard cap, so this never asks the ring to hold more than it already did.
inline std::vector<std::uint32_t> add_seam_neighbors(std::vector<std::uint32_t> ranked,
                                                     std::uint32_t begin, std::uint32_t end,
                                                     std::uint32_t budget, std::uint32_t reach) {
    std::vector<std::uint32_t> out;
    if (reach == 0 || budget == 0 || ranked.empty()) { return ranked; }
    out.reserve(ranked.size() + 2U * reach * ranked.size());
    const auto contains = [](const std::vector<std::uint32_t>& pages, std::uint32_t page) {
        return std::find(pages.begin(), pages.end(), page) != pages.end();
    };
    for (const std::uint32_t page : ranked) {
        if (page < begin || page >= end) { continue; }
        if (!contains(out, page)) { out.push_back(page); }
        for (std::uint32_t step = 1; step <= reach; ++step) {
            // FORWARD first: the measured symptom is a truncated TAIL ("...->  ZX-700"), and what is
            // missing sits in the page that follows. Backward is added next, for a needle whose
            // beginning was the part that fell on the far side of the seam.
            const std::uint32_t forward = page + step;
            if (forward < end && !contains(out, forward)) { out.push_back(forward); }
            if (out.size() >= budget) { break; }
            const std::uint32_t backward = page >= step ? page - step : 0U;
            if (page >= step && backward >= begin && !contains(out, backward)) {
                out.push_back(backward);
            }
            if (out.size() >= budget) { break; }
        }
        if (out.size() >= budget) { break; }
    }
    if (out.size() > budget) { out.resize(budget); }
    return out;
}

// D2 (Stage 5, 2026-10-17): score-gated seam completion. Same walk as add_seam_neighbors, but a seam
// neighbour is only bought when its own score clears `threshold` (or it is the evidence page itself).
// This turns the fixed 2*reach width tax into a data-dependent one: pages the scorer did not value are
// not pulled in just because they sit next to a kept page. `score_of(page)` returns the page's ranking
// score (0 if absent); `threshold` is typically the cut score * 0.5.
inline std::vector<std::uint32_t> add_seam_neighbors_gated(
    std::vector<std::uint32_t> ranked, std::uint32_t begin, std::uint32_t end,
    std::uint32_t budget, std::uint32_t reach,
    const std::function<float(std::uint32_t)>& score_of, float threshold) {
    std::vector<std::uint32_t> out;
    if (reach == 0 || budget == 0 || ranked.empty()) { return ranked; }
    out.reserve(ranked.size() + 2U * reach * ranked.size());
    const auto contains = [](const std::vector<std::uint32_t>& pages, std::uint32_t page) {
        return std::find(pages.begin(), pages.end(), page) != pages.end();
    };
    for (const std::uint32_t page : ranked) {
        if (page < begin || page >= end) { continue; }
        if (!contains(out, page)) { out.push_back(page); }
        for (std::uint32_t step = 1; step <= reach; ++step) {
            const std::uint32_t forward = page + step;
            if (forward < end && !contains(out, forward) &&
                (score_of == nullptr || score_of(forward) >= threshold)) {
                out.push_back(forward);
            }
            if (out.size() >= budget) { break; }
            const std::uint32_t backward = page >= step ? page - step : 0U;
            if (page >= step && backward >= begin && !contains(out, backward) &&
                (score_of == nullptr || score_of(backward) >= threshold)) {
                out.push_back(backward);
            }
            if (out.size() >= budget) { break; }
        }
        if (out.size() >= budget) { break; }
    }
    if (out.size() > budget) { out.resize(budget); }
    if (out.empty()) {
        // fall back to the non-gated result's shape on degenerate input
        out = ranked;
        if (out.size() > budget) { out.resize(budget); }
    }
    return out;
}

// How thin the budget's boundary is. `ratio` = the gap at the cut divided by the typical gap above it;
// the gate that reads it is ProgramImpl's RingBudgetMargin. Lives here rather than in program_impl.h so
// the measurement can be qualified without constructing a Program.
struct KvMemBudgetMargin {
    float ratio = 0.0F;
    bool  valid = false;
    // D3 (Stage 5, 2026-10-17): number of seam neighbours that sat just below the budget cut.
    // A high value means the budget line sliced through a cluster of related pages.
    std::uint32_t neighbor_dropped = 0;
};

// LOCAL (G7/G4 ring adaptive budget, 2026-10-15): measure how thin the budget's boundary is.
// `ranked` must be sorted by DESCENDING score and `budget` is how many entries the caller is about to
// keep. See RingBudgetMargin (program_impl.h) for what the ratio means and why it is normalised.
// ALWAYS WRITES, and clears `valid` when there is no boundary to measure -- either nothing was kept, or
// nothing was dropped because the budget covers the whole range. The clearing is the point: in both of
// those cases the budget line decided nothing, so there is NO margin to read. Keeping a previous
// reselection's reading there instead would let a razor-thin margin from an earlier chunk authorise
// growth on prompts where the budget is not binding at all -- the gate would climb on no evidence.
inline void kvmem_measure_budget_margin(
    const std::vector<std::pair<float, std::uint32_t>>& ranked, std::uint32_t budget,
    KvMemBudgetMargin* margin_out) {
    if (margin_out == nullptr) { return; }
    const std::size_t cut = std::min<std::size_t>(budget, ranked.size());
    if (cut == 0 || cut >= ranked.size()) {
        margin_out->ratio = 0.0F;
        margin_out->valid = false;
        return;
    }
    const float gap     = ranked[cut - 1].first - ranked[cut].first;   // >= 0: sorted descending
    const float steps   = static_cast<float>(cut > 1 ? cut - 1 : 1);
    const float typical = (ranked.front().first - ranked[cut - 1].first) / steps;
    const float floor_step = 1e-12F;   // guards the division only; it is not a tuned threshold
    margin_out->ratio = gap / (typical > floor_step ? typical : floor_step);
    margin_out->valid = true;
}

// The lexical fallback path scores in double (IDF sums), so it needs its own instantiation rather than a
// conversion copy -- the fallback is the negative control's own path and must stay untouched numerically.
inline void kvmem_measure_budget_margin(
    const std::vector<std::pair<double, std::uint32_t>>& ranked, std::uint32_t budget,
    KvMemBudgetMargin* margin_out) {
    if (margin_out == nullptr) { return; }
    const std::size_t cut = std::min<std::size_t>(budget, ranked.size());
    if (cut == 0 || cut >= ranked.size()) {
        margin_out->ratio = 0.0F;
        margin_out->valid = false;
        return;
    }
    const double gap     = ranked[cut - 1].first - ranked[cut].first;
    const double steps   = static_cast<double>(cut > 1 ? cut - 1 : 1);
    const double typical = (ranked.front().first - ranked[cut - 1].first) / steps;
    const double floor_step = 1e-12;
    margin_out->ratio = static_cast<float>(gap / (typical > floor_step ? typical : floor_step));
    margin_out->valid = true;
}

} // namespace ninfer::ops::detail
