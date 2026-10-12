#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"
#include "../src/gpu2d_test.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {
constexpr std::array<std::uint32_t, 9> controls{
    0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
    0x04000245, 0x04000246, 0x04000248, 0x04000249};
constexpr std::array<std::uint32_t, 9> lcdc{
    0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
    0x06890000, 0x06894000, 0x06898000, 0x068a0000};
constexpr std::array<std::uint32_t, 9> sizes{
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
int failures = 0;
void expect(bool value, const char* rule) {
    if (!value) { ++failures; std::cerr << "FAIL: " << rule << '\n'; }
}
void map(dsr::Gpu2D& gpu, unsigned bank, unsigned mode, unsigned offset = 0) {
    gpu.io_write(controls[bank], 0x80U | mode | (offset << 3), 8);
}
void seed(dsr::Gpu2D& gpu, unsigned bank, std::uint16_t value, std::uint32_t offset = 0) {
    map(gpu, bank, 0);
    gpu.arm9_write(lcdc[bank] + offset, value, 16);
}
void test_bank_modes() {
    for (unsigned bank = 0; bank < 9; ++bank) {
        dsr::Gpu2D gpu;
        seed(gpu, bank, 0x1234);
        gpu.arm9_write(lcdc[bank] + sizes[bank] - 2, 0x5678, 16);
        expect(gpu.arm9_read(lcdc[bank], 16) == 0x1234, "LCDC physical base A-I");
        expect(gpu.arm9_read(lcdc[bank] + sizes[bank] - 2, 16) == 0x5678, "bank A-I sizes");
        expect(gpu.frame_view().banks[bank].byte_count == sizes[bank], "physical bank size view");
        gpu.io_write(controls[bank], 0, 8);
        expect(gpu.arm9_read(lcdc[bank], 16) == 0, "disabled bank is inaccessible");
        for (unsigned offset = 0; offset < 4; ++offset) {
            map(gpu, bank, 1, offset);
            const auto small = ((offset & 1U) + (offset >> 1U) * 4U) * 0x4000U;
            const auto address = bank < 4 ? 0x06000000U + offset * 0x20000U :
                bank == 4 ? 0x06000000U : bank < 7 ? 0x06000000U + small :
                bank == 7 ? 0x06200000U : 0x06208000U;
            expect(gpu.arm9_read(address, 16) == 0x1234, "BG bank MST/offset table");
            expect(gpu.arm9_read(lcdc[bank], 16) == 0, "BG remap hides LCDC CPU window");
        }
        if (bank == 2 || bank == 3) {
            for (unsigned offset = 0; offset < 4; ++offset) {
                map(gpu, bank, 2, offset);
                expect(gpu.vramstat() == (bank == 2 ? 1 : 2), "VRAMSTAT enabled ARM7 ownership");
                const auto addr = 0x06000000U + (offset & 1U) * 0x20000U;
                expect(gpu.arm7_read(addr, 16) == (offset < 2 ? 0x1234U : 0U), "C/D ARM7 legal offsets");
                expect(gpu.arm9_read(addr, 16) == 0, "ARM7 ownership excludes ARM9");
            }
            map(gpu, bank, 4);
            expect(gpu.arm9_read(bank == 2 ? 0x06200000 : 0x06600000, 16) == 0x1234,
                   "C engine B BG/D engine B OBJ");
        } else if (bank < 2 || bank == 4 || bank == 5 || bank == 6 || bank == 8) {
            for (unsigned offset = 0; offset < 4; ++offset) {
                map(gpu, bank, 2, offset);
                const auto small = ((offset & 1U) + (offset >> 1U) * 4U) * 0x4000U;
                const auto addr = bank < 2 ? 0x06400000U + (offset & 1U) * 0x20000U :
                    bank == 4 ? 0x06400000U : bank < 7 ? 0x06400000U + small : 0x06600000U;
                expect(gpu.arm9_read(addr, 16) == (bank < 2 && offset >= 2 ? 0U : 0x1234U),
                       "OBJ bank MST/offset table");
            }
        }
        gpu.io_write(controls[bank], 0, 8);
        expect(gpu.vramstat() == 0, "VRAMSTAT clears disabled ownership");
    }
}
void test_texture_slots() {
    for (unsigned bank = 0; bank < 4; ++bank) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            dsr::Gpu2D gpu;
            seed(gpu, bank, 0x801f);
            map(gpu, bank, 3, slot);
            expect(gpu.texture_slot(static_cast<int>(slot))[0] == 0x1f, "A-D texture slot offset");
            expect(gpu.arm9_read(lcdc[bank], 16) == 0, "texture-only bank not CPU LCDC");
            for (unsigned other = 0; other < 4; ++other)
                if (other != slot) expect(gpu.texture_slot(static_cast<int>(other)) == nullptr, "unmapped texture slot");
        }
    }
    dsr::Gpu2D gpu;
    for (unsigned slot = 0; slot < 4; ++slot) seed(gpu, 4, static_cast<std::uint16_t>(slot + 1), slot * 0x4000);
    map(gpu, 4, 3);
    for (unsigned slot = 0; slot < 4; ++slot)
        expect(gpu.texpal_slot(static_cast<int>(slot))[0] == slot + 1, "E 64KiB texture palette slots0-3");
    for (unsigned bank = 5; bank < 7; ++bank) {
        for (unsigned offset = 0; offset < 4; ++offset) {
            dsr::Gpu2D small;
            seed(small, bank, 0x4321);
            map(small, bank, 3, offset);
            const auto slot = (offset & 1U) + (offset >> 1U) * 4U;
            expect(small.texpal_slot(static_cast<int>(slot))[0] == 0x21, "F/G palette offsets select0,1,4,5 (not0,1,2,3)");
        }
    }
}
void test_cpu_overlap_and_byte_writes() {
    dsr::Gpu2D gpu;
    seed(gpu, 0, 0x000f); seed(gpu, 1, 0x00f0);
    map(gpu, 0, 1); map(gpu, 1, 1);
    expect(gpu.arm9_vram_page(0x06000000) == nullptr, "overlap disables single-bank CPU fast path");
    expect(gpu.arm9_read(0x06000000, 16) == 0x00ff, "overlapping CPU VRAM read ORs banks");
    gpu.arm9_write(0x06000000, 0x1234, 16);
    gpu.arm9_write(0x06000000, 0xff, 8);
    map(gpu, 0, 0); map(gpu, 1, 0);
    expect(gpu.arm9_read(lcdc[0], 16) == 0x1234 && gpu.arm9_read(lcdc[1], 16) == 0x1234,
           "overlap writes every bank; ARM9 BG byte writes ignored");
    gpu.arm9_write(lcdc[0], 0xff, 8);
    expect(gpu.arm9_read(lcdc[0], 16) == 0x1234, "ARM9 LCDC byte writes ignored");
    map(gpu, 0, 2); gpu.arm9_write(0x06400000, 0xff, 8);
    expect(gpu.arm9_read(0x06400000, 16) == 0x1234, "ARM9 OBJ byte writes ignored");
    seed(gpu, 2, 0x1000); seed(gpu, 3, 0x0100);
    map(gpu, 2, 2); map(gpu, 3, 2);
    expect(gpu.arm7_read(0x06000000, 16) == 0x1100, "ARM7 overlap read ORs banks");
    gpu.arm7_write(0x06000000, 0x55, 8);
    expect(gpu.arm7_read(0x06000000, 16) == 0x1155, "ARM7 byte writes allowed to both banks");
}
void test_palette_oam_mirrors() {
    dsr::Gpu2D gpu;
    for (const auto base : {0x05000000U, 0x07000000U}) {
        gpu.arm9_write(base + 0x7fc, 0x12345678, 32);
        expect(gpu.arm9_read(base + 0x17fc, 32) == 0x12345678, "palette/OAM 2KB read mirror");
        gpu.arm9_write(base + 0xfff7fc, 0xabcd1234, 32);
        expect(gpu.arm9_read(base + 0x7fc, 32) == 0xabcd1234, "palette/OAM mirrors across16MB region");
        gpu.arm9_write(base + 0x17fc, 0xff, 8);
        expect(gpu.arm9_read(base + 0x17fc, 32) == 0xabcd1234, "palette/OAM mirrored byte writes ignored");
    }
}
void command(dsr::Gpu3D& gpu, unsigned code, std::uint32_t value) {
    gpu.io_write(0x04000440U + (code - 0x10U) * 4U, value, 32);
}
void test_texture_overlap_and_live_upload() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D renderer;
    renderer.attach_gpu2d(&gpu);
    seed(gpu, 0, 0x800f); seed(gpu, 1, 0x0010);
    map(gpu, 0, 3); map(gpu, 1, 3);
    renderer.io_write(0x04000060, 1, 32);
    renderer.io_write(0x04000354, 0x7fff, 32);
    command(renderer, 0x2a, 7U << 26); // direct RGB555 texture
    command(renderer, 0x29, 0xc0U | (31U << 16));
    command(renderer, 0x20, 0x7fff);
    command(renderer, 0x40, 0);
    command(renderer, 0x23, 0xf000f000); command(renderer, 0x23, 0);
    command(renderer, 0x23, 0xf0001000); command(renderer, 0x23, 0);
    command(renderer, 0x23, 0x10000000); command(renderer, 0x23, 0);
    command(renderer, 0x41, 0); command(renderer, 0x50, 0);
    renderer.run(100000); renderer.vblank_start();
    renderer.render_start();
    expect((renderer.line(96)[128] & 63U) == 63, "overlapping texture banks OR instead of vanishing");
    seed(gpu, 0, 0x83e0); seed(gpu, 1, 0); // LCDC upload between renders, no geometry resubmit
    map(gpu, 0, 3); map(gpu, 1, 3); renderer.vblank_start();
    renderer.render_start();
    expect((renderer.line(96)[128] & 0xfffU) == (63U << 6), "LCDC remap/upload restored texture uses live bytes next render");
    // Indexed texel 1; overlapping E/F palette colours combine to red31.
    seed(gpu, 0, 0x0101); map(gpu, 0, 3); gpu.io_write(controls[1], 0, 8);
    seed(gpu, 4, 0x000f, 2); seed(gpu, 5, 0x0010, 2);
    map(gpu, 4, 3); map(gpu, 5, 3);
    command(renderer, 0x2a, 4U << 26);
    command(renderer, 0x29, 0xc0U | (31U << 16)); command(renderer, 0x40, 0);
    command(renderer, 0x23, 0xf000f000); command(renderer, 0x23, 0);
    command(renderer, 0x23, 0xf0001000); command(renderer, 0x23, 0);
    command(renderer, 0x23, 0x10000000); command(renderer, 0x23, 0);
    command(renderer, 0x41, 0); command(renderer, 0x50, 0);
    renderer.run(100000); renderer.vblank_start();
    renderer.render_start();
    expect((renderer.line(96)[128] & 63U) == 63, "overlapping texture palette banks OR instead of black");
}
void test_clear_image_overlap() {
    dsr::Gpu2D gpu; dsr::Gpu3D renderer;
    renderer.attach_gpu2d(&gpu);
    seed(gpu, 0, 0x800f); seed(gpu, 1, 0x0010); seed(gpu, 2, 0x1234);
    map(gpu, 0, 3, 2); map(gpu, 1, 3, 2); map(gpu, 2, 3, 3);
    renderer.io_write(0x04000060, 1U << 14, 32);
    renderer.vblank_start();
    renderer.render_start();
    expect(renderer.line(0)[0] == (63U | (31U << 24)), "clear colour slot2 supports overlapping banks");
}
void test_vram_generation() {
    dsr::Gpu2D gpu;
    expect(gpu.vram_generation() == 0, "reset texture invalidation generation");
    for (unsigned bank = 0; bank < 9; ++bank) {
        const auto before_map = gpu.vram_generation();
        map(gpu, bank, 3);
        expect(gpu.vram_generation() == before_map + 1, "every VRAMCNT bank write invalidates");
        const auto before_write = gpu.vram_generation();
        // CPU windows are inaccessible while a bank is texture-mapped: exercise
        // the physical-byte hook shared by slow CPU/DMA and capture writes.
        dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(
            gpu, static_cast<std::uint8_t>(bank), 0, 0x42);
        expect(gpu.vram_generation() == before_write + (bank < 7 ? 1 : 0),
               "only image A-D and texture palette E-G physical writes invalidate");
        map(gpu, bank, 0);
        const auto before_lcdc = gpu.vram_generation();
        gpu.arm9_write(lcdc[bank], 0x1234, 16);
        expect(gpu.vram_generation() == before_lcdc, "non-texture LCDC writes do not invalidate");
        map(gpu, bank, 1);
        const auto before_bg = gpu.vram_generation();
        dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(
            gpu, static_cast<std::uint8_t>(bank), 0, 0x56);
        expect(gpu.vram_generation() == before_bg, "non-texture BG writes do not invalidate");
        gpu.io_write(controls[bank], 0, 8);
        const auto before_disabled = gpu.vram_generation();
        dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(
            gpu, static_cast<std::uint8_t>(bank), 0, 0x78);
        expect(gpu.vram_generation() == before_disabled, "disabled texture bank clears texture flag");
    }
    gpu.io_write(controls[2], 0x82, 8);
    const auto before_arm7 = gpu.vram_generation();
    gpu.arm7_write(0x06000000, 0x99, 8);
    expect(gpu.vram_generation() == before_arm7, "ARM7-owned bank writes are non-texture");
    map(gpu, 0, 3);
    const auto before_blocked = gpu.vram_generation();
    gpu.arm9_write(lcdc[0], 0xabcd, 16);
    expect(gpu.vram_generation() == before_blocked && gpu.texture_slot(0)[0] == 0x78,
           "texture mapping blocks CPU LCDC writes without invalidation");
    const auto before_upload = gpu.vram_generation();
    seed(gpu, 0, 0xbeef);
    map(gpu, 0, 3);
    expect(gpu.vram_generation() == before_upload + 2 && gpu.texture_slot(0)[0] == 0xef,
           "real texture uploads invalidate on LCDC/texture remaps");
    gpu.reset();
    expect(gpu.vram_generation() == 0, "reset clears texture invalidation generation");
}
}
int main() {
    test_bank_modes(); test_texture_slots(); test_cpu_overlap_and_byte_writes();
    test_palette_oam_mirrors(); test_texture_overlap_and_live_upload(); test_clear_image_overlap();
    test_vram_generation();
    std::cout << "VRAM mapping failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
