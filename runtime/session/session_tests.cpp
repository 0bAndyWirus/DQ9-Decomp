#include "dsr/session.h"

#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// A full-screen opaque red 3D clear plane on BG0 at scale N: the hi-res overlay must be
// all-red at 256N x 192N, and it must follow POWCNT1 bit 15 (the screen engine A drives).
static void test_compose_hires(int scale, std::uint32_t powcnt1, bool engine_a_on_top) {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    auto& gpu2d = session.machine().gpu2d();
    auto& gpu3d = session.machine().gpu3d();
    gpu2d.io_write(0x04000304U, powcnt1, 16);
    constexpr auto display = (1U << 16) | (1U << 8) | (1U << 3); // normal display, BG0 = 3D
    gpu2d.io_write(0x04000000U, display, 32);
    gpu3d.io_write(0x04000350U, (31U << 16) | 31U, 32); // opaque red clear colour
    std::vector<std::uint32_t> out(static_cast<std::size_t>(256 * scale) * (192 * scale), 0U);
    bool on_top = !engine_a_on_top;
    // Nothing is latched before a step, and scale 1 never composes.
    assert(!session.compose_hires(out.data(), scale, &on_top));
    gpu3d.set_output_scale(scale);
    // The first step renders the scaled frame at scanline 214; the next one displays it.
    session.step({});
    session.step({});
    assert(session.compose_hires(out.data(), scale, &on_top));
    assert(on_top == engine_a_on_top);
    for (const std::uint32_t pixel : out) assert(pixel == 0xFFFF0000U);
    {
        // Stacked variant: the 3D screen is all red, the other screen is the native frame upscaled.
        std::vector<std::uint32_t> both(out.size() * 2, 0U);
        assert(session.compose_both_hires(both.data(), scale));
        const std::size_t half = out.size();
        const std::uint32_t* three_d_screen = engine_a_on_top ? both.data() : both.data() + half;
        const std::uint32_t* other_screen = engine_a_on_top ? both.data() + half : both.data();
        for (std::size_t i = 0; i < half; ++i) assert(three_d_screen[i] == 0xFFFF0000U);
        const std::uint32_t* native_other = engine_a_on_top ? session.screen_bottom() : session.screen_top();
        for (std::size_t y = 0; y < static_cast<std::size_t>(192 * scale); ++y)
            for (std::size_t x = 0; x < static_cast<std::size_t>(256 * scale); ++x)
                assert(other_screen[y * 256 * scale + x] == native_other[(y / scale) * 256 + x / scale]);
        assert(!session.compose_both_hires(both.data(), 1));
    }
    // A different requested scale than the latched one is refused, never reinterpreted.
    assert(!session.compose_hires(out.data(), scale % 4 + 1, &on_top));
    gpu3d.set_output_scale(1);
    session.step({});
    assert(!session.compose_hires(out.data(), scale, &on_top));
}

static void test_save_restore() {
    struct TempDir {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("dsr-session-save-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        TempDir() { std::filesystem::create_directory(path); }
        ~TempDir() { std::error_code error; std::filesystem::remove_all(path, error); }
    } temp;
    const auto write = [](const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(output.good());
    };
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto rom = temp.path / "synthetic.nds";
    const auto save = temp.path / "session.sav";
    const auto backups = temp.path / "backups";
    const std::string original(512, '\x19'), restored(512, '\x63');
    write(rom, std::string(0x20000, '\0'));
    write(save, original);
    dsr::MachineConfig config;
    config.rom_path = rom;
    config.save_path = save;
    config.direct_boot = false;
    config.guest_abi.backup_type = dsr::BackupType::Eeprom512B;
    {
        dsr::Session listing(config);
        assert(listing.save_capacity() == 512);
        assert(listing.save_backups().empty());
    }
    assert(read(save) == original);
    assert(!std::filesystem::exists(backups));
    std::filesystem::create_directory(backups);
    const auto older = backups / "session.sav.20260101T000000.000000000Z-000001.bak";
    const auto newer = backups / "session.sav.20260101T000000.000000000Z-000002.bak";
    write(newer, std::string(8192, '\x47'));
    write(older, restored);
    std::filesystem::last_write_time(older,
        std::filesystem::file_time_type::clock::now() + std::chrono::hours(24));
    {
        dsr::Session session(config);
        const auto listed = session.save_backups();
        assert(listed.size() == 2);
        assert(listed[0].path == newer && listed[0].size == 8192);
        assert(listed[1].path == older && listed[1].size == 512);
        bool mismatched = false;
        try {
            session.restore_save(); // Newest is wrong-sized: do not silently choose an older match.
        } catch (const std::runtime_error& error) {
            mismatched = std::string(error.what()).find("raw size") != std::string::npos;
        }
        assert(mismatched);
        assert(session.frame_count() == 0);
        assert(session.save_capacity() == 512);
        assert(read(save) == original);
        assert(session.save_backups().size() == listed.size());
        session.restore_save(older);
        assert(session.frame_count() == 0);
        assert(session.save_capacity() == 512);
        assert(read(save) == restored);
        const auto after_restore = session.save_backups();
        assert(after_restore.size() == 3);
        assert(read(after_restore.front().path) == original); // Current target preserved by GameCard.
        assert(session.flush_save());
        assert(session.save_backups().size() == after_restore.size());
        session.step({});
        const auto paused_ticks = session.ticks();
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool refused = false;
            try {
                session.restore_save(older);
            } catch (const std::logic_error& error) {
                refused = std::string(error.what()).find("unstarted") != std::string::npos;
            }
            assert(refused); // Not stepping again (a paused/stopped host) never makes restore safe.
            assert(session.ticks() == paused_ticks);
            assert(session.frame_count() == 1);
            assert(session.save_capacity() == 512);
            assert(read(save) == restored);
            assert(session.save_backups().size() == after_restore.size());
        }
    }
    assert(read(save) == restored);
    config.rom_path.clear();
    dsr::Session no_cartridge(config);
    assert(no_cartridge.save_capacity() == 0);
    assert(no_cartridge.save_backups().empty());
    bool refused = false;
    try {
        no_cartridge.restore_save(older);
    } catch (const std::logic_error&) {
        refused = true;
    }
    assert(refused);
}

int main() {
    test_compose_hires(2, 0x8207U, true);
    test_compose_hires(3, 0x0207U, false); // engine A on the bottom screen
    test_save_restore();
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    assert(session.frame_count() == 0);
    assert(session.audio_rate() > 32'000.0 && session.audio_rate() < 33'000.0);
    const auto before = session.ticks();
    session.step({});
    assert(session.frame_count() == 1);
    assert(session.ticks() > before);
    assert(session.screen_top() != nullptr);
    assert(session.screen_bottom() != nullptr);
    assert(session.audio_frames() <= 4096);
    return 0;
}
