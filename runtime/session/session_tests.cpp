#include "dsr/session.h"

#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static void test_card_file_frame() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("dsr-card-frame-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    std::vector<std::uint8_t> rom(0x9000);
    const auto put32 = [&](std::size_t p, std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) rom[p + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(0x40, 0x200); put32(0x44, 47);
    put32(0x48, 0x300); put32(0x4c, 8);
    put32(0x200, 24); rom[0x206] = 3;
    put32(0x208, 32); rom[0x20e] = 0; rom[0x20f] = 0xf0;
    put32(0x210, 39); rom[0x216] = 1; rom[0x217] = 0xf0;
    const std::array<std::uint8_t, 23> names{
        0x84, 'd', 'a', 't', 'a', 1, 0xf0, 0,
        0x83, 'm', 'a', 'p', 2, 0xf0, 0,
        6, 'a', '.', 'a', 'm', 'b', 'l', 0};
    std::copy(names.begin(), names.end(), rom.begin() + 0x218);
    put32(0x300, 0x8100); put32(0x304, 0x8104);
    const auto path = dir / "synthetic.nds";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        assert(out.good());
    }
    {
        dsr::MachineConfig config;
        config.direct_boot = false;
        config.rom_path = path;
        config.save_path = dir / "save";
        dsr::Session session(config);
        assert(session.last_card_file_id() == -1 && session.last_card_file_frame() == -1);
        auto& card = *session.machine().game_card();
        assert(!card.scene_file_tracking_enabled() && card.scene_file_index_size() == 0);
        const auto read = [&] {
            const std::array<std::uint8_t, 5> command{0xb7, 0, 0, 0x81, 0};
            for (std::size_t i = 0; i < command.size(); ++i)
                card.io_write(0x040001a8 + static_cast<std::uint32_t>(i), command[i], 8);
            card.io_write(0x040001a4, 0x87000000, 32);
            card.run(44);
            (void)card.io_read(0x04100010, 32);
        };
        read();
        assert(session.last_card_file_path().empty() && session.last_card_file_id() == -1);
        assert(session.last_card_file_frame() == -1);
        session.enable_scene_file_tracking();
        session.enable_scene_file_tracking();
        assert(card.scene_file_tracking_enabled() && card.scene_file_index_size() == 1);
        read();
        assert(session.last_card_file_path() == "data/map/a.ambl" && session.last_card_file_id() == 0);
        assert(session.last_card_file_frame() == 0);
        session.step({});
        bool rejected_late_enable = false;
        try { session.enable_scene_file_tracking(); }
        catch (const std::logic_error&) { rejected_late_enable = true; }
        assert(rejected_late_enable);
        read();
        assert(session.last_card_file_frame() == 1);
    }
    {
        dsr::MachineConfig config;
        config.direct_boot = false;
        config.rom_path = path;
        config.save_path = dir / "default-save";
        dsr::Session session(config);
        auto& card = *session.machine().game_card();
        session.step({});
        assert(!card.scene_file_tracking_enabled() && card.scene_file_index_size() == 0);
        assert(session.last_card_file_path().empty() && session.last_card_file_id() == -1 &&
               session.last_card_file_frame() == -1);
        bool rejected = false;
        try { session.enable_scene_file_tracking(); }
        catch (const std::logic_error&) { rejected = true; }
        assert(rejected && !card.scene_file_tracking_enabled() && card.scene_file_index_size() == 0);
    }
    std::filesystem::remove_all(dir);
}

static void test_scene_classifier() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    auto& machine = session.machine();
    constexpr auto cpu = dsr::CpuId::Arm9;
    dsr_mcr(&machine.cpu(cpu), 15, 0, 9, 1, 0, 0x01ffc00a, 0);
    auto write8 = [&](std::uint32_t address, std::uint8_t value) {
        machine.write8(cpu, address, value);
    };
    auto write32 = [&](std::uint32_t address, std::uint32_t value) {
        machine.write32(cpu, address, value);
    };
    constexpr std::uint32_t row = 0x01ffd364;
    for (unsigned i = 0; i < 6; ++i) write8(row + i, 0xff);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(row, 16);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(row + 1, 21);
    assert(dsr::classify_scene(machine) == DSR_SCENE_CHARACTER_CREATION);
    write8(0x01ffd340, 1);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(0x01ffd340, 0);
    write32(0x01ffd348, 8);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write32(0x01ffd348, 0);
    write8(row + 2, 0x40 | 17);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(row, 17);
    write8(row + 1, 0xff);
    write8(row + 2, 0xff);
    constexpr std::uint32_t world = 0x02010000;
    constexpr std::uint32_t owner = 0x02020000;
    constexpr std::uint32_t dispatcher = 0x02030000;
    write32(0x021d82e4, world);
    write32(world + 0x3718, owner);
    write32(owner + 0x6ac, dispatcher);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write32(dispatcher + 0xea8, 1);
    assert(dsr::classify_scene(machine) == DSR_SCENE_BATTLE);
    write8(0x01ffd340, 1);
    assert(dsr::classify_scene(machine) == DSR_SCENE_BATTLE);
    write8(row + 1, 0x40 | 4);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(row + 1, 0xff);
    write8(row, 0x40 | 17);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write8(row, 17);
    write32(dispatcher + 0xea8, 0);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write32(dispatcher + 0xea8, 1);
    write8(0x01ffd340, 0);
    write32(owner + 0x6ac, 0x023ffffc);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write32(owner + 0x6ac, dispatcher + 1);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    write32(owner + 0x6ac, dispatcher);
    write32(world + 0x3718, 0);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    // Exercise both ends of the allowed RAM range at every pointer hop.
    // Seed the last valid word so an off-by-one rejection cannot pass unnoticed.
    write32(world + 0x3718, owner);
    constexpr std::uint32_t ram_start = 0x02000000;
    constexpr std::uint32_t ram_end = 0x02400000;
    const std::uint32_t slots[] = {0x021d82e4, world + 0x3718, owner + 0x6ac};
    const std::uint32_t offsets[] = {0x3718, 0x6ac, 0xea8};
    const std::uint32_t next[] = {owner, dispatcher, 1};
    const std::uint32_t normal[] = {world, owner, dispatcher};
    for (unsigned hop = 0; hop < 3; ++hop) {
        const auto last = ram_end - 4 - offsets[hop];
        for (const auto base : {ram_start, last}) {
            write32(base + offsets[hop], next[hop]);
            write32(slots[hop], base);
            assert(dsr::classify_scene(machine) == DSR_SCENE_BATTLE);
        }
        for (const auto base : {0u, ram_start - 4, last + 4, normal[hop] + 1, 0xfffffffcu}) {
            write32(slots[hop], base);
            assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
        }
        write32(slots[hop], normal[hop]);
    }

    // The current tracker row, not row zero, determines the scene.
    for (unsigned selected = 1; selected < 8; ++selected) {
        const auto selected_row = row + selected * 6;
        for (unsigned i = 0; i < 6; ++i) write8(selected_row + i, 0xff);
        write32(0x01ffd348, selected);
        assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
        write8(selected_row + 5, 21);
        assert(dsr::classify_scene(machine) == DSR_SCENE_CHARACTER_CREATION);
        write8(selected_row + 5, 17);
        assert(dsr::classify_scene(machine) == DSR_SCENE_BATTLE);
        write8(selected_row + 4, 0x40 | 4);
        assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
    }
    write32(0x01ffd348, 0);
    write8(row, 0xff);
    assert(dsr::classify_scene(machine) == DSR_SCENE_OTHER);
}

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

static void test_snapshot_ram() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    dsr::Session control(config);
    auto& machine = session.machine();
    machine.write32(dsr::CpuId::Arm9, 0x02000100, 0x78563412);
    control.machine().write32(dsr::CpuId::Arm9, 0x02000100, 0x78563412);
    std::array<std::uint8_t, 4> snapshot{};
    assert(machine.copy_snapshot_ram(0x02000100, snapshot.data(), snapshot.size()));
    assert((snapshot == std::array<std::uint8_t, 4>{0x12, 0x34, 0x56, 0x78}));
    machine.write8(dsr::CpuId::Arm9, 0x023fffff, 0xab);
    control.machine().write8(dsr::CpuId::Arm9, 0x023fffff, 0xab);
    assert(machine.copy_snapshot_ram(0x023fffff, snapshot.data(), 1) && snapshot[0] == 0xab);
    assert(!machine.copy_snapshot_ram(0x01000000, snapshot.data(), 1));
    assert(!machine.copy_snapshot_ram(0x023fffff, snapshot.data(), 2));
    assert(!machine.copy_snapshot_ram(0x02400000, snapshot.data(), 1));
    assert(!machine.copy_snapshot_ram(0x02000000, snapshot.data(), std::numeric_limits<std::size_t>::max()));
    assert(!machine.copy_snapshot_ram(0x02000000, nullptr, 1));
    assert(!machine.copy_snapshot_ram(0x02000000, snapshot.data(), 0));
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x01ff800c, 0);
    dsr_mcr(&control.machine().cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x01ff800c, 0);
    machine.write32(dsr::CpuId::Arm9, 0x01ffd348, 0x44332211);
    control.machine().write32(dsr::CpuId::Arm9, 0x01ffd348, 0x44332211);
    assert(machine.copy_snapshot_ram(0x01ffd348, snapshot.data(), snapshot.size()));
    assert((snapshot == std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44}));
    assert(!machine.copy_snapshot_ram(0, snapshot.data(), 1));
    machine.write32(dsr::CpuId::Arm9, 0x00800004, 0xaabbccdd);
    control.machine().write32(dsr::CpuId::Arm9, 0x00800004, 0xaabbccdd);
    assert(machine.copy_snapshot_ram(0x00800004, snapshot.data(), snapshot.size()));
    assert((snapshot == std::array<std::uint8_t, 4>{0xdd, 0xcc, 0xbb, 0xaa}));
    assert(!machine.copy_snapshot_ram(0x00803fff, snapshot.data(), 2));
    session.step({});
    control.step({});
    assert(machine.copy_snapshot_ram(0x01ffd348, snapshot.data(), snapshot.size()));
    assert(std::memcmp(session.screen_top(), control.screen_top(), 256 * 192 * sizeof(std::uint32_t)) == 0);
    assert(std::memcmp(session.screen_bottom(), control.screen_bottom(), 256 * 192 * sizeof(std::uint32_t)) == 0);
    assert(session.ticks() == control.ticks());
    // ITCM overlays main RAM; DTCM overlays ITCM and mirrors its backing.
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x0200000c, 0);
    machine.write8(dsr::CpuId::Arm9, 0x02003fff, 0x51);
    machine.write8(dsr::CpuId::Arm9, 0x02004000, 0x52);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x0200400c, 0);
    machine.write8(dsr::CpuId::Arm9, 0x02004000, 0x61);
    assert(machine.copy_snapshot_ram(0x02003fff, snapshot.data(), 2));
    assert(snapshot[0] == 0x51 && snapshot[1] == 0x61);
    machine.write8(dsr::CpuId::Arm9, 0x02007fff, 0x71);
    machine.write8(dsr::CpuId::Arm9, 0x02008000, 0x72);
    assert(machine.copy_snapshot_ram(0x02007fff, snapshot.data(), 2));
    assert(snapshot[0] == 0x71 && snapshot[1] == 0x72);
    assert(machine.copy_snapshot_ram(0x02004000, snapshot.data(), 1) && snapshot[0] == 0x72);
    // A sub-page TCM over RAM does not replace an existing bus page mapping.
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x02004000, 0);
    assert(machine.copy_snapshot_ram(0x02004000, snapshot.data(), 1) && snapshot[0] == 0x52);
    // A 4 KiB-aligned TCM base can map a page across its physical backing boundary.
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x0200000c, 0);
    machine.write8(dsr::CpuId::Arm9, 0x02003fff, 0xa5);
    machine.write8(dsr::CpuId::Arm9, 0x02000000, 0x5a);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x0200100c, 0);
    assert(machine.copy_snapshot_ram(0x02000fff, snapshot.data(), 2));
    assert(snapshot[0] == 0xa5 && snapshot[1] == 0x5a);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x0080000a, 0);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x0200000c, 0);
    machine.write8(dsr::CpuId::Arm9, 0x02007fff, 0xc3);
    machine.write8(dsr::CpuId::Arm9, 0x02000000, 0x3c);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x0200100c, 0);
    assert(machine.copy_snapshot_ram(0x02000fff, snapshot.data(), 2));
    assert(snapshot[0] == 0xc3 && snapshot[1] == 0x3c);
    // On the slow path, ITCM takes priority partway through a DTCM region.
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 0, 0x00800008, 0);
    machine.write8(dsr::CpuId::Arm9, 0x00800fff, 0x93);
    machine.write8(dsr::CpuId::Arm9, 0x00801000, 0x94);
    dsr_mcr(&machine.cpu(dsr::CpuId::Arm9), 15, 0, 9, 1, 1, 0x00801000, 0);
    machine.write8(dsr::CpuId::Arm9, 0x00801000, 0xa4);
    assert(machine.read8(dsr::CpuId::Arm9, 0x00800fff) == 0x93);
    assert(machine.read8(dsr::CpuId::Arm9, 0x00801000) == 0xa4);
    assert(machine.copy_snapshot_ram(0x00800fff, snapshot.data(), 2));
    assert(snapshot[0] == machine.read8(dsr::CpuId::Arm9, 0x00800fff));
    assert(snapshot[1] == machine.read8(dsr::CpuId::Arm9, 0x00801000));
}

int main() {
    test_card_file_frame();
    test_scene_classifier();
    test_compose_hires(2, 0x8207U, true);
    test_compose_hires(3, 0x0207U, false); // engine A on the bottom screen
    test_save_restore();
    test_snapshot_ram();
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
