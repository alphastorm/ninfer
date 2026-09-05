// Restore reads must be planned per staging window, not per device segment: this is the
// executable invariant behind the native-lane restore-bandwidth fix (alphastorm/ninfer#36).
#include "runtime/contract/continuation_checkpoint.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using ninfer::runtime::ContinuationCheckpointReadPiece;
using ninfer::runtime::ContinuationCheckpointReadPlan;
using ninfer::runtime::ContinuationCheckpointReadRequest;
using ninfer::runtime::ContinuationCheckpointReadWindow;
using ninfer::runtime::plan_continuation_checkpoint_reads;
using ninfer::runtime::split_continuation_checkpoint_read;

int check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); }
    return condition ? 0 : 1;
}

bool plan_is_consistent(const ContinuationCheckpointReadPlan& plan,
                        std::span<const std::size_t> segment_bytes, std::size_t window_bytes) {
    std::uint64_t file_offset = 0;
    std::size_t piece_cursor  = 0;
    std::vector<std::size_t> covered(segment_bytes.size(), 0);
    for (std::size_t index = 0; index < plan.windows.size(); ++index) {
        const ContinuationCheckpointReadWindow& window = plan.windows[index];
        if (window.file_offset != file_offset || window.bytes == 0 ||
            window.bytes > window_bytes || window.first_piece != piece_cursor ||
            (index + 1 < plan.windows.size() && window.bytes != window_bytes)) {
            return false;
        }
        std::size_t window_sum = 0;
        for (std::size_t piece = window.first_piece;
             piece < window.first_piece + window.piece_count; ++piece) {
            const ContinuationCheckpointReadPiece& item = plan.pieces[piece];
            if (item.segment >= segment_bytes.size() || item.offset != covered[item.segment] ||
                item.bytes == 0 || item.offset + item.bytes > segment_bytes[item.segment]) {
                return false;
            }
            covered[item.segment] += item.bytes;
            window_sum += item.bytes;
        }
        if (window_sum != window.bytes) { return false; }
        piece_cursor += window.piece_count;
        file_offset += window.bytes;
    }
    if (piece_cursor != plan.pieces.size() || file_offset != plan.total_bytes) { return false; }
    for (std::size_t index = 0; index < segment_bytes.size(); ++index) {
        if (covered[index] != segment_bytes[index]) { return false; }
    }
    return true;
}

int test_small_plan_is_exact() {
    int failures = 0;
    const std::vector<std::size_t> segments{3, 5, 2};
    const ContinuationCheckpointReadPlan plan = plan_continuation_checkpoint_reads(segments, 4);
    failures += check(plan.total_bytes == 10, "total bytes sum the segments");
    failures += check(plan.windows.size() == 3, "ten bytes in four-byte windows need three reads");
    const std::vector<ContinuationCheckpointReadPiece> expected_pieces{
        {0, 0, 3}, {1, 0, 1}, {1, 1, 4}, {2, 0, 2}};
    failures += check(plan.pieces == expected_pieces, "pieces split segments at window edges");
    const std::vector<ContinuationCheckpointReadWindow> expected_windows{
        {0, 4, 0, 2}, {4, 4, 2, 1}, {8, 2, 3, 1}};
    failures += check(plan.windows == expected_windows, "windows are contiguous and full but the last");
    failures += check(plan_is_consistent(plan, segments, 4), "small plan is consistent");
    return failures;
}

int test_window_larger_than_payload_is_one_read() {
    const std::vector<std::size_t> segments{7, 9};
    const ContinuationCheckpointReadPlan plan = plan_continuation_checkpoint_reads(segments, 1 << 20);
    return check(plan.windows.size() == 1 && plan.windows[0].bytes == 16 &&
                     plan.windows[0].piece_count == 2 && plan_is_consistent(plan, segments, 1 << 20),
                 "a payload smaller than the window is one read");
}

int test_page_segments_collapse_to_window_count() {
    // 250,000 rk2v4-e8-class page segments of 4 KiB: one read per 8 MiB window instead of one
    // read per segment.
    const std::vector<std::size_t> segments(250'000, 4096);
    constexpr std::size_t window = 8ULL << 20;
    const ContinuationCheckpointReadPlan plan = plan_continuation_checkpoint_reads(segments, window);
    const std::uint64_t total = 250'000ULL * 4096;
    const std::size_t expected = static_cast<std::size_t>((total + window - 1) / window);
    int failures = 0;
    failures += check(plan.windows.size() == expected, "windows equal ceil(total / window)");
    failures += check(plan.pieces.size() == segments.size(), "aligned segments are never split");
    failures += check(plan_is_consistent(plan, segments, window), "page plan is consistent");
    return failures;
}

int test_segment_larger_than_window_spans_windows() {
    const std::vector<std::size_t> segments{10};
    const ContinuationCheckpointReadPlan plan = plan_continuation_checkpoint_reads(segments, 4);
    return check(plan.windows.size() == 3 && plan.pieces.size() == 3 &&
                     plan.pieces[2] == ContinuationCheckpointReadPiece{0, 8, 2} &&
                     plan_is_consistent(plan, segments, 4),
                 "an oversized segment is split across consecutive windows");
}

int test_invalid_inputs_throw() {
    int failures = 0;
    bool threw = false;
    try {
        (void)plan_continuation_checkpoint_reads(std::vector<std::size_t>{1}, 0);
    } catch (const std::invalid_argument&) { threw = true; }
    failures += check(threw, "zero window is refused");
    threw = false;
    try {
        (void)plan_continuation_checkpoint_reads(std::vector<std::size_t>{1, 0}, 4);
    } catch (const std::invalid_argument&) { threw = true; }
    failures += check(threw, "empty segment is refused");
    threw = false;
    std::vector<std::byte> bytes(8);
    try {
        (void)split_continuation_checkpoint_read(0, bytes, 0);
    } catch (const std::invalid_argument&) { threw = true; }
    failures += check(threw, "zero request bound is refused");
    return failures;
}

int test_split_partitions_a_large_read() {
    std::vector<std::byte> bytes(100);
    const std::vector<ContinuationCheckpointReadRequest> requests =
        split_continuation_checkpoint_read(7, bytes, 32);
    int failures = 0;
    failures += check(requests.size() == 4, "100 bytes at 32-byte requests is four requests");
    std::uint64_t offset = 7;
    std::size_t covered  = 0;
    bool contiguous      = true;
    for (const ContinuationCheckpointReadRequest& request : requests) {
        contiguous = contiguous && request.file_offset == offset &&
                     request.destination.data() == bytes.data() + covered &&
                     request.destination.size() <= 32 && !request.destination.empty();
        offset += request.destination.size();
        covered += request.destination.size();
    }
    failures += check(contiguous && covered == bytes.size() && requests.back().destination.size() == 4,
                      "requests are contiguous, bounded, and cover the destination exactly");
    failures += check(split_continuation_checkpoint_read(0, std::span<std::byte>(), 32).empty(),
                      "an empty destination is no requests");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_small_plan_is_exact();
    failures += test_window_larger_than_payload_is_one_read();
    failures += test_page_segments_collapse_to_window_count();
    failures += test_segment_larger_than_window_spans_windows();
    failures += test_invalid_inputs_throw();
    failures += test_split_partitions_a_large_read();
    if (failures != 0) {
        std::fprintf(stderr, "%d continuation checkpoint read plan check(s) failed\n", failures);
        return 1;
    }
    std::puts("continuation checkpoint read plan tests passed");
    return 0;
}
