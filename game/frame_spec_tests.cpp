#include "frame_spec.h"

#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace {
using FrameIndex = std::int64_t;
int failures = 0;

void expect(bool value, const char* rule) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", rule);
    }
}

void expect_frames(const std::string& spec, std::initializer_list<FrameIndex> expected,
                   const char* rule) {
    std::set<FrameIndex> frames;
    std::size_t requested = 0;
    dq9::parse_frame_spec(spec, frames, requested);
    expect(frames == std::set<FrameIndex>(expected), rule);
}

void expect_limit(const std::string& spec, std::set<FrameIndex>& frames, std::size_t& requested) {
    try {
        dq9::parse_frame_spec(spec, frames, requested);
        expect(false, "oversized frame selection is rejected");
    } catch (const std::length_error& error) {
        expect(std::string_view(error.what()) == dq9::kDumpFrameLimitMessage,
               "limit failure explains the --dump-frames cap");
    }
}

void test_existing_grammar() {
    expect_frames("100,500-900/100,,750,100", {100, 500, 600, 700, 750, 800, 900},
                  "lists, inclusive ranges and duplicates retain their meaning");
    expect_frames("5-4,1-3/0,7-9/-2", {1, 2, 3, 7, 8, 9},
                  "descending ranges are empty and nonpositive steps still clamp to one");
    expect_frames("1-8/3", {1, 4, 7}, "range end is omitted unless reached by the step");
}

void test_signed_boundaries() {
    constexpr FrameIndex maximum = std::numeric_limits<FrameIndex>::max();
    constexpr FrameIndex minimum = std::numeric_limits<FrameIndex>::min();
    const std::string hi = std::to_string(maximum);
    const std::string lo = std::to_string(minimum);
    expect_frames(hi + "," + lo, {minimum, maximum}, "signed endpoint singletons parse");
    expect_frames(hi + "-" + hi, {maximum}, "maximum singleton range does not increment");
    expect_frames(lo + "-" + lo, {minimum}, "minimum singleton range does not increment");
    expect_frames(std::to_string(maximum - 4) + "-" + hi + "/2",
                  {maximum - 4, maximum - 2, maximum}, "step landing on INT64_MAX stops safely");
    expect_frames(std::to_string(maximum - 3) + "-" + hi + "/2",
                  {maximum - 3, maximum - 1}, "step crossing INT64_MAX stops before addition");
    expect_frames(lo + "-" + std::to_string(minimum + 4) + "/2",
                  {minimum, minimum + 2, minimum + 4}, "negative range distance is safe");
    expect_frames("0-" + hi + "/" + hi, {0, maximum}, "maximum signed step is accepted");
    expect_frames(lo + "-" + hi + "/" + hi, {minimum, -1, maximum - 1},
                  "full signed span uses unsigned distance without overflowing");
    expect_frames(hi + "-" + lo, {}, "reversed signed extremes select no frames");

    std::set<FrameIndex> frames{17};
    std::size_t requested = 1;
    expect_limit("0-" + hi, frames, requested);
    expect_limit(lo + "-" + hi, frames, requested);
    expect_limit("0-1000000", frames, requested);
    expect_limit("0-2000000/2", frames, requested);
    expect(frames == std::set<FrameIndex>{17},
           "oversized individual ranges fail before allocating selected frames");
}

void test_total_limit() {
    std::set<FrameIndex> frames;
    std::size_t requested = 0;
    dq9::parse_frame_spec("0-1999999/2", frames, requested);
    expect(frames.size() == dq9::kMaxDumpFrames && *frames.begin() == 0 &&
               *frames.rbegin() == 1'999'998,
           "exactly one million stepped frames are accepted");
    expect(requested == dq9::kMaxDumpFrames, "expanded frame count includes inclusive endpoints");
    expect_limit("0", frames, requested);
    expect_limit("1", frames, requested);
    expect_limit("2000000-2000001", frames, requested);
    expect(frames.size() == dq9::kMaxDumpFrames && !frames.count(1) && !frames.count(2'000'000),
           "later specs cannot exceed the combined limit even by one frame");
}

void test_overlapping_options() {
    std::set<FrameIndex> frames;
    std::size_t requested = 0;
    dq9::parse_frame_spec("0-499999", frames, requested);
    expect_limit("0-500000", frames, requested);
    expect(requested == 500'000 && frames.size() == 500'000,
           "overlapping oversized request is rejected before range expansion");
    dq9::parse_frame_spec("0-499999", frames, requested);
    expect(requested == dq9::kMaxDumpFrames && frames.size() == 500'000,
           "duplicate frames consume the cumulative expansion budget");
    expect_limit("0", frames, requested);
}
} // namespace

int main() {
    test_existing_grammar();
    test_signed_boundaries();
    test_total_limit();
    test_overlapping_options();
    std::printf("Frame specification failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
