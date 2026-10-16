// Seam completion and budget-margin measurement: the two computations that decide WHICH pages the
// ring keeps and whether the budget line is what dropped the rest.
//
// Both were reachable only from ProgramImpl::select_retrieval_pages, so neither had a single test --
// which is the wrong place for them to live. They are closed computations over a page set, they have
// no device work and no sequence state, and D-5 / G4 both turn on their exact behaviour. They now sit
// in ops/kvmem/kvmem_page_selection.h and are qualified here against the written policy, not against
// the current implementation.
//
// The policy under test (context.cpp's D-5 note, and RingBudgetMargin's own comment):
//
//   add_seam_neighbors   walk the ranked pages best-first; for each one, take the page, then its
//                        FORWARD neighbours (1..reach), then its BACKWARD ones, skipping anything
//                        already taken and anything outside [begin, end); stop the moment the budget
//                        is full. reach == 0 is the negative control and must be the identity.
//   budget margin        measure how thin the budget's cut is, as the gap at the cut divided by the
//                        typical gap above it. ALWAYS writes: a budget that dropped nothing has no
//                        margin, and a stale reading must not survive to authorise growth.
#include "ops/kvmem/kvmem_page_selection.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace ninfer::ops::detail;

namespace {

void require(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

bool same(const std::vector<std::uint32_t>& got, const std::vector<std::uint32_t>& want) {
    return got == want;
}

void show(const char* name, const std::vector<std::uint32_t>& got) {
    std::fprintf(stderr, "  %s got [", name);
    for (const std::uint32_t page : got) { std::fprintf(stderr, " %u", page); }
    std::fprintf(stderr, " ]\n");
}

// require() with the actual value printed, so a red run names the page set it saw.
void require_pages(const std::vector<std::uint32_t>& got, const std::vector<std::uint32_t>& want,
                   const char* name) {
    if (!same(got, want)) { show(name, got); }
    require(same(got, want), name);
}

bool has_duplicates(const std::vector<std::uint32_t>& pages) {
    std::vector<std::uint32_t> sorted = pages;
    std::sort(sorted.begin(), sorted.end());
    return std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
}

void test_seam_completion() {
    constexpr std::uint32_t begin = 10, end = 20, ample = 64;

    // The negative control: with no reach there is nothing to complete, and the caller's ranked order
    // must come back untouched -- this is what NINFER_KV_NEIGHBOR_PAGES=0 measures.
    require_pages(add_seam_neighbors({15, 12, 18}, begin, end, ample, 0), {15, 12, 18},
                  "reach 0 is the identity");
    require_pages(add_seam_neighbors({}, begin, end, ample, 1), {}, "empty ranked stays empty");

    // One page, room to spare: the page and BOTH neighbours, forward first.
    require_pages(add_seam_neighbors({15}, begin, end, ample, 1), {15, 16, 14},
                  "one page completes to itself and both neighbours");

    // Forward wins the budget. The measured symptom is a truncated TAIL, so the page that follows is
    // the one worth buying when only one neighbour fits.
    require_pages(add_seam_neighbors({15}, begin, end, 2, 1), {15, 16},
                  "forward neighbour outranks backward at a tight budget");

    // The budget is a hard cap, not a target: reach 2 wants 5 pages and gets 3.
    require_pages(add_seam_neighbors({15}, begin, end, 3, 2), {15, 16, 14},
                  "budget caps the neighbourhood");
    require(add_seam_neighbors({15, 5, 18}, begin, end, 4, 2).size() == 4,
            "budget caps across several ranked pages");

    // Best-first: the first ranked page is completed before the second is even considered. Page 11's
    // backward neighbour is 10, which is still inside [begin, end), so it comes back too.
    require_pages(add_seam_neighbors({15, 11}, begin, end, ample, 1), {15, 16, 14, 11, 12, 10},
                  "completion follows ranked order");

    // Edges clamp instead of wrapping or underflowing.
    require_pages(add_seam_neighbors({10}, begin, end, ample, 1), {10, 11},
                  "no backward neighbour below begin");
    require_pages(add_seam_neighbors({19}, begin, end, ample, 1), {19, 18},
                  "no forward neighbour at end");

    // Pages outside the legal range are dropped, not completed: completing them would spend budget on
    // pages the ring cannot hold.
    require_pages(add_seam_neighbors({5, 15}, begin, end, ample, 1), {15, 16, 14},
                  "out-of-range page is skipped entirely");

    // Neighbours that are already present are not taken twice -- the result feeds a binary search.
    const auto dedup = add_seam_neighbors({15, 16}, begin, end, ample, 1);
    require(!has_duplicates(dedup), "no duplicate pages");
    require_pages(dedup, {15, 16, 14, 17}, "already-present neighbour is not re-added");

    // Property, over a spread of budgets and reaches: always in range, never duplicated, never over.
    // The caller truncates `ranked` to the budget before calling (context.cpp:1830), so the cap is
    // asserted on the completion path; reach == 0 is the identity by contract and returns its input.
    for (std::uint32_t reach = 0; reach <= 3; ++reach) {
        for (std::uint32_t budget = 1; budget <= 12; ++budget) {
            const auto out = add_seam_neighbors({15, 11, 18, 13}, begin, end, budget, reach);
            require(!has_duplicates(out), "property: never duplicated");
            for (const std::uint32_t page : out) {
                require(page >= begin && page < end, "property: always in range");
            }
            if (reach == 0) {
                require_pages(out, {15, 11, 18, 13}, "property: reach 0 identity");
            } else {
                require(out.size() <= budget, "property: never over budget");
            }
            // The caller's actual precondition: ranked already fits the budget.
            const auto capped = add_seam_neighbors({15, 11}, begin, end, 2, reach);
            require(capped.size() <= 2, "property: capped input never grows past the budget");
        }
    }
}

void test_budget_margin() {
    // The two overloads are indistinguishable to a braced-init-list, so every call names its type.
    // That is a property of the interface worth knowing: the float path is the content scorer's and
    // the double path is the lexical fallback's, and they are never interchangeable.
    using FRanked = std::vector<std::pair<float, std::uint32_t>>;
    using DRanked = std::vector<std::pair<double, std::uint32_t>>;

    KvMemBudgetMargin margin;

    // A budget that dropped nothing decided nothing. The reading must be CLEARED, not left stale --
    // a thin margin from an earlier reselection would otherwise authorise growth here.
    margin = KvMemBudgetMargin{.ratio = 0.9F, .valid = true};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {8.0F, 2}, {6.0F, 3}}, 3, &margin);
    require(!margin.valid && margin.ratio == 0.0F, "non-binding budget clears the margin");

    margin = KvMemBudgetMargin{.ratio = 0.9F, .valid = true};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {8.0F, 2}}, 7, &margin);
    require(!margin.valid && margin.ratio == 0.0F, "budget past the end clears the margin");

    margin = KvMemBudgetMargin{.ratio = 0.9F, .valid = true};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {8.0F, 2}}, 0, &margin);
    require(!margin.valid && margin.ratio == 0.0F, "empty cut clears the margin");

    kvmem_measure_budget_margin(FRanked{}, 0, nullptr);   // must not dereference a null out-param
    kvmem_measure_budget_margin(FRanked{{1.0F, 1}}, 1, nullptr);

    // Evenly spaced scores: the cut's gap equals the typical gap above it, so the ratio is 1.
    margin = KvMemBudgetMargin{};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {8.0F, 2}, {6.0F, 3}, {4.0F, 4}, {2.0F, 5}}, 3,
                                &margin);
    require(margin.valid, "binding budget yields a margin");
    require(margin.ratio > 0.999F && margin.ratio < 1.001F, "even spacing reads as ratio 1");

    // The failure the gate exists to catch: the kept pages are spread out, and the page just below
    // the line is nearly tied with the last kept one. gap 0.1 against a typical gap of 4.55.
    margin = KvMemBudgetMargin{};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {1.0F, 2}, {0.9F, 3}, {0.8F, 4}}, 3, &margin);
    require(margin.valid, "thin margin is still a valid reading");
    require(margin.ratio < 0.05F, "near-tie at the cut reads as a thin margin");

    // A wide gap at the cut against a tight field above it reads as a fat margin: the budget line is
    // not what decided, so growing it would buy nothing.
    margin = KvMemBudgetMargin{};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {9.9F, 2}, {9.8F, 3}, {1.0F, 4}}, 3, &margin);
    require(margin.valid && margin.ratio > 10.0F, "wide gap at the cut reads as a fat margin");

    // cut == 1 has no spread above it to normalise by, and must not divide by zero.
    margin = KvMemBudgetMargin{};
    kvmem_measure_budget_margin(FRanked{{10.0F, 1}, {1.0F, 2}}, 1, &margin);
    require(margin.valid && margin.ratio > 0.0F && margin.ratio < 1e30F, "cut of one is finite");

    // All-equal scores: gap 0 over a typical of 0, guarded by the floor rather than producing inf/nan.
    margin = KvMemBudgetMargin{};
    kvmem_measure_budget_margin(FRanked{{5.0F, 1}, {5.0F, 2}, {5.0F, 3}}, 2, &margin);
    require(margin.valid, "flat scores still read");
    require(margin.ratio == 0.0F, "flat scores read as zero margin");

    // The lexical fallback scores in double and has its own instantiation; it must agree with the
    // float one on the same input rather than being a second, drifting definition.
    KvMemBudgetMargin wide;
    kvmem_measure_budget_margin(DRanked{{10.0, 1}, {8.0, 2}, {6.0, 3}, {4.0, 4}, {2.0, 5}}, 3, &wide);
    require(wide.valid && wide.ratio > 0.999F && wide.ratio < 1.001F, "double path agrees on spacing");
}

} // namespace

int main() {
    test_seam_completion();
    test_budget_margin();
    std::fprintf(stderr, "kvmem page selection: ok\n");
    return 0;
}
