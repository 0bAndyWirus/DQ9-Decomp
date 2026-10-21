#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dq9 {

inline constexpr std::size_t kMaxDumpFrames = 1'000'000;
inline constexpr char kDumpFrameLimitMessage[] =
    "--dump-frames is limited to 1000000 frames total";

// Frame lists: comma-separated indices; `a-b` and `a-b/step` are inclusive ranges
// (same grammar as the oracle driver's --snapshots). Carry requested_frames across
// options to bound total expansion work, including duplicate/overlapping entries.
inline void parse_frame_spec(const std::string& spec, std::set<std::int64_t>& frames,
                             std::size_t& requested_frames) {

    std::stringstream list(spec);
    for (std::string item; std::getline(list, item, ',');) {
        if (item.empty()) continue;
        const std::size_t dash = item.find('-', 1);
        if (dash == std::string::npos) {
            const auto frame = std::stoll(item);
            if (requested_frames >= kMaxDumpFrames)
                throw std::length_error(kDumpFrameLimitMessage);
            ++requested_frames;
            frames.insert(frame);
            continue;
        }
        const std::size_t slash = item.find('/', dash);
        const std::int64_t first = std::stoll(item.substr(0, dash));
        const std::int64_t last = std::stoll(item.substr(dash + 1,
            slash == std::string::npos ? std::string::npos : slash - dash - 1));
        const std::int64_t step = slash == std::string::npos ? 1 :
            std::max<std::int64_t>(1, std::stoll(item.substr(slash + 1)));
        if (first > last) continue;

        // Unsigned subtraction also represents INT64_MIN..INT64_MAX without overflow.
        // Check the number of intervals before adding one for the inclusive endpoint.
        std::uint64_t remaining = static_cast<std::uint64_t>(last) -
                                  static_cast<std::uint64_t>(first);
        const auto stride = static_cast<std::uint64_t>(step);
        const std::uint64_t intervals = remaining / stride;
        if (requested_frames >= kMaxDumpFrames || intervals >= kMaxDumpFrames - requested_frames)
            throw std::length_error(kDumpFrameLimitMessage);
        requested_frames += static_cast<std::size_t>(intervals) + 1;
        for (std::int64_t frame = first;;) {
            frames.insert(frame);
            if (remaining < stride) break;
            remaining -= stride;
            // Another selected frame is at most last, so signed addition is safe.
            frame += step;
        }
    }
}

} // namespace dq9
