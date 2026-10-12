// Sound processing unit (ARM7): 16 channels (PCM8/PCM16/IMA-ADPCM/PSG/
// noise), sound capture 0/1, master control and bias.
//
// Owner: runtime/spu. The core bus routes ARM7 IO 0x04000400-0x0400051F here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
namespace dsr {

class Spu {
public:
    // Output sample rate of mix_samples(): one stereo frame every 1024 bus
    // ticks of the 33,513,982 Hz clock (~32,728.5 Hz), as on hardware.
    static constexpr std::int64_t kTicksPerSample = 1024;

    Spu();
    ~Spu();
    Spu(const Spu&) = delete;
    Spu& operator=(const Spu&) = delete;

    void reset();

    // ARM7 guest memory access used for sample fetch and capture writes.
    std::function<std::uint32_t(std::uint32_t addr)> read8;
    std::function<void(std::uint32_t addr, std::uint8_t value)> write8;

    std::uint32_t io_read(std::uint32_t addr, int width);
    void io_write(std::uint32_t addr, std::uint32_t value, int width);

    // Advance by `ticks` bus cycles, appending produced stereo frames
    // (interleaved L,R signed 16-bit) to the internal output ring.
    void run(std::int64_t ticks);

    // Remove up to max_frames stereo frames from the output ring into out.
    // Returns the number of frames written.
    std::size_t drain(std::int16_t* out, std::size_t max_frames);
    struct ChannelDebug {
        std::uint32_t format = 0;
        std::uint32_t volume = 0;
        std::uint32_t pan = 0;
        std::uint32_t sample_position = 0;
        std::int32_t current_sample = 0;
        bool busy = false;
        bool muted = false;
    };
    struct DebugState {
        std::array<ChannelDebug, 16> channels{};
        std::uint16_t sound_control = 0;
        std::uint16_t bias = 0;
        int solo_channel = -1;
    };
    DebugState debug_state() const;
    void set_channel_muted(unsigned channel, bool muted);
    void set_solo_channel(int channel);

    struct Impl;

private:
    Impl* impl_;
};

} // namespace dsr
