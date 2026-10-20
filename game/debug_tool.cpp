#include "debug_tool.h"
#include "dsr/host_path.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <string_view>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"
#include "dsr/spu.h"
#include "dsr/periph.h"

#ifndef DQ9_BUILD_ID
#define DQ9_BUILD_ID "unknown"
#endif

namespace dq9 {
namespace {
constexpr std::uint32_t kScreenWidth = 256;
constexpr std::uint32_t kScreenHeight = 192;
constexpr double kSpuRate = 33513982.0 / 1024.0;

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string slug(std::string_view comment) {
    std::string out;
    bool separator = false;
    for (unsigned char c : comment) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (separator && !out.empty() && out.size() < 48) out.push_back('-');
            separator = false;
            if (out.size() < 48) out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            separator = true;
        }
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "issue" : out;
}

std::string shell_quote(const std::string& value) {
    std::string result = "'";
    for (const char c : value) {
        if (c == '\'') result += "'\\''";
        else result.push_back(c);
    }
    result.push_back('\'');
    return result;
}

#if defined(_WIN32)
std::string powershell_quote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        result.push_back(c);
        if (c == '\'') result.push_back('\'');
    }
    result.push_back('\'');
    return result;
}
#endif

void put_u32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                           static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes, 4);
}
void put_u16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value), static_cast<char>(value >> 8)};
    out.write(bytes, 2);
}

std::uint32_t crc32_bytes(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void png_chunk(std::vector<std::uint8_t>& png, const char tag[4], const std::vector<std::uint8_t>& data) {
    append_be32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t crc_start = png.size();
    png.insert(png.end(), tag, tag + 4);
    png.insert(png.end(), data.begin(), data.end());
    append_be32(png, crc32_bytes(png.data() + crc_start, png.size() - crc_start));
}

bool write_png_rgb(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                   const std::vector<std::uint32_t>& pixels) {
    if (pixels.size() != static_cast<std::size_t>(width) * height || width == 0 || height == 0) return false;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 3));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // PNG filter: None
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t pixel = pixels[static_cast<std::size_t>(y) * width + x];
            raw.push_back(static_cast<std::uint8_t>(pixel >> 16));
            raw.push_back(static_cast<std::uint8_t>(pixel >> 8));
            raw.push_back(static_cast<std::uint8_t>(pixel));
        }
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t count = std::min<std::size_t>(65535, raw.size() - offset);
        const bool final = offset + count == raw.size();
        zlib.push_back(final ? 1 : 0); // stored deflate block, byte-aligned
        const auto len = static_cast<std::uint16_t>(count);
        const auto nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<std::uint8_t>(len));
        zlib.push_back(static_cast<std::uint8_t>(len >> 8));
        zlib.push_back(static_cast<std::uint8_t>(nlen));
        zlib.push_back(static_cast<std::uint8_t>(nlen >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    append_be32(zlib, (b << 16) | a);
    std::vector<std::uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<std::uint8_t> ihdr;
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // RGB, 8-bit
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", zlib);
    png_chunk(png, "IEND", {});
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

void write_ppm_rgb(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                   const std::vector<std::uint32_t>& pixels) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint32_t pixel : pixels) {
        const char rgb[3] = {static_cast<char>(pixel >> 16), static_cast<char>(pixel >> 8),
                             static_cast<char>(pixel)};
        out.write(rgb, 3);
    }
}

std::vector<std::uint32_t> screen_pixels(const std::uint32_t* source, std::size_t count) {
    return std::vector<std::uint32_t>(source, source + count);
}

void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm) {
    std::ofstream out(path, std::ios::binary);
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    out.write("RIFF", 4);
    put_u32(out, 36 + bytes);
    out.write("WAVEfmt ", 8);
    put_u32(out, 16);
    put_u16(out, 1);
    put_u16(out, 2);
    put_u32(out, static_cast<std::uint32_t>(kSpuRate + 0.5));
    put_u32(out, static_cast<std::uint32_t>(kSpuRate + 0.5) * 4);
    put_u16(out, 4);
    put_u16(out, 16);
    out.write("data", 4);
    put_u32(out, bytes);
    for (const std::int16_t sample : pcm) put_u16(out, static_cast<std::uint16_t>(sample));
}

void write_screen(const std::filesystem::path& directory, const char* base,
                  const std::uint32_t* pixels) {
    std::vector<std::uint32_t> image = screen_pixels(pixels, kScreenWidth * kScreenHeight);
    write_png_rgb(directory / (std::string(base) + ".png"), kScreenWidth, kScreenHeight, image);
    write_ppm_rgb(directory / (std::string(base) + ".ppm"), kScreenWidth, kScreenHeight, image);
}

void write_guest_regions(const std::filesystem::path& path, dsr::Machine& machine,
                         std::uint32_t address, std::uint32_t size) {
    std::ofstream out(path, std::ios::binary);
    std::array<char, 4> bytes{};
    for (std::uint32_t offset = 0; offset < size; offset += 4) {
        const std::uint32_t value = machine.read32(dsr::CpuId::Arm9, address + offset);
        bytes = {static_cast<char>(value), static_cast<char>(value >> 8),
                 static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        out.write(bytes.data(), static_cast<std::streamsize>(std::min<std::uint32_t>(4, size - offset)));
    }
}

void write_vram(const std::filesystem::path& path, const dsr::Gpu2D& gpu2d) {
    std::ofstream out(path, std::ios::binary);
    const std::array<std::uint8_t, 0x20000> zero{};
    for (int i = 0; i < 4; ++i) {
        const auto* slot = gpu2d.texture_slot(i);
        out.write(reinterpret_cast<const char*>(slot ? slot : zero.data()), 0x20000);
    }
    for (int i = 0; i < 6; ++i) {
        const auto* slot = gpu2d.texpal_slot(i);
        out.write(reinterpret_cast<const char*>(slot ? slot : zero.data()), 0x4000);
    }
}

std::string hex32(std::uint32_t v) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return out.str();
}

std::string file_hash(const std::filesystem::path& path, std::uintmax_t& size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { size = 0; return "unavailable"; }
    std::uint64_t hash = 0xcbf29ce484222325ull;
    size = 0;
    std::array<char, 8192> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        size += static_cast<std::uintmax_t>(count);
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= 0x100000001b3ull;
        }
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
}

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t size) {
        total_bytes_ += size;
        while (size != 0) {
            const std::size_t amount = std::min(size, block_.size() - buffered_);
            std::copy_n(data, amount, block_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += amount;
            data += amount;
            size -= amount;
            if (buffered_ == block_.size()) {
                transform();
                buffered_ = 0;
            }
        }
    }

    std::string finish() {
        const std::uint64_t bit_count = total_bytes_ * 8;
        block_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_), block_.end(), 0);
            transform();
            buffered_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_), block_.begin() + 56, 0);
        for (unsigned i = 0; i < 8; ++i)
            block_[63 - i] = static_cast<std::uint8_t>(bit_count >> (8 * i));
        transform();
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto word : state_) out << std::setw(8) << word;
        return out.str();
    }

private:
    void transform() {
        static constexpr std::array<std::uint32_t, 64> constants{{
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        }};
        std::array<std::uint32_t, 64> words{};
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned at = i * 4;
            words[i] = (static_cast<std::uint32_t>(block_[at]) << 24) |
                       (static_cast<std::uint32_t>(block_[at + 1]) << 16) |
                       (static_cast<std::uint32_t>(block_[at + 2]) << 8) | block_[at + 3];
        }
        for (unsigned i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 = rotate_right(words[i - 15], 7) ^
                                     rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(words[i - 2], 17) ^
                                     rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        std::uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        std::uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (unsigned i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
            const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    }};
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buffered_ = 0;
};

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 digest;
    std::array<char, 8192> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count > 0)
            digest.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                          static_cast<std::size_t>(count));
    }
    return in.eof() ? digest.finish() : std::string{};
}


void write_debug_cpu(std::ostream& out, const char* name, const dsr::CpuDebugSnapshot& cpu) {
    out << name << " pc=" << hex32(cpu.current_pc) << " entry=" << hex32(cpu.entry_pc)
        << " lr=" << hex32(cpu.registers[14]) << " sp=" << hex32(cpu.registers[13])
        << " cpsr=" << hex32(cpu.cpsr_control) << '\n';
    for (unsigned i = 0; i < 16; ++i) out << "  r" << i << '=' << hex32(cpu.registers[i]) << (i % 4 == 3 ? '\n' : ' ');
}

void append_shader_texture(const std::filesystem::path& dir, const dsr::Gpu2D& gpu2d) {
    for (int slot = 0; slot < 4; ++slot) {
        const auto* data = gpu2d.texture_slot(slot);
        constexpr std::size_t width = 256;
        constexpr std::size_t height = 512;
        constexpr std::size_t slot_bytes = 0x20000;
        std::vector<std::uint32_t> pixels(width * height, 0xff000000u);
        const std::size_t count = std::min(slot_bytes, pixels.size());
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t v = data ? data[i] : 0;
            pixels[i] = 0xff000000u | (static_cast<std::uint32_t>(v) << 16) |
                        (static_cast<std::uint32_t>(v) << 8) | v;
        }
        write_png_rgb(dir / ("texture_slot" + std::to_string(slot) + ".png"),
                      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), pixels);
    }
}

} // namespace

DebugSession::DebugSession(const std::filesystem::path& data_dir, const std::filesystem::path& rom,
                           const std::filesystem::path& save, std::string language,
                           std::filesystem::path executable, bool dump_memory,
                           std::int64_t rtc_epoch, std::int32_t rtc_timezone_offset_minutes,
                           std::filesystem::path abi_config, std::optional<std::string> backup_type)
    : rom_(std::filesystem::absolute(rom)), save_(std::filesystem::absolute(save)),
      abi_config_(abi_config.empty() ? std::filesystem::path{} : std::filesystem::absolute(abi_config)),
      executable_(std::filesystem::absolute(executable)), language_(std::move(language)),
      backup_type_(std::move(backup_type)),
      audio_ring_(static_cast<std::size_t>(std::ceil(kSpuRate)) * 5 * 2),
      dump_memory_default_(dump_memory), rtc_epoch_(rtc_epoch),
      rtc_timezone_offset_minutes_(rtc_timezone_offset_minutes) {
    const auto root = data_dir / "debug";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const std::string timestamp = timestamp_now();
    std::ostringstream name;
    name << "session-" << timestamp << '-' <<
#if defined(_WIN32)
        static_cast<unsigned long long>(GetCurrentProcessId());
#else
        static_cast<unsigned long long>(::getpid());
#endif
    session_dir_ = root / name.str();
    std::filesystem::create_directories(session_dir_, ec);
    bundle_root_ = root;
    session_input_path_ = session_dir_ / "session_input.txt";
    session_input_.open(session_input_path_, std::ios::binary | std::ios::trunc);
    markers_.open(session_dir_ / "markers.log", std::ios::binary | std::ios::app);
    std::uintmax_t size = 0;
    const std::string hash = file_hash(save_, size);
    std::ostringstream ref;
    ref << "path: " << dsr::host::path_to_utf8(save_) << '\n' << "size: " << size << '\n' << "hash: " << hash << '\n';
    save_reference_ = ref.str();
    const auto launch_save = session_dir_ / "session_start.sav";
    if (std::filesystem::exists(save_, ec)) std::filesystem::copy_file(save_, launch_save, std::filesystem::copy_options::overwrite_existing, ec);
    else { std::ofstream empty(launch_save, std::ios::binary); }
}

DebugSession::~DebugSession() {
    if (session_input_.is_open()) session_input_.flush();
}

void DebugSession::record_frame(std::uint64_t frame, const dsr::InputState& input, double frame_ms) {
    const std::uint16_t buttons = input.buttons;
    const int x = input.pen_down ? input.pen_x : -1;
    const int y = input.pen_down ? input.pen_y : -1;
    if (session_input_) session_input_ << frame << ' ' << buttons << ' ' << x << ' ' << y << '\n';
    input_tail_[input_pos_] = InputRecord{frame, input};
    input_pos_ = (input_pos_ + 1) % input_tail_.size();
    input_count_ = std::min(input_count_ + 1, input_tail_.size());
    frame_times_[frame_time_pos_] = static_cast<float>(frame_ms);
    frame_time_pos_ = (frame_time_pos_ + 1) % frame_times_.size();
    frame_time_count_ = std::min(frame_time_count_ + 1, frame_times_.size());
    frames_recorded_ = std::max(frames_recorded_, frame + 1);
}

void DebugSession::update_last_frame_time(double frame_ms) {
    if (frame_time_count_ == 0) return;
    const std::size_t last = (frame_time_pos_ + frame_times_.size() - 1) % frame_times_.size();
    frame_times_[last] = static_cast<float>(frame_ms);
}

void DebugSession::record_audio(const std::int16_t* pcm, std::size_t frames) {
    const std::size_t capacity = audio_ring_.size() / 2;
    for (std::size_t i = 0; i < frames; ++i) {
        audio_ring_[audio_pos_ * 2] = pcm[i * 2];
        audio_ring_[audio_pos_ * 2 + 1] = pcm[i * 2 + 1];
        audio_pos_ = (audio_pos_ + 1) % capacity;
        audio_count_ = std::min(audio_count_ + 1, capacity);
    }
}

double DebugSession::fps() const {
    if (frame_time_count_ == 0) return 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < frame_time_count_; ++i) total += frame_times()[i];
    return total > 0.0 ? 1000.0 * frame_time_count_ / total : 0.0;
}

std::array<float, 120> DebugSession::frame_times() const {
    std::array<float, 120> ordered{};
    const std::size_t first = (frame_time_pos_ + frame_times_.size() - frame_time_count_) % frame_times_.size();
    for (std::size_t i = 0; i < frame_time_count_; ++i) ordered[i] = frame_times_[(first + i) % frame_times_.size()];
    return ordered;
}

bool DebugSession::capture(const DebugCaptureOptions& options, dsr::Machine& machine,
                           dsr::Frontend& frontend, const dsr::FrontendConfig& config) {
    if (session_input_) session_input_.flush();
    if (auto* card = machine.game_card(); card && !card->flush_save()) return false;
    const std::string timestamp = timestamp_now();
    std::string name = timestamp + "-" + std::to_string(++bundle_sequence_) + "-" + slug(options.comment);
    std::filesystem::path bundle = bundle_root_ / name;
    std::error_code ec;
    while (std::filesystem::exists(bundle, ec)) bundle = bundle_root_ / (name + "-" + std::to_string(++bundle_sequence_));
    std::filesystem::create_directories(bundle, ec);
    if (ec) return false;
    const auto save_at_note = bundle / "save_at_note.sav";
    std::error_code save_error;
    if (std::filesystem::exists(save_, save_error) && !save_error) {
        std::filesystem::copy_file(save_, save_at_note, std::filesystem::copy_options::overwrite_existing,
                                   save_error);
    } else if (!save_error) {
        std::ofstream empty_save(save_at_note, std::ios::binary);
        if (!empty_save) return false;
    }
    if (save_error) return false;
    const std::string save_at_note_sha256 = sha256_file(save_at_note);
    if (save_at_note_sha256.empty()) return false;

    const auto audio_state = machine.spu().debug_state();
    std::ostringstream audio_debug;
    audio_debug << "audio_debug: solo=" << audio_state.solo_channel << " muted=";
    bool any_muted = false;
    for (std::size_t i = 0; i < audio_state.channels.size(); ++i) {
        if (!audio_state.channels[i].muted) continue;
        if (any_muted) audio_debug << ',';
        audio_debug << i;
        any_muted = true;
    }
    if (!any_muted) audio_debug << "none";
    audio_debug << '\n';
    const std::uint32_t* top = machine.gpu2d().screen_top();
    const std::uint32_t* bottom = machine.gpu2d().screen_bottom();
    write_screen(bundle, "screenshot_top", top);
    write_screen(bundle, "screenshot_bottom", bottom);
    std::vector<std::uint32_t> both;
    both.reserve(kScreenWidth * kScreenHeight * 2);
    both.insert(both.end(), top, top + kScreenWidth * kScreenHeight);
    both.insert(both.end(), bottom, bottom + kScreenWidth * kScreenHeight);
    write_png_rgb(bundle / "screenshot_both.png", kScreenWidth, kScreenHeight * 2, both);
    write_ppm_rgb(bundle / "screenshot_both.ppm", kScreenWidth, kScreenHeight * 2, both);

    {
        std::ofstream comment(bundle / "comment.txt", std::ios::binary);
        comment << audio_debug.str() << options.comment << '\n';
        comment << "timestamp: " << timestamp << '\n'
                << "local_time: " << timestamp << '\n'
                << "frame: " << options.frame << '\n'
                << "emulated_ticks: " << machine.ticks() << '\n'
                << "wall_seconds_since_launch: " << std::fixed << std::setprecision(3) << options.wall_seconds << '\n'
                << "rtc_epoch: " << rtc_epoch_ << '\n'
                << "rtc_timezone_offset_minutes: " << rtc_timezone_offset_minutes_ << '\n'
                << "save_at_note: save_at_note.sav\n"
                << "save_at_note_sha256: " << save_at_note_sha256 << '\n';
    }
    {
        std::ofstream status(bundle / "state.txt", std::ios::binary);
        status << audio_debug.str() << "Machine::debug_status()\n" << machine.debug_status() << '\n';
        write_debug_cpu(status, "ARM9", machine.debug_cpu_snapshot(dsr::CpuId::Arm9));
        write_debug_cpu(status, "ARM7", machine.debug_cpu_snapshot(dsr::CpuId::Arm7));
        status << "ARM9 DISP3DCNT=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x04000060)) << '\n'
               << "DISPCNT_A=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x04000000)) << '\n'
               << "DISPCNT_B=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x04001000)) << '\n'
               << "POWCNT=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x04000304)) << '\n';
        for (unsigned bank = 0; bank < 9; ++bank) {
            status << "VRAMCNT_" << static_cast<char>('A' + bank) << "=0x" << std::hex
                   << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(machine.read8(dsr::CpuId::Arm9, 0x04000240 + bank))
                   << std::dec << std::setfill(' ') << '\n';
        }
        status << "GXSTAT=" << hex32(machine.gpu3d().io_read(0x04000600, 32)) << '\n'
               << "SOUNDCNT=" << hex32(machine.read32(dsr::CpuId::Arm7, 0x04000500)) << '\n'
               << "SOUNDBIAS=" << hex32(machine.read32(dsr::CpuId::Arm7, 0x04000504)) << '\n'
               << "active_overlay_ids=unavailable: Machine exposes no active overlay query\n";
        const auto spu = machine.spu().debug_state();
        status << "SPU channels (index format volume pan busy sample_position current_sample muted)\n";
        for (unsigned i = 0; i < spu.channels.size(); ++i) {
            const auto& c = spu.channels[i];
            status << i << ' ' << c.format << ' ' << c.volume << ' ' << c.pan << ' '
                   << c.busy << ' ' << c.sample_position << ' ' << c.current_sample << ' ' << c.muted << '\n';
        }
        status << "SPU solo_channel=" << spu.solo_channel << " SOUNDCNT=" << spu.sound_control
               << " BIAS=" << spu.bias << '\n';
    }
    {
        std::ofstream texture(bundle / "texture_state.txt", std::ios::binary);
        texture << "3D texture/palette slot mappings; each texture slot is 128 KiB.\n";
        for (int i = 0; i < 4; ++i)
            texture << "texture_slot_" << i << '=' << (machine.gpu2d().texture_slot(i) ? "mapped" : "unmapped") << " size=131072\n";
        for (int i = 0; i < 6; ++i)
            texture << "palette_slot_" << i << '=' << (machine.gpu2d().texpal_slot(i) ? "mapped" : "unmapped") << " size=16384\n";
        texture << "GXSTAT=" << hex32(machine.gpu3d().io_read(0x04000600, 32)) << '\n'
                << "GX_RAM_COUNT_REGISTER=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x04000604)) << '\n'
                << "texture_params=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x040004a4)) << '\n'
                << "palette_params=" << hex32(machine.read32(dsr::CpuId::Arm9, 0x040004a8)) << '\n'
                << "polygon_list=unavailable: Gpu3D does not expose retained polygon commands or per-polygon texture counts\n";
    }
    if (options.dump_memory) {
        write_vram(bundle / "vram.bin", machine.gpu2d());
        write_guest_regions(bundle / "mainram.bin", machine, 0x02000000, 0x00400000);
    }
    if (options.texture_inspector) append_shader_texture(bundle, machine.gpu2d());

    std::vector<std::int16_t> audio;
    audio.reserve(audio_count_ * 2);
    const std::size_t capacity = audio_ring_.size() / 2;
    const std::size_t audio_first = (audio_pos_ + capacity - audio_count_) % capacity;
    for (std::size_t i = 0; i < audio_count_; ++i) {
        const std::size_t at = (audio_first + i) % capacity;
        audio.push_back(audio_ring_[at * 2]);
        audio.push_back(audio_ring_[at * 2 + 1]);
    }
    write_wav(bundle / "audio_tail.wav", audio);
    const std::size_t last_second_frames = std::min<std::size_t>(audio_count_, static_cast<std::size_t>(kSpuRate));
    const std::size_t sample_begin = audio.size() - last_second_frames * 2;
    double square_sum = 0.0;
    std::uint64_t clipped = 0;
    int peak = 0;
    for (std::size_t i = sample_begin; i < audio.size(); ++i) {
        const int sample = audio[i];
        peak = std::max(peak, std::abs(sample));
        square_sum += static_cast<double>(sample) * sample;
        clipped += (sample == 32767 || sample == -32768) ? 1u : 0u;
    }
    const double rms = sample_begin < audio.size() ? std::sqrt(square_sum / (audio.size() - sample_begin)) : 0.0;
    dsr::FrontendDiagnostics diagnostics;
    frontend.diagnostics(diagnostics);
    const auto times = frame_times();
    {
        std::ofstream stats(bundle / "audio_stats.txt", std::ios::binary);
        stats << std::fixed << std::setprecision(3)
              << "rms_last_1s: " << rms << '\n'
              << "peak_last_1s: " << peak << '\n'
              << "clipped_samples_last_1s: " << clipped << '\n'
              << "queue_backlog_frames: " << diagnostics.audio_backlog_frames << '\n'
              << "queue_backlog_ms: " << (config.audio_rate > 0 ? diagnostics.audio_backlog_frames * 1000.0 / config.audio_rate : 0.0) << '\n'
              << "dropped_audio_blocks: " << diagnostics.dropped_audio_blocks << '\n'
              << "empty_audio_queue_events_approx: " << diagnostics.empty_audio_queue_events << '\n'
              << "measured_emulation_fps: " << fps() << '\n'
              << "frame_time_sample_count: " << frame_time_count_ << '\n'
              << "frame_times_ms_oldest_to_newest:";
        for (std::size_t i = 0; i < frame_time_count_; ++i) stats << ' ' << times[i];
        stats << '\n';
    }
    {
        std::ofstream tail(bundle / "input_tail.txt", std::ios::binary);
        const std::size_t first = (input_pos_ + input_tail_.size() - input_count_) % input_tail_.size();
        for (std::size_t i = 0; i < input_count_; ++i) {
            const auto& record = input_tail_[(first + i) % input_tail_.size()];
            const auto& in = record.input;
            tail << record.frame << ' ' << in.buttons << ' ' << (in.pen_down ? in.pen_x : -1)
                 << ' ' << (in.pen_down ? in.pen_y : -1) << '\n';
        }
    }
    std::error_code copy_error;
    std::filesystem::copy_file(session_input_path_, bundle / "session_input.txt",
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    {
        std::ofstream count(bundle / "session_input.txt.frames", std::ios::binary);
        count << frames_recorded_ << '\n';
    }
    const auto start_save = session_dir_ / "session_start.sav";
    std::filesystem::copy_file(start_save, bundle / "session_start.sav",
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    if (copy_error) return false;
    {
        std::ofstream reference(bundle / "save_before.sav", std::ios::binary);
        reference << save_reference_;
    }
    {
        std::ofstream env(bundle / "env.txt", std::ios::binary);
        env << "build_id: " << DQ9_BUILD_ID << '\n'
            << "sdl_version: " << diagnostics.sdl_version << '\n'
            << "audio_driver: " << diagnostics.audio_driver << '\n'
            << "audio_device: " << diagnostics.audio_device << '\n'
            << "window_resolution: " << diagnostics.width << 'x' << diagnostics.height << '\n'
            << "controller_name: " << diagnostics.controller_name << '\n'
            << "controller_guid: " << diagnostics.controller_guid << '\n'
            << "rom_path: " << dsr::host::path_to_utf8(rom_) << '\n'
            << "save_path: " << dsr::host::path_to_utf8(save_) << '\n'
            << "abi_config_path: " << dsr::host::path_to_utf8(abi_config_) << '\n'
            << "backup_type: " << (backup_type_ ? backup_type_->c_str() : "auto") << '\n'
            << "language: " << options.language << '\n'
            << "rtc_epoch: " << rtc_epoch_ << '\n'
            << "rtc_timezone_offset_minutes: " << rtc_timezone_offset_minutes_ << '\n';
    }
    {
        std::ofstream replay(bundle / "replay.sh", std::ios::binary);
        replay << "#!/bin/sh\n"
               << "set -eu\n"
               << "SCRIPT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
               << "OUT=\"$SCRIPT_DIR/replay_out\"\n"
               << "mkdir -p \"$OUT\"\n"
               << "cp \"$SCRIPT_DIR/session_start.sav\" \"$OUT/replay.sav\"\n"
               << "cd \"$SCRIPT_DIR\"\n"
               << "exec " << shell_quote(dsr::host::path_to_utf8(executable_))
               << " --rom " << shell_quote(dsr::host::path_to_utf8(rom_)) << " --save 'replay_out/replay.sav'"
               << " --abi-config " << shell_quote(dsr::host::path_to_utf8(abi_config_));
        if (backup_type_) replay << " --backup-type " << shell_quote(*backup_type_);
        replay << " --language " << shell_quote(options.language)
               << " --rtc-epoch " << rtc_epoch_
               << " --rtc-timezone-offset " << rtc_timezone_offset_minutes_
               << " --headless --frames " << std::max<std::uint64_t>(1, frames_recorded_)
               << " --input-script 'session_input.txt' --dump-frames " << options.frame
               << " --dump-dir 'replay_out' --dump-audio 'replay_out/audio.wav'"
               << " --dump-ram --dump-vram\n";
    }
#if defined(_WIN32)
    {
        // Retain replay.sh for the host-independent Python bundle inspector;
        // provide a directly runnable script for native Windows as well.
        std::ofstream replay(bundle / "replay.ps1", std::ios::binary);
        replay << "\xef\xbb\xbf" // Windows PowerShell 5.1 requires a BOM for UTF-8.
               << "$ErrorActionPreference = 'Stop'\n"
               << "Set-Location -LiteralPath $PSScriptRoot\n"
               << "New-Item -ItemType Directory -Force -Path 'replay_out' | Out-Null\n"
               << "Copy-Item -LiteralPath 'session_start.sav' -Destination 'replay_out/replay.sav' -Force\n"
               << "& " << powershell_quote(dsr::host::path_to_utf8(executable_))
               << " --rom " << powershell_quote(dsr::host::path_to_utf8(rom_))
               << " --abi-config " << powershell_quote(dsr::host::path_to_utf8(abi_config_));
        if (backup_type_) replay << " --backup-type " << powershell_quote(*backup_type_);
        replay << " --save 'replay_out/replay.sav' --language " << powershell_quote(options.language)
               << " --rtc-epoch " << rtc_epoch_
               << " --rtc-timezone-offset " << rtc_timezone_offset_minutes_
               << " --headless --frames " << std::max<std::uint64_t>(1, frames_recorded_)
               << " --input-script 'session_input.txt' --dump-frames " << options.frame
               << " --dump-dir 'replay_out' --dump-audio 'replay_out/audio.wav'"
               << " --dump-ram --dump-vram\nexit $LASTEXITCODE\n";
    }
#else
    std::filesystem::permissions(bundle / "replay.sh", std::filesystem::perms::owner_exec |
                                 std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
#endif
    {
        std::ofstream index(bundle_root_ / "index.tsv", std::ios::binary | std::ios::app);
        std::string preview = options.comment.substr(0, 80);
        for (char& c : preview) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        index << timestamp << '\t' << bundle.filename().string() << '\t' << preview << '\t' << options.frame << '\n';
    }
    return true;
}

std::filesystem::path save_screenshot_pixels(const std::filesystem::path& directory,
                                             const std::vector<std::uint32_t>& pixels, std::uint32_t width,
                                             std::uint32_t height) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return {};
    const std::string stem = "dq9-" + timestamp_now();
    std::filesystem::path path = directory / (stem + ".png");
    for (int n = 1; std::filesystem::exists(path, ec) && n < 1000; ++n)
        path = directory / (stem + "-" + std::to_string(n) + ".png");
    return write_png_rgb(path, width, height, pixels) ? path : std::filesystem::path{};
}

std::filesystem::path save_screenshot(const std::filesystem::path& directory, dsr::Machine& machine) {
    constexpr std::size_t pixels_per_screen =
        static_cast<std::size_t>(kScreenWidth) * kScreenHeight;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return {};
    std::vector<std::uint32_t> both;
    both.reserve(pixels_per_screen * 2);
    both.insert(both.end(), machine.gpu2d().screen_top(),
                machine.gpu2d().screen_top() + pixels_per_screen);
    both.insert(both.end(), machine.gpu2d().screen_bottom(),
                machine.gpu2d().screen_bottom() + pixels_per_screen);
    const std::string stem = "dq9-" + timestamp_now();
    std::filesystem::path path = directory / (stem + ".png");
    for (int n = 1; std::filesystem::exists(path, ec) && n < 1000; ++n)
        path = directory / (stem + "-" + std::to_string(n) + ".png");
    return write_png_rgb(path, kScreenWidth, kScreenHeight * 2, both) ? path : std::filesystem::path{};
}

bool DebugSession::screenshot_only(dsr::Machine& machine, std::uint64_t frame) {
    constexpr std::size_t pixels_per_screen =
        static_cast<std::size_t>(kScreenWidth) * kScreenHeight;
    std::vector<std::uint32_t> both;
    both.reserve(pixels_per_screen * 2);
    both.insert(both.end(), machine.gpu2d().screen_top(),
                machine.gpu2d().screen_top() + pixels_per_screen);
    both.insert(both.end(), machine.gpu2d().screen_bottom(),
                machine.gpu2d().screen_bottom() + pixels_per_screen);
    const std::string stem = "screenshot-only-" + timestamp_now() + "-" + std::to_string(frame);
    const bool saved = write_png_rgb(session_dir_ / (stem + ".png"),
                                     kScreenWidth, kScreenHeight * 2, both);
    write_ppm_rgb(session_dir_ / (stem + ".ppm"), kScreenWidth, kScreenHeight * 2, both);
    return saved;
}

void DebugSession::mark(std::uint64_t frame, std::uint64_t ticks, dsr::Machine& machine) {
    if (!markers_) return;
    const std::string timestamp = timestamp_now();
    const std::string stem = "marker-" + timestamp + "-" + std::to_string(frame);
    markers_ << timestamp << "\tframe=" << frame << "\tticks=" << ticks << '\n';
    markers_.flush();
    auto save_thumbnail = [&](const char* suffix, const std::uint32_t* source) {
        constexpr std::uint32_t w = 64, h = 48;
        std::vector<std::uint32_t> small(w * h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                small[y * w + x] = source[(y * 4) * kScreenWidth + (x * 4)];
        write_png_rgb(session_dir_ / (stem + suffix + ".png"), w, h, small);
    };
    save_thumbnail("-top", machine.gpu2d().screen_top());
    save_thumbnail("-bottom", machine.gpu2d().screen_bottom());
}

} // namespace dq9
