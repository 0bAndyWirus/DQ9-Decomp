#ifdef NDEBUG
#undef NDEBUG
#endif
#include "frontend_logic.h"

#include <chrono>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using namespace dsr;
    using namespace dsr::frontend_detail;
    UserPathEnvironment environment;
    environment.home = "/tmp/dq9-test-home";
    auto paths = user_paths_from_environment(environment);
    assert(paths.config_dir == std::filesystem::path("/tmp/dq9-test-home/.config/dq9-decomp"));
    assert(paths.data_dir == std::filesystem::path("/tmp/dq9-test-home/.local/share/dq9-decomp"));
    environment.config_home = "/custom/config";
    environment.data_home = "/custom/data";
    paths = user_paths_from_environment(environment);
    assert(paths.config_dir == std::filesystem::path("/custom/config/dq9-decomp"));
    assert(paths.data_dir == std::filesystem::path("/custom/data/dq9-decomp"));
    environment.windows = true;
    environment.appdata = "C:/dq9-test-home/AppData/Roaming";
    environment.local_appdata = "D:/dq9-test-data";
    paths = user_paths_from_environment(environment);
    assert(paths.config_dir == environment.appdata / "dq9-decomp");
    assert(paths.data_dir == environment.local_appdata / "dq9-decomp");
    // Windows roots win over XDG/HOME, including backslash-style local roots.
    environment.appdata = R"(C:\dq9-test-home\AppData\Roaming)";
    environment.local_appdata.clear();
    paths = user_paths_from_environment(environment);
    assert(paths.config_dir == environment.appdata / "dq9-decomp");
    assert(paths.data_dir == paths.config_dir);
    environment.appdata.clear();
    environment.user_profile = "C:/dq9-test-home";
    paths = user_paths_from_environment(environment);
    assert(paths.config_dir == environment.user_profile / "AppData" / "Roaming" / "dq9-decomp");
    assert(paths.data_dir == paths.config_dir);
    environment.user_profile.clear();
    paths = user_paths_from_environment(environment);
    assert(paths.config_dir == std::filesystem::path(".") / "dq9-decomp");
    assert(paths.data_dir == paths.config_dir);
    paths = user_paths_from_environment({});
    assert(paths.config_dir == std::filesystem::path(".") / "dq9-decomp");
    assert(paths.data_dir == paths.config_dir);

    for (const auto& size : {std::pair{256, 192}, std::pair{256, 384}, std::pair{768, 1152}, std::pair{1518, 864}}) {
        const auto controls = debug_control_rects(size.first, size.second);
        assert(controls.note.w >= 72 && controls.note.w <= 120 && controls.note.h == 32);
        if (size.first == 256) assert(controls.note.w >= 72 && controls.note.w <= 76);
        const Rect rects[] = {controls.note, controls.pause, controls.overlay};
        const DebugControl expected[] = {DebugControl::Note, DebugControl::Pause, DebugControl::Overlay};
        for (std::size_t i = 0; i < 3; ++i) {
            const Rect rect = rects[i];
            assert(rect.w > 0 && rect.h > 0);
            assert(rect.x >= 0 && rect.y >= 0);
            assert(rect.x + rect.w <= size.first && rect.y + rect.h <= size.second);
            assert(debug_control_at(rect.x + rect.w / 2, rect.y + rect.h / 2,
                                    size.first, size.second) == expected[i]);
            assert(debug_control_at(rect.x - 1, rect.y + rect.h / 2,
                                    size.first, size.second) == DebugControl::None);
            assert(debug_control_at(rect.x + rect.w, rect.y + rect.h / 2,
                                    size.first, size.second) == DebugControl::None);
            assert(debug_control_at(rect.x + rect.w / 2, rect.y - 1,
                                    size.first, size.second) == DebugControl::None);
            assert(debug_control_at(rect.x + rect.w / 2, rect.y + rect.h,
                                    size.first, size.second) == DebugControl::None);
            for (std::size_t j = i + 1; j < 3; ++j) {
                const Rect other = rects[j];
                assert(rect.x + rect.w <= other.x || other.x + other.w <= rect.x ||
                       rect.y + rect.h <= other.y || other.y + other.h <= rect.y);
            }
        }
        assert(debug_control_at(-1, -1, size.first, size.second) == DebugControl::None);
    }
    for (const auto& size : {std::pair{-1, 384}, std::pair{256, -1}, std::pair{0, 384},
                             std::pair{384, 0}, std::pair{80, 80}, std::pair{191, 384},
                             std::pair{256, 47}, std::pair{239, 192}}) {
        const auto controls = debug_control_rects(size.first, size.second);
        for (const Rect rect : {controls.note, controls.pause, controls.overlay})
            assert(rect.x == 0 && rect.y == 0 && rect.w == 0 && rect.h == 0);
        assert(debug_control_at(0, 0, size.first, size.second) == DebugControl::None);
    }

    for (const auto layout : {ScreenLayout::Vertical, ScreenLayout::Horizontal, ScreenLayout::TopFocus,
                              ScreenLayout::BottomFocus, ScreenLayout::SingleTop, ScreenLayout::SingleBottom}) {
        for (const auto& size : {std::pair{1280, 800}, std::pair{1920, 1080}, std::pair{800, 600}}) {
            const auto rects = layout_rects(layout, size.first, size.second);
            assert(rects.top.w >= 0 && rects.bottom.w >= 0);
            const Rect bottom = rects.bottom;
            if (bottom.w == 0 || bottom.h == 0) {
                int px = 0, py = 0;
                assert(!map_touch(layout, size.first, size.second, 0, 0, px, py));
                continue; // SingleTop intentionally has no touchscreen image.
            }
            for (const auto& point : {std::pair{0, 0}, std::pair{128, 96}, std::pair{255, 191}}) {
                const int x = bottom.x + point.first * bottom.w / 256;
                const int y = bottom.y + point.second * bottom.h / 192;
                int px = -1, py = -1;
                assert(map_touch(layout, size.first, size.second, x, y, px, py));
                assert(std::abs(px - point.first) <= 1 && std::abs(py - point.second) <= 1);
            }
            const auto integral = layout_rects(layout, size.first, size.second, true);
            assert(integral.top.w == 0 || (integral.top.w % 256 == 0 && integral.top.h % 192 == 0));
            assert(integral.bottom.w == 0 || (integral.bottom.w % 256 == 0 && integral.bottom.h % 192 == 0));
            int px = 0, py = 0;
            assert(!map_touch(layout, size.first, size.second, 0, 0, px, py) ||
                   (rects.bottom.x == 0 && rects.bottom.y == 0));
        }
    }
    ConfigData config = default_config();
    config.frontend.layout = ScreenLayout::BottomFocus;
    config.frontend.window_scale = 4;
    config.frontend.linear_filter = true;
    config.frontend.audio_buffer_ms = 120;
    config.frontend.stick_deadzone = 8000;
    config.frontend.dpad_from_stick = false;
    config.frontend.controller_guid = "GUID%with=equals";
    config.keycodes[0] = 55;
    config.gamepad_buttons[0] = 16; // SDL_CONTROLLER_BUTTON_PADDLE1 on supported SDL releases.
    config.language = "fr";
    config.rom_path = "/tmp/a=b.nds";
    config.save_path = "/tmp/save file.sav";
    config.debug_tool = true;
    config.frame_time_overlay = true;
    config.capture_memory_dumps = false;
    config.key_layout = 66;
    config.key_fast_forward = 77;
    config.key_screenshot = 78;
    config.key_gpu3d_scale = 79;
    config.frontend.gpu3d_scale = 3;
    const auto decoded = parse_config(serialize_config(config));
    assert(decoded.frontend.layout == config.frontend.layout);
    assert(decoded.frontend.window_scale == 4 && decoded.frontend.linear_filter);
    assert(decoded.frontend.audio_buffer_ms == 120 && decoded.frontend.stick_deadzone == 8000);
    assert(!decoded.frontend.dpad_from_stick && decoded.frontend.controller_guid == config.frontend.controller_guid);
    assert(decoded.keycodes[0] == 55 && decoded.key_layout == 66 && decoded.gamepad_buttons[0] == 16);
    assert(decoded.language == "fr" && decoded.rom_path == config.rom_path && decoded.save_path == config.save_path);
    assert(decoded.debug_tool && decoded.frame_time_overlay && !decoded.capture_memory_dumps);
    assert(decoded.key_fast_forward == 77 && decoded.key_screenshot == 78);
    assert(decoded.key_gpu3d_scale == 79 && decoded.frontend.gpu3d_scale == 3);
    assert(parse_config("gpu3d_scale=9\n").frontend.gpu3d_scale == 4);
    assert(parse_config("gpu3d_scale=0\n").frontend.gpu3d_scale == 1);
    assert(default_config().frontend.gpu3d_scale == 1);
    assert(!default_config().frontend.pause_when_unfocused);
    assert(parse_config("pause_when_unfocused=1\n").frontend.pause_when_unfocused);
    config.key_volume_down = 80; config.key_volume_up = 81; config.key_mute = 82; config.frontend.volume = 40;
    const auto with_volume = parse_config(serialize_config(config));
    assert(with_volume.key_volume_down == 80 && with_volume.key_volume_up == 81 && with_volume.key_mute == 82);
    assert(with_volume.frontend.volume == 40 && default_config().frontend.volume == 100);
    assert(parse_config("volume=250\n").frontend.volume == 100 && parse_config("volume=-5\n").frontend.volume == 0);
    assert(default_config().key_volume_down == 45 && default_config().key_volume_up == 46 && default_config().key_mute == 16);
    assert(volume_gain_q8(100, false) == 256 && volume_gain_q8(50, false) == 128 && volume_gain_q8(0, false) == 0);
    assert(volume_gain_q8(100, true) == 0 && volume_gain_q8(900, false) == 256);
    {
        std::int16_t pcm[4] = {32767, -32768, 1000, -1000};
        apply_volume_gain(pcm, 4, 256);
        assert(pcm[0] == 32767 && pcm[1] == -32768);       // unity gain leaves samples alone
        apply_volume_gain(pcm, 4, 128);
        assert(pcm[0] == 16383 && pcm[1] == -16384 && pcm[2] == 500 && pcm[3] == -500);
        apply_volume_gain(pcm, 4, 0);
        assert(pcm[0] == 0 && pcm[1] == 0 && pcm[2] == 0 && pcm[3] == 0);
    }
    // F7 is the hi-res scale key; a pre-existing file that bound the swap to F7 moves the swap.
    assert(default_config().key_gpu3d_scale == 64 &&
           default_config().key_swap != default_config().key_gpu3d_scale);
    assert(parse_config("hotkey_swap=64\n").key_swap == default_config().key_swap);
    assert(parse_config("hotkey_swap=70\n").key_swap == 70);
    // The named key_* settings must not be swallowed by the numeric key_<N> branch, and a
    // legacy file without them keeps the defaults (Tab held, F12).
    const auto defaults = default_config();
    assert(defaults.key_fast_forward == 43 && defaults.key_screenshot == 69);
    const auto old_file = parse_config("key_0=55\n");
    assert(old_file.key_fast_forward == defaults.key_fast_forward &&
           old_file.key_screenshot == defaults.key_screenshot);
    // Out-of-range scancodes are rejected; the numeric key_<N> table is unaffected.
    const auto bad = parse_config("key_fast_forward=9999\nkey_screenshot=-1\nkey_1=60\n");
    assert(bad.key_fast_forward == defaults.key_fast_forward && bad.key_screenshot == defaults.key_screenshot);
    assert(bad.keycodes[1] == 60);
    const auto legacy = parse_config("layout=2\nscale=4\nkey_0=55\n");
    assert(legacy.frontend.layout == ScreenLayout::TopFocus && legacy.frontend.window_scale == 4);
    assert(legacy.keycodes[0] == 55 && legacy.gamepad_buttons[0] == 1);
    assert(configured_gamepad_mask(decoded.gamepad_buttons, 16) == 1);
    assert(gamepad_button_mask(0) == kBtnB && gamepad_button_mask(1) == kBtnA);
    assert(gamepad_button_mask(2) == kBtnY && gamepad_button_mask(3) == kBtnX);
    assert(gamepad_button_mask(4) == kBtnSelect && gamepad_button_mask(6) == kBtnStart);
    assert(gamepad_button_mask(9) == kBtnL && gamepad_button_mask(10) == kBtnR);
    assert(gamepad_button_mask(11) == kBtnUp && gamepad_button_mask(14) == kBtnRight);
    assert(audio_resample_correction(4800, 4800, 48000) == 0.0);
    assert(audio_resample_correction(0, 4800, 48000) == -0.01);
    assert(audio_resample_correction(9600, 4800, 48000) == 0.01);
    assert(audio_resample_correction(10, 0, 48000) == 0.0);
    assert(!audio_queue_needs_emergency_clear(500, 500));
    assert(audio_queue_needs_emergency_clear(501, 500));
    assert(!audio_priming_should_start(false, 39, 40));
    assert(audio_priming_should_start(false, 40, 40));
    assert(!audio_priming_should_start(true, 4000, 40));
    // pause -> clear -> resume with an already-started device must require priming again
    bool started = true;
    started = audio_device_started_after_clear();
    assert(!started);
    assert(!audio_priming_should_start(started, 10, 40));
    assert(audio_priming_should_start(started, 40, 40));
    assert(!audio_would_underrun(0.010, 0.010));
    assert(!audio_would_underrun(0.008, 0.010));
    assert(audio_would_underrun(0.011, 0.010));
    assert(!audio_queue_starved(false, true, 0, 1.0, 0.010)); // initial priming / paused
    assert(!audio_queue_starved(true, false, 0, 1.0, 0.010)); // no playback observation
    assert(!audio_queue_starved(true, true, 0, 0.008, 0.010)); // empty SDL queue, device still playing
    assert(!audio_queue_starved(true, true, 0, 0.010, 0.010));
    assert(!audio_queue_starved(true, true, 512, 1.0, 0.010)); // backend delayed, but PCM still queued
    assert(audio_queue_starved(true, true, 0, 0.011, 0.010));
    const auto mixer = kDebugChannelUp | kDebugChannelDown | kDebugChannelMute | kDebugChannelSolo;
    assert(allowed_debug_actions(mixer, false, false, false, false) == 0);
    assert(allowed_debug_actions(mixer, true, false, false, false) == 0);
    assert(allowed_debug_actions(mixer, true, true, false, false) == mixer);
    assert(allowed_debug_actions(mixer, false, false, true, true) == mixer);
    assert(allowed_debug_actions(mixer, false, false, true, false) == 0);
    const auto letters = kDebugToggleDumps | kDebugToggleTextureInspector | kDebugToggleAudioMixer |
        kDebugToggleFrameGraph | kDebugToggleAudioBar | kDebugToggleCpuPc |
        kDebugToggleTextureSlots | kDebugScreenshot | kDebugShowFolder;
    assert(allowed_debug_actions(letters, false, false, true, true) == 0);
    assert(allowed_debug_actions(letters, true, false, false, false) == letters);
    assert(allowed_debug_actions(kDebugAnnotate | kDebugToggleMenu, false, false, false, false) ==
           (kDebugAnnotate | kDebugToggleMenu));
    std::array<bool, 16> muted{};
    assert(audio_debug_status(-1, muted).empty());
    assert(audio_debug_status(0, muted) == "AUDIO DEBUG: SOLO CH0");
    muted[3] = true;
    muted[15] = true;
    assert(audio_debug_status(-1, muted) == "AUDIO DEBUG: MUTED CH3 / MUTED CH15");
    assert(audio_debug_status(0, muted) == "AUDIO DEBUG: SOLO CH0 / MUTED CH3 / MUTED CH15");
    using Clock = std::chrono::steady_clock;
    const auto anchor = Clock::time_point{} + std::chrono::seconds(10);
    const auto period = std::chrono::milliseconds(16);
    auto deadline = anchor;
    for (int frame = 0; frame < 10000; ++frame) {
        deadline = advance_frame_deadline(deadline, anchor, period);
        assert(deadline == anchor + period * (frame + 1));
    }
    assert(advance_frame_deadline(anchor, anchor + period * 10, period) == anchor + period * 10);
    assert(advance_frame_deadline(anchor, anchor + period, period) == anchor + period);
    assert(advance_frame_deadline(anchor, anchor + period - std::chrono::milliseconds(1), period) ==
           anchor + period);
    assert(resampled_frame_count(100.0, 32768.0, 48000, 0.01) == 145);
    assert(resampled_frame_count(100.0, 32768.0, 48000, -0.01) == 147);
    assert(resampled_frame_count(100.0, 32768.0, 48000) == 146);
    std::vector<std::int16_t> source(100 * 2, 1000);
    double position = 0;
    const auto converted = resample_stereo_cubic(source.data(), 100, 32768.0, 48000, position);
    assert(converted.size() / 2 == 147);
    assert(std::abs(position - 0.352) < 0.001);
    assert(std::abs(converted[100] - 1000) <= 1);
    double faster_position = 0.0, slower_position = 0.0;
    const auto faster = resample_stereo_cubic(source.data(), 100, 32768.0, 48000,
                                              faster_position, 0.01);
    const auto slower = resample_stereo_cubic(source.data(), 100, 32768.0, 48000,
                                              slower_position, -0.01);
    assert(faster.size() / 2 < converted.size() / 2);
    assert(slower.size() / 2 > converted.size() / 2);
    double drift = 0.0;
    std::size_t total_output_frames = 0;
    for (int chunk = 0; chunk < 1000; ++chunk)
        total_output_frames += resample_stereo_cubic(source.data(), 100, 32768.0, 48000, drift).size() / 2;
    assert(std::abs(static_cast<double>(total_output_frames) - (100000.0 * 48000.0 / 32768.0)) <= 1.0);
    std::cout << "frontend pure logic tests passed\n";
}
