// The retrieval mask is per execution-table row.
//
// Several sequences share one cache and one Device page pool, but each one's logical-to-physical
// mapping is its own, so each one's resident set is its own. A single shared bitmap therefore let
// whichever sequence installed last decide which pages every sequence attended -- the same defect
// class as the lane-keyed score slots, and the reason the ring refused --max-concurrency > 1.
//
// Two published access paths have to agree, because the kernels use both: a single-sequence view
// carries its own row's bitmap, while a batched view carries the buffer base plus the stride a kernel
// multiplies by the table row it resolves. This test reads both and compares them.

#include "core/device.h"
#include "models/qwen3_5/state/decoder_state.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace q36 = ninfer::models::qwen3_5;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool cuda_unavailable(cudaError_t error) {
    return error == cudaErrorNoDevice || error == cudaErrorInsufficientDriver;
}

constexpr std::uint32_t kCapacity       = 4096; // 64 logical pages = 2 bitmap words per row
constexpr std::uint32_t kWordsPerRow    = 2;
constexpr std::uint32_t kRows           = 4;
constexpr std::uint32_t kPhysicalGroups = 8;   // smaller than the 64 logical pages, so the ring is on

q36::DecoderStateSpec selection_spec() {
    return q36::DecoderStateSpec{
        .full_attention_layers     = 1,
        .mtp_layers                = 0,
        .capacity                  = kCapacity,
        .kv_heads                  = 2,
        .attention_head_dim        = 256,
        .kv_storage                = ninfer::KvCacheStorage::BFloat16,
        .enable_mtp                = false,
        .kv_table_rows             = static_cast<std::int32_t>(kRows),
        .text_physical_page_groups = kPhysicalGroups,
    };
}

std::vector<std::uint32_t> read_words(const std::uint32_t* device_bits) {
    std::vector<std::uint32_t> host(kWordsPerRow, 0U);
    if (device_bits == nullptr) { return host; }
    CUDA_CHECK(cudaMemcpy(host.data(), device_bits, kWordsPerRow * sizeof(std::uint32_t),
                          cudaMemcpyDeviceToHost));
    return host;
}

std::vector<std::uint32_t> read_row(const q36::PagedKVCache& cache,
                                    const ninfer::KVExecutionRowLease& row) {
    return read_words(cache.execution_view(row).layer_view(0).selected_blocks);
}

std::string show(const std::vector<std::uint32_t>& words) {
    std::string text;
    for (const std::uint32_t word : words) {
        char cell[16];
        std::snprintf(cell, sizeof(cell), "%08x ", word);
        text += cell;
    }
    return text;
}

void test_selection_is_per_row(ninfer::DeviceContext& device) {
    const q36::DecoderStateSpec spec = selection_spec();
    ninfer::LayoutBuilder builder;
    const q36::DecoderStateLayout layout = q36::plan_decoder_state(builder, spec);
    ninfer::DeviceArena arena(builder.finish(256));
    const ninfer::DeviceSpan backing{arena.base(), arena.capacity()};
    q36::PagedKVCache cache(backing, layout.text_kv);

    const std::vector<std::uint32_t> all_selected(kWordsPerRow, 0xffffffffU);
    const std::vector<std::uint32_t> row_one{0x0000ffffU, 0xffff0000U};
    const std::vector<std::uint32_t> row_one_again{0x00ff00ffU, 0xff00ff00U};
    const std::vector<std::uint32_t> row_three{0x00000001U, 0x80000000U};
    const std::vector<std::uint32_t> row_two_short{0x0000000fU};
    const std::vector<std::uint32_t> row_two_expected{0x0000000fU, 0xffffffffU};

    expect(cache.execution_tables().row_count() == static_cast<std::int32_t>(kRows),
           "the mask buffer is not sized to the execution-table row count");
    expect(cache.selection_row_words() == static_cast<std::int32_t>(kWordsPerRow),
           "the published mask stride is not one row's bitmap");

    ninfer::KVExecutionRowLease row0 = cache.execution_tables().acquire(0);
    ninfer::KVExecutionRowLease row1 = cache.execution_tables().acquire(1);
    ninfer::KVExecutionRowLease row2 = cache.execution_tables().acquire(2);
    ninfer::KVExecutionRowLease row3 = cache.execution_tables().acquire(3);

    cache.install_selection(row1.row_index(), row_one);
    cache.install_selection(row3.row_index(), row_three);
    cache.install_selection(row2.row_index(), row_two_short);
    device.synchronize();

    const std::vector<std::uint32_t> read0 = read_row(cache, row0);
    const std::vector<std::uint32_t> read1 = read_row(cache, row1);
    const std::vector<std::uint32_t> read2 = read_row(cache, row2);
    const std::vector<std::uint32_t> read3 = read_row(cache, row3);
    std::cerr << "read0=" << show(read0) << "read1=" << show(read1) << "read2=" << show(read2)
              << "read3=" << show(read3) << '\n';

    expect(read1 == row_one, "row 1 did not read back the mask row 1 installed");
    expect(read3 == row_three, "row 3 did not read back the mask row 3 installed");
    expect(read2 == row_two_expected,
           "a short mask did not leave the rest of its own row selected");
    expect(read0 == all_selected, "a row that installed nothing lost the all-selected default");

    // A reinstall replaces its own row and nothing else.
    cache.install_selection(row1.row_index(), row_one_again);
    device.synchronize();
    expect(read_row(cache, row1) == row_one_again, "row 1 did not read back its reinstalled mask");
    expect(read_row(cache, row3) == row_three, "reinstalling row 1 disturbed row 3");

    // The batched view publishes the buffer base and the stride; base + row * stride has to land on
    // the same bitmap the single-sequence view carries, because that is how a kernel finds its row.
    const ninfer::PagedKVBatchLayerView batch = cache.batch_layer_view(0);
    expect(batch.selection_row_words == static_cast<std::int32_t>(kWordsPerRow),
           "the batched view published the wrong row stride");
    expect(batch.selected_blocks != nullptr, "the batched view dropped the mask buffer");
    expect(batch.selected_blocks != cache.execution_view(row3).layer_view(0).selected_blocks,
           "the batched view published one row's bitmap instead of the buffer base");
    // A null base reads as an empty bitmap, so a dropped buffer fails these rather than skipping them.
    const std::uint32_t* const base = batch.selected_blocks;
    const std::size_t stride        = static_cast<std::size_t>(batch.selection_row_words);
    const auto at_row               = [&](std::int32_t row) {
        return base == nullptr ? std::vector<std::uint32_t>{}
                               : read_words(base + static_cast<std::size_t>(row) * stride);
    };
    expect(at_row(row1.row_index()) == row_one_again,
           "base + row * stride did not reach row 1's bitmap");
    expect(at_row(row3.row_index()) == row_three,
           "base + row * stride did not reach row 3's bitmap");
    expect(at_row(row0.row_index()) == all_selected,
           "base + 0 * stride did not reach row 0's bitmap");

    // A mask longer than one row would overrun into the next sequence's bitmap, so it is refused --
    // and refusing it leaves every row as it was.
    bool rejected = false;
    try {
        cache.install_selection(row1.row_index(),
                                std::vector<std::uint32_t>(kWordsPerRow + 1, 0U));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "a mask longer than one row was accepted");
    device.synchronize();
    expect(read_row(cache, row1) == row_one_again, "a rejected mask changed its own row");
    expect(read_row(cache, row2) == row_two_expected, "a rejected mask reached the next row");
}

} // namespace

int main() {
    int count                   = 0;
    const cudaError_t count_err = cudaGetDeviceCount(&count);
    if (cuda_unavailable(count_err) || count == 0) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }
    CUDA_CHECK(count_err);

    try {
        ninfer::DeviceContext device(0);
        test_selection_is_per_row(device);
        device.synchronize();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
