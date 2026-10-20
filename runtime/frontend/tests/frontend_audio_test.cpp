#include "dsr/frontend.h"
#include <SDL.h>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <unistd.h>

namespace {
std::chrono::steady_clock::time_point now{};
Uint32 queued_bytes = 0;
std::size_t submitted_bytes = 0;
unsigned pauses = 0, starts = 0, clears = 0;
}

// Model SDL's queue and clock, not the emulation: no callback thread or wall-clock race.
extern "C" std::chrono::steady_clock::time_point
__wrap__ZNSt6chrono3_V212steady_clock3nowEv() noexcept { return now; }
extern "C" Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID) { return queued_bytes; }
extern "C" int __wrap_SDL_QueueAudio(SDL_AudioDeviceID, const void*, Uint32 bytes) {
    queued_bytes += bytes;
    submitted_bytes += bytes;
    return 0;
}
extern "C" void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause) {
    if (pause) ++pauses; else ++starts;
}
extern "C" void __wrap_SDL_ClearQueuedAudio(SDL_AudioDeviceID) {
    ++clears;
    queued_bytes = 0;
}

int main() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_setenv("DSR_AUDIO_METRICS", "0", 1); // recovery must not depend on diagnostics
    const auto directory = std::filesystem::temp_directory_path() /
        ("dq9-audio-policy-" + std::to_string(getpid()));
    SDL_setenv("XDG_CONFIG_HOME", directory.string().c_str(), 1);
    SDL_setenv("XDG_DATA_HOME", directory.string().c_str(), 1);
    auto frontend = dsr::create_sdl_frontend();
    dsr::FrontendConfig config;
    config.window_scale = 1;
    config.audio_rate = 48000;
    assert(frontend->init(config));
    std::array<std::int16_t, 2000> pcm{}; // 1000 stereo frames, roughly 21 ms
    const auto submit = [&] { frontend->queue_audio(pcm.data(), 1000, 48000.0); };
    const auto initial_pauses = pauses;
    submit();
    assert(starts == 0 && frontend->audio_backlog() < 1920);
    submit();
    assert(starts == 1 && frontend->audio_backlog() >= 1920);
    // An empty software queue still has a device buffer playing: do not re-prime early.
    queued_bytes = 0;
    now += std::chrono::milliseconds(5);
    submit();
    assert(pauses == initial_pauses && starts == 1);
    // A slow frame exhausts both queue and device buffer. Retain the submitted PCM
    // while paused, then restart only when the 40 ms reserve has been rebuilt.
    queued_bytes = 0;
    now += std::chrono::milliseconds(100);
    const auto bytes_before = submitted_bytes;
    submit();
    assert(pauses == initial_pauses + 1 && starts == 1);
    assert(queued_bytes == submitted_bytes - bytes_before && queued_bytes > 0);
    assert(frontend->audio_backlog() < 1920 && clears == 0);
    now += std::chrono::milliseconds(100); // time spent priming is not another underrun
    submit();
    assert(pauses == initial_pauses + 1 && starts == 2);
    assert(frontend->audio_backlog() >= 1920 && clears == 0);
    frontend.reset();
    std::filesystem::remove_all(directory);
}
