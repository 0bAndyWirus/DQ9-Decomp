// DQ9-Decomp entry point: separately supplied guest code and the runtime.
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <limits>
#include <cstdint>
#include <ctime>
#include <cinttypes>

#include "dsr/frontend.h"
#include "dsr/host_path.h"
#if defined(_WIN32)
#include <shellapi.h>
#endif
#include "dsr/gpu2d.h"
#include "dsr/machine.h"
#include "dsr/session.h"
#include "dsr/rt.h"

#include <atomic>
#include <chrono>
#include <thread>
#include "dsr/spu.h"

#include <vector>
#include "dsr/gpu3d.h"
#include "debug_tool.h"
#include "debug_workflow_test.h"

namespace {
using FrameIndex = std::int64_t;


struct ScriptedInput {
    std::uint16_t buttons = 0;
    int x = -1;
    int y = -1;
};

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save;
    std::filesystem::path abi_config;
    bool headless = false;
    FrameIndex frames = -1; // -1: run until quit
    FrameIndex pause_at_frame = -1; // replay to the end of this zero-based frame, then pause/stop
    std::optional<std::int64_t> rtc_epoch;
    std::optional<int> rtc_timezone_offset_minutes;
    std::set<FrameIndex> dump_frames;
    std::filesystem::path dump_dir = "build/dq9-frames";
    std::filesystem::path input_script;
    std::uint8_t language = 1;
    FrameIndex debug_every = 0; // print machine status every N frames
    bool dump_ram = false; // also write main RAM and video memory for each --dump-frames frame
    bool dump_vram = false; // also write the physical LCDC VRAM window, including GX texture/palette banks
    std::filesystem::path dump_audio; // write all emitted audio as a 16-bit stereo WAV
    std::filesystem::path hash_file; // per-frame FNV-1a of both screens, for oracle comparison
    std::filesystem::path record_input; // write the input of every frame (same grammar as --input-script) for replay
    bool launcher = false;
    bool debug_tool = false;
    bool frame_time_overlay = false;
    bool debug_selftest = false;
    bool debug_workflow_selftest = false;
    bool capture_memory_dumps = true;
    // Flush the cartridge save every N emulated frames (a no-op while it is unchanged), so a crash,
    // kill or power loss costs at most N frames of progress. -1: 30 s interactively, off headless.
    std::int64_t autosave_frames = -1;
    int gpu3d_scale = 1; // optional hi-res 3D overlay scale, 1-4 (1 = off)
    bool gpu3d_scale_given = false;
};

[[noreturn]] void usage(int code) {
    std::fprintf(code ? stderr : stdout,
                 "usage: dq9 --rom PATH [--save PATH] [--abi-config PATH] [--language en|fr|de|es|it]\n"
                 "           [--launcher] [--debug-tool] [--frame-time-overlay] [--debug-selftest]\n"
                 "Select an already locally available compatible image via --rom or $DQ9_ROM.\n"
                 "--abi-config (or $DQ9_ABI_CONFIG) supplies optional guest overrides as an INI file.\n"
                 "Without it, thread overrides are disabled. --dump-ram adds frame_NNNN_mainram.bin (logical\n"
                 "main RAM from 0x02000000, 4 MiB) and frame_NNNN_video.bin (palettes 0x800, OAM 0x800,\n"
                 "BG-A 0x80000, BG-B 0x20000, OBJ-A 0x40000, OBJ-B 0x20000) beside each dumped frame.\n"
                 "--dump-vram adds frame_NNNN_vram.bin: texture slots 0-3 (128 KiB each),\n"
                 "texture-palette slots 0-5 (16 KiB each), in slot order; unmapped slots are zero.\n"
                 "`frame button_mask touch_x touch_y` (mask in KEYINPUT bit order, X=1<<10,\n"
                 "Y=1<<11; touch -1 -1 when released); each line applies to one frame.\n"
                 "--hash-file FILE writes `frame top_fnv1a64 bottom_fnv1a64` for every frame (FNV-1a over the\n"
                 "RGB bytes of each screen, the same digest the oracle's --hashes option produces).\n"
                 "--record-input FILE logs the input of every frame that has any (same grammar as\n"
                 "--input-script) so an interactive session can be replayed headless with the same save.\n"
                 "--gpu3d-scale N (1-4, default 1 = off) re-renders the 3D scene at N times the native\n"
                 "resolution and composes it over the top screen; with --dump-frames it also writes\n"
                 "frame_NNNN_top_hires.ppm. Hash files and the native frame dumps stay 256x192.\n"
                 "--autosave-frames N flushes the save every N emulated frames (default 1800 = 30 s in a\n"
                 "window, off with --headless; 0 disables). The save is also written on exit.\n"
                 "--pause-at-frame N replays through zero-based frame N without pacing/audio, then pauses (or stops headless).\n"
                 "--rtc-epoch seconds and --rtc-timezone-offset minutes freeze RTC to emulated time.\n");
    std::exit(code);
}

std::uint8_t parse_language(std::string_view s) {
    if (s == "en") return 1;
    if (s == "fr") return 2;
    if (s == "de") return 3;
    if (s == "it") return 4;
    if (s == "es") return 5;
    std::fprintf(stderr, "dq9: unknown language '%.*s'\n", static_cast<int>(s.size()), s.data());
    usage(2);
}
int local_timezone_offset_minutes(std::time_t epoch) {
    std::tm local{};
#if defined(_WIN32)
    if (::localtime_s(&local, &epoch) != 0) return 0;
    long timezone_seconds = 0;
    long daylight_bias_seconds = 0;
    if (_get_timezone(&timezone_seconds) != 0) return 0;
    if (local.tm_isdst > 0 && _get_dstbias(&daylight_bias_seconds) != 0) return 0;
    return static_cast<int>(-(timezone_seconds + (local.tm_isdst > 0 ? daylight_bias_seconds : 0)) / 60);
#else
    if (::localtime_r(&epoch, &local) == nullptr) return 0;
    return static_cast<int>(local.tm_gmtoff / 60);
#endif
}


// Frame lists: comma-separated indices; `a-b` and `a-b/step` are inclusive ranges
// (same grammar as the oracle driver's --snapshots).
void parse_frame_spec(const std::string& spec, std::set<FrameIndex>& frames) {
    std::stringstream list(spec);
    for (std::string item; std::getline(list, item, ',');) {
        if (item.empty()) continue;
        const std::size_t dash = item.find('-', 1);
        if (dash == std::string::npos) {
            frames.insert(std::stoll(item));
            continue;
        }
        const std::size_t slash = item.find('/', dash);
        const FrameIndex first = std::stoll(item.substr(0, dash));
        const FrameIndex last = std::stoll(item.substr(dash + 1, slash == std::string::npos ? std::string::npos : slash - dash - 1));
        const FrameIndex step = slash == std::string::npos ? 1 : std::max<FrameIndex>(1, std::stoll(item.substr(slash + 1)));
        for (FrameIndex f = first; f <= last; f += step) frames.insert(f);
    }
}
void load_recorded_rtc(Options& options) {
    if (options.input_script.empty()) return;
    std::ifstream input(dsr::host::path_with_suffix(options.input_script, ".rtc"));
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 1);
        try {
            if (key == "rtc_epoch" && !options.rtc_epoch)
                options.rtc_epoch = std::stoll(value);
            else if (key == "rtc_timezone_offset_minutes" && !options.rtc_timezone_offset_minutes) {
                const long offset = std::stol(value);
                if (offset >= -840 && offset <= 840)
                    options.rtc_timezone_offset_minutes = static_cast<int>(offset);
            }
        } catch (...) {
            std::fprintf(stderr, "dq9: invalid RTC metadata beside input script\n");
            usage(2);
        }
    }
}


Options parse_args(int argc, char** argv) {
    Options o;
    o.abi_config = dsr::guest_abi_config_path_from_environment();
#if defined(_WIN32)
    if (const wchar_t* env = _wgetenv(L"DQ9_ROM")) o.rom = env;
#else
    if (const char* env = std::getenv("DQ9_ROM")) o.rom = env;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) usage(2);
            return argv[++i];
        };
        if (a == "--rom") o.rom = dsr::host::path_from_utf8(value());
        else if (a == "--save") o.save = dsr::host::path_from_utf8(value());
        else if (a == "--abi-config") o.abi_config = dsr::host::path_from_utf8(value());
        else if (a == "--language") o.language = parse_language(value());
        else if (a == "--headless") o.headless = true;
        else if (a == "--pause-at-frame") {
            o.pause_at_frame = std::stoll(std::string(value()));
            if (o.pause_at_frame < 0) usage(2);
        }
        else if (a == "--rtc-epoch") o.rtc_epoch = std::stoll(std::string(value()));
        else if (a == "--rtc-timezone-offset") {
            const long offset = std::stol(std::string(value()));
            if (offset < -840 || offset > 840) usage(2);
            o.rtc_timezone_offset_minutes = static_cast<int>(offset);
        }
        else if (a == "--frames") o.frames = std::stoll(std::string(value()));
        else if (a == "--gpu3d-scale") {
            const long long n = std::stoll(std::string(value()));
            if (n < 1 || n > 4) usage(2);
            o.gpu3d_scale = static_cast<int>(n);
            o.gpu3d_scale_given = true;
        }
        else if (a == "--autosave-frames") {
            o.autosave_frames = std::stoll(std::string(value()));
            if (o.autosave_frames < 0) usage(2);
        }
        else if (a == "--launcher") o.launcher = true;
        else if (a == "--debug-tool") o.debug_tool = true;
        else if (a == "--frame-time-overlay") o.frame_time_overlay = true;
        else if (a == "--debug-selftest") o.debug_selftest = true;
        else if (a == "--debug-workflow-selftest") o.debug_workflow_selftest = true;
        else if (a == "--no-debug-dumps") o.capture_memory_dumps = false;
        else if (a == "--dump-dir") o.dump_dir = dsr::host::path_from_utf8(value());
        else if (a == "--input-script") o.input_script = dsr::host::path_from_utf8(value());
        else if (a == "--record-input") o.record_input = dsr::host::path_from_utf8(value());
        else if (a == "--debug-every") o.debug_every = std::stoll(std::string(value()));
        else if (a == "--dump-ram") o.dump_ram = true;
        else if (a == "--dump-vram") o.dump_vram = true;
        else if (a == "--dump-audio") o.dump_audio = dsr::host::path_from_utf8(value());
        else if (a == "--hash-file") o.hash_file = dsr::host::path_from_utf8(value());
        else if (a == "--dump-frames") parse_frame_spec(std::string(value()), o.dump_frames);
        else if (a == "--help" || a == "-h") usage(0);
        else {
            std::fprintf(stderr, "dq9: unknown argument '%s'\n", argv[i]);
            usage(2);
        }
    }
    if (o.rom.empty() && !o.launcher && !o.debug_selftest && !o.debug_workflow_selftest) {
        std::fprintf(stderr, "dq9: no ROM given (--rom or $DQ9_ROM)\n");
        usage(2);
    }
    if (o.pause_at_frame == std::numeric_limits<FrameIndex>::max()) {
        std::fprintf(stderr, "dq9: --pause-at-frame must be a non-negative frame index\n");
        usage(2);
    }
    if (o.headless && o.frames < 0) {
        if (o.pause_at_frame < 0) {
            std::fprintf(stderr, "dq9: --headless requires --frames or --pause-at-frame\n");
            usage(2);
        }
        o.frames = o.pause_at_frame + 1;
    }
    if (o.pause_at_frame >= 0 && o.frames >= 0 && o.frames <= o.pause_at_frame) {
        std::fprintf(stderr, "dq9: --frames must include --pause-at-frame\n");
        usage(2);
    }
    load_recorded_rtc(o);
    return o;
}

std::map<FrameIndex, ScriptedInput> load_script(const std::filesystem::path& path) {
    std::map<FrameIndex, ScriptedInput> script;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "dq9: cannot read input script %s\n", path.string().c_str());
        std::exit(2);
    }
    FrameIndex frame;
    unsigned mask;
    int x, y;
    while (in >> frame >> mask >> x >> y) {
        script[frame] = ScriptedInput{static_cast<std::uint16_t>(mask), x, y};
    }
    return script;
}

// Interleaved stereo 16-bit PCM, little-endian, as emitted by the SPU drain.
void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm, double rate) {
    std::ofstream out(path, std::ios::binary);
    const auto put32 = [&](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                           static_cast<char>(v >> 24)};
        out.write(b, 4);
    };
    const auto put16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(b, 2);
    };
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const std::uint32_t sample_rate = static_cast<std::uint32_t>(rate + 0.5);
    out.write("RIFF", 4);
    put32(36 + bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(2); // stereo
    put32(sample_rate);
    put32(sample_rate * 4);
    put16(4);
    put16(16);
    out.write("data", 4);
    put32(bytes);
    for (const std::int16_t s : pcm) put16(static_cast<std::uint16_t>(s));
}

void write_ppm(const std::filesystem::path& path, const std::uint32_t* px, int width = 256, int height = 192) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int i = 0; i < width * height; ++i) {
        const char rgb[3] = {static_cast<char>((px[i] >> 16) & 0xFF), static_cast<char>((px[i] >> 8) & 0xFF),
                             static_cast<char>(px[i] & 0xFF)};
        out.write(rgb, 3);
    }
}

// FNV-1a over the R, G, B bytes of a screen in raster order (matches the oracle's frame hashes).
std::uint64_t hash_screen(const std::uint32_t* px) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (int i = 0; i < 256 * 192; ++i) {
        for (const int shift : {16, 8, 0}) {
            hash ^= (px[i] >> shift) & 0xFFu;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

struct GuestRegion {
    std::uint32_t base;
    std::uint32_t size;
};

// Writes the concatenation of ARM9-visible guest regions as little-endian bytes.
void dump_guest_regions(const std::filesystem::path& path, dsr::Machine& machine,
                        std::initializer_list<GuestRegion> regions) {
    std::ofstream out(path, std::ios::binary);
    std::vector<std::uint8_t> bytes;
    for (const GuestRegion& region : regions) {
        bytes.resize(region.size);
        for (std::uint32_t i = 0; i < region.size; i += 4) {
            const std::uint32_t w = machine.read32(dsr::CpuId::Arm9, region.base + i);
            bytes[i] = static_cast<std::uint8_t>(w);
            bytes[i + 1] = static_cast<std::uint8_t>(w >> 8);
            bytes[i + 2] = static_cast<std::uint8_t>(w >> 16);
            bytes[i + 3] = static_cast<std::uint8_t>(w >> 24);
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}
// Writes the active 3D texture and texture-palette slots in hardware-sized blocks.
// Unmapped slots are zero-filled, so the binary has a stable, documented layout.
void dump_texture_vram(const std::filesystem::path& path, const dsr::Gpu2D& gpu2d) {
    std::ofstream out(path, std::ios::binary);
    const std::array<std::uint8_t, 0x20000> empty{};
    const auto write_slots = [&](int count, std::size_t size, const auto& get_slot) {
        for (int i = 0; i < count; ++i) {
            const auto* data = get_slot(i);
            out.write(reinterpret_cast<const char*>(data ? data : empty.data()),
                      static_cast<std::streamsize>(size));
        }
    };
    write_slots(4, 0x20000U, [&](int slot) { return gpu2d.texture_slot(slot); });
    write_slots(6, 0x4000U, [&](int slot) { return gpu2d.texpal_slot(slot); });
}

class NullFrontend final : public dsr::Frontend {
public:
    bool init(const dsr::FrontendConfig&) override { return true; }
    void poll(dsr::InputState&) override {}
    void present(const std::uint32_t*, const std::uint32_t*) override {}
    void present_hires(const std::uint32_t*, int, const std::uint32_t*, int) override {}
    int requested_gpu3d_scale() const override { return 1; }
    void queue_audio(const std::int16_t*, std::size_t, double) override {}
    std::size_t audio_backlog() const override { return 0; }
    void pace_frame() override {}
};

std::string language_name(std::uint8_t language) {
    switch (language) {
    case 2: return "fr";
    case 3: return "de";
    case 4: return "it";
    case 5: return "es";
    default: return "en";
    }
}
} // namespace
int main(int argc, char** argv) {
#if defined(_WIN32)
    SDL_SetMainReady();
    // SDL_MAIN_HANDLED keeps a normal console entry point. Read the native
    // command line ourselves so paths are not truncated to the Windows ACP.
    int wide_argc = 0;
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> wide_arguments(
        CommandLineToArgvW(GetCommandLineW(), &wide_argc), &LocalFree);
    if (!wide_arguments) {
        std::fprintf(stderr, "dq9: cannot read the Windows command line (%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return 2;
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(wide_argc));
    for (int i = 0; i < wide_argc; ++i)
        arguments.push_back(dsr::host::utf8_from_wide(wide_arguments.get()[i]));
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argument_pointers.push_back(argument.data());
    argument_pointers.push_back(nullptr);
    argc = wide_argc;
    argv = argument_pointers.data();
#endif

    Options opt = parse_args(argc, argv);
    if (opt.pause_at_frame >= 0 && !opt.headless) opt.debug_tool = true;
    const dsr::UserPaths paths = dsr::default_user_paths();
    if (opt.debug_selftest) {
        if (opt.rom.empty() || opt.input_script.empty()) {
            std::fprintf(stderr, "[debug-selftest] SKIP: requires --rom/DQ9_ROM and --input-script\n");
            return 77;
        }
        if (opt.frames < 0) opt.frames = 300;
        opt.headless = true;
        opt.launcher = false;
        opt.debug_tool = true;
    }
    dq9::workflow_test::VirtualController workflow_controller;
    if (opt.debug_workflow_selftest) {
        const char* script_env = std::getenv("DQ9_WORKFLOW_INPUT");
        const char* save_env = std::getenv("DQ9_WORKFLOW_SAVE");
        if (script_env && *script_env) opt.input_script = dsr::host::path_from_utf8(script_env);
        const std::filesystem::path source_save = save_env && *save_env
            ? dsr::host::path_from_utf8(save_env) : opt.save;
        if (!std::filesystem::exists(opt.rom) || !std::filesystem::exists(opt.input_script) ||
            !std::filesystem::exists(source_save)) {
            std::fprintf(stderr, "[debug-workflow-selftest] SKIP: requires ROM, input script, and save (%s, %s, %s)\n",
                         opt.rom.string().c_str(), opt.input_script.string().c_str(), source_save.string().c_str());
            return 77;
        }
        std::error_code ec;
        std::filesystem::create_directories(paths.data_dir, ec);
        if (ec) {
            std::fprintf(stderr, "[debug-workflow-selftest] cannot create test data directory: %s\n", ec.message().c_str());
            return 1;
        }
        opt.save = paths.data_dir / "debug-workflow-save.sav";
        std::filesystem::copy_file(source_save, opt.save, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "[debug-workflow-selftest] cannot copy test save: %s\n", ec.message().c_str());
            return 1;
        }
        std::string controller_error;
        if (!dq9::workflow_test::attach_virtual_controller(workflow_controller, controller_error)) {
            std::fprintf(stderr, "[debug-workflow-selftest] SKIP: %s\n", controller_error.c_str());
            SDL_Quit();
            return 77;
        }
        opt.frames = 1000;
        opt.headless = false;
        opt.launcher = false;
        opt.debug_tool = true;
        opt.pause_at_frame = 199;
    }
    dsr::LauncherOptions launch_options;
    if (opt.debug_workflow_selftest) launch_options.frontend.controller_guid = workflow_controller.guid;
    if (opt.launcher) {
        if (opt.headless) {
            std::fprintf(stderr, "dq9: --launcher cannot be combined with --headless\n");
            return 2;
        }
        launch_options.rom_path = opt.rom;
        launch_options.save_path = opt.save;
        launch_options.language = language_name(opt.language);
        launch_options.debug_tool = opt.debug_tool;
        launch_options.frame_time_overlay = opt.frame_time_overlay;
        launch_options.capture_memory_dumps = opt.capture_memory_dumps;
        if (!dsr::run_sdl_launcher(launch_options)) return 0;
        opt.rom = launch_options.rom_path;
        opt.save = launch_options.save_path;
        opt.language = parse_language(launch_options.language);
        opt.debug_tool = launch_options.debug_tool;
        opt.frame_time_overlay = launch_options.frame_time_overlay;
        opt.capture_memory_dumps = launch_options.capture_memory_dumps;
    }
    char* executable_directory = SDL_GetBasePath();
    opt.rom = dsr::resolve_rom_path(opt.rom, paths.config_dir,
                                  dsr::host::path_from_utf8(executable_directory ? executable_directory : ""));
    SDL_free(executable_directory);
    std::error_code rom_error;
    if (!std::filesystem::is_regular_file(opt.rom, rom_error)) {
        if (opt.debug_workflow_selftest) {
            std::fprintf(stderr, "[debug-workflow-selftest] SKIP: ROM is unavailable: %s\n", opt.rom.string().c_str());
            dq9::workflow_test::close_virtual_controller(workflow_controller);
            SDL_Quit();
            return 77;
        }
        std::fprintf(stderr, "dq9: ROM not found or not a regular file: %s\n"
                     "Relative ROM paths are searched in the working directory, config directory (%s), "
                     "and repository ancestors of the executable. Use --rom /absolute/path/to/game.nds.\n",
                     opt.rom.string().c_str(), paths.config_dir.string().c_str());
        return 2;
    }
    std::fprintf(stderr, "debug tool: %s (hotkeys F10 menu, F9 note, F8 step)\n",
                 opt.debug_tool ? "ON" : "OFF");

    std::unique_ptr<dsr::Frontend> frontend;
    dsr::FrontendConfig fc;
    if (!opt.headless) {
        frontend = dsr::create_sdl_frontend();
        fc = launch_options.frontend;
        fc.title = "DQ9-Decomp";
        if (!opt.launcher) fc.fullscreen = dsr::running_on_steam_deck();
        fc.debug_tool_enabled = opt.debug_tool;
        fc.frame_time_overlay_enabled = opt.frame_time_overlay;
        if (opt.gpu3d_scale_given) {
            fc.gpu3d_scale = opt.gpu3d_scale;
            fc.gpu3d_scale_explicit = true;
        }
        if (!frontend->init(fc)) {
            if (opt.debug_workflow_selftest) {
                dq9::workflow_test::close_virtual_controller(workflow_controller);
                std::fprintf(stderr, "[debug-workflow-selftest] SKIP: SDL dummy video/audio backend unavailable\n");
                return 77;
            }
            std::fprintf(stderr, "dq9: frontend initialisation failed\n");
            return 1;
        }
    } else {
        fc.debug_tool_enabled = opt.debug_tool;
        fc.frame_time_overlay_enabled = opt.frame_time_overlay;
    }

    const std::filesystem::path save_path = opt.save.empty() ? paths.data_dir / "dq9.sav" : opt.save;
    const bool deterministic_rtc = opt.debug_tool || !opt.record_input.empty() ||
                                   opt.pause_at_frame >= 0 || opt.rtc_epoch.has_value() ||
                                   opt.rtc_timezone_offset_minutes.has_value();
    const bool needs_launch_clock = deterministic_rtc &&
        (!opt.rtc_epoch.has_value() || !opt.rtc_timezone_offset_minutes.has_value());
    const std::time_t launch_wall_time = needs_launch_clock ? std::time(nullptr) : 0;
    const std::int64_t rtc_epoch = opt.rtc_epoch.value_or(static_cast<std::int64_t>(launch_wall_time));
    const int rtc_timezone_offset = opt.rtc_timezone_offset_minutes.value_or(
        needs_launch_clock ? local_timezone_offset_minutes(launch_wall_time) : 0);
    std::unique_ptr<dq9::DebugSession> debug_session;
    if (opt.debug_tool) {
        std::error_code ec;
        std::filesystem::create_directories(paths.data_dir, ec);
        debug_session = std::make_unique<dq9::DebugSession>(
            paths.data_dir, opt.rom, save_path, language_name(opt.language),
            std::filesystem::absolute(dsr::host::path_from_utf8(argv[0])), opt.capture_memory_dumps,
            rtc_epoch, rtc_timezone_offset, opt.abi_config);
    }
    dsr::MachineConfig mc;
    mc.rom_path = opt.rom;
    mc.save_path = save_path;
    mc.firmware.language = opt.language;
    mc.deterministic_rtc = deterministic_rtc;
    mc.rtc_epoch_seconds = rtc_epoch;
    mc.rtc_timezone_offset_minutes = rtc_timezone_offset;
    mc.arm9_table = dsr::gen_table_arm9();
    mc.arm7_table = dsr::gen_table_arm7();
    mc.arm9_regions = dsr::gen_dispatch_regions_arm9();
    mc.arm7_regions = dsr::gen_dispatch_regions_arm7();
    try {
        mc.guest_abi = dsr::load_guest_abi_config(opt.abi_config);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dq9: %s\n", error.what());
        if (opt.debug_workflow_selftest) dq9::workflow_test::close_virtual_controller(workflow_controller);
        return 2;
    }
    dsr::Session session(mc);
    dsr::Machine& machine = session.machine();
    NullFrontend null_frontend;
    dsr::Frontend& capture_frontend = frontend ? *frontend : null_frontend;
    bool debug_visible = opt.frame_time_overlay;
    bool paused = false;
    bool annotation_open = false;
    bool menu_open = false;
    bool menu_previous_paused = false;
    bool annotation_restore_paused = false;
    bool help_open = false;
    bool texture_inspector = opt.debug_selftest;
    bool audio_mixer = false;
    bool frame_graph = true, audio_bar = true, cpu_pc = true, show_texture_slots = true;
    bool capture_dumps = opt.capture_memory_dumps;
    bool fast_forward = false;
    bool step_requested = false;
    double slow_motion = 1.0;
    dsr::AnnotationEditor annotation;
    dsr::DebugMenuModel debug_menu;
    int keyboard_selection = 0;
    int audio_channel_selection = 0;
    std::array<bool, 16> audio_muted{};
    std::uint64_t annotation_buttons_held = 0;
    std::uint64_t frames_recorded = 0;
    std::size_t previous_unimplemented_io = machine.unimplemented_io_count();
    unsigned slow_frame_streak = 0;
    double clipped_audio_seconds = 0.0;
    bool auto_capture_latched = false;
    bool unimplemented_capture_done = false;
    const auto launch_time = std::chrono::steady_clock::now();
    const auto debug_banner_deadline = launch_time + std::chrono::seconds(5);
    std::string debug_status_message;

    auto capture_issue = [&](const std::string& text, std::uint64_t frame_number) {
        if (!debug_session) return false;
        dq9::DebugCaptureOptions capture;
        capture.comment = text;
        capture.language = language_name(opt.language);
        capture.executable = std::filesystem::absolute(argv[0]);
        capture.dump_memory = capture_dumps;
        capture.texture_inspector = texture_inspector;
        capture.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch_time).count();
        capture.frame = frame_number;
        const auto capture_start = std::chrono::steady_clock::now();
        const bool saved = debug_session->capture(capture, machine, capture_frontend, fc);
        debug_status_message = saved ? "Saved bundle: " + debug_session->directory().string() : "Bundle save failed";
        if (saved) capture_dumps = false;
        const double capture_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - capture_start).count();
        if (saved) std::fprintf(stderr, "[debug] captured frame %llu in %s (%.3f ms; audio %s)\n",
                                static_cast<unsigned long long>(frame_number),
                                debug_session->directory().string().c_str(), capture_ms,
                                paused ? "intentionally paused; re-prime on resume" : "running");
        return saved;
    };
    auto update_overlay = [&](std::uint64_t frame_number) {
        if (!frontend || (!opt.debug_tool && !opt.frame_time_overlay)) return;
        dsr::DebugOverlayState state;
        state.visible = debug_visible;
        state.debug_tool_enabled = opt.debug_tool;
        state.startup_banner_visible = opt.debug_tool &&
            std::chrono::steady_clock::now() < debug_banner_deadline;
        state.paused = paused;
        state.help = help_open;
        state.annotation_open = annotation_open;
        state.menu_open = menu_open;
        state.menu_selection = static_cast<int>(debug_menu.selected());
        state.frame_graph = frame_graph;
        state.audio_bar = audio_bar;
        state.cpu_pc = cpu_pc;
        state.show_texture_slots = show_texture_slots;
        state.texture_inspector = texture_inspector;
        state.audio_mixer = audio_mixer;
        state.capture_dumps = capture_dumps;
        state.speed = fast_forward ? 4.0 : slow_motion;
        state.fps = debug_session ? static_cast<float>(debug_session->fps()) : 0.0f;
        state.audio_backlog_ms = static_cast<float>(frontend->audio_backlog() * 1000.0 / std::max(1, fc.audio_rate));
        state.frame = frame_number;
        state.arm9_pc = machine.debug_cpu_snapshot(dsr::CpuId::Arm9).current_pc;
        if (debug_session) {
            state.frame_times_ms = debug_session->frame_times();
            state.frame_time_count = debug_session->frame_time_count();
        }
        state.keyboard_selection = keyboard_selection;
        state.audio_channel_selection = audio_channel_selection;
        state.audio_muted = audio_muted;
        state.audio_solo = machine.spu().debug_state().solo_channel;
        for (int slot = 0; slot < 4; ++slot) state.texture_slots[static_cast<std::size_t>(slot)] = machine.gpu2d().texture_slot(slot);
        state.annotation = annotation.text();
        state.status = debug_status_message;
        frontend->set_debug_overlay(state);
    };

    // DQ9_WATCHDOG=1: report machine state when a frame takes > 5 s (hang
    // diagnosis). Reads are racy by design; diagnostics only.
    std::atomic<FrameIndex> frames_done{0};
    std::atomic<bool> finished{false};
    std::thread watchdog;
    if (std::getenv("DQ9_WATCHDOG")) {
        watchdog = std::thread([&] {
            FrameIndex last = -1;
            while (!finished) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const FrameIndex now = frames_done;
                if (now == last && !finished) {
                    std::fprintf(stderr, "[watchdog] stuck in frame %" PRId64 ": %s\n", now, machine.debug_status().c_str());
                }
                last = now;
            }
        });
    }

    const std::map<FrameIndex, ScriptedInput> script =
        opt.input_script.empty() ? std::map<FrameIndex, ScriptedInput>{} : load_script(opt.input_script);
    if (!opt.dump_frames.empty()) {
        std::filesystem::create_directories(opt.dump_dir);
    }
    std::ofstream hash_out;
    if (!opt.hash_file.empty()) {
        hash_out.open(opt.hash_file);
        if (!hash_out) {
            std::fprintf(stderr, "dq9: cannot write hash file %s\n", opt.hash_file.string().c_str());
            return 2;
        }
    }
    std::ofstream record_out;
    if (!opt.record_input.empty()) {
        record_out.open(opt.record_input);
        if (!record_out) {
            std::fprintf(stderr, "dq9: cannot write input recording %s\n", opt.record_input.string().c_str());
            return 2;
        }
        std::ofstream rtc_out(dsr::host::path_with_suffix(opt.record_input, ".rtc"), std::ios::binary | std::ios::trunc);
        if (!rtc_out) {
            std::fprintf(stderr, "dq9: cannot write RTC metadata for input recording %s\n",
                         opt.record_input.string().c_str());
            return 2;
        }
        rtc_out << "rtc_epoch: " << rtc_epoch << '\n'
                << "rtc_timezone_offset_minutes: " << rtc_timezone_offset << '\n';
    }
    std::vector<std::int16_t> audio_capture;
    // The frontend updates pen and lid only on events, so its state must
    // persist across frames; scripted input is rebuilt per frame below.
    dsr::InputState frontend_input;
    // Hi-res 3D overlay: the scale in effect on the 3D engine, and the composed screen.
    std::vector<std::uint32_t> hires_screen;
    auto present_frame = [&]() {
        const int scale = machine.gpu3d().output_scale();
        bool on_top = true;
        if (scale > 1) hires_screen.resize(static_cast<std::size_t>(256 * scale) * (192 * scale));
        if (scale > 1 && session.compose_hires(hires_screen.data(), scale, &on_top)) {
            if (on_top) frontend->present_hires(hires_screen.data(), scale, machine.gpu2d().screen_bottom(), 1);
            else frontend->present_hires(machine.gpu2d().screen_top(), 1, hires_screen.data(), scale);
        } else {
            frontend->present(machine.gpu2d().screen_top(), machine.gpu2d().screen_bottom());
        }
    };
    const std::int64_t autosave_every = opt.autosave_frames >= 0 ? opt.autosave_frames
                                       : (opt.headless ? 0 : 1800);
    bool held_fast_forward_was_active = false;
    bool focus_pause_was_active = false;
    const std::filesystem::path screenshot_dir =
        (save_path.has_parent_path() ? save_path.parent_path() : std::filesystem::path{}) / "screenshots";
    auto handle_debug_input = [&](const dsr::InputState& input, FrameIndex current_frame) {
        if (!opt.debug_tool) return;
        const std::uint64_t capture_frame = current_frame >= 0
            ? static_cast<std::uint64_t>(current_frame) : 0;
        auto set_paused = [&](bool value) {
            if (value && !paused && frontend) frontend->clear_audio_queue();
            paused = value;
            if (paused) step_requested = false;
        };
        auto begin_annotation = [&](bool resume_paused) {
            annotation_restore_paused = resume_paused;
            set_paused(true);
            annotation_open = true;
            annotation.clear();
            keyboard_selection = 0;
            annotation_buttons_held = 0;
        };
        auto end_annotation = [&] {
            annotation_open = false;
            set_paused(annotation_restore_paused);
        };
        auto open_menu = [&] {
            if (menu_open || annotation_open) return;
            debug_visible = true;
            menu_previous_paused = paused;
            menu_open = true;
            set_paused(true);
            debug_menu.select(input.menu_selection);
        };
        auto close_menu = [&](bool restore_pause) {
            menu_open = false;
            if (restore_pause) set_paused(menu_previous_paused);
        };
        auto activate_menu = [&](dsr::DebugMenuItem item) {
            switch (item) {
            case dsr::DebugMenuItem::PauseResume:
                menu_open = false;
                set_paused(!menu_previous_paused);
                break;
            case dsr::DebugMenuItem::Step:
                menu_open = false;
                set_paused(true);
                step_requested = true;
                break;
            case dsr::DebugMenuItem::SlowHalf:
                slow_motion = 0.5;
                break;
            case dsr::DebugMenuItem::SlowQuarter:
                slow_motion = 0.25;
                break;
            case dsr::DebugMenuItem::FastForward:
                fast_forward = !fast_forward;
                break;
            case dsr::DebugMenuItem::AddNote:
                menu_open = false;
                begin_annotation(menu_previous_paused);
                break;
            case dsr::DebugMenuItem::Marker:
                if (debug_session) debug_session->mark(capture_frame, machine.ticks(), machine);
                break;
            case dsr::DebugMenuItem::QuickBundle:
                capture_issue("[quick] debug menu bundle", capture_frame);
                break;
            case dsr::DebugMenuItem::FrameGraph: frame_graph = !frame_graph; break;
            case dsr::DebugMenuItem::AudioBar: audio_bar = !audio_bar; break;
            case dsr::DebugMenuItem::CpuPc: cpu_pc = !cpu_pc; break;
            case dsr::DebugMenuItem::TextureSlots: show_texture_slots = !show_texture_slots; break;
            case dsr::DebugMenuItem::AudioMixer: audio_mixer = !audio_mixer; break;
            case dsr::DebugMenuItem::TextureInspector: texture_inspector = !texture_inspector; break;
            case dsr::DebugMenuItem::Screenshot:
                if (!debug_session || !debug_session->screenshot_only(machine, capture_frame))
                    debug_status_message = "Screenshot failed";
                else debug_status_message = "Screenshot saved in " + debug_session->directory().string();
                break;
            case dsr::DebugMenuItem::Folder:
                debug_status_message = debug_session
                    ? "Debug folder: " + debug_session->directory().parent_path().string()
                    : "Debug folder is not available";
                std::fprintf(stderr, "%s\n", debug_status_message.c_str());
                break;
            case dsr::DebugMenuItem::Help: help_open = !help_open; break;
            case dsr::DebugMenuItem::Close: close_menu(true); break;
            case dsr::DebugMenuItem::Count: break;
            }
        };

        const std::uint32_t actions = input.debug_actions;
        if ((actions & dsr::kDebugToggleMenu) && !menu_open && !annotation_open)
            open_menu();

        if (menu_open) {
            for (const int action : input.menu_actions) {
                if (action == -1 || action == 1) debug_menu.move(action);
                else if (action == static_cast<int>(dsr::DebugMenuItem::Close))
                    close_menu(true);
                else if (action == static_cast<int>(dsr::DebugMenuItem::Count))
                    activate_menu(debug_menu.activate());
                else if (action > static_cast<int>(dsr::kDebugMenuRowCount) &&
                         action <= static_cast<int>(dsr::kDebugMenuRowCount * 2))
                    debug_menu.select(action - static_cast<int>(dsr::kDebugMenuRowCount) - 1);
                else if (action >= 0 && action < static_cast<int>(dsr::kDebugMenuRowCount))
                    debug_menu.select(action);
            }
            return;
        }

        if (!annotation_open) {
            if (actions & dsr::kDebugTogglePause) set_paused(!paused);
            if (actions & dsr::kDebugSingleStep) {
                set_paused(true);
                step_requested = true;
            }
            if (actions & dsr::kDebugSlowMotion)
                slow_motion = slow_motion == 1.0 ? 0.5 : slow_motion == 0.5 ? 0.25 : 1.0;
            if (actions & dsr::kDebugFastForward) fast_forward = !fast_forward;
            if (actions & dsr::kDebugToggleHelp) help_open = !help_open;
            if (actions & dsr::kDebugToggleFrameGraph) frame_graph = !frame_graph;
            if (actions & dsr::kDebugToggleAudioBar) audio_bar = !audio_bar;
            if (actions & dsr::kDebugToggleCpuPc) cpu_pc = !cpu_pc;
            if (actions & dsr::kDebugToggleTextureSlots) show_texture_slots = !show_texture_slots;
            if (actions & dsr::kDebugScreenshot) {
                if (!debug_session || !debug_session->screenshot_only(machine, capture_frame))
                    debug_status_message = "Screenshot failed";
                else debug_status_message = "Screenshot saved in " + debug_session->directory().string();
            }
            if (actions & dsr::kDebugShowFolder) {
                debug_status_message = debug_session
                    ? "Debug folder: " + debug_session->directory().parent_path().string()
                    : "Debug folder is not available";
                std::fprintf(stderr, "%s\n", debug_status_message.c_str());
            }
            if (actions & dsr::kDebugToggleDumps) capture_dumps = !capture_dumps;
            if (actions & dsr::kDebugToggleTextureInspector) texture_inspector = !texture_inspector;
            if (actions & dsr::kDebugToggleAudioMixer) audio_mixer = !audio_mixer;
            if (actions & dsr::kDebugChannelUp) audio_channel_selection = (audio_channel_selection + 15) % 16;
            if (actions & dsr::kDebugChannelDown) audio_channel_selection = (audio_channel_selection + 1) % 16;
            if (actions & dsr::kDebugChannelMute) {
                audio_muted[static_cast<std::size_t>(audio_channel_selection)] =
                    !machine.spu().debug_state().channels[static_cast<std::size_t>(audio_channel_selection)].muted;
                machine.spu().set_channel_muted(static_cast<unsigned>(audio_channel_selection),
                                                audio_muted[static_cast<std::size_t>(audio_channel_selection)]);
                std::fprintf(stderr, "[audio-debug] channel %d mute=%s\n", audio_channel_selection,
                    audio_muted[static_cast<std::size_t>(audio_channel_selection)] ? "ON" : "OFF");
            }
            if (actions & dsr::kDebugChannelSolo) {
                const int solo = machine.spu().debug_state().solo_channel;
                machine.spu().set_solo_channel(solo == audio_channel_selection ? -1 : audio_channel_selection);
                std::fprintf(stderr, "[audio-debug] solo=%d\n", machine.spu().debug_state().solo_channel);
            }
            if ((actions & dsr::kDebugAnnotate) && !annotation_open)
                begin_annotation(paused);
            if (actions & dsr::kDebugMark) {
                if (debug_session) debug_session->mark(capture_frame, machine.ticks(), machine);
            }
        }
        if (!annotation_open) return;
        for (const dsr::EditorEvent& editor_event : input.editor_events) {
            const dsr::EditorResult result = annotation.apply(editor_event);
            if (result == dsr::EditorResult::Save) {
                capture_issue(annotation.text(), capture_frame);
                end_annotation();
                return;
            }
            if (result == dsr::EditorResult::Cancel) {
                end_annotation();
                return;
            }
        }
        const std::uint16_t buttons = input.editor_buttons;
        const std::uint16_t pressed = static_cast<std::uint16_t>(buttons & ~annotation_buttons_held);
        annotation_buttons_held = buttons;
        if (pressed & dsr::kBtnUp) keyboard_selection = (keyboard_selection + 95 - 12) % 95;
        if (pressed & dsr::kBtnDown) keyboard_selection = (keyboard_selection + 12) % 95;
        if (pressed & dsr::kBtnLeft) keyboard_selection = (keyboard_selection + 94) % 95;
        if (pressed & dsr::kBtnRight) keyboard_selection = (keyboard_selection + 1) % 95;
        if (pressed & dsr::kBtnA && annotation.size() < 4096) {
            const char ch = static_cast<char>(32 + keyboard_selection);
            annotation.insert(std::string_view(&ch, 1));
        }
        if (pressed & dsr::kBtnStart) {
            capture_issue(annotation.text(), capture_frame);
            end_annotation();
        } else if (pressed & dsr::kBtnB) end_annotation();
    };
    FrameIndex frame = 0;
    enum class WorkflowPhase {
        WaitReplayPause, WaitReplayLiveInput, WaitForPause, WaitPaused, WaitMenuOpen,
        WaitMenuDispatch, WaitAnnotation, WaitText, WaitBundle,
        WaitFallbackAnnotation, WaitFallbackText, WaitFallbackCancelled,
        WaitPauseFallbackResumed, WaitPauseFallbackPaused, WaitStep, WaitResume,
        WaitControllerAnnotation, ControllerInputs, WaitControllerBundle, Done
    };
    WorkflowPhase workflow_phase = WorkflowPhase::WaitReplayPause;
    bool workflow_failed = false;
    std::string workflow_error;
    std::uint64_t workflow_iterations = 0;
    std::uint64_t workflow_pause_frame = 0, workflow_pause_ticks = 0;
    std::uint64_t workflow_audio_blocks = 0;
    unsigned workflow_text_paused_loops = 0;
    unsigned workflow_note_input_stage = 0;
    unsigned workflow_fallback_note_stage = 0;
    std::uint64_t workflow_step_frame = 0, workflow_step_ticks = 0;
    std::uint64_t workflow_note_frame = 0, workflow_note_ticks = 0;
    std::uint64_t workflow_controller_frame = 0, workflow_controller_ticks = 0;
    bool workflow_expect_live_input = false;
    std::vector<std::uint32_t> workflow_top, workflow_bottom;
    std::vector<std::pair<SDL_GameControllerButton, bool>> controller_events;
    std::size_t controller_event_index = 0;
    constexpr std::string_view workflow_note = "axb texture flickers  after inn\nwhile paused";
    constexpr std::string_view controller_note = "HI";
    const std::filesystem::path workflow_debug_root = paths.data_dir / "debug";
    auto workflow_fail = [&](std::string message) {
        workflow_failed = true;
        workflow_error = std::move(message);
        std::fprintf(stderr, "[debug-workflow-selftest] FAIL: %s\n", workflow_error.c_str());
    };
    auto workflow_push_controller = [&](SDL_GameControllerButton button, bool down) {
        if (!dq9::workflow_test::set_controller_button(workflow_controller, button, down))
            workflow_fail("could not push a synthetic SDL controller event");
    };
    auto workflow_diagnostics = [&] {
        dsr::FrontendDiagnostics diagnostics;
        return diagnostics;
    };
    auto workflow_drive_events = [&] {
        if (!opt.debug_workflow_selftest || workflow_failed) return;
        if (++workflow_iterations > 1500) {
            workflow_fail("workflow did not reach its completion state within 1500 event-loop iterations");
            return;
        }
        switch (workflow_phase) {
        case WorkflowPhase::WaitReplayPause:
            if (frame == opt.pause_at_frame + 1 && paused) {
                workflow_pause_frame = static_cast<std::uint64_t>(frame);
                workflow_pause_ticks = machine.ticks();
                workflow_push_controller(SDL_CONTROLLER_BUTTON_B, true);
                if (!workflow_failed && !dq9::workflow_test::push_key(SDL_SCANCODE_SPACE))
                    workflow_fail("could not push the replay-handoff resume key");
                if (!workflow_failed) {
                    workflow_expect_live_input = true;
                    workflow_phase = WorkflowPhase::WaitReplayLiveInput;
                }
            } else if (frame > opt.pause_at_frame + 1 || (frame == opt.pause_at_frame + 1 && !paused)) {
                workflow_fail("replay did not pause at the configured inclusive target");
            }
            break;
        case WorkflowPhase::WaitReplayLiveInput:
            if (workflow_expect_live_input || paused ||
                static_cast<std::uint64_t>(frame) != workflow_pause_frame + 1 ||
                machine.ticks() <= workflow_pause_ticks) {
                workflow_fail("resume did not deliver the live controller input on the first post-target frame");
                break;
            }
            workflow_push_controller(SDL_CONTROLLER_BUTTON_B, false);
            if (!workflow_failed) workflow_phase = WorkflowPhase::WaitForPause;
            break;
        case WorkflowPhase::WaitForPause:
            if (frame >= 200) {
                if (!dq9::workflow_test::push_key(SDL_SCANCODE_SPACE))
                    workflow_fail("could not push the pause key event");
                else workflow_phase = WorkflowPhase::WaitPaused;
            }
            break;
        case WorkflowPhase::WaitPaused:
            if (paused) {
                if (annotation_open) { workflow_fail("Space pause unexpectedly opened annotation"); break; }
                workflow_pause_frame = static_cast<std::uint64_t>(frame);
                workflow_pause_ticks = machine.ticks();
                const auto diagnostics = workflow_diagnostics();
                workflow_audio_blocks = diagnostics.queued_audio_blocks;
                if (frontend->audio_backlog() != 0) { workflow_fail("audio remained queued after pausing"); break; }
                if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_F10))
                    workflow_fail("could not push F10 to open the debug menu");
                else workflow_phase = WorkflowPhase::WaitMenuOpen;
            }
            break;
        case WorkflowPhase::WaitMenuOpen:
            if (!paused || !menu_open || static_cast<std::uint64_t>(frame) != workflow_pause_frame ||
                machine.ticks() != workflow_pause_ticks) {
                workflow_fail("F10 did not open the paused debug menu");
                break;
            }
            if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN) ||
                !dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN) ||
                !dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN) ||
                !dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN) ||
                !dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN) ||
                !dq9::workflow_test::push_key_tap(SDL_SCANCODE_RETURN))
                workflow_fail("could not navigate the debug menu to Add note");
            else workflow_phase = WorkflowPhase::WaitMenuDispatch;
            break;
        case WorkflowPhase::WaitMenuDispatch:
            workflow_phase = WorkflowPhase::WaitAnnotation;
            break;
        case WorkflowPhase::WaitAnnotation: {
            if (!paused || !annotation_open || static_cast<std::uint64_t>(frame) != workflow_pause_frame ||
                machine.ticks() != workflow_pause_ticks || !SDL_IsTextInputActive()) {
                workflow_fail("F9 key tap did not preserve paused state or start SDL text input");
                break;
            }
            workflow_note_input_stage = 0;
            const bool queued =
                dq9::workflow_test::push_text("ab") &&
                dq9::workflow_test::push_key_tap(SDL_SCANCODE_LEFT) &&
                dq9::workflow_test::push_text("x") &&
                dq9::workflow_test::push_key_tap(SDL_SCANCODE_END) &&
                dq9::workflow_test::push_text(" ") &&
                dq9::workflow_test::push_text("texture flickers") &&
                dq9::workflow_test::push_repeated_text_key(SDL_SCANCODE_SPACE, " ") &&
                dq9::workflow_test::push_text("after inn");
            if (!queued)
                workflow_fail("could not push ordered same-poll annotation keyboard events");
            else
                workflow_phase = WorkflowPhase::WaitText;
            break;
        }
        case WorkflowPhase::WaitText: {
            if (!paused || !annotation_open || static_cast<std::uint64_t>(frame) != workflow_pause_frame ||
                machine.ticks() != workflow_pause_ticks) {
                workflow_fail("Machine frame/ticks advanced while paused with a typed note");
                break;
            }
            const auto diagnostics = workflow_diagnostics();
            if (frontend->audio_backlog() != 0 || diagnostics.queued_audio_blocks != workflow_audio_blocks) {
                workflow_fail("audio was queued while the paused annotation was open");
                break;
            }
            bool queued = true;
            switch (workflow_note_input_stage) {
            case 0:
                if (annotation.text() != "axb texture flickers  after inn") {
                    workflow_fail("typed letters/spaces did not match before Enter (got [" +
                                  annotation.text() + "])");
                    break;
                }
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_RETURN);
                break;
            case 1:
                if (annotation.text() != "axb texture flickers  after inn\n") {
                    workflow_fail("Enter did not insert a newline");
                    break;
                }
                queued = dq9::workflow_test::push_text("while pausedzq");
                break;
            case 2:
                if (annotation.text() != "axb texture flickers  after inn\nwhile pausedzq") {
                    workflow_fail("typed second-line letters did not match");
                    break;
                }
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_LEFT);
                break;
            case 3:
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_BACKSPACE);
                break;
            case 4:
                if (annotation.text() != "axb texture flickers  after inn\nwhile pausedq") {
                    workflow_fail("Backspace did not remove the character before the cursor");
                    break;
                }
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_DELETE);
                break;
            case 5:
                if (annotation.text() != workflow_note) {
                    workflow_fail("Delete did not remove the character at the cursor");
                    break;
                }
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_UP);
                break;
            case 6:
                if (annotation.text() != workflow_note) {
                    workflow_fail("Up moved or changed the note text");
                    break;
                }
                queued = dq9::workflow_test::push_key_tap(SDL_SCANCODE_DOWN);
                break;
            case 7:
                if (annotation.text() != workflow_note) {
                    workflow_fail("Down moved or changed the note text");
                    break;
                }
                queued =
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F1) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F3) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F4) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F8) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F9) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F10) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F12) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F5) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F6) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_F7) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_D) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_T) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_N,
                        static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT)) &&
                    dq9::workflow_test::push_key_tap(SDL_SCANCODE_SPACE);
                break;
            default:
                if (annotation.text() != workflow_note) {
                    workflow_fail("SDL_TEXTINPUT did not enter the exact note (got [" + annotation.text() +
                        "], expected [" + std::string(workflow_note) + "])");
                    break;
                }
                if (workflow_text_paused_loops < 8) {
                    ++workflow_text_paused_loops;
                    break;
                }
                workflow_note_frame = frame > 0 ? static_cast<std::uint64_t>(frame - 1) : 0;
                workflow_note_ticks = machine.ticks();
                workflow_top.assign(machine.gpu2d().screen_top(), machine.gpu2d().screen_top() + 256 * 192);
                workflow_bottom.assign(machine.gpu2d().screen_bottom(), machine.gpu2d().screen_bottom() + 256 * 192);
                if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_RETURN, KMOD_CTRL))
                    workflow_fail("could not push Ctrl+Enter to save the annotation");
                else workflow_phase = WorkflowPhase::WaitBundle;
                break;
            }
            if (!workflow_failed && !queued)
                workflow_fail("could not push a paused annotation keyboard event");
            else if (!workflow_failed && workflow_note_input_stage < 8)
                ++workflow_note_input_stage;
            break;
        }
        case WorkflowPhase::WaitBundle: {
            std::string error;
            if (!dq9::workflow_test::verify_latest_bundle(workflow_debug_root, workflow_note, workflow_note_frame,
                    workflow_note_ticks, workflow_top, workflow_bottom, false, error)) {
                workflow_fail("keyboard note bundle validation failed: " + error);
                break;
            }
            if (debug_status_message != "Saved bundle: " + debug_session->directory().string()) {
                workflow_fail("keyboard note capture did not report a new saved bundle");
                break;
            }
            if (SDL_IsTextInputActive()) {
                workflow_fail("SDL text input remained active after saving a note");
                break;
            }
            if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_N,
                    static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT)))
                workflow_fail("could not push the Ctrl+Shift+N note shortcut");
            else workflow_phase = WorkflowPhase::WaitFallbackAnnotation;
            break;
        }
        case WorkflowPhase::WaitFallbackAnnotation:
            if (!paused || !annotation_open || !SDL_IsTextInputActive()) {
                workflow_fail("Ctrl+Shift+N did not open the note and enable SDL text input");
                break;
            }
            workflow_fallback_note_stage = 0;
            if (!dq9::workflow_test::push_key_event_one(SDL_SCANCODE_Z, SDL_KEYDOWN))
                workflow_fail("could not queue one-event-per-poll text keydown");
            else workflow_phase = WorkflowPhase::WaitFallbackText;
            break;
        case WorkflowPhase::WaitFallbackText:
            if (workflow_fallback_note_stage == 0) {
                if (!dq9::workflow_test::push_text_event_one("z"))
                    workflow_fail("could not queue one-event-per-poll SDL_TEXTINPUT");
                else ++workflow_fallback_note_stage;
            } else if (workflow_fallback_note_stage == 1) {
                if (!dq9::workflow_test::push_key_event_one(SDL_SCANCODE_Z, SDL_KEYUP))
                    workflow_fail("could not queue one-event-per-poll text keyup");
                else ++workflow_fallback_note_stage;
            } else if (!paused || !annotation_open || annotation.text() != "z" ||
                       static_cast<std::uint64_t>(frame) != workflow_pause_frame ||
                       machine.ticks() != workflow_pause_ticks) {
                workflow_fail("one-event-per-poll KEYDOWN/TEXTINPUT/KEYUP was reordered or advanced the machine");
            } else if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_ESCAPE))
                workflow_fail("could not push Escape to cancel the fallback note");
            else workflow_phase = WorkflowPhase::WaitFallbackCancelled;
            break;
        case WorkflowPhase::WaitFallbackCancelled:
            if (!paused || annotation_open || SDL_IsTextInputActive()) {
                workflow_fail("Escape did not close the fallback note and stop SDL text input");
                break;
            }
            if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_PAUSE))
                workflow_fail("could not push Pause/Break to resume");
            else workflow_phase = WorkflowPhase::WaitPauseFallbackResumed;
            break;
        case WorkflowPhase::WaitPauseFallbackResumed:
            if (paused) {
                workflow_fail("Pause/Break did not resume the paused game");
                break;
            }
            if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_PAUSE))
                workflow_fail("could not push Pause/Break to pause again");
            else workflow_phase = WorkflowPhase::WaitPauseFallbackPaused;
            break;
        case WorkflowPhase::WaitPauseFallbackPaused:
            if (!paused) {
                workflow_fail("Pause/Break did not pause the resumed game");
                break;
            }
            workflow_step_frame = static_cast<std::uint64_t>(frame);
            workflow_step_ticks = machine.ticks();
            if (!dq9::workflow_test::push_key_tap(SDL_SCANCODE_F8))
                workflow_fail("could not push the single-step key tap");
            else workflow_phase = WorkflowPhase::WaitStep;
            break;
        case WorkflowPhase::WaitStep:
            if (!paused || static_cast<std::uint64_t>(frame) != workflow_step_frame + 1 ||
                machine.ticks() <= workflow_step_ticks) {
                workflow_fail("F8 did not advance exactly one emulated frame while remaining paused");
                break;
            }
            workflow_step_ticks = machine.ticks();
            if (!dq9::workflow_test::push_key(SDL_SCANCODE_SPACE))
                workflow_fail("could not push the resume key event");
            else workflow_phase = WorkflowPhase::WaitResume;
            break;
        case WorkflowPhase::WaitResume:
            if (paused || static_cast<std::uint64_t>(frame) != workflow_step_frame + 2 ||
                machine.ticks() <= workflow_step_ticks) {
                workflow_fail("Space did not resume and advance the next emulated frame");
                break;
            }
            workflow_push_controller(SDL_CONTROLLER_BUTTON_GUIDE, true);
            if (!workflow_failed) workflow_push_controller(SDL_CONTROLLER_BUTTON_Y, true);
            if (!workflow_failed) workflow_phase = WorkflowPhase::WaitControllerAnnotation;
            break;
        case WorkflowPhase::WaitControllerAnnotation:
            if (!paused || !annotation_open || !debug_visible) {
                workflow_fail("Guide+Y controller chord did not open the debug annotation");
                break;
            }
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_Y, false);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_GUIDE, false);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_GUIDE, true);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_GUIDE, false);
            // Move to H (index 40) and I (index 41) using only repeated D-pad Down steps
            // (each step advances 12 cells modulo the 95-character grid).
            for (int i = 0; i < 35; ++i) {
                controller_events.emplace_back(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
                controller_events.emplace_back(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            }
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_B, true); // DS A via the default GameController map
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_B, false);
            for (int i = 0; i < 8; ++i) {
                controller_events.emplace_back(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
                controller_events.emplace_back(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            }
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_B, true);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_B, false);
            controller_events.emplace_back(SDL_CONTROLLER_BUTTON_START, true);
            workflow_phase = WorkflowPhase::ControllerInputs;
            break;
        case WorkflowPhase::ControllerInputs:
            if (controller_event_index >= controller_events.size()) {
                workflow_fail("controller annotation save event was not delivered");
                break;
            }
            if (controller_event_index + 1 == controller_events.size()) {
                workflow_controller_frame = frame > 0 ? static_cast<std::uint64_t>(frame - 1) : 0;
                workflow_controller_ticks = machine.ticks();
                workflow_top.assign(machine.gpu2d().screen_top(), machine.gpu2d().screen_top() + 256 * 192);
                workflow_bottom.assign(machine.gpu2d().screen_bottom(), machine.gpu2d().screen_bottom() + 256 * 192);
            }
            workflow_push_controller(controller_events[controller_event_index].first,
                                     controller_events[controller_event_index].second);
            if (!workflow_failed && ++controller_event_index == controller_events.size())
                workflow_phase = WorkflowPhase::WaitControllerBundle;
            break;
        case WorkflowPhase::WaitControllerBundle: {
            std::string error;
            if (!dq9::workflow_test::verify_latest_bundle(workflow_debug_root, controller_note,
                    workflow_controller_frame, workflow_controller_ticks, workflow_top, workflow_bottom, true, error)) {
                workflow_fail("controller keyboard bundle validation failed: " + error);
                break;
            }
            if (debug_status_message != "Saved bundle: " + debug_session->directory().string()) {
                workflow_fail("controller note capture did not report a new saved bundle");
                break;
            }
            workflow_phase = WorkflowPhase::Done;
            std::fprintf(stderr, "[debug-workflow-selftest] PASS: replay handoff, live controller input, pause/freeze/audio, realistic annotation text/key events, repeat/Backspace/cursor, input isolation, Ctrl+Enter save, text-input lifecycle, Escape cancel, Pause/Break, F8 step, resume, and controller keyboard\n");
            break;
        }
        case WorkflowPhase::Done:
            break;
        }
    };
    while (opt.frames < 0 || frame < opt.frames) {
        const bool replay_seeking = opt.pause_at_frame >= 0 && frame < opt.pause_at_frame;
        if (opt.debug_workflow_selftest) {
            if (workflow_phase == WorkflowPhase::Done) break;
            workflow_drive_events();
            if (workflow_failed) break;
        }
        dsr::InputState in;
        if (frontend) {
            frontend->poll(frontend_input);
            if (opt.debug_workflow_selftest && annotation_open &&
                (frontend_input.debug_actions != 0 || frontend_input.buttons != 0 ||
                 frontend_input.pen_down))
                workflow_fail("keyboard/gameplay input leaked during the paused annotation");
            if (frontend_input.quit_requested) break;
            in = frontend_input;
        }
        handle_debug_input(frontend_input, frame - 1);
        if (frontend) {
            // The held key / Select+R and the screenshot hotkeys work without the debug tool.
            if (held_fast_forward_was_active && !frontend_input.fast_forward) frontend->clear_audio_queue();
            held_fast_forward_was_active = frontend_input.fast_forward;
            if (frontend_input.screenshot_requested) {
                const auto saved = dq9::save_screenshot(screenshot_dir, machine);
                if (saved.empty()) std::fprintf(stderr, "Screenshot failed: %s\n", screenshot_dir.string().c_str());
                else {
                    std::fprintf(stderr, "Screenshot saved: %s\n", saved.string().c_str());
                    frontend->show_message("SCREENSHOT SAVED: " + saved.filename().string());
                }
            }
        }
        if (annotation_open) step_requested = false;
        // Optional: idle while the window is unfocused (never during a --pause-at-frame replay).
        const bool focus_paused = frontend && !replay_seeking && frontend_input.window_unfocused;
        if (focus_paused && !focus_pause_was_active) frontend->clear_audio_queue();
        focus_pause_was_active = focus_paused;
        if ((paused && !step_requested) || focus_paused) {
            update_overlay(frame > 0 ? static_cast<std::uint64_t>(frame - 1) : 0);
            if (frontend) {
                present_frame();
                frontend->pace_frame();
            }
            continue;
        }
        if (!script.empty() && (opt.pause_at_frame < 0 || frame <= opt.pause_at_frame)) {
            in = {};
            if (const auto it = script.find(frame); it != script.end()) {
                in.buttons = it->second.buttons;
                in.pen_down = it->second.x >= 0;
                in.pen_x = it->second.x;
                in.pen_y = it->second.y;
            }
        }
        in.debug_actions = 0;
        in.editor_events.clear();
        in.menu_actions.clear();
        in.editor_buttons = 0;
        if (annotation_open) {
            in.buttons = 0;
            in.pen_down = false;
        }
        if (record_out.is_open() && (in.buttons != 0 || in.pen_down)) {
            record_out << frame << ' ' << static_cast<unsigned>(in.buttons) << ' ' << (in.pen_down ? in.pen_x : -1)
                       << ' ' << (in.pen_down ? in.pen_y : -1) << std::endl;
        }
        const auto frame_start = opt.debug_tool ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Between frames, on the machine thread: follow the scale the frontend (hotkey/config)
        // or --gpu3d-scale asks for. 1 keeps every native path untouched.
        {
            const int wanted = frontend ? frontend->requested_gpu3d_scale() : opt.gpu3d_scale;
            if (wanted != machine.gpu3d().output_scale()) machine.gpu3d().set_output_scale(wanted);
        }
        session.step(in);
        if (autosave_every > 0 && (frame + 1) % autosave_every == 0) session.flush_save();
        if (opt.debug_workflow_selftest && workflow_expect_live_input) {
            if ((machine.keyinput() & dsr::kBtnA) != 0)
                workflow_fail("scripted input replaced the live DS A press after the replay target");
            workflow_expect_live_input = false;
        }
        const double frame_ms = opt.debug_tool
            ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count() : 0.0;
        if (debug_session) {
            debug_session->record_frame(static_cast<std::uint64_t>(frame), in, frame_ms);
            frames_recorded = static_cast<std::uint64_t>(frame + 1);
        }
        step_requested = false;
        frames_done = frame + 1;
        if (!opt.record_input.empty() && (frame % 600 == 0)) {
            std::ofstream(dsr::host::path_with_suffix(opt.record_input, ".frames")) << frame + 1 << '\n';
        }
        if (opt.debug_every > 0 && frame % opt.debug_every == 0) {
            std::fprintf(stderr, "[frame %" PRId64 "] %s\n", frame, machine.debug_status().c_str());
        }

        const std::size_t samples = session.audio_frames();
        const std::int16_t* audio = session.audio_data();
        if (!opt.dump_audio.empty()) {
            audio_capture.insert(audio_capture.end(), audio, audio + samples * 2);
        }
        if (debug_session) {
            debug_session->record_audio(audio, samples);
            if (!opt.debug_selftest && !opt.debug_workflow_selftest) {
            std::uint64_t clipped = 0;
            for (std::size_t i = 0; i < samples * 2; ++i)
                clipped += (audio[i] == 32767 || audio[i] == -32768) ? 1u : 0u;
            if (samples && clipped * 100 > samples * 2) clipped_audio_seconds += samples / (33'513'982.0 / dsr::Spu::kTicksPerSample);
            else clipped_audio_seconds = 0.0;
            slow_frame_streak = frame_ms > 33.4 ? slow_frame_streak + 1 : 0;
            const std::size_t unimplemented_now = machine.unimplemented_io_count();
            if (unimplemented_now > previous_unimplemented_io && !unimplemented_capture_done) {
                unimplemented_capture_done = true;
                auto_capture_latched = true;
                capture_issue("[auto] unimplemented IO access observed", static_cast<std::uint64_t>(frame));
            }
            previous_unimplemented_io = unimplemented_now;
            if (slow_frame_streak >= 6 && !auto_capture_latched) {
                auto_capture_latched = true;
                capture_issue("[auto] frame time exceeded 33.4 ms for six consecutive frames",
                              static_cast<std::uint64_t>(frame));
            }
            if (clipped_audio_seconds >= 0.1 && !auto_capture_latched) {
                auto_capture_latched = true;
                capture_issue("[auto] audio clipping exceeded one percent for at least 100 ms",
                              static_cast<std::uint64_t>(frame));
            }
            if (slow_frame_streak == 0 && clipped_audio_seconds == 0.0) auto_capture_latched = false;
            }
        }
        if (opt.pause_at_frame == frame) paused = true;
        update_overlay(static_cast<std::uint64_t>(frame));
        if (frontend && !replay_seeking) {
            present_frame();
            if (samples != 0 && !paused) {
                frontend->queue_audio(audio, samples, dsr::Session::audio_rate());
            }
            if (paused) frontend->pace_frame();
            else if (!fast_forward && !frontend_input.fast_forward) {
                frontend->pace_frame();
                if (slow_motion < 1.0) {
                    const auto extra = std::chrono::duration<double>(1.0 / 59.8261 * (1.0 / slow_motion - 1.0));
                    std::this_thread::sleep_for(extra);
                }
            }
        }
        if (debug_session) {
            const double recorded_frame_ms = opt.debug_tool
                ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count()
                : frame_ms;
            debug_session->update_last_frame_time(recorded_frame_ms);
        }
        if (hash_out.is_open()) {
            char row[64];
            std::snprintf(row, sizeof row, "%" PRId64 " %016llx %016llx\n", frame,
                          static_cast<unsigned long long>(hash_screen(machine.gpu2d().screen_top())),
                          static_cast<unsigned long long>(hash_screen(machine.gpu2d().screen_bottom())));
            hash_out << row;
        }
        if (opt.dump_frames.count(frame)) {
            char name[64];
            std::snprintf(name, sizeof name, "frame_%04" PRId64 "_top.ppm", frame);
            write_ppm(opt.dump_dir / name, machine.gpu2d().screen_top());
            std::snprintf(name, sizeof name, "frame_%04" PRId64 "_bottom.ppm", frame);
            write_ppm(opt.dump_dir / name, machine.gpu2d().screen_bottom());
            if (opt.gpu3d_scale > 1) {
                // Top screen at hi-res: the composed 3D overlay when engine A drives it, else a
                // nearest-neighbour upscale of the native frame.
                const int scale = opt.gpu3d_scale;
                const int w = 256 * scale, h = 192 * scale;
                hires_screen.resize(static_cast<std::size_t>(w) * h);
                bool on_top = true;
                if (!session.compose_hires(hires_screen.data(), scale, &on_top) || !on_top) {
                    const std::uint32_t* native = machine.gpu2d().screen_top();
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            hires_screen[static_cast<std::size_t>(y) * w + x] = native[(y / scale) * 256 + x / scale];
                }
                std::snprintf(name, sizeof name, "frame_%04" PRId64 "_top_hires.ppm", frame);
                write_ppm(opt.dump_dir / name, hires_screen.data(), w, h);
            }
            // I/O registers for oracle comparison (read at the frame boundary).
            std::snprintf(name, sizeof name, "frame_%04" PRId64 "_regs.txt", frame);
#if defined(_WIN32)
            std::FILE* regs = _wfopen((opt.dump_dir / name).c_str(), L"w");
#else
            std::FILE* regs = std::fopen((opt.dump_dir / name).c_str(), "w");
#endif
            if (regs) {
                struct Window {
                    std::uint32_t first;
                    std::uint32_t last; // inclusive
                };
                static constexpr Window kWindows[] = {
                    {0x04000000, 0x0400006c}, // engine A display registers
                    {0x04000100, 0x0400010c}, // timers 0-3
                    {0x04000130, 0x04000136}, // key and touch input
                    {0x04000208, 0x04000214}, // IME, IE, IF
                    {0x04000240, 0x04000248}, // VRAMCNT, WRAMCNT
                    {0x04000304, 0x04000304}, // POWCNT1
                    {0x04000350, 0x04000350}, // CLEAR_COLOR
                    {0x040004a0, 0x040004ac}, // current GX polygon/texture/viewport state
                    {0x04000600, 0x04000600}, // GXSTAT
                    {0x04000620, 0x040006a0}, // 3D matrix result registers
                    {0x04001000, 0x0400106c}, // engine B display registers
                };
                for (const Window& window : kWindows) {
                    for (std::uint32_t reg = window.first; reg <= window.last; reg += 4) {
                        std::fprintf(regs, "%08x %08x\n", reg, machine.read32(dsr::CpuId::Arm9, reg));
                    }
                }
                std::fclose(regs);
            }
            if (opt.dump_ram) {
                // Logical main RAM, same byte order as the oracle's frame_NNNN_mainram.bin
                // (the oracle file additionally mirrors it up to 16 MiB).
                std::snprintf(name, sizeof name, "frame_%04" PRId64 "_mainram.bin", frame);
                dump_guest_regions(opt.dump_dir / name, machine, {{0x02000000u, 4u << 20}});
                // Palettes, OAM and the display-visible VRAM windows (main BG, sub BG,
                // main OBJ, sub OBJ), concatenated in that order.
                std::snprintf(name, sizeof name, "frame_%04" PRId64 "_video.bin", frame);
                dump_guest_regions(opt.dump_dir / name,
                                   machine,
                                   {{0x05000000u, 0x800u},
                                    {0x07000000u, 0x800u},
                                    {0x06000000u, 0x80000u},
                                    {0x06200000u, 0x20000u},
                                    {0x06400000u, 0x40000u},
                                    {0x06600000u, 0x20000u}});
            }
            if (opt.dump_vram) {
                std::snprintf(name, sizeof name, "frame_%04" PRId64 "_vram.bin", frame);
                dump_texture_vram(opt.dump_dir / name, machine.gpu2d());
            }
        }
        if (opt.pause_at_frame == frame) {
            std::fprintf(stderr, "[pause-at-frame] frame=%" PRId64 " ticks=%llu\n", frame,
                         static_cast<unsigned long long>(machine.ticks()));
        }
        if (opt.headless && opt.pause_at_frame == frame) break;
        ++frame;
    }
    if (opt.debug_workflow_selftest && !workflow_failed && workflow_phase != WorkflowPhase::Done)
        workflow_fail("workflow exited before all scripted checks completed");
    if (opt.debug_selftest) {
        if (frames_recorded == 0 || !capture_issue("[selftest] debug bundle writer smoke capture",
                                                    frames_recorded - 1)) {
            std::fprintf(stderr, "dq9: debug self-test did not produce a bundle\n");
            finished = true;
            return 1;
        }
        std::fprintf(stderr, "[debug-selftest] bundle session: %s\n",
                     debug_session->directory().string().c_str());
    }
    finished = true;
    if (!opt.record_input.empty()) {
        // Total emulated frames, so idle frames after the last input survive replay (`--frames N`).
        std::ofstream(dsr::host::path_with_suffix(opt.record_input, ".frames")) << frames_done.load() << '\n';
    }
    if (!opt.dump_audio.empty()) {
        write_wav(opt.dump_audio, audio_capture, 33'513'982.0 / dsr::Spu::kTicksPerSample);
    }
    if (watchdog.joinable()) watchdog.join();
    session.flush_save();
    if (opt.debug_workflow_selftest) {
        dq9::workflow_test::close_virtual_controller(workflow_controller);
        frontend.reset();
        std::fprintf(stderr, "[debug-workflow-selftest] session: %s\n", debug_session->directory().string().c_str());
        if (workflow_failed) return 1;
    }
    return 0;
}
