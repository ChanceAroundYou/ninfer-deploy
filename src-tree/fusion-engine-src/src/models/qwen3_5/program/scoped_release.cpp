// Scoped (per-layer) cache release implementation, isolation batch-a.
//
// Lives INSIDE ProgramImpl because handle validation (valid_continuation /
// valid_shared_prefix, storage/context.cpp) requires owner(handle) == this:
// the Engine cannot fabricate handles from outside. Runs entirely under the
// caller's execution_mutex_; preconditions mirror release_continuation /
// release_shared_prefix (no live context transaction, no pending transaction).
//
// Layer map (CacheScope bits, runtime/engine/cache_clear.h):
//   bit0 device_main   text KV pages of retained owners only
//   bit1 backend       backend KV pages of retained owners only (dflash/mtp page
//                      bundles ride inside SequenceKVBundle; text untouched)
//   bit2 host_kv       HostKVExtentStore opportunity回收 (release_unreferenced)
//   bit3 host_state    device/host StateImage replicas of retained owners
//                      (drop_device_replica/drop_host_replica; checkpoints kept)
//   bit4 shared_prefix whole catalogued shared-prefix owners (strict)
//   bit5 continuation  whole catalogued continuation owners (strict)
//   bit6 anchors       long-anchor checkpoints of retained owners only
//                      (endpoint/rewrite/reserved states kept)
//
// Layers with no releasable content count stats.skipped, never fail the op.
// Whole-owner layers (bit4/5) use the strict path: can_* first, then release;
// a failed can_* counts skipped (never terminate, never half-clear).

#include "models/qwen3_5/program/program_impl.h"

#include "runtime/engine/scoped_release_stats.h"

namespace ninfer::models::qwen3_5::detail {

namespace {

constexpr std::uint32_t kScopeDeviceMain = 1u << 0;
constexpr std::uint32_t kScopeBackend = 1u << 1;
constexpr std::uint32_t kScopeHostKV = 1u << 2;
constexpr std::uint32_t kScopeHostState = 1u << 3;
constexpr std::uint32_t kScopeSharedPrefix = 1u << 4;
constexpr std::uint32_t kScopeContinuation = 1u << 5;
constexpr std::uint32_t kScopeAnchors = 1u << 6;

}  // namespace

bool ProgramImpl::scoped_release(std::uint32_t scope_mask,
                                 runtime::ScopedReleaseStats& stats) noexcept {
    stats = runtime::ScopedReleaseStats{};
    const std::uint32_t mask = scope_mask & 0x7Fu;
    if (mask == 0) {
        return true;
    }
    // Preconditions: same as release_continuation/release_shared_prefix.
    // On violation return false so the caller refuses instead of half-clearing.
    try {
        if (has_context_transaction() || pending_transaction_) {
            return false;
        }
    } catch (...) {
        return false;
    }

    const ActiveExclusiveBaseline baseline = active_exclusive_baseline();

    // ---- bit5/4: whole retained owners, strict path (mirrors fail_all_cleanup loops) ----
    if ((mask & kScopeContinuation) != 0) {
        std::uint32_t n = 0;
        for (std::uint32_t index = 0; index < continuation_capacity; ++index) {
            if (continuation_slots[index].role == ContinuationSlotRole::Free) {
                continue;
            }
            bool ok = false;
            try {
                ok = can_release_continuation_slot_strict(index);
            } catch (...) {
                ok = false;
            }
            if (!ok) {
                ++stats.skipped[5];
                continue;
            }
            try {
                release_continuation_slot_strict(index);
                ++n;
            } catch (...) {
                ++stats.skipped[5];
            }
        }
        stats.released[5] = n;
    }
    if ((mask & kScopeSharedPrefix) != 0) {
        std::uint32_t n = 0;
        for (std::uint32_t index = 0; index < shared_prefix_capacity; ++index) {
            if (shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued) {
                continue;
            }
            bool ok = false;
            try {
                ok = can_release_shared_prefix_state(index, SharedPrefixSlotRole::Catalogued);
            } catch (...) {
                ok = false;
            }
            if (!ok) {
                ++stats.skipped[4];
                continue;
            }
            try {
                (void)release_shared_prefix_state_strict(index, SharedPrefixSlotRole::Catalogued);
                ++n;
            } catch (...) {
                ++stats.skipped[4];
            }
        }
        stats.released[4] = n;
    }

    // ---- bit0/1/3/6: sub-owner slices of retained continuation sequences ----
    // Whole-owner layers above already freed their sequences; the slices below only
    // touch survivors (still Catalogued). Bit ordering vs bit4/5 is irrelevant: a
    // slot freed above is Free now and skipped here by the role check.
    const bool want_kv = (mask & (kScopeDeviceMain | kScopeBackend | kScopeHostKV)) != 0;
    const bool want_state = (mask & kScopeHostState) != 0;
    const bool want_anchors = (mask & kScopeAnchors) != 0;
    if (want_kv || want_state || want_anchors) {
        for (std::uint32_t index = 0; index < continuation_capacity; ++index) {
            if (continuation_slots[index].role != ContinuationSlotRole::Catalogued) {
                continue;
            }
            SequenceState& sequence = continuation_states[index];
            if (!sequence.kv && want_kv) {
                if ((mask & kScopeDeviceMain) != 0) {
                    ++stats.skipped[0];
                }
                if ((mask & kScopeBackend) != 0) {
                    ++stats.skipped[1];
                }
            }
            // text-only (bit0): release the text bundle, keep backend + state.
            // Guarded by can_release: when the text row is still referenced
            // (e.g. pinned by the catalog summary path), count skipped instead of
            // forcing a release whose pages outlive the owner.
            // INVARIANT (checkpoint_recovery_work): a Catalogued slot must own a KV
            // bundle. A backend-less owner whose text is released would become a
            // kv-less zombie that 500s the next admission, so retire the slot
            // (whole-owner release) instead; the reconciler drops it by slot role.
            if ((mask & kScopeDeviceMain) != 0 && sequence.kv) {
                bool done = false;
                try {
                    if (text_kv_addresses &&
                        text_kv_addresses->can_release(sequence.kv->text)) {
                        const bool has_backend = sequence.kv->backend.has_value();
                        unbind_sequence_kv(sequence);
                        done = text_kv_addresses->release(sequence.kv->text);
                        if (done && !has_backend) {
                            // No backend to keep the owner alive: retire the slot
                            // rather than leaving a kv-less Catalogued zombie.
                            sequence.kv.reset();
                            release_sequence_state(sequence);
                            retire_continuation_slot(index);
                        }
                    }
                } catch (...) {
                    done = false;
                }
                if (done) {
                    ++stats.released[0];
                } else {
                    ++stats.skipped[0];
                }
            }
            // backend-only (bit1): release the backend bundle, keep text + state.
            if ((mask & kScopeBackend) != 0) {
                if (sequence.kv && sequence.kv->backend && backend_kv_addresses) {
                    bool done = false;
                    try {
                        if (backend_kv_addresses->can_release(*sequence.kv->backend)) {
                            done = backend_kv_addresses->release(*sequence.kv->backend);
                            if (done) {
                                sequence.kv->backend.reset();
                            }
                        }
                    } catch (...) {
                        done = false;
                    }
                    if (done) {
                        ++stats.released[1];
                    } else {
                        ++stats.skipped[1];
                    }
                } else {
                    ++stats.skipped[1];
                }
            }
            // host-state slice (bit3): drop device/host replicas of retained states.
            // Checkpoint references are kept (checkpoints survive a host_state clear).
            if (want_state) {
                std::uint32_t dropped = 0;
                try {
                    if (state_store) {
                        auto drop = [&](StateImageHandle h) {
                            if (!state_store->valid(h)) {
                                return;
                            }
                            if (state_store->drop_device_replica(h)) {
                                ++dropped;
                            }
                            if (state_store->drop_host_replica(h)) {
                                ++dropped;
                            }
                        };
                        drop(sequence.state.write);
                        if (sequence.state.read != sequence.state.write) {
                            drop(sequence.state.read);
                        }
                        if (sequence.rewrite_state) {
                            drop(*sequence.rewrite_state);
                        }
                        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
                            drop(anchor.state);
                        }
                        if (sequence.reserved_state) {
                            drop(*sequence.reserved_state);
                        }
                    }
                } catch (...) {
                }
                if (dropped != 0) {
                    stats.released[3] += dropped;
                } else {
                    ++stats.skipped[3];
                }
            }
            // anchors-only (bit6): release anchor checkpoint references + images,
            // keep endpoint/rewrite/reserved states.
            if (want_anchors) {
                if (sequence.long_anchors.empty()) {
                    ++stats.skipped[6];
                } else {
                    std::uint32_t n = 0;
                    try {
                        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
                            if (state_store && state_store->valid(anchor.state)) {
                                if (state_store->checkpoint_references(anchor.state) != 0) {
                                    state_store->release_checkpoint_reference(anchor.state);
                                }
                                (void)state_store->release(anchor.state);
                                ++n;
                            }
                        }
                        sequence.long_anchors.clear();
                    } catch (...) {
                    }
                    if (n != 0) {
                        stats.released[6] += n;
                    } else {
                        ++stats.skipped[6];
                    }
                }
            }
        }
        // host-kv opportunity回收 (bit2): unreferenced extents after the slices above.
        if ((mask & kScopeHostKV) != 0) {
            std::size_t freed = 0;
            try {
                if (host_kv_extents) {
                    freed = host_kv_extents->release_unreferenced();
                }
            } catch (...) {
            }
            if (freed != 0) {
                stats.released[2] += static_cast<std::uint32_t>(freed);
            } else {
                ++stats.skipped[2];
            }
        }
    }

    // Shared-prefix sub-slices (bit0/1/3 for shared owners): whole shared owners went
    // through bit4 above; when bit4 is NOT requested but KV/state slices are, release
    // the slices of surviving catalogued shared states.
    const bool shared_slices =
        !((mask & kScopeSharedPrefix) != 0) &&
        ((mask & (kScopeDeviceMain | kScopeBackend | kScopeHostState)) != 0);
    if (shared_slices) {
        for (std::uint32_t index = 0; index < shared_prefix_capacity; ++index) {
            if (shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued) {
                continue;
            }
            SharedPrefixState& shared = shared_prefix_states[index];
            if (!shared.kv) {
                continue;
            }
            if ((mask & kScopeDeviceMain) != 0 && text_kv_addresses) {
                try {
                    if (text_kv_addresses->can_release(shared.kv->text) &&
                        text_kv_addresses->release(shared.kv->text)) {
                        if (!shared.kv->backend) {
                            // Same invariant as above: a backend-less shared owner
                            // without text KV is unrecoverable; retire it strictly.
                            if (can_release_shared_prefix_state(
                                    index, SharedPrefixSlotRole::Catalogued)) {
                                (void)release_shared_prefix_state_strict(
                                    index, SharedPrefixSlotRole::Catalogued);
                            }
                        }
                        ++stats.released[0];
                    } else {
                        ++stats.skipped[0];
                    }
                } catch (...) {
                    ++stats.skipped[0];
                }
            }
            if ((mask & kScopeBackend) != 0 && shared.kv->backend && backend_kv_addresses) {
                try {
                    if (backend_kv_addresses->can_release(*shared.kv->backend) &&
                        backend_kv_addresses->release(*shared.kv->backend)) {
                        ++stats.released[1];
                    } else {
                        ++stats.skipped[1];
                    }
                } catch (...) {
                    ++stats.skipped[1];
                }
            }
            if ((mask & kScopeHostState) != 0 && state_store &&
                state_store->valid(shared.state)) {
                std::uint32_t dropped = 0;
                try {
                    if (state_store->drop_device_replica(shared.state)) {
                        ++dropped;
                    }
                    if (state_store->drop_host_replica(shared.state)) {
                        ++dropped;
                    }
                } catch (...) {
                }
                if (dropped != 0) {
                    stats.released[3] += dropped;
                } else {
                    ++stats.skipped[3];
                }
            }
        }
        if (host_kv_extents && (mask & kScopeHostKV) != 0) {
            try {
                (void)host_kv_extents->release_unreferenced();
            } catch (...) {
            }
        }
    }

    credit_active_ownership_transfers(baseline);
    try {
        advance_resource_revision();
    } catch (...) {
    }
    stats.all_empty = true;
    for (std::uint32_t b = 0; b < 7; ++b) {
        if ((mask & (1u << b)) != 0 && stats.released[b] != 0) {
            stats.all_empty = false;
        }
    }
    return true;
}

}  // namespace ninfer::models::qwen3_5::detail
