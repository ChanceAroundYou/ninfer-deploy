// Reconciliation probes for scoped release (isolation batch-a, fix).
// Read-only: never consume handles, never terminate. The returning rule is
// conservative: any inconsistency reports "free/invalid" (drop the catalog
// entry; the next admission re-catalogues from live Program state).

#include "models/qwen3_5/program/program_impl.h"

namespace ninfer::models::qwen3_5::detail {

bool ProgramImpl::continuation_slot_free(const ContinuationHandle& handle) const noexcept {
    try {
        const std::uint32_t index = ContractAccess::index(handle);
        if (index >= continuation_capacity) {
            return true;
        }
        if (ContractAccess::epoch(handle) != continuation_slots[index].generation) {
            return true;
        }
        return continuation_slots[index].role == ContinuationSlotRole::Free;
    } catch (...) {
        return true;
    }
}

bool ProgramImpl::shared_prefix_slot_free(const SharedPrefixHandle& handle) const noexcept {
    try {
        const std::uint32_t index = ContractAccess::index(handle);
        if (index >= shared_prefix_capacity) {
            return true;
        }
        if (ContractAccess::epoch(handle) != shared_prefix_slots[index].generation) {
            return true;
        }
        return shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued;
    } catch (...) {
        return true;
    }
}

bool ProgramImpl::valid_continuation_handle(const ContinuationHandle& handle) const noexcept {
    try {
        return valid_continuation(handle);
    } catch (...) {
        return false;
    }
}

bool ProgramImpl::valid_shared_prefix_handle(const SharedPrefixHandle& handle) const noexcept {
    try {
        return valid_shared_prefix(handle);
    } catch (...) {
        return false;
    }
}

}  // namespace ninfer::models::qwen3_5::detail
