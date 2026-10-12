#include <algorithm>
#include "dsr/frontend.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    int frames = -1;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--frames" && i + 1 < argc) frames = std::max(0, std::atoi(argv[++i]));
    auto frontend = dsr::create_sdl_frontend();
    dsr::FrontendConfig config;
    config.title = "DQ9-Decomp frontend demo - NON-GAME TEST PATTERN";
    config.window_scale = 2;
    if (!frontend->init(config)) { std::fprintf(stderr, "Unable to initialize SDL frontend\n"); return 1; }
    std::vector<std::uint32_t> top(256 * 192), bottom(256 * 192);
    constexpr double pi = 3.14159265358979323846;
    constexpr int sample_rate = 32768;
    std::vector<std::int16_t> tone(547 * 2);
    double phase = 0.0;
    dsr::InputState input;
    for (int frame = 0; frames < 0 || frame < frames; ++frame) {
        frontend->poll(input);
        if (input.quit_requested) break;
        for (int y = 0; y < 192; ++y) for (int x = 0; x < 256; ++x) {
            const auto index = static_cast<std::size_t>(y * 256 + x);
            top[index] = 0xff000000u | (static_cast<std::uint32_t>(x) << 16) | (static_cast<std::uint32_t>(y) << 8) | static_cast<std::uint32_t>((x ^ y) & 255);
            const bool checker = ((x / 16) ^ (y / 16)) & 1;
            const std::uint32_t color = checker ? 0xff30c0ffu : 0xff182040u;
            bottom[index] = color;
        }
        // Visible indicator and echoed button mask are deliberately synthetic.
        for (int bit = 0; bit < 12; ++bit) if (input.buttons & (1u << bit)) {
            const int x0 = 8 + bit * 19;
            for (int y = 8; y < 19; ++y) for (int x = x0; x < x0 + 13 && x < 256; ++x) bottom[static_cast<std::size_t>(y * 256 + x)] = 0xffffd020u;
        }
        if (input.pen_down) for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
            const int x = input.pen_x + dx, y = input.pen_y + dy;
            if (x >= 0 && x < 256 && y >= 0 && y < 192) bottom[static_cast<std::size_t>(y * 256 + x)] = 0xffff3030u;
        }
        frontend->present(top.data(), bottom.data());
        for (std::size_t i = 0; i < tone.size(); i += 2) {
            const auto sample = static_cast<std::int16_t>(std::sin(phase) * 3000.0);
            phase += 2.0 * pi * 440.0 / sample_rate;
            if (phase >= 2.0 * pi) phase -= 2.0 * pi;
            tone[i] = sample; tone[i + 1] = sample;
        }
        frontend->queue_audio(tone.data(), tone.size() / 2, sample_rate);
        frontend->pace_frame();
    }
    return 0;
}
