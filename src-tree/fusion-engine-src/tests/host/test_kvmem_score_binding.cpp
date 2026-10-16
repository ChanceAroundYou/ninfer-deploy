// Who owns the retrieval score probe.
//
// The probe carries the score buffer, the selection the last finished round published (`kept`), and the
// query-span state. It is read back by ProgramImpl::select_retrieval_pages as the ring's `preferred`
// set, so a stale probe is not a stale counter -- it is the engine restoring the pages a DIFFERENT
// request asked for.
//
// The contract on `kept` is that a round which did not score keeps the previous selection, which is
// what makes an unowned probe dangerous: "previous" is then the previous SEQUENCE. These tests pin the
// ownership down to SequenceState::kvmem_score_workspace, so the accessor reports the absence of a
// binding instead of handing back a process-resident one.
//
// No CUDA call is made: the probe's device pointers stay null, so its destructor does not free
// anything. This runs wherever the test suite runs.
#include "ops/kvmem/kvmem_score.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ninfer::ops::detail;

namespace {

void require(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

void test_probe_is_owned_by_the_sequence() {
    require(kvmem_score_probe(0) == nullptr, "an unbound lane reports no probe");

    auto first = std::make_shared<KvMemScoreProbe>();
    kvmem_score_bind(0, first, nullptr);
    require(kvmem_score_probe(0) == first.get(), "a bound lane returns its own probe");

    // The selection a finished round published, as the ring would read it.
    first->kept            = {7, 8, 9};
    first->scored_n_blocks = 716;
    require(kvmem_score_probe(0)->kept.size() == 3, "the published selection is readable");

    kvmem_score_unbind(0, first);
    require(kvmem_score_probe(0) == nullptr, "unbinding removes the probe rather than falling back");

    // THE REGRESSION (2026-10-16). Two requests admitted on lane 0 one after the other share the lane.
    // With a process-resident fallback the second one read back {7, 8, 9} -- the pages the FIRST
    // request asked for -- and the ring would have restored them. This is the assertion that fails if
    // the fallback ever comes back.
    auto second = std::make_shared<KvMemScoreProbe>();
    kvmem_score_bind(0, second, nullptr);
    require(kvmem_score_probe(0) == second.get(), "the next sequence gets its own probe");
    require(kvmem_score_probe(0)->kept.empty(),
            "the next sequence on the same lane inherits no selection");

    // Ownership is identity, not position: unbinding a probe that is not the bound one is a no-op.
    kvmem_score_unbind(0, first);
    require(kvmem_score_probe(0) == second.get(), "a foreign unbind leaves the binding alone");
    kvmem_score_unbind(0, second);
    require(kvmem_score_probe(0) == nullptr, "the owner's unbind clears the binding");
}

void test_lanes_and_rounds() {
    auto lane_three = std::make_shared<KvMemScoreProbe>();
    kvmem_score_bind(3, lane_three, nullptr);
    require(kvmem_score_probe(0) == nullptr && kvmem_score_probe(3) == lane_three.get(),
            "lanes do not share a probe");
    kvmem_score_unbind(3, lane_three);

    // The LRFU round is a member of the sequence, so it must not outlive the binding: a probe held by
    // a reader mid-call would otherwise expose a dangling pointer to a destroyed sequence.
    KvMemLrfuRound round;
    auto with_round = std::make_shared<KvMemScoreProbe>();
    kvmem_score_bind(1, with_round, &round);
    require(with_round->lrfu == &round, "bind publishes the round");
    require(kvmem_score_lrfu_round(1) == round.preview.ks_round, "the round is readable while bound");
    kvmem_score_unbind(1, with_round);
    require(with_round->lrfu == nullptr, "unbind drops the round pointer with the binding");
    require(kvmem_score_lrfu_round(1) == 0, "an unbound lane reports no round");

    // Scoring accessors must be safe, and do nothing, when no sequence owns the lane.
    kvmem_score_set_query_span(2, 10, 20);
    kvmem_score_set_chunk_origin(2, 0);
    kvmem_score_set_action_span(2, 30, 40, "test");
    kvmem_score_set_harvest_layers(2, 3, 4);
    require(kvmem_score_rank_key(2, {1.0F, 2.0F}, 1) == 2.0F,
            "the rank key falls back to the round's own score when unbound");
    require(kvmem_score_lrfu_score(2, 1) == 0.0F, "no round means no LRFU score");
}

} // namespace

int main() {
    test_probe_is_owned_by_the_sequence();
    test_lanes_and_rounds();
    std::fprintf(stderr, "kvmem score binding: ok\n");
    return 0;
}
