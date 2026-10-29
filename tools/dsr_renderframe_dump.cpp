#include "dsr/render_frame.h"
#include "dsr/session.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace {
struct ScriptInput {
    std::uint16_t buttons = 0;
    int x = -1;
    int y = -1;
};

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save;
    std::filesystem::path script;
    std::uint64_t frames = 0;
    std::uint8_t language = 1;
    std::optional<std::int64_t> rtc_epoch;
    std::optional<int> rtc_timezone_offset_minutes;
    bool widescreen_camera = false;
};

void usage() {
    std::cerr << "usage: dsr_renderframe_dump --rom ROM --save SAVE --input-script FILE --frames N [--language N] "
                 "[--widescreen-camera 0|1] [--rtc-epoch seconds] [--rtc-timezone-offset minutes]\n";
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return false;
        const std::string value = argv[++i];
        if (arg == "--rom") options.rom = value;
        else if (arg == "--save") options.save = value;
        else if (arg == "--input-script") options.script = value;
        else if (arg == "--frames") {
            try {
                std::size_t used = 0;
                const auto n = std::stoull(value, &used);
                if (used != value.size()) return false;
                options.frames = n;
            } catch (...) { return false; }
        } else if (arg == "--language") {
            try {
                std::size_t used = 0;
                const auto n = std::stoul(value, &used);
                if (used != value.size() || n > 5) return false;
                options.language = static_cast<std::uint8_t>(n);
            } catch (...) { return false; }
        } else if (arg == "--widescreen-camera") {
            if (value != "0" && value != "1") return false;
            options.widescreen_camera = value == "1";
        } else if (arg == "--rtc-epoch") {
            try {
                std::size_t used = 0;
                const auto n = std::stoll(value, &used);
                if (used != value.size()) return false;
                options.rtc_epoch = n;
            } catch (...) { return false; }
        } else if (arg == "--rtc-timezone-offset") {
            try {
                std::size_t used = 0;
                const auto n = std::stol(value, &used);
                if (used != value.size() || n < -840 || n > 840) return false;
                options.rtc_timezone_offset_minutes = static_cast<int>(n);
            } catch (...) { return false; }
        } else return false;
    }
    return !options.rom.empty() && !options.save.empty() && !options.script.empty() && options.frames > 0;
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

bool load_script(const std::filesystem::path& path, std::map<std::uint64_t, ScriptInput>& script) {
    std::ifstream in(path);
    if (!in) return false;
    std::uint64_t frame = 0;
    unsigned mask = 0;
    int x = -1, y = -1;
    while (in >> frame >> mask >> x >> y) {
        if (mask > 0xffffU) return false;
        script[frame] = {static_cast<std::uint16_t>(mask), x, y};
    }
    return in.eof() && !in.bad();
}

class SaveCopy {
public:
    explicit SaveCopy(const std::filesystem::path& source) {
        std::error_code ec;
        path_ = std::filesystem::temp_directory_path(ec) /
            ("dsr-renderframe-save-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".sav");
        if (ec) return;
        std::filesystem::copy_file(source, path_, ec);
        valid_ = !ec;
    }
    ~SaveCopy() {
        if (valid_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    bool valid() const { return valid_; }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
    bool valid_ = false;
};

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        usage();
        return 2;
    }
    std::map<std::uint64_t, ScriptInput> script;
    if (!load_script(options.script, script)) {
        std::cerr << "dsr_renderframe_dump: cannot read or parse input script\n";
        return 2;
    }
    SaveCopy save_copy(options.save);
    if (!save_copy.valid()) {
        std::cerr << "dsr_renderframe_dump: cannot copy save file\n";
        return 2;
    }

    dsr::SessionConfig config;
    config.rom_path = options.rom;
    config.save_path = save_copy.path();
    config.language = options.language;
    config.widescreen_camera = options.widescreen_camera;
    config.deterministic_rtc = options.rtc_epoch.has_value() || options.rtc_timezone_offset_minutes.has_value();
    const bool needs_launch_clock = config.deterministic_rtc &&
        (!options.rtc_epoch.has_value() || !options.rtc_timezone_offset_minutes.has_value());
    const std::time_t launch_wall_time = needs_launch_clock ? std::time(nullptr) : 0;
    config.rtc_epoch_seconds = options.rtc_epoch.value_or(static_cast<std::int64_t>(launch_wall_time));
    config.rtc_timezone_offset_minutes = options.rtc_timezone_offset_minutes.value_or(
        needs_launch_clock ? local_timezone_offset_minutes(launch_wall_time) : 0);
    dsr::Session session(config);

    std::chrono::nanoseconds step_total{};
    std::chrono::nanoseconds capture_total{};
    std::uint64_t vertex_overflow_frames = 0, polygon_overflow_frames = 0;
    std::uint64_t vertex_capacity_frames = 0, polygon_capacity_frames = 0;
    dsr::RenderFrame frame{};
    for (std::uint64_t index = 0; index < options.frames; ++index) {
        dsr::InputState input{};
        if (const auto found = script.find(index); found != script.end()) {
            input.buttons = found->second.buttons;
            input.pen_down = found->second.x >= 0;
            input.pen_x = found->second.x;
            input.pen_y = found->second.y;
        }
        const auto step_begin = std::chrono::steady_clock::now();
        session.step(input);
        const auto step_end = std::chrono::steady_clock::now();
        step_total += step_end - step_begin;
        const auto capture_begin = std::chrono::steady_clock::now();
        dsr::capture_render_frame(session.machine(), index, frame);
        const auto capture_end = std::chrono::steady_clock::now();
        capture_total += capture_end - capture_begin;

        const auto& g = frame.geometry;
        vertex_overflow_frames += g.vertex_overflow;
        polygon_overflow_frames += g.polygon_overflow;
        vertex_capacity_frames += g.vertex_count >= 12288U;
        polygon_capacity_frames += g.polygon_count >= 12288U;
        const auto& r = frame.raster;
        const auto& v = frame.gpu2d;
        std::cout << "{\"frame\":" << frame.frame_number
                  << ",\"ticks\":" << frame.ticks
                  << ",\"geometry_generation\":" << g.generation
                  << ",\"vertices\":" << g.vertex_count
                  << ",\"polygons\":" << g.polygon_count
                  << ",\"vertex_overflow\":" << (g.vertex_overflow ? "true" : "false")
                  << ",\"polygon_overflow\":" << (g.polygon_overflow ? "true" : "false")
                  << ",\"swap_mode\":" << g.swap_mode
                  << ",\"viewport\":" << r.viewport
                  << ",\"disp3d\":" << r.disp3d
                  << ",\"texture_mapping_generation\":" << frame.texture.mapping_generation
                  << ",\"vram_mapping_generation\":" << v.mapping_generation
                  << ",\"mapped_texture_slots\":[";
        for (std::size_t slot = 0; slot < frame.texture.textures.size(); ++slot) {
            if (slot) std::cout << ',';
            std::cout << (frame.texture.textures[slot].mapped ? "true" : "false");
        }
        std::cout << "],\"mapped_palette_slots\":[";
        for (std::size_t slot = 0; slot < frame.texture.palettes.size(); ++slot) {
            if (slot) std::cout << ',';
            std::cout << (frame.texture.palettes[slot].mapped ? "true" : "false");
        }
        std::cout << "],\"powcnt1\":" << v.powcnt1
                  << ",\"top_pixel0\":" << frame.screen_top[0]
                  << ",\"bottom_pixel0\":" << frame.screen_bottom[0]
                  << "}\n";
        if (!std::cout) {
            std::cerr << "dsr_renderframe_dump: output write failed\n";
            return 1;
        }
    }
    session.flush_save();
    const auto step_ms = std::chrono::duration<double, std::milli>(step_total).count();
    const auto capture_ms = std::chrono::duration<double, std::milli>(capture_total).count();
    std::cerr << "timing frames=" << options.frames << " run_frame_ms=" << std::fixed
              << std::setprecision(3) << step_ms << " capture_ms=" << capture_ms << '\n';
    std::cerr << "geometry frames=" << options.frames
              << " vertex_overflow_frames=" << vertex_overflow_frames
              << " polygon_overflow_frames=" << polygon_overflow_frames
              << " vertex_capacity_frames=" << vertex_capacity_frames
              << " polygon_capacity_frames=" << polygon_capacity_frames << '\n';
    return 0;
}
