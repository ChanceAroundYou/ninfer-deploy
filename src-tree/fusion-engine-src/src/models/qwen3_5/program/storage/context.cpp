#include "models/qwen3_5/program/program_impl.h"
#include "models/qwen3_5/program/storage/ring_lane_budget.h"
#include "models/qwen3_5/program/context_work.h"
#include "core/device.h"
// LOCAL (KVMem content scoring, 2026-10-02): the scorer's published selection + scores. Header-only,
// host-safe (it pulls cuda_runtime, not a CUDA kernel header), so no build wiring is needed.
#include "ops/kvmem/kvmem_page_selection.h"
#include "ops/kvmem/kvmem_score.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace ninfer::models::qwen3_5::detail {

bool ProgramImpl::valid_sequence(SequenceHandle handle) const noexcept {
    if (ContractAccess::owner(handle) != this) { return false; }
    const std::uint32_t lane = ContractAccess::lane(handle).value;
    if (lane >= max_concurrency || ContractAccess::epoch(handle) != lane_epochs[lane]) {
        return false;
    }
    if (active_continuations[lane] >= continuation_capacity ||
        continuation_slots[active_continuations[lane]].role != ContinuationSlotRole::Active) {
        return false;
    }
    const Lifecycle lifecycle = requests[lane].lifecycle;
    return lifecycle == Lifecycle::Prefilling || lifecycle == Lifecycle::Active ||
           lifecycle == Lifecycle::Pending || lifecycle == Lifecycle::Finishable;
}

bool ProgramImpl::valid_continuation(const ContinuationHandle& handle) const noexcept {
    if (ContractAccess::owner(handle) != this) { return false; }
    const std::uint32_t index = ContractAccess::index(handle);
    return index < continuation_capacity &&
           ContractAccess::epoch(handle) == continuation_slots[index].generation &&
           continuation_slots[index].role == ContinuationSlotRole::Catalogued;
}

bool ProgramImpl::valid_shared_prefix(const SharedPrefixHandle& handle) const noexcept {
    if (ContractAccess::owner(handle) != this) { return false; }
    const std::uint32_t index = ContractAccess::index(handle);
    return index < shared_prefix_capacity &&
           ContractAccess::epoch(handle) == shared_prefix_slots[index].generation &&
           shared_prefix_slots[index].role == SharedPrefixSlotRole::Catalogued;
}

bool ProgramImpl::valid_capture_offer(const CaptureOffer& offer) const noexcept {
    if (ContractAccess::owner(offer) != this) { return false; }
    const std::uint32_t lane = ContractAccess::lane(offer).value;
    if (lane >= max_concurrency || ContractAccess::epoch(offer) != lane_epochs[lane] ||
        (requests[lane].lifecycle != Lifecycle::Prefilling &&
         requests[lane].lifecycle != Lifecycle::Active) ||
        !requests[lane].prefill) {
        return false;
    }
    const RequestControl::Prefill& prefill = *requests[lane].prefill;
    return prefill.pending_capture_offer != 0 &&
           prefill.pending_capture_offer == ContractAccess::id(offer) &&
           prefill.next_capture < prefill.capture_groups.size() &&
           prefill.cursor == prefill.capture_groups[prefill.next_capture].frontier;
}

bool ProgramImpl::materialization_pins(std::uint32_t index,
                                       std::uint64_t generation) const noexcept {
    const MaterializationTransaction* transaction_ptr =
        std::get_if<MaterializationTransaction>(&context_transaction_);
    if (transaction_ptr == nullptr) { return false; }
    const MaterializationTransaction& transaction = *transaction_ptr;
    if (transaction.has_source && transaction.source_index == index &&
        transaction.source_generation == generation) {
        return true;
    }
    for (std::size_t victim = 0; victim < transaction.victim_count; ++victim) {
        if (!transaction.victim_released[victim] && transaction.victim_indices[victim] == index &&
            transaction.victim_generations[victim] == generation) {
            return true;
        }
    }
    return false;
}

bool ProgramImpl::valid_pending(const PendingBatch& pending) const noexcept {
    if (ContractAccess::owner(pending) != this || !pending_transaction_ ||
        ContractAccess::transaction(pending) != pending_transaction_->id) {
        return false;
    }
    const auto rows = ContractAccess::rows(pending);
    if (rows.size() != pending_transaction_->size) { return false; }
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (!valid_sequence(rows[row]) ||
            ContractAccess::lane(rows[row]).value != pending_transaction_->lanes[row] ||
            ContractAccess::epoch(rows[row]) != pending_transaction_->epochs[row] ||
            requests[pending_transaction_->lanes[row]].lifecycle != Lifecycle::Pending) {
            return false;
        }
    }
    return true;
}

void ProgramImpl::invalidate_lane(std::uint32_t lane) noexcept {
    if (lane >= max_concurrency) { return; }
    ++lane_epochs[lane];
    if (lane_epochs[lane] == 0) { ++lane_epochs[lane]; }
}

SequenceState& ProgramImpl::active_sequence(std::uint32_t lane) {
    if (lane >= max_concurrency) { throw std::out_of_range("active lane is out of range"); }
    const std::uint32_t index = active_continuations[lane];
    if (index >= continuation_capacity ||
        continuation_slots[index].role != ContinuationSlotRole::Active) {
        throw std::logic_error("active lane has no continuation binding");
    }
    return continuation_states[index];
}

const SequenceState& ProgramImpl::active_sequence(std::uint32_t lane) const {
    if (lane >= max_concurrency) { throw std::out_of_range("active lane is out of range"); }
    const std::uint32_t index = active_continuations[lane];
    if (index >= continuation_capacity ||
        continuation_slots[index].role != ContinuationSlotRole::Active) {
        throw std::logic_error("active lane has no continuation binding");
    }
    return continuation_states[index];
}

std::optional<std::uint32_t> ProgramImpl::allocate_continuation_slot() noexcept {
    for (std::uint32_t index = 0; index < continuation_capacity; ++index) {
        if (continuation_slots[index].role == ContinuationSlotRole::Free) {
            continuation_slots[index].role = ContinuationSlotRole::Active;
            return index;
        }
    }
    return std::nullopt;
}

bool ProgramImpl::can_release_continuation_slot_strict(std::uint32_t index) const {
    if (index >= continuation_capacity || !state_store || !text_kv_addresses || !text_kv_pages ||
        continuation_slots[index].role != ContinuationSlotRole::Catalogued) {
        return false;
    }
    const SequenceState& sequence = continuation_states[index];
    if (sequence.state.fork_pending || !sequence.shared_prefix_references.empty() || !sequence.kv ||
        !text_kv_addresses->can_release(sequence.kv->text)) {
        return false;
    }
    if (sequence.kv->backend) {
        if (!backend_kv_addresses || !backend_kv_pages ||
            !backend_kv_addresses->can_release(*sequence.kv->backend)) {
            return false;
        }
    }

    const auto validate_state = [&](StateImageHandle handle, bool release_object) {
        if (!state_store->valid(handle)) { return false; }
        const std::uint32_t owned = owned_checkpoint_references(sequence, handle);
        const std::uint32_t total = state_store->checkpoint_references(handle);
        if (owned > total ||
            (owned != 0 && state_store->role(handle) != StateImageRole::CheckpointImmutable)) {
            return false;
        }
        return !release_object || total != owned ||
               state_store->can_release_after_checkpoint_references(handle, owned);
    };
    const auto repeated_before_anchor = [&](std::size_t anchor_index, StateImageHandle handle) {
        if (handle == sequence.state.read || handle == sequence.state.write ||
            (sequence.rewrite_state && handle == *sequence.rewrite_state)) {
            return true;
        }
        for (std::size_t prior = 0; prior < anchor_index; ++prior) {
            if (sequence.long_anchors[prior].state == handle) { return true; }
        }
        return false;
    };

    if (sequence.endpoint_valid) {
        if (!validate_state(sequence.state.read, !sequence.state.read_has_external_owner() ||
                                                     sequence.state.read == sequence.state.write)) {
            return false;
        }
        if (sequence.state.write != sequence.state.read &&
            !validate_state(sequence.state.write, true)) {
            return false;
        }
    } else if (state_store->valid(sequence.state.read) ||
               state_store->valid(sequence.state.write) || sequence.state.borrows_read()) {
        return false;
    }
    if (sequence.rewrite_state && *sequence.rewrite_state != sequence.state.read &&
        *sequence.rewrite_state != sequence.state.write &&
        !validate_state(*sequence.rewrite_state, true)) {
        return false;
    }
    for (std::size_t anchor = 0; anchor < sequence.long_anchors.size(); ++anchor) {
        const StateImageHandle handle = sequence.long_anchors[anchor].state;
        if (!repeated_before_anchor(anchor, handle) && !validate_state(handle, true)) {
            return false;
        }
    }
    if (sequence.reserved_state) {
        const StateImageHandle handle = *sequence.reserved_state;
        bool repeated = handle == sequence.state.read || handle == sequence.state.write ||
                        (sequence.rewrite_state && handle == *sequence.rewrite_state);
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            repeated = repeated || anchor.state == handle;
        }
        if (!repeated && !validate_state(handle, true)) { return false; }
    }
    return true;
}

void ProgramImpl::release_continuation_slot_strict(std::uint32_t index,
                                                   bool written_to_disk) noexcept {
    try {
        if (!can_release_continuation_slot_strict(index)) { std::terminate(); }
    } catch (...) { std::terminate(); }
    const ActiveExclusiveBaseline baseline = active_exclusive_baseline();
    SequenceState& sequence                = continuation_states[index];
    release_sequence_kv_strict(sequence, written_to_disk);
    release_sequence_state_strict(sequence);
    retire_continuation_slot(index);
    credit_active_ownership_transfers(baseline);
}

void ProgramImpl::release_continuation_slot_best_effort(std::uint32_t index) noexcept {
    if (index >= continuation_capacity ||
        continuation_slots[index].role == ContinuationSlotRole::Free) {
        return;
    }
    const ActiveExclusiveBaseline baseline = active_exclusive_baseline();
    SequenceState& sequence                = continuation_states[index];
    release_active_shared_references(sequence);
    release_sequence_kv(sequence);
    release_sequence_state(sequence);
    retire_continuation_slot(index);
    credit_active_ownership_transfers(baseline);
}

void ProgramImpl::retire_continuation_slot(std::uint32_t index) noexcept {
    if (index >= continuation_capacity) { std::terminate(); }
    SequenceState& sequence     = continuation_states[index];
    if (sequence.kvmem_score_workspace) {
        ops::detail::kvmem_score_unbind(sequence.lane, sequence.kvmem_score_workspace);
        sequence.kvmem_score_workspace.reset();
    }
    sequence.lrfu = {};
    sequence.execution_frontier = 0;
    sequence.ledger_frontier    = 0;
    sequence.ledger.clear();
    sequence.prefix_identity.clear();
    sequence.prefix_digests.clear();
    sequence.rope_delta              = 0;
    sequence.text_kv_valid           = 0;
    sequence.mtp_kv_valid            = 0;
    sequence.dflash_context_frontier = 0;
    sequence.mtp_draft_count         = 0;
    sequence.tail_hidden_valid       = false;
    sequence.endpoint_valid          = false;
    sequence.rewrite_checkpoint      = {};
    sequence.rebuild_work            = {};
    sequence.rebuild_tail_begin      = 0;
    for (std::uint32_t lane = 0; lane < max_concurrency; ++lane) {
        if (active_continuations[lane] == index) {
            active_continuations[lane] = continuation_capacity;
        }
    }
    ContinuationSlot& slot = continuation_slots[index];
    slot.role              = ContinuationSlotRole::Free;
    if (++slot.generation == 0) { ++slot.generation; }
}

detail::PhysicalResources
ProgramImpl::sequence_exclusive_state_resources(const SequenceState& sequence) const {
    if (!state_store) {
        throw std::logic_error("sequence StateImage resources have no physical store");
    }
    detail::PhysicalResources out;
    std::array<StateImageHandle, 4> states{};
    std::uint32_t state_count = 0;
    const auto add_state      = [&](StateImageHandle handle) {
        if (!state_store->valid(handle)) {
            throw std::logic_error("sequence owner has a stale StateImage");
        }
        if (!state_exclusive_to_sequence(sequence, handle)) { return; }
        for (std::uint32_t index = 0; index < state_count; ++index) {
            if (states[index] == handle) { return; }
        }
        states[state_count++]                 = handle;
        const StateReplicaResidency residency = state_store->residency(handle);
        if (residency == StateReplicaResidency::DeviceOnly ||
            residency == StateReplicaResidency::Both) {
            ++out.device.state_slots;
        }
        if (residency == StateReplicaResidency::HostOnly ||
            residency == StateReplicaResidency::Both) {
            ++out.host.state_slots;
        }
    };
    const bool has_read_state  = sequence.state.read.valid();
    const bool has_write_state = sequence.state.write.valid();
    if (has_read_state != has_write_state) {
        throw std::logic_error("sequence owner has a partial primary StateImage pair");
    }
    if (sequence.state.borrows_read() &&
        (!sequence.state.fork_pending || sequence.state.read == sequence.state.write)) {
        throw std::logic_error("sequence has an invalid borrowed StateImage source");
    }
    if (has_read_state) {
        if (!sequence.state.borrows_read() || sequence.state.read == sequence.state.write) {
            add_state(sequence.state.read);
        }
        add_state(sequence.state.write);
    }
    if (sequence.rewrite_state) { add_state(*sequence.rewrite_state); }
    if (sequence.reserved_state) { add_state(*sequence.reserved_state); }
    for (std::size_t anchor_index = 0; anchor_index < sequence.long_anchors.size();
         ++anchor_index) {
        const StateImageHandle handle = sequence.long_anchors[anchor_index].state;
        if (!state_store->valid(handle)) {
            throw std::logic_error("sequence owner has a stale long-anchor StateImage");
        }
        if (!state_exclusive_to_sequence(sequence, handle)) { continue; }
        bool seen = false;
        for (std::uint32_t index = 0; index < std::min<std::uint32_t>(state_count, states.size());
             ++index) {
            if (states[index] == handle) { seen = true; }
        }
        for (std::size_t prior = 0; !seen && prior < anchor_index; ++prior) {
            if (sequence.long_anchors[prior].state == handle) { seen = true; }
        }
        if (seen) { continue; }
        const StateReplicaResidency residency = state_store->residency(handle);
        if (residency == StateReplicaResidency::DeviceOnly ||
            residency == StateReplicaResidency::Both) {
            ++out.device.state_slots;
        }
        if (residency == StateReplicaResidency::HostOnly ||
            residency == StateReplicaResidency::Both) {
            ++out.host.state_slots;
        }
    }
    return out;
}

detail::PhysicalResources
ProgramImpl::owner_exclusive_resources(const SequenceState& sequence) const {
    if (!state_store || !text_kv_addresses || !text_kv_pages) {
        throw std::logic_error("sequence owner resources have no physical stores");
    }
    detail::PhysicalResources out = sequence_exclusive_state_resources(sequence);

    {
        if (!sequence.kv) { throw std::logic_error("sequence owner has no KV address bundle"); }
        const auto add_kv = [&](const KVAddressSpaceStore& addresses,
                                const LogicalKVPageStore& pages, KVAddressSpaceHandle address,
                                std::uint32_t& device_pages) {
            if (!addresses.valid(address)) { throw std::logic_error("stale KV address space"); }
            for (std::uint32_t page = 0; page < addresses.mapped_pages(address); ++page) {
                const LogicalKVPageHandle logical = addresses.logical_page(address, page);
                // A shared logical page contributes to aggregate occupancy once. Releasing this
                // address cannot free either replica while another address still references it,
                // so it is not part of this owner's exact transition effect.
                if (pages.address_references(logical) > 1) { continue; }
                if (pages.device_resident(logical)) { ++device_pages; }
                if (pages.host_resident(logical)) {
                    if (!host_kv_extents) {
                        throw std::logic_error("missing Host KV extent store");
                    }
                    const HostKVPageReplica& replica = pages.host_replica(logical);
                    const std::size_t stride =
                        host_kv_extents->view(replica.extent).layout().page_stride;
                    if (stride > std::numeric_limits<std::size_t>::max() - out.host.kv_bytes) {
                        throw std::overflow_error("resident Host KV byte count overflow");
                    }
                    out.host.kv_bytes += stride;
                }
            }
            if (addresses.active(address)) {
                const std::uint32_t mapped      = addresses.mapped_pages(address);
                const std::uint32_t entitlement = addresses.entitlement(address);
                // LOCAL FIX (ring device footprint): the Device footprint of an active
                // address is its resident pages plus its unmapped entitlement, but never
                // more than the pool. The ring keeps the excess on Host, so a demoted or
                // host-backed page is not Device footprint. Without this cap the
                // materialization check failed at the pool boundary
                // ("materialized sequence does not match its active entitlement").
                const std::uint32_t pool       = pages.physical_pool().usable_pages();
                const std::uint32_t device_cap =
                    pool == 0 ? entitlement : std::min(entitlement, pool);
                if (device_cap > mapped) {
                    const std::uint32_t delta = device_cap - mapped;
                    if (delta > std::numeric_limits<std::uint32_t>::max() - device_pages) {
                        throw std::overflow_error("owner active KV entitlement overflow");
                    }
                    device_pages += delta;
                }
            }
        };
        add_kv(*text_kv_addresses, *text_kv_pages, sequence.kv->text, out.device.main_kv_pages);
        if (sequence.kv->backend) {
            if (!backend_kv_addresses || !backend_kv_pages) {
                throw std::logic_error("missing Backend KV stores");
            }
            add_kv(*backend_kv_addresses, *backend_kv_pages, *sequence.kv->backend,
                   out.device.backend_kv_pages);
        }
    }
    return out;
}

detail::PhysicalResources
ProgramImpl::owner_exclusive_resources(const SharedPrefixState& shared) const {
    if (!state_store || !text_kv_addresses || !text_kv_pages) {
        throw std::logic_error("shared owner resources have no physical stores");
    }
    detail::PhysicalResources out;
    {
        if (!shared.kv || !shared.identity || !state_store->valid(shared.state)) {
            throw std::logic_error("shared prefix has incomplete resident physical state");
        }
        if (state_store->checkpoint_references(shared.state) == 0) {
            throw std::logic_error("shared prefix StateImage has no checkpoint reference");
        }
        const StateReplicaResidency residency = state_store->residency(shared.state);
        if (state_store->checkpoint_references(shared.state) == 1) {
            if (residency == StateReplicaResidency::DeviceOnly ||
                residency == StateReplicaResidency::Both) {
                ++out.device.state_slots;
            }
            if (residency == StateReplicaResidency::HostOnly ||
                residency == StateReplicaResidency::Both) {
                ++out.host.state_slots;
            }
        }
        const auto add_kv = [&](const KVAddressSpaceStore& addresses,
                                const LogicalKVPageStore& pages, KVAddressSpaceHandle address,
                                std::uint32_t& device_pages) {
            if (!addresses.valid(address)) { throw std::logic_error("stale shared KV address"); }
            for (std::uint32_t page = 0; page < addresses.mapped_pages(address); ++page) {
                const LogicalKVPageHandle logical = addresses.logical_page(address, page);
                if (pages.address_references(logical) != 1) { continue; }
                if (pages.device_resident(logical)) { ++device_pages; }
                if (pages.host_resident(logical)) {
                    if (!host_kv_extents) {
                        throw std::logic_error("missing Host KV extent store");
                    }
                    const HostKVPageReplica& replica = pages.host_replica(logical);
                    const std::size_t stride =
                        host_kv_extents->view(replica.extent).layout().page_stride;
                    if (stride > std::numeric_limits<std::size_t>::max() - out.host.kv_bytes) {
                        throw std::overflow_error("shared Host KV byte count overflow");
                    }
                    out.host.kv_bytes += stride;
                }
            }
        };
        add_kv(*text_kv_addresses, *text_kv_pages, shared.kv->text, out.device.main_kv_pages);
        if (shared.kv->backend) {
            if (!backend_kv_addresses || !backend_kv_pages) {
                throw std::logic_error("missing shared Backend KV stores");
            }
            add_kv(*backend_kv_addresses, *backend_kv_pages, *shared.kv->backend,
                   out.device.backend_kv_pages);
        }
    }
    return out;
}

detail::PhysicalResources
ProgramImpl::active_snapshot_shared_resources(const SequenceState& sequence) const {
    if (!sequence.kv || !text_kv_addresses || !text_kv_pages) {
        throw std::logic_error("active snapshot source has no KV stores");
    }
    detail::PhysicalResources out;
    const auto add_kv = [&](const KVAddressSpaceStore& addresses, const LogicalKVPageStore& pages,
                            KVAddressSpaceHandle address, std::uint32_t frontier,
                            std::uint32_t& device_pages) {
        const std::uint32_t full_pages = frontier / static_cast<std::uint32_t>(kPagedKVPageSize);
        if (full_pages > addresses.mapped_pages(address)) {
            throw std::logic_error("active snapshot frontier exceeds its mapped KV pages");
        }
        for (std::uint32_t page = 0; page < full_pages; ++page) {
            const LogicalKVPageHandle logical = addresses.logical_page(address, page);
            if (pages.address_references(logical) != 1) { continue; }
            if (pages.device_resident(logical)) { ++device_pages; }
            if (pages.host_resident(logical)) {
                if (!host_kv_extents) { throw std::logic_error("missing Host KV extent store"); }
                const std::size_t stride =
                    host_kv_extents->view(pages.host_replica(logical).extent).layout().page_stride;
                if (stride > std::numeric_limits<std::size_t>::max() - out.host.kv_bytes) {
                    throw std::overflow_error("active snapshot Host KV byte count overflow");
                }
                out.host.kv_bytes += stride;
            }
        }
    };
    add_kv(*text_kv_addresses, *text_kv_pages, sequence.kv->text, sequence.text_kv_valid,
           out.device.main_kv_pages);
    if (sequence.kv->backend) {
        if (!backend_kv_addresses || !backend_kv_pages) {
            throw std::logic_error("missing Backend KV stores");
        }
        add_kv(*backend_kv_addresses, *backend_kv_pages, *sequence.kv->backend,
               backend_kv_valid(sequence), out.device.backend_kv_pages);
    }
    return out;
}

ProgramImpl::ActiveExclusiveBaseline ProgramImpl::active_exclusive_baseline() const noexcept {
    ActiveExclusiveBaseline out{};
    // A Hybrid lane's entitlement is its admission quote, not exclusive page ownership.
    if (hybrid_prefix_cache()) { return out; }
    for (std::uint32_t lane = 0; lane < max_concurrency; ++lane) {
        const std::uint32_t continuation = active_continuations[lane];
        if (requests[lane].lifecycle == Lifecycle::Empty || continuation >= continuation_capacity) {
            continue;
        }
        try {
            out[lane] = ActiveExclusiveEntry{
                .continuation = continuation,
                .resources    = owner_exclusive_resources(continuation_states[continuation]),
            };
        } catch (...) {}
    }
    return out;
}

void ProgramImpl::credit_active_ownership_transfers(
    const ActiveExclusiveBaseline& baseline) noexcept {
    for (std::uint32_t lane = 0; lane < max_concurrency; ++lane) {
        if (!baseline[lane] || requests[lane].lifecycle == Lifecycle::Empty ||
            active_continuations[lane] != baseline[lane]->continuation) {
            continue;
        }
        try {
            const detail::PhysicalResources exclusive =
                owner_exclusive_resources(continuation_states[baseline[lane]->continuation]);
            requests[lane].active_resources = checked_resource_sum(
                requests[lane].active_resources,
                positive_resource_difference(exclusive, baseline[lane]->resources));
        } catch (...) {}
    }
}

detail::PhysicalResources ProgramImpl::physical_occupancy() const noexcept {
    detail::PhysicalResources out;
    for (const RequestControl& request : requests) {
        if (request.lifecycle != Lifecycle::Empty) { ++out.device.active_lanes; }
    }
    if (state_store) {
        out.device.state_slots = state_store->device_occupied();
        out.host.state_slots   = state_store->host_occupied();
    }
    if (text_kv_pages) {
        const DeviceKVPagePool& pool = text_kv_pages->physical_pool();
        // LOCAL FIX (ring physical occupancy): under a logical reservation the reserved
        // count is a promise the ring satisfies by demotion on demand, not memory already
        // held. Counting it made the Device look permanently oversubscribed
        // (allocated 1500 + reserved 938 = 2438 over a 1500-page pool) and drove the
        // capture/materialization paths into a state they could not fund.
        out.device.main_kv_pages = pool.allocated_pages() +
                                  (ring_requested() ? 0U : pool.reserved_pages());
    }
    if (backend_kv_pages) {
        const DeviceKVPagePool& pool = backend_kv_pages->physical_pool();
        out.device.backend_kv_pages = pool.allocated_pages() +
                                     (ring_requested() ? 0U : pool.reserved_pages());
    }
    if (host_kv_arena) { out.host.kv_bytes = host_kv_arena->occupied_bytes(); }
    return out;
}

detail::PhysicalResources
ProgramImpl::materialization_deficit(const ResourceCandidateState& admission) const {
    // Pressure is relative to this candidate's real peak. Treating every dimension as scarce
    // would forbid Device-to-Host demotion even when Host capacity is available.
    const detail::PhysicalResources required =
        checked_resource_sum(physical_occupancy(), admission.demand.physical_peak_additional);
    const detail::PhysicalResources limits = admission_capacity();
    if (!ring_requested()) { return positive_resource_difference(required, limits); }
    // LOCAL PROTOTYPE (KVMem-style ring): a logical Main KV entitlement larger than the pool is
    // not a real deficit; the ring recycles resident pages. Clamp before measuring pressure.
    detail::PhysicalResources clamped = required;
    if (clamped.device.main_kv_pages > limits.device.main_kv_pages) {
        clamped.device.main_kv_pages = limits.device.main_kv_pages;
    }
    if (clamped.device.backend_kv_pages > limits.device.backend_kv_pages) {
        clamped.device.backend_kv_pages = limits.device.backend_kv_pages;
    }
    return positive_resource_difference(clamped, limits);
}

detail::PhysicalResources
ProgramImpl::guided_materialization_deficit(const ResourceCandidateState& admission,
                                            const detail::PhysicalDelta& pressure) const {
    // Pressure acts on the candidate's complete peak, not on its already-clamped deficit.  Applying
    // a demotion directly to a zero Host deficit would otherwise manufacture Host pressure even
    // when the arena has ample slack and steer the heuristic toward unnecessary destruction.
    const detail::PhysicalResources projected_peak = positive_resource_difference(
        checked_resource_sum(admission.demand.physical_peak_additional, pressure.added),
        pressure.removed);
    const detail::PhysicalResources required =
        checked_resource_sum(physical_occupancy(), projected_peak);
    const detail::PhysicalResources limits = admission_capacity();
    if (!ring_requested()) { return positive_resource_difference(required, limits); }
    // LOCAL PROTOTYPE (KVMem-style ring): see materialization_deficit.
    detail::PhysicalResources clamped = required;
    if (clamped.device.main_kv_pages > limits.device.main_kv_pages) {
        clamped.device.main_kv_pages = limits.device.main_kv_pages;
    }
    if (clamped.device.backend_kv_pages > limits.device.backend_kv_pages) {
        clamped.device.backend_kv_pages = limits.device.backend_kv_pages;
    }
    return positive_resource_difference(clamped, limits);
}

bool ProgramImpl::physical_peak_fits(detail::PhysicalResources peak) const noexcept {
    const detail::PhysicalResources occupied = physical_occupancy();
    const detail::PhysicalResources limits   = admission_capacity();
    const auto fits_u32 = [](std::uint32_t used, std::uint32_t added, std::uint32_t capacity) {
        return added <= capacity && used <= capacity - added;
    };
    const auto fits_size = [](std::size_t used, std::size_t added, std::size_t capacity) {
        return added <= capacity && used <= capacity - added;
    };
    // LOCAL PROTOTYPE (KVMem-style ring): when the Device pool is smaller than the logical context
    // the pool is managed by the ring (pages are demoted to the Host and retrieved back), so a
    // request's logical page entitlement must NOT be compared against it. Clamping the entitlement
    // to the pool does not work either: the check is `used + added <= capacity`, so clamping
    // `added` to `capacity` forces `used == 0` and rejects every request once the pool holds
    // anything. Skip the dimension instead.
    // LOCAL FIX (ring-fits-used): zeroing only `added` is not enough: `used` itself is the
    // logical mapped count, legitimately above pool capacity by design (1660 mapped vs 1500
    // pool observed), so `used <= capacity` fails every capture past the pool even with
    // zero added. Skip the whole dimension as the comment intends, not just the added term.
    const bool ring = ring_requested() && text_kv_pages != nullptr && text_kv_addresses != nullptr &&
                      text_kv_pages->physical_pool().usable_pages() <
                          text_kv_addresses->logical_page_capacity();
    const bool ok =
        fits_u32(occupied.device.active_lanes, peak.device.active_lanes,
                 limits.device.active_lanes) &&
        fits_u32(occupied.device.state_slots, peak.device.state_slots,
                 limits.device.state_slots) &&
        (ring || fits_u32(occupied.device.main_kv_pages, peak.device.main_kv_pages,
                          limits.device.main_kv_pages)) &&
        (ring || fits_u32(occupied.device.backend_kv_pages, peak.device.backend_kv_pages,
                          limits.device.backend_kv_pages)) &&
        fits_u32(occupied.host.state_slots, peak.host.state_slots, limits.host.state_slots) &&
        fits_size(occupied.host.kv_bytes, peak.host.kv_bytes, limits.host.kv_bytes);
    return ok;
}

StateImageHandle
ProgramImpl::selected_state(const SequenceState& sequence, ReusePath reuse,
                            std::optional<runtime::CheckpointRef> checkpoint) const {
    if (reuse == ReusePath::PrivateEndpoint) {
        if (!sequence.endpoint_valid || !state_store->valid(sequence.state.read)) {
            throw std::logic_error("private endpoint StateImage is stale");
        }
        return sequence.state.read;
    }
    if (is_rewrite_checkpoint_restore(reuse) && sequence.rewrite_state &&
        state_store->valid(*sequence.rewrite_state)) {
        return *sequence.rewrite_state;
    }
    if (reuse == ReusePath::PrivateLongAnchor) {
        if (!checkpoint || checkpoint->kind != runtime::CheckpointKind::LongAnchor) {
            throw std::logic_error("long-anchor materialization has no selected checkpoint");
        }
        const auto anchor = std::find_if(sequence.long_anchors.begin(), sequence.long_anchors.end(),
                                         [&](const LongAnchorCheckpoint& candidate) {
                                             return candidate.frontier == checkpoint->frontier &&
                                                    candidate.ordinal == checkpoint->ordinal;
                                         });
        if (anchor != sequence.long_anchors.end() && state_store->valid(anchor->state)) {
            return anchor->state;
        }
    }
    throw std::logic_error("materialization path has no selected StateImage");
}

std::uint32_t
ProgramImpl::selected_state_consumed_references(const SequenceState& sequence, ReusePath reuse,
                                                RewriteCheckpointDisposition rewrite_disposition,
                                                std::optional<runtime::CheckpointRef> checkpoint,
                                                std::uint32_t reuse_base) const {
    const StateImageHandle selected   = selected_state(sequence, reuse, checkpoint);
    std::uint32_t consumed_references = 0;
    if (is_rewrite_checkpoint_restore(reuse) &&
        rewrite_disposition != RewriteCheckpointDisposition::RetainExisting) {
        if (!sequence.rewrite_state || *sequence.rewrite_state != selected) {
            throw std::logic_error("selected rewrite StateImage is unavailable");
        }
        consumed_references = 1;
    } else if (reuse == ReusePath::PrivateEndpoint &&
               rewrite_disposition != RewriteCheckpointDisposition::RetainExisting &&
               sequence.rewrite_state && *sequence.rewrite_state == selected) {
        consumed_references = 1;
    }
    for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
        if (anchor.frontier > reuse_base && anchor.state == selected) {
            if (consumed_references == std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("consumed StateImage reference inventory overflow");
            }
            ++consumed_references;
        }
    }
    const std::uint32_t references = state_store->checkpoint_references(selected);
    if (consumed_references > references) {
        throw std::logic_error("selected StateImage reference inventory is inconsistent");
    }
    return consumed_references;
}

bool ProgramImpl::selected_state_requires_fork(const SequenceState& sequence, ReusePath reuse,
                                               RewriteCheckpointDisposition rewrite_disposition,
                                               std::optional<runtime::CheckpointRef> checkpoint,
                                               std::uint32_t reuse_base) const {
    const StateImageHandle selected = selected_state(sequence, reuse, checkpoint);
    return state_store->checkpoint_references(selected) !=
           selected_state_consumed_references(sequence, reuse, rewrite_disposition, checkpoint,
                                              reuse_base);
}

bool ProgramImpl::can_retain_rewrite_checkpoint(const PreparedPromptData& prompt,
                                                const RewriteCheckpointSpec& desired,
                                                const SequenceState& sequence, ReusePath reuse,
                                                std::uint32_t reuse_base) const {
    if (!sequence.rewrite_checkpoint.valid || !sequence.rewrite_state ||
        !state_store->valid(*sequence.rewrite_state) ||
        !qwen3_5::detail::prefix_matches(prompt, sequence.ledger, sequence.prefix_identity,
                                         sequence.rewrite_checkpoint.frontier)) {
        return false;
    }
    if (sequence.rewrite_checkpoint.frontier == desired.frontier) { return true; }
    return is_rewrite_checkpoint_restore(reuse) &&
           sequence.rewrite_checkpoint.frontier == reuse_base && desired.frontier <= reuse_base;
}

std::uint32_t ProgramImpl::device_kv_prefix_pages(const KVAddressSpaceStore& addresses,
                                                  KVAddressSpaceHandle address,
                                                  std::uint32_t frontier) const {
    const std::uint32_t required = kv_pages_for_frontier(frontier);
    if (required > addresses.mapped_pages(address)) {
        throw std::logic_error("checkpoint KV requirement exceeds address membership");
    }
    const LogicalKVPageStore& pages =
        (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
    std::uint32_t resident = 0;
    for (std::uint32_t page = 0; page < required; ++page) {
        if (pages.device_resident(addresses.logical_page(address, page))) { ++resident; }
    }
    return resident;
}

std::uint32_t ProgramImpl::shared_kv_prefix_pages(const KVAddressSpaceStore& addresses,
                                                  KVAddressSpaceHandle address,
                                                  std::uint32_t frontier) const {
    const std::uint32_t required = kv_pages_for_frontier(frontier);
    if (required > addresses.mapped_pages(address)) {
        throw std::logic_error("checkpoint KV requirement exceeds address membership");
    }
    const LogicalKVPageStore& pages =
        (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
    std::uint32_t shared = 0;
    for (std::uint32_t page = 0; page < required; ++page) {
        if (pages.address_references(addresses.logical_page(address, page)) <= 1) { continue; }
        if (page + 1U == required && frontier % static_cast<std::uint32_t>(kPagedKVPageSize) != 0) {
            continue;
        }
        ++shared;
    }
    return shared;
}

std::uint32_t ProgramImpl::shared_device_kv_prefix_pages(const KVAddressSpaceStore& addresses,
                                                         KVAddressSpaceHandle address,
                                                         std::uint32_t frontier) const {
    const std::uint32_t required = kv_pages_for_frontier(frontier);
    if (required > addresses.mapped_pages(address)) {
        throw std::logic_error("checkpoint KV requirement exceeds address membership");
    }
    const LogicalKVPageStore& pages =
        (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
    std::uint32_t resident = 0;
    for (std::uint32_t page = 0; page < required; ++page) {
        const LogicalKVPageHandle logical = addresses.logical_page(address, page);
        if (pages.address_references(logical) > 1 && pages.device_resident(logical)) { ++resident; }
    }
    return resident;
}

bool ProgramImpl::partial_tail_cow_required(const KVAddressSpaceStore& addresses,
                                            KVAddressSpaceHandle address,
                                            std::uint32_t frontier) const {
    if (frontier == 0 || frontier % static_cast<std::uint32_t>(kPagedKVPageSize) == 0) {
        return false;
    }
    const std::uint32_t required = kv_pages_for_frontier(frontier);
    if (required > addresses.mapped_pages(address)) {
        throw std::logic_error("checkpoint KV requirement exceeds address membership");
    }
    const LogicalKVPageStore& pages =
        (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
    const LogicalKVPageHandle tail = addresses.logical_page(address, required - 1U);
    return pages.address_references(tail) > 1 || !pages.device_resident(tail);
}

std::uint32_t
ProgramImpl::missing_shared_device_kv_prefix_pages(const KVAddressSpaceStore& addresses,
                                                   KVAddressSpaceHandle address,
                                                   std::uint32_t frontier) const {
    const std::uint32_t required = kv_pages_for_frontier(frontier);
    if (required > addresses.mapped_pages(address)) {
        throw std::logic_error("checkpoint KV requirement exceeds address membership");
    }
    const LogicalKVPageStore& pages =
        (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
    std::uint32_t missing = 0;
    for (std::uint32_t page = 0; page < required; ++page) {
        const LogicalKVPageHandle logical = addresses.logical_page(address, page);
        if (pages.address_references(logical) > 1 && !pages.device_resident(logical)) { ++missing; }
    }
    return missing;
}

std::size_t ProgramImpl::host_kv_prefix_bytes(const KVAddressSpaceStore& addresses,
                                              KVAddressSpaceHandle address,
                                              std::uint32_t frontier) const noexcept {
    if (!host_kv_extents) { return 0; }
    try {
        const LogicalKVPageStore& pages =
            (&addresses == text_kv_addresses.get()) ? *text_kv_pages : *backend_kv_pages;
        const std::uint32_t required_pages = kv_pages_for_frontier(frontier);
        if (required_pages > addresses.mapped_pages(address)) { return 0; }
        std::size_t bytes = 0;
        for (std::uint32_t page = 0; page < required_pages; ++page) {
            const LogicalKVPageHandle logical = addresses.logical_page(address, page);
            if (pages.address_references(logical) > 1) { continue; }
            if (!pages.host_resident(logical)) { continue; }
            if (page + 1U == required_pages &&
                frontier % static_cast<std::uint32_t>(kPagedKVPageSize) != 0 &&
                partial_tail_cow_required(addresses, address, frontier)) {
                continue;
            }
            const std::uint32_t begin = page * static_cast<std::uint32_t>(kPagedKVPageSize);
            const std::uint32_t selected_columns =
                std::min(static_cast<std::uint32_t>(kPagedKVPageSize), frontier - begin);
            if (selected_columns != pages.committed_columns(logical)) {
                // A destructive private rewrite changes this tail page's content epoch, so its
                // old Host replica cannot remain part of the active entitlement.
                continue;
            }
            const std::size_t stride =
                host_kv_extents->view(pages.host_replica(logical).extent).layout().page_stride;
            if (stride > std::numeric_limits<std::size_t>::max() - bytes) { return 0; }
            bytes += stride;
        }
        return bytes;
    } catch (...) { return 0; }
}

qwen3_5::CheckpointSummary
ProgramImpl::checkpoint_summary(const SequenceState& sequence, runtime::CheckpointRef checkpoint,
                                StateImageHandle state, runtime::PrefillWork rebuild_work) const {
    if (!sequence.kv) { throw std::logic_error("checkpoint summary has no KV address space"); }
    if (checkpoint.frontier == 0) {
        throw std::logic_error("checkpoint summary has an empty frontier");
    }
    if (!state_store->valid(state)) {
        throw std::logic_error("checkpoint summary has a stale StateImage");
    }
    const StateReplicaResidency state_location = state_store->residency(state);
    runtime::ReplicaResidency residency        = runtime::ReplicaResidency::DeviceOnly;
    if (state_location == StateReplicaResidency::HostOnly) {
        residency = runtime::ReplicaResidency::HostOnly;
    } else if (state_location == StateReplicaResidency::Both) {
        residency = runtime::ReplicaResidency::Both;
    } else if (state_location != StateReplicaResidency::DeviceOnly) {
        throw std::logic_error("checkpoint StateImage has no published replica");
    }
    const std::uint32_t backend_frontier =
        speculative_backend == SpeculativeBackend::Mtp      ? checkpoint.frontier - 1U
        : speculative_backend == SpeculativeBackend::DFlash ? checkpoint.frontier
                                                            : 0U;
    const std::uint32_t identity_tag = capture_identity_tag();
    // LOCAL FIX (host-backed checkpoint): count the pages of this checkpoint's range whose Device
    // replica the ring gave up while a CURRENT Host replica exists. Same predicate as the publish gate
    // in capture.cpp (`scan_range`); the byte total reuses `host_kv_prefix_bytes` so the two never
    // disagree about the stride.
    const auto count_host_backed = [&](const KVAddressSpaceStore& addresses,
                                       const LogicalKVPageStore& pages, KVAddressSpaceHandle address,
                                       std::uint32_t page_limit) {
        std::uint32_t count = 0;
        if (!addresses.valid(address)) { return count; }
        const std::uint32_t mapped = addresses.mapped_pages(address);
        const std::uint32_t limit  = page_limit < mapped ? page_limit : mapped;
        for (std::uint32_t page = 0; page < limit; ++page) {
            const LogicalKVPageHandle logical = addresses.logical_page(address, page);
            if (!pages.valid(logical) || pages.device_resident(logical)) { continue; }
            if (pages.host_resident(logical) && pages.host_replica_current(logical)) { ++count; }
        }
        return count;
    };
    std::uint32_t host_backed_pages = 0;
    std::uint64_t host_backed_bytes = 0;
    if (text_kv_addresses != nullptr && text_kv_pages != nullptr) {
        host_backed_pages += count_host_backed(*text_kv_addresses, *text_kv_pages, sequence.kv->text,
                                               kv_pages_for_frontier(checkpoint.frontier));
        host_backed_bytes += static_cast<std::uint64_t>(
            host_kv_prefix_bytes(*text_kv_addresses, sequence.kv->text, checkpoint.frontier));
    }
    if (sequence.kv->backend && backend_kv_addresses != nullptr && backend_kv_pages != nullptr) {
        host_backed_pages +=
            count_host_backed(*backend_kv_addresses, *backend_kv_pages, *sequence.kv->backend,
                              kv_pages_for_frontier(backend_frontier));
        host_backed_bytes += static_cast<std::uint64_t>(host_kv_prefix_bytes(
            *backend_kv_addresses, *sequence.kv->backend, backend_frontier));
    }
    return qwen3_5::CheckpointSummary{
        .ref   = checkpoint,
        .scope = runtime::CheckpointScope::Private,
        .shortlist_key =
            {
                .digests      = sequence.prefix_digests.at(checkpoint.frontier),
                .frontier     = checkpoint.frontier,
                .identity_tag = identity_tag,
            },
        .state_residency = residency,
        .required_kv =
            {
                .main_frontier    = checkpoint.frontier,
                .backend_frontier = backend_frontier,
                .main_pages       = kv_pages_for_frontier(checkpoint.frontier),
                .backend_pages    = kv_pages_for_frontier(backend_frontier),
            },
        .rebuild_work = validated_rebuild_work(rebuild_work, checkpoint.frontier),
        // LOCAL FIX (host-backed checkpoint): register how much of the range is Host-backed only.
        // `host_kv_prefix_bytes` (context.cpp:824-858) already sums exactly those strides; the page
        // count repeats the publish gate's predicate (capture.cpp `scan_range`: no Device replica and a
        // CURRENT Host replica). Nothing consumes these fields yet.
        .host_backed_pages = host_backed_pages,
        .host_backed_bytes = host_backed_bytes,
    };
}

qwen3_5::ContinuationSummary
ProgramImpl::continuation_summary(const SequenceState& sequence) const {
    qwen3_5::ContinuationSummary summary;
    summary.long_anchors.reserve(sequence.long_anchors.size());
    populate_continuation_summary(sequence, summary);
    return summary;
}

void ProgramImpl::populate_continuation_summary(const SequenceState& sequence,
                                                qwen3_5::ContinuationSummary& summary) const {
    validate_long_anchor_ordinals(sequence.long_anchors,
                                  context_cache.max_long_anchors_per_continuation.value_or(0));
    if (summary.long_anchors.capacity() < sequence.long_anchors.size()) {
        throw std::logic_error("continuation summary backing was not reserved");
    }
    summary.endpoint.reset();
    summary.rewrite.reset();
    summary.long_anchors.clear();
    summary.active_references = 0;
    if (sequence.endpoint_valid) {
        const runtime::CheckpointRef endpoint{
            .kind     = runtime::CheckpointKind::SessionEndpoint,
            .frontier = sequence.execution_frontier,
        };
        runtime::PrefillWork endpoint_work = sequence.rebuild_work;
        summary.endpoint =
            checkpoint_summary(sequence, endpoint, sequence.state.read, endpoint_work);
    }
    if (sequence.rewrite_checkpoint.valid) {
        if (!sequence.rewrite_state) {
            throw std::logic_error("rewrite checkpoint has no StateImage");
        }
        const runtime::CheckpointRef rewrite{
            .kind     = checkpoint_kind(sequence.rewrite_checkpoint.kind),
            .frontier = sequence.rewrite_checkpoint.frontier,
        };
        summary.rewrite = checkpoint_summary(sequence, rewrite, *sequence.rewrite_state,
                                             sequence.rewrite_checkpoint.rebuild_work);
    }
    for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
        summary.long_anchors.push_back(
            checkpoint_summary(sequence,
                               runtime::CheckpointRef{.kind = runtime::CheckpointKind::LongAnchor,
                                                      .frontier = anchor.frontier,
                                                      .ordinal  = anchor.ordinal},
                               anchor.state, anchor.rebuild_work));
    }
    if (!summary.endpoint && !summary.rewrite && summary.long_anchors.empty()) {
        throw std::logic_error("private continuation has no checkpoint");
    }
    const auto* begin = continuation_states.data();
    const auto* end   = begin + continuation_capacity;
    if (&sequence >= begin && &sequence < end) {
        const std::size_t index = static_cast<std::size_t>(&sequence - begin);
        summary.active_references =
            continuation_slots[index].role == ContinuationSlotRole::Active ? 1U : 0U;
    }
}

qwen3_5::SharedPrefixSummary
ProgramImpl::shared_prefix_summary(const SharedPrefixState& shared) const {
    if (!shared.kv || !shared.identity || shared.frontier == 0 ||
        !state_store->valid(shared.state)) {
        throw std::logic_error("shared-prefix summary source is incomplete");
    }
    const StateReplicaResidency state_location = state_store->residency(shared.state);
    runtime::ReplicaResidency residency        = runtime::ReplicaResidency::DeviceOnly;
    if (state_location == StateReplicaResidency::HostOnly) {
        residency = runtime::ReplicaResidency::HostOnly;
    } else if (state_location == StateReplicaResidency::Both) {
        residency = runtime::ReplicaResidency::Both;
    } else if (state_location != StateReplicaResidency::DeviceOnly) {
        throw std::logic_error("shared-prefix StateImage has no published replica");
    }
    return qwen3_5::SharedPrefixSummary{
        .checkpoint =
            {
                .ref =
                    {
                        .kind     = runtime::CheckpointKind::SharedStablePrefix,
                        .frontier = shared.frontier,
                    },
                .scope           = runtime::CheckpointScope::Shared,
                .shortlist_key   = shared.identity->shortlist_key,
                .state_residency = residency,
                .required_kv =
                    {
                        .main_frontier    = shared.frontier,
                        .backend_frontier = shared.backend_frontier,
                        .main_pages       = kv_pages_for_frontier(shared.frontier),
                        .backend_pages    = kv_pages_for_frontier(shared.backend_frontier),
                    },
                .rebuild_work = validated_rebuild_work(shared.rebuild_work, shared.frontier),
            },
        .active_references = shared.active_references,
    };
}

PrefillProgress ProgramImpl::advance_prefill(SequenceHandle sequence,
                                             runtime::ExecutionTiming* failed_timing) {
    if (pending_transaction_ || !valid_sequence(sequence)) {
        throw std::logic_error("prefill sequence capability is invalid");
    }
    const std::uint32_t lane = ContractAccess::lane(sequence).value;
    if (requests[lane].lifecycle != Lifecycle::Prefilling) {
        throw std::logic_error("prefill advance requires a prefilling sequence");
    }
    try {
        runtime::PrefillStepResult step = advance_prefill_raw(lane, failed_timing);
        if (failed_timing != nullptr) { *failed_timing += step.timing; }
        return wrap_prefill(lane, std::move(step));
    } catch (...) {
        const Clock::time_point cleanup_started = Clock::now();
        clear_execution_failure_lanes(std::span<const std::uint32_t>(&lane, 1));
        if (failed_timing != nullptr) {
            failed_timing->post_host_ns += elapsed_ns(cleanup_started);
        }
        throw;
    }
}

bool ProgramImpl::can_clear_lane_strict(const SequenceState& sequence) const {
    const auto* begin = continuation_states.data();
    const auto* end   = begin + continuation_capacity;
    if (&sequence < begin || &sequence >= end || !state_store || !text_kv_addresses ||
        !text_kv_pages || !sequence.kv) {
        return false;
    }
    const std::uint32_t continuation = static_cast<std::uint32_t>(&sequence - begin);
    if (continuation_slots[continuation].role != ContinuationSlotRole::Active ||
        !text_kv_addresses->can_release_after_deactivate(sequence.kv->text) ||
        (sequence.kv->backend &&
         (!backend_kv_addresses || !backend_kv_pages ||
          !backend_kv_addresses->can_release_after_deactivate(*sequence.kv->backend)))) {
        return false;
    }

    for (std::size_t position = 0; position < sequence.shared_prefix_references.size();
         ++position) {
        const std::uint32_t index = sequence.shared_prefix_references[position];
        if (index >= shared_prefix_capacity ||
            shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued) {
            return false;
        }
        const std::uint32_t required = static_cast<std::uint32_t>(std::count(
            sequence.shared_prefix_references.begin(),
            sequence.shared_prefix_references.begin() + static_cast<std::ptrdiff_t>(position + 1U),
            index));
        if (shared_prefix_states[index].active_references < required) { return false; }
    }

    if (!state_store->valid(sequence.state.read) || !state_store->valid(sequence.state.write) ||
        (sequence.state.fork_pending &&
         (!sequence.state.borrows_read() ||
          !state_store->can_abort_fork(sequence.state.read, sequence.state.write)))) {
        return false;
    }
    enum class ForkEndpoint : std::uint8_t { None, Source, Destination };
    const auto validate_state = [&](StateImageHandle handle, bool release_object,
                                    ForkEndpoint fork_endpoint = ForkEndpoint::None) {
        if (!state_store->valid(handle)) { return false; }
        const std::uint32_t owned = owned_checkpoint_references(sequence, handle);
        const std::uint32_t total = state_store->checkpoint_references(handle);
        if (owned > total ||
            (owned != 0 && state_store->role(handle) != StateImageRole::CheckpointImmutable &&
             fork_endpoint != ForkEndpoint::Destination)) {
            return false;
        }
        if (!release_object || total != owned) { return true; }
        if (fork_endpoint == ForkEndpoint::Source) {
            return state_store->can_release_source_after_fork_abort(sequence.state.read,
                                                                    sequence.state.write, owned);
        }
        if (fork_endpoint == ForkEndpoint::Destination) {
            return state_store->can_release_destination_after_fork_abort(
                sequence.state.read, sequence.state.write, owned);
        }
        return state_store->can_release_after_checkpoint_references(handle, owned);
    };
    const auto duplicates_binding = [&](StateImageHandle handle) {
        return handle == sequence.state.read || handle == sequence.state.write;
    };

    if (!validate_state(sequence.state.read,
                        !sequence.state.read_has_external_owner() ||
                            sequence.state.read == sequence.state.write,
                        sequence.state.fork_pending ? ForkEndpoint::Source : ForkEndpoint::None)) {
        return false;
    }
    if (sequence.state.write != sequence.state.read &&
        !validate_state(sequence.state.write, true,
                        sequence.state.fork_pending ? ForkEndpoint::Destination
                                                    : ForkEndpoint::None)) {
        return false;
    }
    if (sequence.rewrite_state && !duplicates_binding(*sequence.rewrite_state) &&
        !validate_state(*sequence.rewrite_state, true)) {
        return false;
    }
    for (std::size_t index = 0; index < sequence.long_anchors.size(); ++index) {
        const StateImageHandle handle = sequence.long_anchors[index].state;
        bool repeated                 = duplicates_binding(handle) ||
                        (sequence.rewrite_state && handle == *sequence.rewrite_state);
        for (std::size_t prior = 0; !repeated && prior < index; ++prior) {
            repeated = sequence.long_anchors[prior].state == handle;
        }
        if (!repeated && !validate_state(handle, true)) { return false; }
    }
    if (sequence.reserved_state) {
        const StateImageHandle handle = *sequence.reserved_state;
        bool repeated                 = duplicates_binding(handle) ||
                        (sequence.rewrite_state && handle == *sequence.rewrite_state);
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            repeated = repeated || anchor.state == handle;
        }
        if (!repeated && !validate_state(handle, true)) { return false; }
    }
    return true;
}

void ProgramImpl::release_active_shared_references_strict(SequenceState& sequence) noexcept {
    for (const std::uint32_t index : sequence.shared_prefix_references) {
        if (index >= shared_prefix_capacity ||
            shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued ||
            shared_prefix_states[index].active_references == 0) {
            std::terminate();
        }
        --shared_prefix_states[index].active_references;
    }
    sequence.shared_prefix_references.clear();
}

bool ProgramImpl::clear_lane_strict(SequenceState& sequence, RequestControl& request) noexcept {
    try {
        if (!can_clear_lane_strict(sequence)) { return false; }
    } catch (...) { return false; }
    const auto* begin                      = continuation_states.data();
    const std::uint32_t continuation       = static_cast<std::uint32_t>(&sequence - begin);
    const ActiveExclusiveBaseline baseline = active_exclusive_baseline();
    hybrid_release_lane(sequence.lane);
    release_active_shared_references_strict(sequence);
    release_active_sequence_kv_strict(sequence);
    release_active_sequence_state_strict(sequence);
    retire_continuation_slot(continuation);
    request.retire();
    credit_active_ownership_transfers(baseline);
    return true;
}

void ProgramImpl::clear_execution_failure_lanes(std::span<const std::uint32_t> lanes) noexcept {
    // A concurrent resource transaction may pin or inspect these active owners. Engine-wide
    // cleanup aborts that transaction before releasing lanes, preserving the only safe order.
    if (has_context_transaction()) { return; }
    for (const std::uint32_t lane : lanes) {
        if (lane >= max_concurrency || active_continuations[lane] >= continuation_capacity) {
            continue;
        }
        clear_lane_best_effort(active_sequence(lane), requests[lane]);
        invalidate_lane(lane);
    }
}

void ProgramImpl::clear_lane_best_effort(SequenceState& sequence,
                                         RequestControl& request) noexcept {
    hybrid_release_lane(sequence.lane);
    request.retire();
    const auto* begin            = continuation_states.data();
    const auto* end              = begin + continuation_capacity;
    if (&sequence >= begin && &sequence < end) {
        release_continuation_slot_best_effort(static_cast<std::uint32_t>(&sequence - begin));
    }
}

StateImageSelectors ProgramImpl::state_selectors(const SequenceState& sequence) const {
    if (!state_store || !state_store->valid(sequence.state.read) ||
        !state_store->valid(sequence.state.write)) {
        throw std::logic_error("sequence has no active StateImage binding");
    }
    return state_store->selectors(sequence.state.read, sequence.state.write);
}

std::uint32_t ProgramImpl::owned_checkpoint_references(const SequenceState& sequence,
                                                       StateImageHandle state) const noexcept {
    std::uint32_t references = 0;
    if (sequence.rewrite_state && *sequence.rewrite_state == state) { ++references; }
    for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
        if (anchor.state == state) { ++references; }
    }
    return references;
}

bool ProgramImpl::state_exclusive_to_sequence(const SequenceState& sequence,
                                              StateImageHandle state) const noexcept {
    if (!state_store || !state_store->valid(state)) { return false; }
    return state_store->checkpoint_references(state) ==
           owned_checkpoint_references(sequence, state);
}

void ProgramImpl::refresh_state_views(SequenceState& sequence) {
    sequence.tail_hidden               = {};
    sequence.rewrite_checkpoint_hidden = {};
    if (state_store->valid(sequence.state.read) && state_store->valid(sequence.state.write) &&
        state_store->residency(sequence.state.read) != StateReplicaResidency::HostOnly &&
        state_store->residency(sequence.state.write) != StateReplicaResidency::HostOnly) {
        const StateImageHandle committed =
            sequence.state.fork_pending ? sequence.state.read : sequence.state.write;
        sequence.tail_hidden =
            state_images->continuation_hidden_slot(state_store->physical_slot(committed));
    }
    if (sequence.rewrite_state && state_store->valid(*sequence.rewrite_state) &&
        state_store->residency(*sequence.rewrite_state) != StateReplicaResidency::HostOnly) {
        sequence.rewrite_checkpoint_hidden = state_images->continuation_hidden_slot(
            state_store->physical_slot(*sequence.rewrite_state));
    }
}

void ProgramImpl::reserve_state_entitlement(SequenceState& sequence, std::uint32_t slots) {
    const std::uint32_t owned = sequence_exclusive_state_resources(sequence).device.state_slots;
    if (slots == 0 || owned > slots) {
        throw std::logic_error("sequence StateImage entitlement is inconsistent");
    }
    if (owned == slots) { return; }
    if (slots - owned != 1 || sequence.reserved_state) {
        throw std::logic_error("sequence StateImage reservation is not a single destination");
    }
    std::optional<StateImageHandle> reserved = state_store->reserve_destination();
    if (!reserved) {
        throw ninfer::ContextCacheExhausted("Device StateImage store has no free slot for the sequence reservation");
    }
    sequence.reserved_state = *reserved;
    if (sequence_exclusive_state_resources(sequence).device.state_slots != slots) {
        throw std::logic_error("sequence StateImage entitlement did not materialize exactly");
    }
}

void ProgramImpl::settle_state_fork(SequenceState& sequence) {
    if (!sequence.state.fork_pending) { return; }
    if (has_context_transaction()) {
        throw std::logic_error("StateImage Fork settlement overlaps a resource transaction");
    }
    const StateImageHandle source      = sequence.state.read;
    const StateImageHandle destination = sequence.state.write;
    const bool external_source         = sequence.state.read_has_external_owner();
    state_store->commit_fork(source, destination);
    sequence.state = ActiveStateBinding{.read = destination, .write = destination};
    if (!external_source && state_store->checkpoint_references(source) == 0 &&
        !state_store->release(source)) {
        throw std::logic_error("unreferenced StateImage fork source could not be released");
    }
    refresh_state_views(sequence);
}

bool ProgramImpl::has_unsettled_state_fork() const noexcept {
    for (std::uint32_t lane = 0; lane < max_concurrency; ++lane) {
        const std::uint32_t continuation = active_continuations[lane];
        if (continuation < continuation_capacity &&
            continuation_states[continuation].state.fork_pending) {
            return true;
        }
    }
    return false;
}

void ProgramImpl::release_active_sequence_state_strict(SequenceState& sequence) noexcept {
    const auto fail = []() noexcept { std::terminate(); };
    if (!state_store) { fail(); }
    try {
        if (sequence.state.fork_pending) {
            state_store->abort_fork(sequence.state.read, sequence.state.write);
        }
        if (sequence.rewrite_state) {
            state_store->release_checkpoint_reference(*sequence.rewrite_state);
        }
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            state_store->release_checkpoint_reference(anchor.state);
        }

        const auto release_if_unreferenced = [&](StateImageHandle handle, bool lifetime_owned) {
            if (!lifetime_owned || !state_store->valid(handle) ||
                state_store->checkpoint_references(handle) != 0) {
                return;
            }
            if (!state_store->release(handle)) { fail(); }
        };
        const auto duplicates_binding = [&](StateImageHandle handle) {
            return handle == sequence.state.read || handle == sequence.state.write;
        };

        release_if_unreferenced(sequence.state.write, true);
        if (sequence.state.read != sequence.state.write) {
            release_if_unreferenced(sequence.state.read, !sequence.state.read_has_external_owner());
        }
        if (sequence.rewrite_state) {
            release_if_unreferenced(*sequence.rewrite_state,
                                    !duplicates_binding(*sequence.rewrite_state));
        }
        for (std::size_t index = 0; index < sequence.long_anchors.size(); ++index) {
            const StateImageHandle handle = sequence.long_anchors[index].state;
            bool repeated                 = duplicates_binding(handle) ||
                            (sequence.rewrite_state && handle == *sequence.rewrite_state);
            for (std::size_t prior = 0; !repeated && prior < index; ++prior) {
                repeated = sequence.long_anchors[prior].state == handle;
            }
            release_if_unreferenced(handle, !repeated);
        }
        if (sequence.reserved_state) {
            const StateImageHandle handle = *sequence.reserved_state;
            bool repeated                 = duplicates_binding(handle) ||
                            (sequence.rewrite_state && handle == *sequence.rewrite_state);
            for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
                repeated = repeated || anchor.state == handle;
            }
            release_if_unreferenced(handle, !repeated);
        }
    } catch (...) { fail(); }

    sequence.state          = {};
    sequence.rewrite_state  = std::nullopt;
    sequence.reserved_state = std::nullopt;
    sequence.endpoint_valid = false;
    sequence.long_anchors.clear();
    sequence.tail_hidden               = {};
    sequence.rewrite_checkpoint_hidden = {};
}

void ProgramImpl::release_sequence_state_strict(SequenceState& sequence) noexcept {
    const auto fail = []() noexcept { std::terminate(); };
    if (!state_store || sequence.state.fork_pending) { fail(); }

    try {
        if (sequence.rewrite_state) {
            state_store->release_checkpoint_reference(*sequence.rewrite_state);
        }
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            state_store->release_checkpoint_reference(anchor.state);
        }

        const auto release_if_unreferenced = [&](StateImageHandle handle, bool lifetime_owned) {
            if (!lifetime_owned || !state_store->valid(handle) ||
                state_store->checkpoint_references(handle) != 0) {
                return;
            }
            if (!state_store->release(handle)) { fail(); }
        };
        const auto repeated_before_anchor = [&](std::size_t anchor_index, StateImageHandle handle) {
            if ((sequence.endpoint_valid &&
                 (handle == sequence.state.read || handle == sequence.state.write)) ||
                (sequence.rewrite_state && handle == *sequence.rewrite_state)) {
                return true;
            }
            for (std::size_t prior = 0; prior < anchor_index; ++prior) {
                if (sequence.long_anchors[prior].state == handle) { return true; }
            }
            return false;
        };

        if (sequence.endpoint_valid) {
            release_if_unreferenced(sequence.state.write, true);
            if (sequence.state.read != sequence.state.write) {
                release_if_unreferenced(sequence.state.read,
                                        !sequence.state.read_has_external_owner());
            }
        }
        if (sequence.rewrite_state) {
            const StateImageHandle handle = *sequence.rewrite_state;
            const bool duplicates_endpoint =
                sequence.endpoint_valid &&
                (handle == sequence.state.read || handle == sequence.state.write);
            release_if_unreferenced(handle, !duplicates_endpoint);
        }
        for (std::size_t index = 0; index < sequence.long_anchors.size(); ++index) {
            const StateImageHandle handle = sequence.long_anchors[index].state;
            release_if_unreferenced(handle, !repeated_before_anchor(index, handle));
        }
        if (sequence.reserved_state) {
            const StateImageHandle handle = *sequence.reserved_state;
            bool repeated                 = sequence.endpoint_valid &&
                            (handle == sequence.state.read || handle == sequence.state.write);
            repeated = repeated || (sequence.rewrite_state && handle == *sequence.rewrite_state);
            for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
                repeated = repeated || anchor.state == handle;
            }
            release_if_unreferenced(handle, !repeated);
        }
    } catch (...) { fail(); }

    sequence.state          = {};
    sequence.rewrite_state  = std::nullopt;
    sequence.reserved_state = std::nullopt;
    sequence.endpoint_valid = false;
    sequence.long_anchors.clear();
    sequence.tail_hidden               = {};
    sequence.rewrite_checkpoint_hidden = {};
}

void ProgramImpl::release_sequence_state(SequenceState& sequence) noexcept {
    if (!state_store) { return; }
    if (sequence.state.fork_pending && state_store->valid(sequence.state.read) &&
        state_store->valid(sequence.state.write)) {
        try {
            state_store->abort_fork(sequence.state.read, sequence.state.write);
        } catch (...) {}
    }

    try {
        if (sequence.rewrite_state && state_store->valid(*sequence.rewrite_state) &&
            state_store->checkpoint_references(*sequence.rewrite_state) != 0) {
            state_store->release_checkpoint_reference(*sequence.rewrite_state);
        }
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            if (state_store->valid(anchor.state) &&
                state_store->checkpoint_references(anchor.state) != 0) {
                state_store->release_checkpoint_reference(anchor.state);
            }
        }
    } catch (...) {}

    const auto releasable = [&](StateImageHandle handle) { return state_store->valid(handle); };
    if (releasable(sequence.state.write)) { (void)state_store->release(sequence.state.write); }
    if (!sequence.state.read_has_external_owner() && sequence.state.read != sequence.state.write &&
        releasable(sequence.state.read)) {
        (void)state_store->release(sequence.state.read);
    }
    if (sequence.rewrite_state) {
        const StateImageHandle handle = *sequence.rewrite_state;
        const bool duplicates_binding =
            handle == sequence.state.write ||
            (!sequence.state.read_has_external_owner() && handle == sequence.state.read);
        if (!duplicates_binding && releasable(handle)) { (void)state_store->release(handle); }
    }
    for (std::size_t index = 0; index < sequence.long_anchors.size(); ++index) {
        const StateImageHandle handle = sequence.long_anchors[index].state;
        bool duplicate =
            handle == sequence.state.write ||
            (!sequence.state.read_has_external_owner() && handle == sequence.state.read) ||
            (sequence.rewrite_state && handle == *sequence.rewrite_state);
        for (std::size_t previous = 0; !duplicate && previous < index; ++previous) {
            duplicate = sequence.long_anchors[previous].state == handle;
        }
        if (!duplicate && releasable(handle)) { (void)state_store->release(handle); }
    }
    if (sequence.reserved_state) {
        const StateImageHandle handle = *sequence.reserved_state;
        bool duplicate =
            handle == sequence.state.write ||
            (!sequence.state.read_has_external_owner() && handle == sequence.state.read) ||
            (sequence.rewrite_state && handle == *sequence.rewrite_state);
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            duplicate = duplicate || anchor.state == handle;
        }
        if (!duplicate && releasable(handle)) { (void)state_store->release(handle); }
    }
    sequence.state          = {};
    sequence.rewrite_state  = std::nullopt;
    sequence.reserved_state = std::nullopt;
    sequence.endpoint_valid = false;
    sequence.long_anchors.clear();
    sequence.tail_hidden               = {};
    sequence.rewrite_checkpoint_hidden = {};
}

void ProgramImpl::release_active_shared_references(SequenceState& sequence) noexcept {
    for (const std::uint32_t index : sequence.shared_prefix_references) {
        if (index >= shared_prefix_capacity ||
            shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued ||
            shared_prefix_states[index].active_references == 0) {
            continue;
        }
        --shared_prefix_states[index].active_references;
    }
    sequence.shared_prefix_references.clear();
}

qwen3_5::PagedKVCache* ProgramImpl::backend_kv_cache() noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) { return decoder->mtp_cache(); }
    if (dflash && dflash->full) { return &*dflash->full; }
    return nullptr;
}

const qwen3_5::PagedKVCache* ProgramImpl::backend_kv_cache() const noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) { return decoder->mtp_cache(); }
    if (dflash && dflash->full) { return &*dflash->full; }
    return nullptr;
}

std::uint32_t ProgramImpl::backend_kv_valid(const SequenceState& sequence) const noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) { return sequence.mtp_kv_valid; }
    if (speculative_backend == SpeculativeBackend::DFlash) {
        return sequence.dflash_context_frontier;
    }
    return 0;
}

void ProgramImpl::resize_sequence_kv_entitlement(SequenceState& sequence, std::uint32_t text_pages,
                                                 std::uint32_t backend_pages) {
    if (!sequence.kv || text_pages == 0 ||
        (sequence.kv->backend.has_value() != (backend_pages != 0))) {
        throw std::invalid_argument("KV resize entitlement does not match the sequence bundle");
    }
    text_kv_addresses->resize_entitlement(sequence.kv->text, text_pages);
    if (sequence.kv->backend) {
        backend_kv_addresses->resize_entitlement(*sequence.kv->backend, backend_pages);
    }
}

void ProgramImpl::bind_sequence_kv(SequenceState& sequence) {
    if (!sequence.kv) { throw std::logic_error("KV allocation bundle is unavailable"); }
    const std::int32_t row = static_cast<std::int32_t>(sequence.lane);
    const bool text_active = text_kv_addresses->active(sequence.kv->text);
    const bool backend_active =
        sequence.kv->backend && backend_kv_addresses->active(*sequence.kv->backend);
    if (sequence.kv->backend && text_active != backend_active) {
        throw std::logic_error("KV address-space activation is not bundle-atomic");
    }
    try {
        if (!text_active) {
            text_kv_addresses->activate(sequence.kv->text,
                                        text_kv_addresses->mapped_pages(sequence.kv->text), row,
                                        compute_streams);
            if (sequence.kv->backend) {
                backend_kv_addresses->activate(
                    *sequence.kv->backend,
                    backend_kv_addresses->mapped_pages(*sequence.kv->backend), row,
                    compute_streams);
            }
        }
        set_device_i32(io.text_kv_table_row, text_kv_addresses->bound_row(sequence.kv->text));
        set_device_i32(io.backend_kv_table_row,
                       sequence.kv->backend ? backend_kv_addresses->bound_row(*sequence.kv->backend)
                                            : 0);
    } catch (...) {
        if (!text_active) {
            if (sequence.kv->backend && backend_kv_addresses->active(*sequence.kv->backend)) {
                backend_kv_addresses->deactivate(*sequence.kv->backend);
            }
            if (text_kv_addresses->active(sequence.kv->text)) {
                text_kv_addresses->deactivate(sequence.kv->text);
            }
        }
        throw;
    }
}

void ProgramImpl::unbind_sequence_kv(SequenceState& sequence) noexcept {
    if (!sequence.kv) { return; }
    try {
        if (sequence.kv->backend && backend_kv_addresses->active(*sequence.kv->backend)) {
            backend_kv_addresses->deactivate(*sequence.kv->backend);
        }
    } catch (...) {}
    try {
        if (text_kv_addresses->active(sequence.kv->text)) {
            text_kv_addresses->deactivate(sequence.kv->text);
        }
    } catch (...) {}
}

// LOCAL PROTOTYPE (KVMem-style ring retrieval): bring the selected Host-resident pages back onto the
// Device (H2D) and re-publish their block-table slots. Returns the retrieved pages that are Device-
// resident once this returns, which is exactly the set the retrieval mask may select (a page that was
// already resident also counts, and a failed restore simply drops out).
std::vector<std::uint32_t>
ProgramImpl::restore_kv_pages_from_host(KVAddressSpaceStore& addresses, LogicalKVPageStore& pages,
                                        const KVAddressSpaceHandle& address,
                                        std::span<const std::uint32_t> logical_pages) {
    std::vector<std::uint32_t> resident;
    if (!addresses.valid(address) || logical_pages.empty()) { return resident; }
    if (host_kv_extents == nullptr) {
        for (const std::uint32_t page : logical_pages) {
            if (pages.device_resident(addresses.logical_page(address, page))) {
                resident.push_back(page);
            }
        }
        return resident;
    }
    for (const std::uint32_t page : logical_pages) {
        const LogicalKVPageHandle logical = addresses.logical_page(address, page);
        if (!pages.valid(logical)) { continue; }
        if (pages.device_resident(logical)) {
            resident.push_back(page);
            continue;
        }
        if (!pages.host_resident(logical)) { continue; }
        // SAFETY: never materialize into a full pool. A page that cannot be brought back is simply
        // left out of the mask, which hides it -- it can never be read from a stale Device slot.
        if (pages.physical_pool().free_runs().empty()) { break; }
        const HostKVPageReplica replica = pages.host_replica(logical);
        DeviceKVPageReservation reservation = pages.physical_pool().make_empty_reservation();
        pages.physical_pool().resize_reservation(reservation, 1);
        const HostKVAllocationConstView source =
            host_kv_extents->view(replica.extent).subview(replica.page_offset, 1);
        if (addresses.restore_page_from_host(address, page, source, reservation, device.stream)) {
            resident.push_back(page);
        }
    }
    return resident;
}

// D-5 seam completion moved to ops/kvmem/kvmem_page_selection.h (2026-10-16): it is a closed
// transformation of a page set, it owns no sequence state, and living here -- behind a TU that pulls
// the whole engine -- is why it had no test. The policy note moved with it.

// How many pages on EITHER side of a selected page are brought back with it.
//   0 = the shipped behaviour, i.e. this fix's NEGATIVE CONTROL (`NINFER_KV_NEIGHBOR_PAGES=0`)
//   1 = +-1 page (the default; a 64-token page boundary can only separate two halves)
//   2 = +-2 pages, only ever if the budget allows it
std::uint32_t proto_kv_neighbor_pages() {
    static const std::uint32_t reach = [] {
        const char* text = std::getenv("NINFER_KV_NEIGHBOR_PAGES");
        if (text == nullptr) { return 1U; }
        const long value = std::strtol(text, nullptr, 10);
        return value <= 0 ? 0U : static_cast<std::uint32_t>(value);
    }();
    return reach;
}

// G7/G4 ring adaptive budget: how thin the budget's boundary is. Moved to
// ops/kvmem/kvmem_page_selection.h (2026-10-16) with add_seam_neighbors, for the same reason -- the
// float overload is the content scorer's path and the double one is the lexical fallback's, and the
// two are the negative control for each other, so they need a test more than they need to be file-local.

// LOCAL PROTOTYPE (KVMem-style ring retrieval): rank the pages that are (or are about to become)
// Host-resident by lexical overlap with the recent context, and return the top `budget` page
// indices. The score is an inverse-document-frequency weighted token overlap, which is what makes
// a rare match (a variable name, an error string, a code) dominate ordinary function words. Token
// ids live on the Host in the sequence ledger, so this needs no Device work.
std::vector<std::uint32_t> ProgramImpl::select_retrieval_pages(SequenceState& sequence,
                                                              std::uint32_t begin,
                                                              std::uint32_t end,
                                                              std::uint32_t budget,
                                                              RingBudgetMargin* margin_out) const {
    std::vector<std::uint32_t> selected;
    // Clear on entry so EVERY early return below (no budget, no candidates, empty ledger, empty query)
    // leaves `valid == false`. Without this, a path that returns before measuring would silently leave
    // the PREVIOUS reselection's margin in place, and the gate would grow the budget on the strength of a
    // cut that is no longer the one being made. Only a cut that actually happened sets `valid`.
    if (margin_out != nullptr) {
        margin_out->ratio = 0.0F;
        margin_out->valid = false;
    }
    // A1: open a retrieval round per select_retrieval_pages call (not per prefill chunk), so
    // history decays per retrieval round instead of per chunk. Only when LRFU is enabled; otherwise
    // the round is left inactive (token==0) and round_add is a no-op.
    if (ops::detail::kvmem_lrfu_enabled()) {
        std::uint64_t next_token = sequence.lrfu.token + 1;
        if (next_token == 0) { ++next_token; }
        ops::detail::kvmem_lrfu_round_begin(sequence.lrfu, next_token, sequence.lrfu.history);
        std::fprintf(stderr, "[lrfu] round_boundary seq_token=%llu round=%llu\n",
                     static_cast<unsigned long long>(sequence.lrfu.token),
                     static_cast<unsigned long long>(sequence.lrfu.preview.ks_round));
    }
    if (budget == 0 || end <= begin) { return selected; }
    // ---- LOCAL (KVMem content scoring, 2026-10-02) ---------------------------------------------
    // Prefer the CONTENT ranking when the scorer published one. It ranks pages by a query-conditioned
    // dot product against the mean-K index (ops/kvmem/kvmem_retrieve.h, Eq.10), so a page is found
    // because it is ABOUT the question, not because it happens to share rare WORDS with it. That
    // lexical dependence is the measured defect: with the IDF path below, a question that is
    // semantically the same but lexically disjoint lost the mid-prompt needle in 2 of 6 trials.
    //
    // The ring's restore budget is smaller than the selector's own budget, so the kept set is ranked
    // again here and cut to `budget`; ids are ascending by contract (kvmem_select.h:30-33).
    //
    // Everything below stays untouched and is the fallback -- and also this change's NEGATIVE CONTROL:
    // with NINFER_TERNARY_KVMEM_SCORE unset nothing is published, this block is skipped, and the
    // engine must reproduce the old readings exactly (the disjoint arm goes back to its pre-fix 4/6).
    {
        // B06 + (2026-10-16): read THIS sequence's probe -- the one the scorer wrote. It is owned by
        // the sequence (prefill binds it unconditionally), so a sequence that has not scored has no
        // probe, and there is deliberately nothing to fall back to: the old fallback was a process-
        // resident thread_local, i.e. the previous request on this lane, and reading it here would
        // restore the pages THAT request asked for. That is the silent wrong retrieval this whole
        // instrument exists to remove, so an absent probe falls through to the lexical path below.
        const ops::detail::KvMemScoreProbe* probe = sequence.kvmem_score_workspace.get();
        static const std::vector<std::int32_t> no_blocks;
        static const std::vector<float> no_scores;
        const std::vector<std::int32_t>& kept = probe != nullptr ? probe->kept : no_blocks;
        const std::vector<float>& scores = probe != nullptr ? probe->score_host : no_scores;
        const std::int32_t scored = probe != nullptr ? probe->scored_n_blocks : 0;
        if (!kept.empty() && scored > 0 && !scores.empty()) {
            std::vector<std::pair<float, std::uint32_t>> ranked;
            ranked.reserve(kept.size());
            // ★ LRFU（2026-10-15，整编点）：LRFU 开时按 lrfu_score（candidate 优先，否则
            //   committed）重排 kept；关时 kvmem_score_rank_key 返回当轮分 = 逐位旧行为（负控）。
            //   kept 仍由当轮 kvmem_select_blocks 产生（上面读的 kept），这里只换重排键 ——
            //   ks 绝不写回 set_retrieval_scores 信号（D1 的另一半）。
            const bool lrfu_on = ops::detail::kvmem_score_lrfu_on();
            for (const std::int32_t block : kept) {
                if (block < 0 || block >= scored) { continue; }
                const std::uint32_t page = static_cast<std::uint32_t>(block);
                if (page < begin || page >= end) { continue; }
                if (static_cast<std::size_t>(block) >= scores.size()) { continue; }
                ranked.emplace_back(
                    lrfu_on ? ops::detail::kvmem_lrfu_score(sequence.lrfu.preview,
                                                          static_cast<std::size_t>(block))
                            : scores[static_cast<std::size_t>(block)],
                    page);
            }
            if (!ranked.empty()) {
                std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
                    return left.first != right.first ? left.first > right.first
                                                     : left.second < right.second;
                });
                ops::detail::kvmem_measure_budget_margin(ranked, budget, margin_out);
                if (ranked.size() > budget) { ranked.resize(budget); }
                // D-5: complete the evidence of the pages we keep, in score order, while the budget
                // lasts. The result is re-sorted below because callers binary-search it
                // (demote_kv_pages_to_host's is_preferred, context.cpp:1871).
                std::vector<std::uint32_t> by_score;
                by_score.reserve(ranked.size());
                for (const auto& entry : ranked) { by_score.push_back(entry.second); }
                selected = ops::detail::add_seam_neighbors(std::move(by_score), begin, end, budget,
                                                           proto_kv_neighbor_pages());
                if (selected.empty()) {
                    selected.reserve(ranked.size());
                    for (const auto& entry : ranked) { selected.push_back(entry.second); }
                }
                std::sort(selected.begin(), selected.end());
                return selected;
            }
        }
    }
    // The ledger is only needed by the lexical fallback below; an empty ledger is not a reason to skip
    // the content path (which reads the index, not the ledger).
    if (sequence.ledger.empty()) { return selected; }
    constexpr std::size_t kQueryWindow = 256;
    const std::size_t query_begin =
        sequence.ledger.size() > kQueryWindow ? sequence.ledger.size() - kQueryWindow : 0;
    std::unordered_set<TokenId> query;
    for (std::size_t index = query_begin; index < sequence.ledger.size(); ++index) {
        query.insert(sequence.ledger[index]);
    }
    if (query.empty()) { return selected; }
    const std::size_t page_size = static_cast<std::size_t>(kPagedKVPageSize);
    std::vector<std::vector<TokenId>> page_tokens;
    page_tokens.reserve(end - begin);
    std::unordered_map<TokenId, std::uint32_t> document_frequency;
    for (std::uint32_t page = begin; page < end; ++page) {
        const std::size_t first = static_cast<std::size_t>(page) * page_size;
        if (first >= sequence.ledger.size()) { break; }
        const std::size_t last = std::min(first + page_size, sequence.ledger.size());
        std::vector<TokenId> distinct;
        for (std::size_t index = first; index < last; ++index) {
            const TokenId token = sequence.ledger[index];
            if (query.count(token) == 0) { continue; }
            if (std::find(distinct.begin(), distinct.end(), token) != distinct.end()) { continue; }
            distinct.push_back(token);
        }
        for (const TokenId token : distinct) { ++document_frequency[token]; }
        page_tokens.push_back(std::move(distinct));
    }
    const double documents = static_cast<double>(page_tokens.size());
    std::unordered_map<TokenId, double> weight;
    weight.reserve(document_frequency.size() * 2 + 1);
    for (const auto& entry : document_frequency) {
        weight.emplace(entry.first,
                       std::log((documents + 1.0) / (static_cast<double>(entry.second) + 1.0)) + 1.0);
    }
    std::vector<std::pair<double, std::uint32_t>> scored;
    scored.reserve(page_tokens.size());
    for (std::size_t offset = 0; offset < page_tokens.size(); ++offset) {
        double score = 0.0;
        for (const TokenId token : page_tokens[offset]) { score += weight[token]; }
        if (score > 0.0) { scored.emplace_back(score, begin + static_cast<std::uint32_t>(offset)); }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
        return left.first != right.first ? left.first > right.first : left.second < right.second;
    });
    ops::detail::kvmem_measure_budget_margin(scored, budget, margin_out);
    if (scored.size() > budget) { scored.resize(budget); }
    selected.reserve(scored.size());
    for (const auto& entry : scored) { selected.push_back(entry.second); }
    std::sort(selected.begin(), selected.end());
    return selected;
}

std::uint32_t ProgramImpl::demote_other_addresses_to_host(KVAddressSpaceStore& addresses,
                                                          LogicalKVPageStore& pages,
                                                          const KVAddressSpaceHandle& skip,
                                                          std::uint32_t target_free) {
    if (host_kv_extents == nullptr || target_free == 0) { return 0; }
    std::uint32_t freed = 0;
    std::uint32_t slots_visited = 0, skip_invalid = 0, skip_noresident = 0, skip_active_hold = 0,
                  skip_pins = 0, skip_activation = 0, skip_stalehost = 0, skip_partial = 0,
                  skip_duplicate = 0;
    // LOCAL DIAGNOSTIC (demote-counters): the fourth way to end with freed=0 was invisible. A
    // refused host-arena reservation (HostKVExtentStore full) returns false from flush() without
    // bumping any counter -- and the tail call at the end of this function dropped the return
    // value outright. Counted here so `demote-other short` can name it. Read-only observation.
    std::uint32_t flush_failed = 0;
    // LOCAL FIX (2026-10-02): a SECOND, distinct refusal reason. `flush_failed` means the Host arena
    // was full; this one means the Host store REFUSED to publish the batch (see the `publish declined`
    // line in host_kv_store.h -- it used to be a silent `std::terminate`). They need different
    // responses from a reader: one says "raise --host-kv-mib", the other says "a page could not be
    // attached", so folding them into one counter would hide which one is happening.
    std::uint32_t flush_declined = 0;
    std::vector<LogicalKVPageHandle> batch;
    batch.reserve(64);
    const auto flush = [&]() -> bool {
        if (batch.empty()) { return true; }
        std::optional<HostKVExtentReservation> reserved = host_kv_extents->prepare(pages, batch);
        if (!reserved) { ++flush_failed; return false; }   // Host arena full: nothing more can be moved
        const std::vector<DeviceKVPageHandle> sources = host_kv_extents->device_sources(*reserved);
        // Copy on the compute stream so the D2H is ordered after the kernels that wrote them.
        pages.physical_pool().copy_to_host(sources, host_kv_extents->writable_view(*reserved),
                                           device.stream);
        // LOCAL FIX (2026-10-02): publish declines instead of terminating, so this batch simply does
        // not happen -- every Device page in it keeps its replica and its active reference (the loop
        // below never runs), i.e. the data is intact and the ring can try again later. Same shape as
        // the `prepare` refusal just above, with its own counter so the log names the real reason.
        if (!host_kv_extents->publish(std::move(*reserved))) {
            ++flush_declined;
            return false;
        }
        for (const LogicalKVPageHandle logical : batch) {
            if (!pages.drop_device_replica(logical)) { return false; }
        }
        freed += static_cast<std::uint32_t>(batch.size());
        batch.clear();
        return true;
    };
    addresses.for_each_address([&](const KVAddressSpaceHandle& handle, bool active,
                                   std::uint32_t mapped) {
        if (freed >= target_free || active || handle == skip) { return; }
        ++slots_visited;
        for (std::uint32_t page = 0; page < mapped && freed + batch.size() < target_free; ++page) {
            const LogicalKVPageHandle logical = addresses.logical_page(handle, page);
            if (!pages.valid(logical)) { ++skip_invalid; continue; }
            if (!pages.device_resident(logical)) { ++skip_noresident; continue; }
            // An inactive alias owns no residency hold. A nonzero aggregate therefore belongs to an
            // active sibling and forbids this alias from taking its shared Device replica.
            if (addresses.has_device_residency_hold(handle, page) ||
                pages.active_address_references(logical) != 0) {
                ++skip_active_hold;
                continue;
            }
            if (pages.source_pins(logical) != 0) { ++skip_pins; continue; }
            // LOCAL FIX (2-lane pending activation, 2026-10-17): a page another lane's activation
            // has already charged to itself is NOT free cache, even though it is reachable through
            // no active address and carries no writer, active reference or source pin. Its commit
            // requires the Device replica (kv_store.h commit_activation), and dropping it here
            // killed the engine: the refusal propagated into an abort that released the address
            // underneath its own staging. Skipped explicitly -- rather than left to
            // can_drop_device_replica -- so a refused page never enters `batch`, whose flush() stops
            // at the first page it cannot drop.
            if (pages.activation_pins(logical) != 0) { ++skip_activation; continue; }
            // LOCAL FIX (ring decode; prescription: 交棒-ninfer解耦第四棒 §4.3): only a page that can
            // never receive another coverage commit may give up its Device replica. A partially
            // committed page keeps a live commit path (commit_coverage requires a Device replica and
            // writer_references == 1), so demoting it made the next commit throw
            // "logical KV committed coverage is not monotonic". Measured 2026-09-30 with pool 16
            // pages < prompt+output during decode.
            if (pages.committed_columns(logical) != static_cast<std::uint32_t>(kPagedKVPageSize)) {
                ++skip_partial;
                continue;
            }
            // NOTE: snapshot protection (protected_columns) is deliberately NOT consulted here.
            // Protection keeps pages resident for fork-based reuse, and reuse of a host-demoted
            // source is declined in ring mode. Demoting to the Host preserves the content and the
            // descriptor, so the checkpoint stays restorable; only the Device replica goes.
            if (pages.host_resident(logical)) {
                // Content already on the Host: just give up the Device replica.
                if (!pages.host_replica_current(logical)) { ++skip_stalehost; continue; }
                pages.set_writer(logical, false);
                if (pages.drop_device_replica(logical)) { ++freed; }
                continue;
            }
            // Content not on the Host yet: the extent store pins through `can_pin_source`, which
            // needs the writer reference clear (inactive addresses normally hold none, but be safe).
            pages.set_writer(logical, false);
            // LOCAL FIX (2026-10-02, step 1 of the publish patch -- the ROOT-CAUSE half):
            // a logical page must not enter the SAME batch twice. fork/COW pages are reference-counted,
            // so one logical page can be reached through two non-live addresses; the batch then asks
            // the Host store to attach two replicas for it, the second attach throws, and with the old
            // `noexcept -> std::terminate` publish the whole batch (up to 64 pages) died with it --
            // `abort()`, no log, no dump (K's ebd7ad46). Skipping the duplicate REMOVES the cause; the
            // decline path in publish() is only the safety net behind it.
            if (std::find(batch.begin(), batch.end(), logical) != batch.end()) {
                ++skip_duplicate;
                continue;
            }
            batch.push_back(logical);
            if (batch.size() >= 64 && !flush()) { return; }
        }
    });
    // The tail flush is the last chance to bank the pending batch; its return value used to be
    // dropped here, so a failure at this exact point was indistinguishable from "no candidates".
    // flush() now counts its own refusals in flush_failed, tail call included.
    (void)flush();
    // LOCAL DIAGNOSTIC (demote-counters): when inactive addresses cannot cover the
    // deficit the live lane cannibalizes its own bankable head, so a bank-miss
    // needs to know WHY the dead residents would not drain. Name the blocker.
    if (freed < target_free) {
        std::fprintf(stderr,
                     "[ninfer] demote-other short: freed=%u want=%u slots=%u invalid=%u "
                     "noresident=%u activehold=%u pins=%u activation=%u stalehost=%u partial=%u dup=%u "
                     "flushfail=%u declined=%u pending=%zu\n",
                     freed, target_free, slots_visited, skip_invalid, skip_noresident,
                     skip_active_hold, skip_pins, skip_activation, skip_stalehost, skip_partial,
                     skip_duplicate, flush_failed, flush_declined, batch.size());
    }
    return freed;
}

std::uint32_t ProgramImpl::demote_kv_pages_to_host(KVAddressSpaceStore& addresses,
                                                   LogicalKVPageStore& pages,
                                                   const KVAddressSpaceHandle& address,
                                                   std::uint32_t sink_pages,
                                                   std::uint32_t target_free,
                                                   std::span<const std::uint32_t> preferred) {
    if (host_kv_extents == nullptr || !addresses.valid(address) || target_free == 0) { return 0; }
    const std::uint32_t mapped = addresses.mapped_pages(address);
    const auto is_preferred = [&](std::uint32_t page) {
        return !preferred.empty() && std::binary_search(preferred.begin(), preferred.end(), page);
    };
    // Oldest first, skipping the attention sink and (in the first pass) the pages retrieval wants to
    // keep. If that is not enough the second pass gives up the retrieved pages too, so the caller is
    // guaranteed to get the room it asked for whenever the Device can supply it at all.
    std::uint32_t demoted   = 0;
    std::uint32_t page      = sink_pages;
    bool second_pass        = false;
    while (demoted < target_free) {
        if (page >= mapped) {
            if (second_pass) { break; }
            second_pass = true;
            page        = sink_pages;
            continue;
        }
        const std::uint32_t current       = page;
        const LogicalKVPageHandle logical = addresses.logical_page(address, current);
        ++page;
        if (!second_pass && is_preferred(current)) { continue; }
        // LOCAL FIX (2-lane pending activation, 2026-10-17): the same hold as in
        // demote_other_addresses_to_host. This lane's own address can share a page with another
        // lane's pending activation through a fork, and the second pass below deliberately gives up
        // the retrieved pages -- it must not give up a page a commit is waiting on.
        if (!pages.valid(logical) || !pages.device_resident(logical) ||
            pages.source_pins(logical) != 0 || pages.activation_pins(logical) != 0) {
            continue;
        }
        // LOCAL FIX (ring decode; prescription: 交棒-ninfer解耦第四棒 §4.3): only a page that can never
        // receive another coverage commit may be demoted. The tail page the decode step is still
        // writing (or the current prefill chunk's pages) is partially committed and keeps a live
        // commit path; demoting it made the next commit throw "logical KV committed coverage is not
        // monotonic" (measured 2026-09-30: pool 16 pages, context grew past the pool during decode).
        if (pages.committed_columns(logical) != static_cast<std::uint32_t>(kPagedKVPageSize)) {
            continue;
        }
        if (!pages.host_resident(logical)) {
            // The content is not on the Host yet: copy it there first. The Host extent store pins
            // through `can_pin_source`, which requires the writer reference to be clear; an active
            // sequence keeps one on every mapped page and these pages are outside the window.
            pages.set_writer(logical, false);
            std::vector<LogicalKVPageHandle> one{logical};
            std::optional<HostKVExtentReservation> reserved = host_kv_extents->prepare(pages, one);
            if (!reserved) { break; }   // Host arena full: nothing more can be moved
            const std::vector<DeviceKVPageHandle> sources =
                host_kv_extents->device_sources(*reserved);
            // Copy on the compute stream so the D2H is ordered after the kernels that wrote it.
            pages.physical_pool().copy_to_host(sources, host_kv_extents->writable_view(*reserved),
                                               device.stream);
            // LOCAL FIX (2026-10-02): a declined publish stops the demotion exactly like the `prepare`
            // refusal above -- `break` skips the address-owned residency-hold release below, so this
            // page keeps its Device replica and its data. Never drop a replica that has no Host copy.
            if (!host_kv_extents->publish(std::move(*reserved))) {
                // No counter here on purpose: this function has no summary line to carry it, and the
                // store already named the page and the reason in its own `publish declined` line.
                break;
            }
        } else {
            // Already preserved on the Host (demoted before and then retrieved): no transfer needed.
            if (!pages.host_replica_current(logical)) { continue; }
            pages.set_writer(logical, false);
        }
        if (!addresses.release_device_residency_hold(address, current)) { continue; }
        if (!pages.drop_device_replica(logical)) {
            // A shared active sibling still owns this replica. This row remains mapped to the same
            // physical page, so it must keep its own hold rather than silently surrender it.
            addresses.acquire_device_residency_hold(address, current);
            continue;
        }
        ++demoted;
    }
    return demoted;
}

namespace {
// LOCAL PROTOTYPE: the retrieval policy's placeholder window, read once from the environment so
// the mask path can be exercised end to end without a CLI change.
//   NINFER_KV_WINDOW=<tokens>  keep only the newest N tokens on the Device (0/unset = dense)
//                             the effective window shrinks to fit the Device pool
// LOCAL PROTOTYPE (KVMem-style ring): a preserved attention "sink" is deliberately NOT supported,
// because the ring demotes the oldest pages first; a sink would read a recycled Device slot.
std::uint32_t proto_env_pages(const char* name) {
    const char* text = std::getenv(name);
    if (text == nullptr) { return 0; }
    const long tokens = std::strtol(text, nullptr, 10);
    if (tokens <= 0) { return 0; }
    return static_cast<std::uint32_t>(tokens / static_cast<long>(kPagedKVPageSize));
}
std::uint32_t proto_kv_window_pages() {
    static const std::uint32_t pages = proto_env_pages("NINFER_KV_WINDOW");
    return pages;
}

std::uint32_t proto_pages_for_tokens(std::uint32_t tokens) {
    if (tokens == 0) { return 0; }
    return (tokens + static_cast<std::uint32_t>(kPagedKVPageSize) - 1U) /
           static_cast<std::uint32_t>(kPagedKVPageSize);
}

std::uint32_t proto_kv_sink_pages() {
    // StreamingLLM-style attention sink: the first pages are never demoted and stay selected.
    static const std::uint32_t pages = [] {
        const char* text = std::getenv("NINFER_KV_SINK");
        if (text == nullptr) { return 4U; }   // 4 pages = 256 tokens
        const long tokens = std::strtol(text, nullptr, 10);
        if (tokens <= 0) { return 0U; }
        return static_cast<std::uint32_t>(tokens / static_cast<long>(kPagedKVPageSize));
    }();
    return pages;
}

std::uint32_t proto_kv_retrieve_pages() {
    // Retrieval budget: how many Host-resident pages to bring back on the basis of relevance.
    static const std::uint32_t pages = proto_env_pages("NINFER_KV_RETRIEVE");
    return pages;
}
// B32 (2026-10-07): is the retrieval budget literally UNSET? proto_env_pages cannot tell "unset" from
// an explicit 0 (both return 0), and an explicit value must stay authoritative -- a switch the operator
// set and the engine silently overrode is the defect class B25 was about ("=0 must mean 0"). The
// fallback below therefore fires only when the variable is absent or empty.
bool retrieve_budget_unset() {
    const char* text = std::getenv("NINFER_KV_RETRIEVE");
    return text == nullptr || text[0] == '\0';
}

// B32 (2026-10-07): the off switch for the retrieval fallback added at its call site. Unset -- or any
// value whose first character is not '0' -- keeps the fallback ON, matching how this file reads every
// other switch. It exists only so the fallback can be turned off for a negative control.
bool retrieve_auto_enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("NINFER_KV_RETRIEVE_AUTO");
        return text == nullptr || text[0] == '\0' || text[0] != '0';
    }();
    return enabled;
}

// LOCAL FIX (ring retrieval budget, 2026-10-02): the budget used to be a hard-wired quarter of the
// pool (`pool / 4U`), and in the delivered shape that quarter is the BINDING term -- pool = 280 pages
// in the shipped tier (`--kv-capacity 17920`), so the budget was min(128 pages from
// NINFER_KV_RETRIEVE=8192, 70) = SEVENTY pages out of 636 mapped. A page whose only overlap with the
// query is ordinary body vocabulary ranks just below the cut, so "the page the question is about"
// was admitted or dropped essentially by luck -- measured: 4 hits in 6 trials for a question that is
// semantically the same but shares no rare word with the marked line.
// The share is now a percentage of the pool with the same default as the old behaviour times two,
// and -- crucially -- it is read from the ENVIRONMENT so that tuning it costs a server restart
// instead of a full rebuild of this tree (an edit to this file alone is cheap; an edit to anything
// the CUDA wrappers include is not). 0 or >100 is ignored; NINFER_KV_RETRIEVE still caps it.
std::uint32_t proto_env_percent(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr) { return fallback; }
    const long value = std::strtol(text, nullptr, 10);
    if (value <= 0 || value > 100) { return fallback; }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t proto_kv_retrieve_share_percent() {
    static const std::uint32_t percent = proto_env_percent("NINFER_KV_RETRIEVE_SHARE", 50U);
    return percent;
}

// NOTE on the two room quantities (do not swap them):
//   usable_pages() - allocated_pages()  = pages materialize_one() can still hand out  (physical)
//   available_pages() = usable - (allocated + reserved) = pages we may still PROMISE          (accounting)
// The ring's room checks (ensure_ring_room, the judge) ask the first question, so they use the first.
// Planning paths (hybrid_program.cpp, context.cpp:2383/2389) ask the second, so they use available_pages().
// Reservations are logical promises (paged_kv_cache.cpp:386-389) and never prune free_page_runs_.
// ---- LOCAL (ring lanes, 2026-10-17): the ring's per-lane share of the Device page pool. -----------
// WHY THE POOL HAS TO BE DIVIDED. The Device page pool is ONE store that every lane maps into, and a
// lane can only demote ITS OWN pages: demote_kv_pages_to_host walks one address space, and
// demote_other_addresses_to_host skips every ACTIVE address. That skip is not an oversight -- a page
// already published in another lane's retrieval mask cannot be dropped, because the mask is installed
// once per step and the victim lane would then attend a physical page whose replica is gone.
// So a lane that sizes its budget from the WHOLE pool promises itself pages that the next lane cannot
// legally take back: with N lanes each asking for `pool * share%` the sum exceeds the pool, and the
// second lane meets the paged-KV reservation invariant with nothing demotable. That is the measured
// reason the ring refused --max-concurrency > 1 outright rather than degrading.
// Dividing by the lane count is what makes the sum fit: N lanes each staying inside pool/N never need
// to take a page from each other. At one lane this is the whole pool -- the arithmetic the single-lane
// ring has always run -- so the ON arm's numbers are unchanged by construction.
// (ring_lane_pool_pages / ring_lane_ceiling_pages / ring_lane_need_pages were here; moved to
// ring_lane_budget.h, definitions below the anonymous namespace closes so tests can link them.)
// ring_lane_ceiling_pages is replaced by ring_lane_dynamic_ceiling(pool, other_lanes_resident) below.

// ---- LOCAL (G7/G4 ring adaptive budget, 2026-10-15) ------------------------------------------------
// Readers for the budget gate. These deliberately do NOT reuse proto_env_pages or the "0 == unset"
// convention that the older knobs use, for two separate reasons:
//   * proto_env_pages returns PAGES and treats <=0 as unset. NINFER_KV_RETRIEVE_MAX does want pages and
//     does treat unset as "gate disabled", so proto_env_pages is exactly right for it (see below).
//   * SCALEUP/COOLDOWN/MARGIN_MIN are not page counts and 0 is a MEANINGFUL value for COOLDOWN (grow at
//     every reselection is aggressive but expressible), so reading 0 as "unset" would make a documented
//     setting unreachable -- the B25 defect class, where a negative control that equals a valid setting
//     silently falls back to the default and the "off" arm is indistinguishable from the "on" arm.
// Only a missing or EMPTY variable falls back.
std::uint32_t proto_env_u32(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') { return fallback; }
    const long value = std::strtol(text, nullptr, 10);
    return value < 0 ? fallback : static_cast<std::uint32_t>(value);
}

float proto_env_f32(const char* name, float fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') { return fallback; }
    const double value = std::strtod(text, nullptr);
    // NaN/Inf must not reach the comparison below: NaN < x is false, which would read as "margin is
    // fine" and silently pin the gate shut. Fall back to the default instead of trusting the parse.
    if (!std::isfinite(value) || value < 0.0) { return fallback; }
    return static_cast<float>(value);
}

// B_upper: the ceiling the gate may grow the retrieval budget to, in PAGES. UNSET (or <= 0) means the
// gate is DISABLED and the budget is bit-for-bit what it was before this change -- that is this
// feature's negative control, and it is the shipping default.
std::uint32_t proto_kv_retrieve_max_pages() {
    static const std::uint32_t pages = proto_env_pages("NINFER_KV_RETRIEVE_MAX");
    return pages;
}

// Growth factor per gate firing (paper §6 `scale_up`). 1 is legal and means "never grow", which makes
// it a second, independent way to disable the gate without unsetting B_upper.
std::uint32_t proto_kv_retrieve_scaleup() {
    static const std::uint32_t value = proto_env_u32("NINFER_KV_RETRIEVE_SCALEUP", 2U);
    return value;
}

// Reselections to skip after a growth (paper §6 cooldown; upstream review P0-5 asks for exactly this).
std::uint32_t proto_kv_retrieve_cooldown() {
    static const std::uint32_t value = proto_env_u32("NINFER_KV_RETRIEVE_COOLDOWN", 3U);
    return value;
}

// Margin threshold, in units of the ranking's own internal spacing (see RingBudgetMargin). The default
// 1.0 reads as: "the gap at the budget line is smaller than a typical step inside the kept set", i.e.
// the cut is a near-tie. That is the paper's thin-margin condition, expressed without inventing an
// absolute score scale -- and it is why this threshold needs no per-model tuning.
float proto_kv_retrieve_margin_min() {
    static const float value = proto_env_f32("NINFER_KV_RETRIEVE_MARGIN_MIN", 1.0F);
    return value;
}

// LOCAL PROTOTYPE (KVMem-style ring): the retrieval mask IS the Device-resident set. Pages past
// what has been mapped are selected too (the current chunk is writing into them). Building the mask
// page by page rather than as a range is what makes a "hole" left by a page that could not be
// demoted safe: a range would expose that hole's stale Device slot.
// LOCAL DIAGNOSTIC (mask-hidden): the mask IS the attention window, so a mapped page that is not
// Device-resident is silently unreadable -- that is the "middle of the prompt disappears" shape.
// Count exactly those pages and name the caller. `which` is the stream (text/backend), `site` is
// the installation point (mapped = the full ring mask, commit = the conservative safety net); both
// are pure labels. Nothing about the selection itself changes.
void install_resident_selection(qwen3_5::PagedKVCache* cache, std::uint32_t span_pages,
                                const KVAddressSpaceStore& addresses,
                                const LogicalKVPageStore& pages,
                                const KVAddressSpaceHandle& address, const char* which,
                                const char* site) {
    if (cache == nullptr || span_pages == 0 || !addresses.valid(address)) { return; }
    // The mask is per execution-table row, because that is the index the attention kernels resolve
    // for the block table and each sequence's logical-to-physical mapping is its own. An activated
    // sequence holds a row for the whole of its life (the row comes with its activation
    // reservation), so an unbound address here is a broken invariant rather than a routine path --
    // and installing no mask would leave the step running DENSE over pages the ring has demoted.
    // Reported once, in its own words, rather than as a mask-hidden line with invented counters.
    const std::int32_t row = addresses.bound_row(address);
    if (row < 0) {
        static bool warned_unbound = false;
        if (!warned_unbound) {
            warned_unbound = true;
            std::fprintf(stderr,
                         "[ninfer] mask NOT installed: which=%s site=%s span=%u -- the address holds "
                         "no execution row, so its selection mask has no row to go in\n",
                         which, site, span_pages);
        }
        return;
    }
    const std::size_t words = (static_cast<std::size_t>(span_pages) + 31U) / 32U;
    std::vector<std::uint32_t> bits(words, 0U);
    const std::uint32_t mapped = addresses.mapped_pages(address);
    std::uint32_t resident = 0, hidden = 0, first_hidden = 0, last_hidden = 0;
    // Where the prompt's middle is: page index in [35%, 70%) of the span handed to this mask.
    // Printed with its bounds so the reading needs no recomputation (page P starts at token P*64).
    const std::uint32_t mid_lo = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(span_pages) * 35U) / 100U);
    const std::uint32_t mid_hi = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(span_pages) * 70U) / 100U);
    std::uint32_t mid_hidden = 0;
    for (std::uint32_t page = 0; page < span_pages; ++page) {
        const bool selected =
            page >= mapped || pages.device_resident(addresses.logical_page(address, page));
        if (selected) {
            bits[page >> 5] |= (1U << (page & 31U));
            ++resident;
        } else {
            if (hidden == 0) { first_hidden = page; }
            last_hidden = page;
            ++hidden;
            if (page >= mid_lo && page < mid_hi) { ++mid_hidden; }
        }
    }
    cache->install_selection(row, bits);
    // Only a non-zero reading is news: a mask that hides nothing stays silent (no per-step spam).
    if (hidden != 0) {
        std::fprintf(stderr,
                     "[ninfer] mask hidden: which=%s site=%s span=%u resident=%u hidden=%u "
                     "first_hidden=%u last_hidden=%u mid_hidden=%u mid_lo=%u mid_hi=%u row=%d\n",
                     which, site, span_pages, resident, hidden, first_hidden, last_hidden,
                     mid_hidden, mid_lo, mid_hi, row);
    }
}
} // namespace

// Per-lane ring arithmetic (definitions; declarations in ring_lane_budget.h).
std::uint32_t ring_lane_pool_pages(std::uint32_t pool, std::uint32_t lanes) {
    return pool / std::max(1U, lanes);
}

std::uint32_t ring_lane_need_pages(std::uint32_t sink_pages, std::uint32_t budget) {
    return sink_pages + budget + kRingSlackPages;
}

std::uint32_t ring_lane_dynamic_ceiling(std::uint32_t pool, std::uint32_t other_lanes_resident) {
    const std::uint32_t slack = kRingSlackPages;
    if (pool <= slack + other_lanes_resident) { return 0U; }
    return pool - slack - other_lanes_resident;
}

void ProgramImpl::ensure_sequence_kv_lease(SequenceState& sequence, std::uint32_t main_tokens,
                                           std::uint32_t backend_tokens) {
    if (!context_cache.kv_lease_growth) { return; }
    // Only a decode step may extend the lease: materialization during admission has to leave the
    // sequence holding exactly the entitlement its plan declared.
    RequestControl& request = requests[sequence.lane];
    if (request.lifecycle != Lifecycle::Active) { return; }
    const std::uint32_t cushion    = kv_lease_cushion_pages();
    const std::uint32_t text_pages = text_kv_addresses->entitlement(sequence.kv->text);
    const std::uint32_t backend_pages =
        sequence.kv->backend ? backend_kv_addresses->entitlement(*sequence.kv->backend) : 0U;
    // The most a lease can use: every frontier up to the request's ceiling plus the drafts a
    // round may still verify past it. A lease that holds this needs no cushion beyond it, so a
    // full pool near the ceiling is not mistaken for a shortfall that ends the answer early.
    const std::uint32_t backend_ceiling =
        std::min(capacity, request.lease_ceiling + kv_lease_backend_allowance_tokens());
    const std::uint32_t reach = kv_pages_for_tokens(backend_ceiling);
    const bool main_thin =
        text_pages < reach && kv_pages_for_tokens(main_tokens) + cushion > text_pages;
    const bool backend_thin = sequence.kv->backend.has_value() && backend_tokens != 0 &&
                              backend_pages < reach &&
                              kv_pages_for_tokens(backend_tokens) + cushion > backend_pages;
    if (!main_thin && !backend_thin) { return; }

    // A full window first; when the pool cannot spare one, a step-sized extension still leaves
    // the cushion. No rung may exceed its pool or what one address can map (the cushion would
    // otherwise overshoot the context limit), so the reservation can only fail on space.
    // The ladder ends at one page group: a pool that can only spare its own allocation
    // granularity would otherwise settle with free page groups that no coarser rung can use.
    const auto target = [reach](std::uint32_t cap, std::uint32_t pages, std::uint32_t wanted) {
        return std::min(cap, std::max(pages, std::min(reach, wanted)));
    };
    const auto targets = [&](std::uint32_t extra_tokens) {
        return DeviceKVPages{
            .main = target(std::min(text_kv_pages->physical_pool().capacity_pages(),
                                    text_kv_addresses->page_capacity()),
                           text_pages,
                           kv_lease_pages_for_tokens(
                               std::min(request.lease_ceiling, main_tokens + extra_tokens))),
            .backend =
                sequence.kv->backend
                    ? target(std::min(backend_kv_pages->physical_pool().capacity_pages(),
                                      backend_kv_addresses->page_capacity()),
                             backend_pages,
                             backend_thin ? kv_lease_pages_for_tokens(std::min(
                                                backend_ceiling, backend_tokens + extra_tokens))
                                          : backend_pages)
                    : 0U,
        };
    };
    const auto grow = [&](DeviceKVPages wanted) {
        if ((main_thin && wanted.main <= text_pages) ||
            (backend_thin && wanted.backend <= backend_pages)) {
            return false;
        }
        resize_sequence_kv_entitlement(sequence, wanted.main, wanted.backend);
        // The extended lease is this owner's resource effect: keep the accounting the population
        // proofs read in step with it.
        request.active_resources.device.main_kv_pages += wanted.main - text_pages;
        request.active_resources.device.backend_kv_pages += wanted.backend - backend_pages;
        return true;
    };
    const std::uint32_t page = static_cast<std::uint32_t>(kPagedKVPageSize);
    bool space_limited       = false;
    for (const std::uint32_t extra_tokens : {kv_lease_growth_margin_tokens(), 2U * page, page}) {
        try {
            if (grow(targets(extra_tokens))) { return; }
        } catch (const std::bad_alloc&) { space_limited = true; }
    }

    // The pool cannot extend this lease. The sequence settles where the lease still covers a
    // step, so no launch can fail coverage mid-round. A space settlement records what its steps
    // asked for, so the engine can give it retained cache pages and resume growth before the
    // request's budget is bounded.
    request.lease_settled        = true;
    request.lease_space_limited  = space_limited;
    request.lease_minimum_target = targets(page);
}

std::optional<std::uint32_t>
ProgramImpl::device_kv_lease_settlement_tokens(SequenceHandle sequence,
                                               std::uint32_t forced_span_tokens) const noexcept {
    if (!valid_sequence(sequence)) { return std::nullopt; }
    const std::uint32_t lane = ContractAccess::lane(sequence).value;
    if (lane >= max_concurrency || active_continuations[lane] >= continuation_capacity ||
        !requests[lane].lease_settled) {
        return std::nullopt;
    }
    const SequenceState& state = active_sequence(lane);
    if (!state.kv) { return std::nullopt; }
    // The settle keeps the widest single step and the caller's forced control span unused, so
    // every launch it still licenses stays inside the lease.
    const std::uint32_t step = std::max(widest_verify_window() + 1U, forced_span_tokens);
    const std::uint32_t covered =
        std::min(state.kv->backend
                     ? kv_tokens_for_pages(backend_kv_addresses->entitlement(*state.kv->backend))
                     : std::numeric_limits<std::uint32_t>::max(),
                 kv_tokens_for_pages(text_kv_addresses->entitlement(state.kv->text)));
    const std::uint32_t frontier = state.execution_frontier;
    const std::uint32_t slack    = covered > frontier + step ? covered - frontier - step : 0U;
    const std::uint32_t forced   = forced_span_tokens == 0 ? 0U : forced_span_tokens + 1U;
    return std::max(std::max(1U, slack), forced);
}

std::optional<DeviceKVLeaseShortfall>
ProgramImpl::device_kv_lease_shortfall(SequenceHandle sequence) const noexcept {
    if (!valid_sequence(sequence)) { return std::nullopt; }
    const std::uint32_t lane = ContractAccess::lane(sequence).value;
    if (lane >= max_concurrency || active_continuations[lane] >= continuation_capacity) {
        return std::nullopt;
    }
    const RequestControl& request = requests[lane];
    if (!request.lease_settled || !request.lease_space_limited) { return std::nullopt; }
    const SequenceState& state = active_sequence(lane);
    if (!state.kv) { return std::nullopt; }
    const auto missing = [](std::uint32_t wanted, std::uint32_t held, std::uint32_t available) {
        const std::uint32_t need = wanted > held ? wanted - held : 0U;
        return need > available ? need - available : 0U;
    };
    const std::uint32_t text_held = text_kv_addresses->entitlement(state.kv->text);
    const std::uint32_t text_free = text_kv_pages->physical_pool().available_pages();
    DeviceKVLeaseShortfall out{
        .main_pages = missing(request.lease_minimum_target.main, text_held, text_free),
    };
    if (state.kv->backend && backend_kv_addresses && backend_kv_pages) {
        const std::uint32_t held = backend_kv_addresses->entitlement(*state.kv->backend);
        const std::uint32_t free = backend_kv_pages->physical_pool().available_pages();
        out.backend_pages        = missing(request.lease_minimum_target.backend, held, free);
    }
    return out;
}

bool ProgramImpl::resume_device_kv_lease(SequenceHandle sequence) noexcept {
    const std::optional<DeviceKVLeaseShortfall> shortfall = device_kv_lease_shortfall(sequence);
    if (!shortfall || shortfall->main_pages != 0 || shortfall->backend_pages != 0) {
        return false;
    }
    RequestControl& request      = requests[ContractAccess::lane(sequence).value];
    request.lease_settled        = false;
    request.lease_space_limited  = false;
    request.lease_minimum_target = {};
    return true;
}

DeviceKVPages
ProgramImpl::retained_device_kv_pages(const ContinuationHandle& continuation) const noexcept {
    if (!valid_continuation(continuation)) { return {}; }
    try {
        const detail::PhysicalResources owned =
            owner_exclusive_resources(continuation_states[ContractAccess::index(continuation)]);
        return {.main = owned.device.main_kv_pages, .backend = owned.device.backend_kv_pages};
    } catch (...) { return {}; }
}

DeviceKVPages
ProgramImpl::retained_device_kv_pages(const SharedPrefixHandle& shared) const noexcept {
    if (!valid_shared_prefix(shared)) { return {}; }
    try {
        const detail::PhysicalResources owned =
            owner_exclusive_resources(shared_prefix_states[ContractAccess::index(shared)]);
        return {.main = owned.device.main_kv_pages, .backend = owned.device.backend_kv_pages};
    } catch (...) { return {}; }
}

// ---- LOCAL (ring lanes, 2026-10-17): the ring's startup feasibility gate. -------------------------
// WHY THIS EXISTS. The ring used to refuse --max-concurrency > 1 outright, on the grounds that it
// "serves ONE active sequence: the ring cannot reclaim another lane's Device pages" (startup.cpp).
// The observation was right and the conclusion was too strong. What is true is that a lane's own
// pages are the only ones it can reclaim, so the pool has to be divided: N lanes each sizing
// themselves from the WHOLE pool ask for N x what exists, and the second lane to grow meets the
// paged-KV reservation invariant with nothing demotable (it cannot take the first lane's pages, and
// demote_other_addresses_to_host only drains INACTIVE addresses -- by design, since a page another
// lane has already published in its retrieval mask cannot be dropped under it).
// Dividing the pool by the lane count makes the sum fit, and then the question is arithmetic, not a
// fixed lane count: does one lane's sink + budget + slack fit inside one lane's share? If it does,
// the configuration is servable and refusing it would be refusing a working setup. If it does not,
// no amount of running would fix it, and the operator needs the numbers rather than a request that
// dies later. Checked against the pool that was actually BUILT (not the one requested), so it also
// catches an automatic kv_capacity that came out too small.
void ProgramImpl::validate_ring_lane_fit() const {
    if (proto_kv_window_pages() == 0 || text_kv_pages == nullptr) { return; }
    const std::uint32_t lanes     = std::max(1U, max_concurrency);
    const std::uint32_t pool      = text_kv_pages->physical_pool().usable_pages();
    const std::uint32_t lane_pool = ring_lane_pool_pages(pool, lanes);
    // The budget the runtime will actually use, evaluated the same way the budget block does: an
    // explicit NINFER_KV_RETRIEVE capped by this lane's share, or (when it is unset and the auto path
    // is enabled) the auto budget, which is itself capped by this lane's share and its slack. An
    // unset NINFER_KV_RETRIEVE with the auto path disabled is budget 0: retrieval never runs, so only
    // the sink and the slack have to fit.
    std::uint32_t budget = std::min(proto_kv_retrieve_pages(),
                                    lane_pool * proto_kv_retrieve_share_percent() / 100U);
    if (budget == 0U && retrieve_budget_unset() && retrieve_auto_enabled()) {
        budget = std::min(lane_pool * proto_kv_retrieve_share_percent() / 100U,
                          lane_pool > proto_kv_sink_pages() + kRingSlackPages
                              ? lane_pool - proto_kv_sink_pages() - kRingSlackPages
                              : 0U);
    }
    const std::uint32_t lane_need = ring_lane_need_pages(proto_kv_sink_pages(), budget);
    // Minimal refusal: not even a single lane can hold its attention-sink pin plus the restore slack.
    // Beyond this, over-subscription is tolerated -- a short request leaves share for other lanes, and
    // an adoption request that needs its whole range resident at once fails cleanly per request
    // (ContextCacheExhausted -> 429, counted in ninfer:context_cache_exhausted_requests_total).
    if (proto_kv_sink_pages() + kRingSlackPages > pool) {
        throw std::invalid_argument(
            std::string("KVMem ring: the Device KV pool cannot hold even one lane's attention-sink "
                        "pin plus restore slack. sink + slack = ") +
            std::to_string(proto_kv_sink_pages()) + " + " + std::to_string(kRingSlackPages) +
            " = " + std::to_string(proto_kv_sink_pages() + kRingSlackPages) +
            " page(s), but the pool only has " + std::to_string(pool) + " page(s).\n"
            "       Ways out: raise --kv-capacity, or lower NINFER_KV_SINK.");
    }
    if (lane_need <= lane_pool) { return; }
    // Over-subscribed for the requested concurrency: warn, do not refuse. Note that
    // lanes * (sink + slack) > pool is also only a warning here, per the user's over-subscription rule;
    // lanes will contend for the sink floor and the loser receives a clean per-request 429.
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
                     "[ninfer] KVMem ring: per-lane demand (%u = sink %u + budget %u + slack %u) exceeds "
                     "per-lane share (%u = pool %u / %u lane(s)) by %u page(s). Over-subscription is "
                     "tolerated; a short request leaves share for other lanes. A prefix-adoption request "
                     "that needs its whole range resident at once will fail cleanly per request.\n",
                     lane_need, proto_kv_sink_pages(), budget, kRingSlackPages,
                     lane_pool, pool, lanes, lane_need - lane_pool);
    }
}

// LOCAL FIX (B01 ring room): this beat used to live inside ensure_sequence_kv_mapped as a local
// lambda, so the two other places that map Device KV pages without going through it -- the DFlash
// context append (decode.cpp) and the causal-scoring lane (program_impl.cpp) -- asked the pool for
// pages without ever demoting, and hit the paged-KV reservation invariant once the pool was full
// with nothing demotable left. Same logic, one home, callable from every mapping site, with the
// early verdict inside so no caller can forget it.
void ProgramImpl::ensure_ring_room(KVAddressSpaceStore& addresses, LogicalKVPageStore& pages,
                                   const KVAddressSpaceHandle& address, std::uint32_t total_tokens,
                                   std::span<const std::uint32_t> preferred) {
    // No-op unless the ring is active: with a pool that covers the logical context every page is
    // resident anyway, and a demotion pass would be pure overhead on the hot path.
    if (proto_kv_window_pages() == 0 ||
        pages.physical_pool().usable_pages() >= addresses.logical_page_capacity()) {
        return;
    }
    // Keep a few pages of slack so the pool never runs at exactly 100%: the restore path always
    // needs a free Device page to work with. kRingSlackPages has one home (core/paged_kv_cache.h)
    // because the startup feasibility check refuses configurations using the same number.
    const auto free_pages = [](const LogicalKVPageStore& store) {
        const std::uint32_t usable    = store.physical_pool().usable_pages();
        const std::uint32_t allocated = store.physical_pool().allocated_pages();
        return usable > allocated ? usable - allocated : 0U;
    };
    const std::uint32_t sink_pages = proto_kv_sink_pages();
    const std::uint32_t total      = proto_pages_for_tokens(total_tokens);
    const std::uint32_t mapped     = addresses.mapped_pages(address);
    const std::uint32_t need       = total > mapped ? total - mapped : 0U;
    std::uint32_t wanted_extra     = 0;
    for (const std::uint32_t page : preferred) {
        const LogicalKVPageHandle logical = addresses.logical_page(address, page);
        if (pages.valid(logical) && !pages.device_resident(logical) &&
            pages.host_resident(logical)) {
            ++wanted_extra;   // a preferred page has to be brought back
        }
    }
    const std::uint32_t want_free   = need + kRingSlackPages + wanted_extra;
    const std::uint32_t free_now    = free_pages(pages);
    // ---- LOCAL (ring lanes, 2026-10-17): THIS LANE'S OWN CEILING, the second way the pool can be
    // short. -----------------------------------------------------------------------------------------
    // The free-page target below is a GLOBAL quantity, and the only pages a lane may reclaim are its
    // own (see ring_lane_pool_pages: another lane's pages are unreclaimable while its mask holds
    // them). So a lane can be inside the global target and still be unable to grow: if this lane
    // already holds more than its share, the pages it needs are ones it must give up itself, and the
    // global term does not ask it to. Enforcing the ceiling here is what makes N lanes coexist --
    // each stays inside pool/N, so the sum fits and no lane ever has to take a page from another.
    // At one lane the two terms are the same number and this changes nothing: free_now is at most
    // pool - lane_resident, so free_now >= want_free already implies
    // lane_resident + need + wanted_extra <= pool - kRingSlackPages == the ceiling.
    // Dynamic ceiling: pool minus slack minus UNIQUE physical replicas held by other lanes. An
    // aliased retained-prefix page consumes one Device slot even when several addresses name it.
    // A short request that holds few pages lets another lane grow into the remainder.
    std::uint32_t other_lanes_resident = 0;
    std::vector<LogicalKVPageHandle> other_lanes_seen;
    other_lanes_seen.reserve(pages.physical_pool().usable_pages());
    addresses.for_each_address([&](const KVAddressSpaceHandle& handle, bool /*active*/,
                                  std::uint32_t mapped) {
        if (handle == address) { return; }
        for (std::uint32_t page = 0; page < mapped; ++page) {
            const LogicalKVPageHandle logical = addresses.logical_page(handle, page);
            if (!pages.valid(logical) || !pages.device_resident(logical) ||
                std::find(other_lanes_seen.begin(), other_lanes_seen.end(), logical) !=
                    other_lanes_seen.end()) {
                continue;
            }
            other_lanes_seen.push_back(logical);
            ++other_lanes_resident;
        }
    });
    const std::uint32_t ceiling   = ring_lane_dynamic_ceiling(pages.physical_pool().usable_pages(),
                                                               other_lanes_resident);
    std::uint32_t lane_resident   = 0;
    for (std::uint32_t page = 0; page < mapped; ++page) {
        const LogicalKVPageHandle logical = addresses.logical_page(address, page);
        if (pages.valid(logical) && pages.device_resident(logical)) { ++lane_resident; }
    }
    const std::uint32_t projected  = lane_resident + need + wanted_extra;
    const std::uint32_t over_share = projected > ceiling ? projected - ceiling : 0U;
    const std::uint32_t target_free =
        std::max(want_free > free_now ? want_free - free_now : 0U, over_share);
    if (target_free == 0) {
        (void)demote_kv_pages_to_host(addresses, pages, address, sink_pages, 0U, preferred);
        return;
    }
    // Cold pages first: inactive addresses are pure cache, so drain them before touching this
    // sequence's own resident window.
    const std::uint32_t freed_cold =
        demote_other_addresses_to_host(addresses, pages, address, target_free);
    const std::uint32_t free_after = free_pages(pages);
    const std::uint32_t target2 =
        std::max(want_free > free_after ? want_free - free_after : 0U, over_share);
    const std::uint32_t freed_own =
        demote_kv_pages_to_host(addresses, pages, address, sink_pages, target2, preferred);
    // LOCAL FIX (B01 ring early verdict): both demotions used to have their return value dropped, so
    // a ring that could free nothing carried on into `ensure_mapped_to_tokens` and failed later --
    // as the paged-KV reservation invariant, as an engine death, or (measured 2026-10-04, community
    // report) as a request that never returns while the prefill loop retries block after block.
    // Decide HERE, where the numbers are known.
    // `need` is the hard requirement: pages this step must map. The slack and the preferred restores
    // are best effort (retrieval quality), so falling short of `want_free` warns instead of failing.
    const std::uint32_t free_final = free_pages(pages);
    if (free_final < need) {
        // ContextCacheExhausted is a std::bad_alloc (include/ninfer/types.h:867), so the worker's
        // out-of-memory path fails this request and keeps serving -- it is not gated by
        // --recover-invariant-failures. This is the readable error the task book's F1 asks for
        // instead of a silent stall or a dead engine.
        throw ninfer::ContextCacheExhausted(
            "KVMem ring cannot free Device KV pages: this step needs " + std::to_string(need) +
            " free, " + std::to_string(free_final) + " available (target " +
            std::to_string(want_free) + " = need + slack " + std::to_string(kRingSlackPages) +
            " + restores " + std::to_string(wanted_extra) + "; freed cold " +
            std::to_string(freed_cold) + ", own " + std::to_string(freed_own) + "; pool usable " +
            std::to_string(pages.physical_pool().usable_pages()) + ", allocated " +
            std::to_string(pages.physical_pool().allocated_pages()) + ")");
    }
    if (free_final < want_free) {
        std::fprintf(stderr,
                     "[ninfer] ring short: freed=%u want=%u need=%u free=%u cold=%u own=%u -> "
                     "continuing without slack\n",
                     freed_cold + freed_own, want_free, need, free_final, freed_cold, freed_own);
    }
}

void ProgramImpl::ensure_sequence_kv_mapped(SequenceState& sequence, std::uint32_t main_tokens,
                                            std::uint32_t backend_tokens) {
    if (!sequence.kv || main_tokens > capacity || backend_tokens > capacity) {
        throw std::logic_error("KV materialization request is outside the sequence bundle");
    }
    if (backend_tokens != 0 && !sequence.kv->backend) {
        throw std::logic_error("backend KV materialization requested without an allocation");
    }
    // ADAPTATION (master keeps kv-lease): K's tree has no kv_lease mechanism at all (zero hits for
    // kv_lease in K), so K's five-beat body never extends a lease and pasting it as-is would
    // silently disable master's own lease-growth behaviour. This port replaces only the MAPPING
    // beat; master's lease accounting is untouched. Kept FIRST because master ran it before any
    // mapping and because the beats below consume what it decides (beat 1 scores over the mapped
    // range, ensure_ring_room computes need = total - mapped, beat 3 maps into the entitlement it
    // sized).
    ensure_sequence_kv_lease(sequence, main_tokens, backend_tokens);
    // LOCAL PROTOTYPE (KVMem-style ring): when a Device pool is smaller than the logical context a
    // sequence cannot keep all of its KV on the Device. Each call then:
    //   1. ranks the pages by relevance to the recent context (retrieval preference),
    //   2. demotes the oldest pages until the pool has room for this call's pages plus slack (and for
    //      the pages retrieval wants to bring back); the preferred pages are given up last,
    //   3. maps the pages this call needs,
    //   4. brings the preferred pages back onto the Device (H2D),
    //   5. installs the mask over EXACTLY the resident set.
    // The dense configuration (pool >= logical capacity, or no window configured) never enters here.
    const std::uint32_t text_total    = proto_pages_for_tokens(main_tokens);
    const std::uint32_t backend_total = proto_pages_for_tokens(backend_tokens);
    const std::uint32_t window_pages  = proto_kv_window_pages();
    const std::uint32_t sink_pages    = proto_kv_sink_pages();
    const bool has_backend =
        backend_tokens != 0 && sequence.kv->backend && backend_kv_addresses && backend_kv_pages;
    const bool ring = window_pages != 0 && text_kv_pages != nullptr && text_kv_addresses != nullptr &&
                      text_kv_pages->physical_pool().usable_pages() <
                          text_kv_addresses->logical_page_capacity();
    std::vector<std::uint32_t> preferred;
    if (ring) {
        // Retrieval preference: a share of the pool (default half -- see the LOCAL FIX comment on
        // proto_kv_retrieve_share_percent), scored on the whole mapped range.
        // Scoring runs only when the mapped page count changed (relevance shifts slowly; within one
        // page the previous selection stays valid). Between rescores the hysteresis set is reused.
        const std::uint32_t pool      = text_kv_pages->physical_pool().usable_pages();
        // LOCAL (ring lanes, 2026-10-17): every term below is sized from THIS LANE's share of the
        // pool, never the whole pool -- see ring_lane_pool_pages for why the sum has to fit.
        const std::uint32_t lane_pool = ring_lane_pool_pages(pool, max_concurrency);
        const std::uint32_t share     = lane_pool * proto_kv_retrieve_share_percent() / 100U;
        std::uint32_t budget          = std::min(proto_kv_retrieve_pages(), share);        // ---- B32 (2026-10-07): the retrieval budget had NO default. ---------------------------------
        // WHY THIS EXISTS: proto_env_pages returns 0 when NINFER_KV_RETRIEVE is unset, and NOTHING in
        // this tree sets it (no launcher, script or doc), so the shipped default is budget = 0: the
        // ring demotes pages and never brings any back. Measured on this build (2026-10-07): such a
        // request answers out of a MASKED middle with HTTP 200 and a plausible wrong answer -- the
        // exact condition generation_service.cpp warns about but does not refuse. The same
        // measurement with the budget supplied by hand: 0/6 correct at budget 0, 6/6 at 8192 tokens
        // on a content-distinct fixture. A fix that ships switched off is a fix the user does not
        // have (startup.cpp:1239), which is why the default below is ON.
        // THE CONDITION IS DELIBERATELY NARROW: only when the caller left NINFER_KV_RETRIEVE unset
        // (budget 0), only on the ring path, and only when this request's own KV (text_total pages)
        // exceeds the pool. An explicit NINFER_KV_RETRIEVE -- including 0 -- is respected untouched.
        // THE CAP RESPECTS THE RESTORE PATH'S SLACK: ensure_ring_room keeps free pages because a
        // restore needs a free slot (context.cpp:2305), so the fallback stops short of the pool
        // instead of taking every free page.
        if (budget == 0U && retrieve_budget_unset() && retrieve_auto_enabled() && text_total > pool) {
            // LOCAL (ring lanes, 2026-10-17): this used to keep its own 4-page slack. That is the
            // same concept as kRingSlackPages (what the restore path needs kept free) with a
            // different number, and the smaller number was the one that mattered: an auto budget
            // built from `lane_pool - sink - 4` then leaves `ensure_ring_room` only 4 pages, so the
            // lane overruns its own ceiling by 4 -- a configuration the startup gate would have to
            // refuse. One constant for one concept, sized so the lane's own ceiling covers it.
            const std::uint32_t room =
                lane_pool > sink_pages + kRingSlackPages
                    ? lane_pool - sink_pages - kRingSlackPages
                    : 0U;
            const std::uint32_t auto_budget = std::min(share, room);
            if (auto_budget != 0U) {
                budget = auto_budget;
                std::fprintf(stderr,
                             "[ring] retrieve AUTO-ENABLED: prompt %u pages > pool %u pages and "
                             "NINFER_KV_RETRIEVE is unset -> budget %u pages (skeleton %u, "
                             "restore-slack %u, lane pool %u of %u across %u lane(s)). Set "
                             "NINFER_KV_RETRIEVE to pick a budget, or "
                             "NINFER_KV_RETRIEVE_AUTO=0 for the old (budget 0) behaviour.\n",
                             text_total, pool, budget, sink_pages, kRingSlackPages, lane_pool, pool,
                             max_concurrency);
            }
        }
        // ---- LOCAL (G7/G4 ring adaptive budget, 2026-10-15): the paper's §6 gate, on this engine's
        // ---- observable signal. ----------------------------------------------------------------------
        // THE PAPER'S RULE, kept as written: "α<0 ⇒ B' = min(B * scale_up, B_upper)", increase-only,
        // checked once per compaction step, with a cooldown.
        // WHAT IS *NOT* USED: the upstream signal. `llama_kvmem_observe_confidence` is dead code with
        // zero callers and could never fire even if wired -- `active == ceiling` held identically, so
        // multiplying by 1.75 and clamping back to the ceiling returned the same value (review P0-1/P0-2).
        // Porting it would import a controller that cannot act. The local analogue is the one quantity
        // this engine actually produces at the decision point: the retrieval cut's BOUNDARY MARGIN
        // (RingBudgetMargin). A near-tie at the budget line means the budget is what dropped the page.
        // WHY THE SIGNAL IS MEASURED ONE RESELECTION LATE: the margin only exists once a cut has been
        // made, and the budget must be known before the cut. So each reselection gates the NEXT one --
        // which is also what makes the cooldown meaningful (a control loop, not a per-token reflex).
        // GROWTH IS A PER-RESELECTION EVENT, NEVER PER-TOKEN: see SequenceState::cooldown_left.
        const std::uint32_t mapped_now = text_kv_addresses->mapped_pages(sequence.kv->text);
        const bool will_rescore =
            budget != 0 && mapped_now > sink_pages &&
            (mapped_now != sequence.ring_scored_mapped || sequence.ring_keep.empty());
        const std::uint32_t gate_upper = proto_kv_retrieve_max_pages();
        bool        gate_grew  = false;
        if (gate_upper != 0) {
            // Seed on first use: the configured budget IS the initial value. The paper starts well below
            // B_upper on purpose ("budget_initial 必须显著小于 b_upper") -- and the upstream module was a
            // permanent no-op precisely because initial == ceiling. Seed-forcing a small start here would
            // be inventing a number this deployment has no measurement for, so the configured budget
            // stands as the start and the ceiling only bounds the GROWTH.
            if (sequence.budget_cur == 0) { sequence.budget_cur = budget; }
            if (sequence.budget_cur == 0) {
                // B_upper set but there is no budget to grow from (NINFER_KV_RETRIEVE unset AND the B32
                // fallback did not fire, so retrieval is off entirely). Say so once, by name, rather than
                // being a silent no-op -- a shipped switch that quietly does nothing is the exact defect
                // class this port exists to remove.
                static bool warned_inert = false;
                if (!warned_inert) {
                    warned_inert = true;
                    std::fprintf(stderr,
                                 "[ring] gate: NINFER_KV_RETRIEVE_MAX=%u is set but the retrieval budget "
                                 "is 0 (NINFER_KV_RETRIEVE unset and the auto budget did not fire) -- "
                                 "the adaptive gate is INERT for this process.\n",
                                 gate_upper);
                }
            } else if (will_rescore) {
                if (sequence.cooldown_left > 0) {
                    --sequence.cooldown_left;
                } else if (sequence.ring_margin.valid &&
                           sequence.ring_margin.ratio < proto_kv_retrieve_margin_min()) {
                    const std::uint64_t grown =
                        static_cast<std::uint64_t>(sequence.budget_cur) *
                        static_cast<std::uint64_t>(proto_kv_retrieve_scaleup());
                    const std::uint32_t next = static_cast<std::uint32_t>(
                        grown < gate_upper ? grown : gate_upper);
                    if (next > sequence.budget_cur) {
                        sequence.budget_cur    = next;
                        sequence.cooldown_left = proto_kv_retrieve_cooldown();
                        gate_grew              = true;
                    }
                }
            }
            // The gate only ever RAISES the budget: `budget` keeps its configured value as a floor, and
            // B_upper caps the grown value, never the configured one. Capping the configured value would
            // silently override an explicit NINFER_KV_RETRIEVE, which the B32 comment above forbids.
            const std::uint32_t capped = std::min(sequence.budget_cur, gate_upper);
            if (capped > budget) { budget = capped; }
        }
        // ---- P1-a BUDGET LINE (2026-10-05): the pool's split, in one auditable row. --------------
        // WHY: the plan's P1-a asks for "[ring] budgets: skeleton=S, evidence=E, recent=R, free=F
        // (pool P)" so that the NEXT person can see where the KV budget went without reading code.
        // WHAT EACH TERM IS, exactly (no invented quantities):
        //   skeleton = the attention-sink pin (proto_kv_sink_pages) -- the only region the demotion
        //              scan refuses to touch (context.cpp:1946/1952). Measured need for a real agent
        //              surface: 2,808 tokens = 44 pages (see 实验-骨架体量实测-20261005.md).
        //   evidence = the retrieval budget, min(NINFER_KV_RETRIEVE, pool * SHARE%) -- the pages the
        //              ring may bring back; it is taken FROM the pool by construction.
        //   recent   = what is left for the recency window and the sequence's own frontier.
        //   free     = pool - everything above.
        // The identity S + E + R + F == P therefore holds BY CONSTRUCTION, which is what makes this
        // row a self-check rather than decoration: if a reader ever sees the four not summing to the
        // pool, one of the terms was redefined without updating the others.
        // Logged once per request (the ring path is per-request), not per chunk, to avoid flooding.
        {
            const std::uint32_t b_skeleton =
                sink_pages < lane_pool ? sink_pages : lane_pool;
            const std::uint32_t b_evidence =
                budget < lane_pool - b_skeleton ? budget : lane_pool - b_skeleton;
            const std::uint32_t b_recent   = 0U;   // recency is whatever the request's own frontier maps
            const std::uint32_t b_free     = lane_pool - b_skeleton - b_evidence - b_recent;
            // ---- LOCAL (G7/G4 gate audit, 2026-10-15): the `gate=` suffix. --------------------------
            // WHY IT IS PART OF THE SAME ROW: this line is the one place the KV budget's arithmetic is
            // already made auditable and already sums to the pool. A separate gate line could drift out of
            // step with it -- the row would say evidence=E while the gate was working on a different E.
            // Reading it: `gate=off` = NINFER_KV_RETRIEVE_MAX unset, so the adaptive controller never ran
            // and `evidence` is the configured budget, bit-for-bit the old behaviour (the negative
            // control). `gate=inert(...)` = the ceiling was set but there is no budget to grow.
            // `gate=grow|hold cur=N upper=U margin=M cooldown=C` = the controller ran: N is what it is
            // holding the budget at (and is what `evidence` above was computed from), U the ceiling, M the
            // boundary margin measured at the PREVIOUS reselection (`n/a` = none measured yet, which is
            // also the state right after a lane recycle), C how many reselections remain before the gate
            // may grow again. `grow` marks the reselection that just raised it, so a climb is visible as a
            // staircase of grow lines with rising N.
            char gate_suffix[192];
            if (gate_upper == 0) {
                std::snprintf(gate_suffix, sizeof(gate_suffix), "gate=off");
            } else if (sequence.budget_cur == 0) {
                std::snprintf(gate_suffix, sizeof(gate_suffix), "gate=inert(upper=%u)", gate_upper);
            } else {
                char margin_text[40];
                if (sequence.ring_margin.valid) {
                    std::snprintf(margin_text, sizeof(margin_text), "%.4f",
                                  static_cast<double>(sequence.ring_margin.ratio));
                } else {
                    std::snprintf(margin_text, sizeof(margin_text), "n/a");
                }
                std::snprintf(gate_suffix, sizeof(gate_suffix),
                              "gate=%s cur=%u upper=%u margin=%s cooldown=%u",
                              gate_grew ? "grow" : "hold", sequence.budget_cur, gate_upper, margin_text,
                              sequence.cooldown_left);
            }
            std::fprintf(stderr,
                         "[ring] budgets: skeleton=%u, evidence=%u, recent=%u, free=%u "
                         "neighbor=%u "
                         "(lane pool %u of %u across %u lane(s)) "
                         "[skeleton+evidence+recent+free == lane pool: %s] %s\n",
                         b_skeleton, b_evidence, b_recent, b_free, proto_kv_neighbor_pages(),
                         lane_pool, pool, max_concurrency,
                         (b_skeleton + b_evidence + b_recent + b_free) == lane_pool ? "yes" : "NO",
                         gate_suffix);
        }
        if (budget != 0 && mapped_now > sink_pages &&
            (mapped_now != sequence.ring_scored_mapped || sequence.ring_keep.empty())) {
            preferred = select_retrieval_pages(sequence, sink_pages, mapped_now, budget, &sequence.ring_margin);
            sequence.ring_keep          = preferred;
            sequence.ring_scored_mapped = mapped_now;
        } else if (mapped_now == sequence.ring_scored_mapped) {
            preferred = sequence.ring_keep;
        } else {
            // Fresh or shrunken range (e.g. a recycled lane): no hysteresis applies.
            preferred.clear();
            sequence.ring_keep.clear();
            sequence.ring_scored_mapped = mapped_now;
            // LOCAL (G7/G4 gate, 2026-10-15): a recycled lane must not inherit the previous request's
            // grown budget. This branch is the request's OPENING phase (it is the one taken while
            // mapped_now <= sink_pages, before the first reselection can happen), so clearing here gives
            // the controller per-request state: it re-seeds from the configured budget and re-measures
            // its own margin, instead of starting a fresh request already tuned for an unrelated one.
            // The measured margin goes too, so the first reselection of the new request cannot act on a
            // stale reading (`margin=n/a` in the row above is the visible consequence).
            sequence.budget_cur    = 0;
            sequence.cooldown_left = 0;
            sequence.ring_margin   = RingBudgetMargin{};
        }
        // The hysteresis set may reference pages beyond the current mapping (stale lane reuse);
        // clamp it so logical_page() below can never throw.
        preferred.erase(std::remove_if(preferred.begin(), preferred.end(),
                                       [&](std::uint32_t page) { return page >= mapped_now; }),
                        preferred.end());
        // B01: room is made by the shared helper, which every mapping site now uses. It takes the
        // token frontier (proto_pages_for_tokens converts) and decides right here -- with these exact
        // numbers -- whether the pages this step must map can be freed at all.
        ensure_ring_room(*text_kv_addresses, *text_kv_pages, sequence.kv->text, main_tokens,
                         preferred);
        if (has_backend) {
            ensure_ring_room(*backend_kv_addresses, *backend_kv_pages, *sequence.kv->backend,
                             backend_tokens, {});
        }
    }
    text_kv_addresses->ensure_mapped_to_tokens(sequence.kv->text, main_tokens, device.stream);
    if (backend_tokens != 0) {
        backend_kv_addresses->ensure_mapped_to_tokens(*sequence.kv->backend, backend_tokens,
                                                      device.stream);
    }
    if (ring) {
        const std::vector<std::uint32_t> restored = restore_kv_pages_from_host(
            *text_kv_addresses, *text_kv_pages, sequence.kv->text, preferred);
        // LOCAL DIAGNOSTIC (ring retrieve): `preferred` vs `restored` separates three different
        // worlds that used to look identical from the outside --
        //   restored == preferred   -> every wanted page is Device-resident now
        //   restored  < preferred   -> the restore ran out of free slots or host arena (it walks
        //                              ascending and BREAKS, so an over-large budget loses its HIGH
        //                              tail -- that is the failure mode a budget change can create)
        //   preferred == 0          -> nothing was wanted at all (budget 0, or every page scored 0)
        // Only printed when the pair changes, so a long run does not emit one line per step.
        {
            // LOCAL FIX (defect D-10, 2026-10-02): the de-dup key used to be the PAIR OF SIZES only, so a
            // round that restored a DIFFERENT SET of the same size printed nothing -- and the printed
            // first/last then belonged to the first occurrence of that size, which invites a wrong
            // conclusion ("page X was never restored"; that exact mistake was made once already).
            // first/last are part of the key now.
            static std::size_t last_preferred = 0;
            static std::size_t last_restored  = 0;
            static std::uint32_t last_first   = 0;
            static std::uint32_t last_last    = 0;
            const std::uint32_t first_page = restored.empty() ? 0U : restored.front();
            const std::uint32_t last_page  = restored.empty() ? 0U : restored.back();
            if (preferred.size() != last_preferred || restored.size() != last_restored ||
                first_page != last_first || last_page != last_last) {
                last_preferred = preferred.size();
                last_restored  = restored.size();
                last_first     = first_page;
                last_last      = last_page;
                std::fprintf(stderr,
                             "[ninfer] ring retrieve: which=text preferred=%zu restored=%zu "
                             "first=%u last=%u\n",
                             preferred.size(), restored.size(),
                             restored.empty() ? 0U : restored.front(),
                             restored.empty() ? 0U : restored.back());
            }
        }
        // The mask IS the resident set, so a page that could not be demoted or restored can only be
        // hidden; it can never expose a stale Device slot.
        install_resident_selection(decoder ? &decoder->text_kv : nullptr, text_total,
                                   *text_kv_addresses, *text_kv_pages, sequence.kv->text, "text",
                                   "mapped");
        if (has_backend) {
            install_resident_selection(backend_kv_cache(), backend_total, *backend_kv_addresses,
                                       *backend_kv_pages, *sequence.kv->backend, "backend",
                                       "mapped");
        }
    }
}

void ProgramImpl::commit_sequence_kv(SequenceState& sequence, std::uint32_t main_tokens,
                                     std::uint32_t backend_tokens) {
    if (!sequence.kv || main_tokens > capacity || backend_tokens > capacity ||
        (backend_tokens != 0 && !sequence.kv->backend)) {
        throw std::logic_error("KV commit request is outside the sequence bundle");
    }
    text_kv_addresses->commit_frontier(sequence.kv->text, main_tokens);
    if (sequence.kv->backend) {
        backend_kv_addresses->commit_frontier(*sequence.kv->backend, backend_tokens);
    }
    // LOCAL PROTOTYPE: keep the retrieval masks in step with the committed frontier as well. This
    // is a CONSERVATIVE safety net (sink + the resident recent window, no retrieval); the full mask
    // is re-installed by ensure_sequence_kv_mapped before the next attention reads the cache.
    const std::uint32_t window_pages = proto_kv_window_pages();
    if (window_pages != 0 && text_kv_pages != nullptr && text_kv_addresses != nullptr &&
        text_kv_pages->physical_pool().usable_pages() <
            text_kv_addresses->logical_page_capacity()) {
        const bool has_backend =
            backend_tokens != 0 && sequence.kv->backend && backend_kv_addresses && backend_kv_pages;
        install_resident_selection(decoder ? &decoder->text_kv : nullptr,
                                   proto_pages_for_tokens(main_tokens), *text_kv_addresses,
                                   *text_kv_pages, sequence.kv->text, "text", "commit");
        if (has_backend) {
            install_resident_selection(backend_kv_cache(), proto_pages_for_tokens(backend_tokens),
                                       *backend_kv_addresses, *backend_kv_pages,
                                       *sequence.kv->backend, "backend", "commit");
        }
    }

    if (hybrid_) { hybrid_publish_blocks(sequence); }
}

void ProgramImpl::trim_sequence_kv(SequenceState& sequence, std::uint32_t main_tokens,
                                   std::uint32_t backend_tokens) {
    if (!sequence.kv || main_tokens > capacity || backend_tokens > main_tokens) {
        throw std::logic_error("KV trim request is outside the sequence bundle");
    }
    if (backend_tokens != 0 && !sequence.kv->backend) {
        throw std::logic_error("backend KV trim requested without an allocation");
    }
    text_kv_addresses->destructive_truncate(sequence.kv->text, main_tokens);
    if (sequence.kv->backend) {
        backend_kv_addresses->destructive_truncate(*sequence.kv->backend, backend_tokens);
    }
}

void ProgramImpl::release_sequence_growth_entitlement(SequenceState& sequence) noexcept {
    if (!sequence.kv) { return; }
    try {
        text_kv_addresses->release_growth_entitlement(sequence.kv->text);
        if (sequence.kv->backend) {
            backend_kv_addresses->release_growth_entitlement(*sequence.kv->backend);
        }
    } catch (...) {}
}

void ProgramImpl::release_active_sequence_kv_strict(SequenceState& sequence) noexcept {
    if (!sequence.kv || !text_kv_addresses ||
        !text_kv_addresses->can_release_after_deactivate(sequence.kv->text) ||
        (sequence.kv->backend &&
         (!backend_kv_addresses ||
          !backend_kv_addresses->can_release_after_deactivate(*sequence.kv->backend)))) {
        std::terminate();
    }
    if (sequence.kv->backend &&
        !backend_kv_addresses->release_after_deactivate(*sequence.kv->backend)) {
        std::terminate();
    }
    if (!text_kv_addresses->release_after_deactivate(sequence.kv->text)) { std::terminate(); }
    sequence.kv.reset();
    if (host_kv_extents) { (void)host_kv_extents->release_unreferenced(); }
}

void ProgramImpl::release_sequence_kv_strict(SequenceState& sequence,
                                             bool written_to_disk) noexcept {
    if (!sequence.kv || !text_kv_addresses || !text_kv_addresses->can_release(sequence.kv->text)) {
        std::terminate();
    }
    if (!written_to_disk) { spill_released_owner(sequence); }
    if (sequence.kv->backend &&
        (!backend_kv_addresses || !backend_kv_addresses->can_release(*sequence.kv->backend))) {
        std::terminate();
    }
    if (sequence.kv->backend && !backend_kv_addresses->release(*sequence.kv->backend)) {
        std::terminate();
    }
    if (!text_kv_addresses->release(sequence.kv->text)) { std::terminate(); }
    sequence.kv.reset();
    if (host_kv_extents) { (void)host_kv_extents->release_unreferenced(); }
}

void ProgramImpl::release_sequence_kv(SequenceState& sequence) noexcept {
    if (!sequence.kv) { return; }
    unbind_sequence_kv(sequence);
    if (sequence.kv->backend && backend_kv_addresses) {
        (void)backend_kv_addresses->release(*sequence.kv->backend);
    }
    if (text_kv_addresses) { (void)text_kv_addresses->release(sequence.kv->text); }
    sequence.kv.reset();
    if (host_kv_extents) { (void)host_kv_extents->release_unreferenced(); }
}

qwen3_5::PagedKVCacheView ProgramImpl::text_kv_view(const SequenceState& sequence) const {
    if (!sequence.kv || !text_kv_addresses->active(sequence.kv->text)) {
        throw std::logic_error("sequence has no active KV execution mapping");
    }
    return decoder->text_kv.execution_view(text_kv_addresses->execution_row(sequence.kv->text));
}

qwen3_5::PagedKVCacheView ProgramImpl::mtp_kv_view(const SequenceState& sequence) const {
    if (speculative_backend != SpeculativeBackend::Mtp) { return {}; }
    if (decoder->mtp_cache() == nullptr || !sequence.kv || !sequence.kv->backend ||
        !backend_kv_addresses->active(*sequence.kv->backend)) {
        throw std::logic_error("sequence has no active MTP KV execution mapping");
    }
    return decoder->mtp_cache()->execution_view(
        backend_kv_addresses->execution_row(*sequence.kv->backend));
}

void ProgramImpl::set_device_i32(Tensor& tensor, std::int32_t value) {
    CUDA_CHECK(
        cudaMemcpyAsync(tensor.data, &value, sizeof(value), cudaMemcpyHostToDevice, device.stream));
}

void ProgramImpl::ordered_reset(SequenceState& sequence) {
    if (!state_store->valid(sequence.state.write)) {
        throw std::logic_error("pre-reset StateImage reservation is missing");
    } else {
        if (sequence.state.fork_pending || sequence.state.read != sequence.state.write ||
            state_store->role(sequence.state.write) != StateImageRole::ActiveMutable) {
            throw std::logic_error("StateImage reset requires a private mutable destination");
        }
    }
    refresh_state_views(sequence);
    work.reset();
    set_device_i32(io.pos, 0);
    set_device_i32(io.rope_pos, 0);
    set_device_i32(io.rope_delta, 0);
    if (io.mtp) { set_device_i32(io.mtp->position, 0); }
    sequence.text_kv_valid           = 0;
    sequence.mtp_kv_valid            = 0;
    sequence.dflash_context_frontier = 0;
}


} // namespace ninfer::models::qwen3_5::detail
