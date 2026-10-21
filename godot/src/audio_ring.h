#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace dq9 {

// Stereo source frames, before host resampling. Synchronization is the caller's
// responsibility. Absolute cursors let a consumer commit a snapshot even when
// the producer has meanwhile discarded older frames to bound latency.
template <std::size_t Capacity>
class AudioRing {
    static_assert(Capacity > 0);
public:
    std::size_t size() const { return write_ - read_; }
    std::size_t read_position() const { return read_; }
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t overflows() const { return overflows_; }
    std::size_t high_water() const { return high_water_; }

    void reset() {
        read_ = write_ = 0;
        dropped_ = overflows_ = 0;
        high_water_ = 0;
    }
    std::size_t clear() {
        const auto count = size();
        read_ = write_;
        return count;
    }
    std::size_t trim_to(std::size_t keep) {
        const auto count = size() > keep ? size() - keep : 0;
        read_ += count;
        dropped_ += count;
        return count;
    }
    void push(const std::int16_t* samples, std::size_t count, std::size_t max_keep) {
        if (!samples || !count) return;
        // Preserve the host's latency policy: first discard old queued frames,
        // then use the physical capacity as a hard bound for oversized input.
        const auto queued = size();
        if (count > max_keep || queued > max_keep - count) {
            const auto drop = count >= max_keep ? queued : queued - (max_keep - count);
            read_ += drop;
            dropped_ += drop;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (size() == Capacity) {
                ++read_;
                ++dropped_;
                ++overflows_;
            }
            const auto at = (write_ % Capacity) * 2;
            data_[at] = samples[i * 2];
            data_[at + 1] = samples[i * 2 + 1];
            ++write_;
        }
        high_water_ = std::max(high_water_, size());
    }
    std::size_t copy(std::int16_t* output, std::size_t count) const {
        count = std::min(count, size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto at = ((read_ + i) % Capacity) * 2;
            output[i * 2] = data_[at];
            output[i * 2 + 1] = data_[at + 1];
        }
        return count;
    }
    void consume(std::size_t snapshot_read, std::size_t count) {
        read_ = std::max(read_, std::min(write_, snapshot_read + count));
    }
private:
    std::array<std::int16_t, Capacity * 2> data_{};
    std::size_t read_ = 0, write_ = 0, high_water_ = 0;
    std::uint64_t dropped_ = 0, overflows_ = 0;
};

} // namespace dq9
