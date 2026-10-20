#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"
#include "../src/composition_order.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>

namespace {
constexpr std::uint32_t kA = 0x04000000U;
constexpr std::uint32_t kVramA = 0x04000240U;
constexpr std::uint32_t kVramB = 0x04000241U;
constexpr std::uint32_t kVramE = 0x04000244U;
constexpr std::uint32_t kPow = 0x04000304U;
constexpr std::uint32_t kVram = 0x06000000U;
constexpr std::uint32_t kLcdcA = 0x06800000U;
constexpr std::uint32_t kPalette = 0x05000000U;
constexpr std::uint32_t kOam = 0x07000000U;

std::array<std::uint32_t, 256> fake_3d_line{};

void write16(dsr::Gpu2D& gpu, std::uint32_t address, std::uint16_t value) {
    gpu.io_write(address, value, 16);
}

// Engine A/B and 3D powered, engine A on the upper screen.
void set_power(dsr::Gpu2D& gpu, std::uint16_t value = 0x8203U) {
    gpu.io_write(kPow, value, 16);
}

void set_bg_palette(dsr::Gpu2D& gpu, std::uint16_t index, std::uint16_t color) {
    gpu.arm9_write(kPalette + static_cast<std::uint32_t>(index) * 2U, color, 16);
}

void setup_text_bg0(dsr::Gpu2D& gpu, std::uint16_t color = 0x001FU) {
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U), 32);
    write16(gpu, kA + 0x08U, 0x0004U); // 4bpp text, character block 1.
    set_bg_palette(gpu, 0U, 0x7C00U);
    set_bg_palette(gpu, 1U, color);
    gpu.arm9_write(kVram + 0x4000U, 0x1111U, 16);
}

std::uint32_t pixel(const std::uint32_t* frame, unsigned x, unsigned y = 0U) {
    return frame[y * 256U + x];
}

void test_text_bg_and_palette_write_rules() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    gpu.arm9_write(kPalette + 2U, 0x001FU, 8); // Palette byte writes are ignored.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
    assert(gpu.arm9_read(kPalette + 2U, 16) == 0x001FU);
    assert(gpu.screen_bottom()[0] == 0xFFFFFFFFU);
}

void test_affine_rotation() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 10U) | 2U, 32); // Mode 2: BG2 affine.
    write16(gpu, kA + 0x0CU, 0x0004U); // BG2 character block 1.
    gpu.io_write(kA + 0x20U, 0U, 16); // PA=0.
    gpu.io_write(kA + 0x24U, 0x0100U, 16); // PC=1.0, rotate the source vertically.
    gpu.io_write(kA + 0x28U, 0U, 32);
    gpu.io_write(kA + 0x2CU, 0U, 32);
    set_bg_palette(gpu, 1U, 0x001FU);
    set_bg_palette(gpu, 2U, 0x03E0U);
    for (std::uint32_t row = 0; row < 8U; ++row) {
        const std::uint16_t pattern = static_cast<std::uint16_t>((row + 1U) * 0x0101U);
        for (std::uint32_t word = 0; word < 4U; ++word) {
            gpu.arm9_write(kVram + 0x4000U + row * 8U + word * 2U, pattern, 16);
        }
    }
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
    assert(pixel(gpu.screen_top(), 1U) == 0xFF00FF00U);
}

void test_extended_256_color_bitmap() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 11U) | 3U, 32); // BG3 extended bitmap.
    write16(gpu, kA + 0x0EU, 0x0080U); // 256-color bitmap, size 128x128.
    gpu.io_write(kA + 0x30U, 0x0100U, 16);
    gpu.io_write(kA + 0x36U, 0x0100U, 16);
    gpu.io_write(kA + 0x38U, 0U, 32);
    gpu.io_write(kA + 0x3CU, 0U, 32);
    set_bg_palette(gpu, 2U, 0x7C00U);
    gpu.arm9_write(kVram, 0x00020202U, 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF0000FFU);
}

void test_extended_direct_color_bitmap() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 11U) | 3U, 32);
    write16(gpu, kA + 0x0EU, 0x0084U); // Extended direct-color bitmap.
    gpu.io_write(kA + 0x30U, 0x0100U, 16);
    gpu.io_write(kA + 0x36U, 0x0100U, 16);
    gpu.io_write(kA + 0x38U, 0U, 32);
    gpu.io_write(kA + 0x3CU, 0U, 32);
    gpu.arm9_write(kVram, 0x801FU, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
}

void test_large_bitmap_mode() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    set_bg_palette(gpu, 1U, 0x03E0U);
    gpu.io_write(kA, (1U << 16U) | (1U << 10U) | 6U, 32);
    write16(gpu, kA + 0x0CU, 0x0100U); // Large bitmap ignores the screen-base field.
    gpu.arm9_write(kVram, 0x0101U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF00FF00U);
}
void test_mode_zero_renders_the_off_display_backdrop() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    set_bg_palette(gpu, 0U, 0x03E0U);
    gpu.io_write(kA, 0U, 32); // Display mode 0 is forced blank (white), not BG/OBJ output.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFFFFFFU);
}


void test_engine_b_text() {
    dsr::Gpu2D gpu;
    gpu.io_write(0x04000242U, 0x84U, 8); // Bank C to Engine B BG.
    set_power(gpu);
    gpu.arm9_write(0x06204000U, 0x1111U, 16);
    gpu.arm9_write(0x05000402U, 0x03E0U, 16);
    gpu.io_write(0x04001000U, (1U << 16U) | (1U << 8U), 32);
    write16(gpu, 0x04001008U, 0x0004U);
    gpu.render_line(0);
    assert(pixel(gpu.screen_bottom(), 0U) == 0xFF00FF00U);

    gpu.io_write(kPow, 0x8003U, 16); // Engine A powered, engine B (bit 9) off.
    gpu.render_line(0);
    assert(pixel(gpu.screen_bottom(), 0U) == 0xFFFFFFFFU);
}

void test_bank_h_and_i_engine_b_bg_mapping() {
    dsr::Gpu2D gpu;
    gpu.io_write(0x04000248U, 0x81U, 8); // Bank H: engine B BG at 0x06200000.
    gpu.io_write(0x04000249U, 0x81U, 8); // Bank I: engine B BG at 0x06208000.
    gpu.arm9_write(0x06200010U, 0x1234U, 16);
    gpu.arm9_write(0x06208010U, 0x5678U, 16);
    assert(gpu.arm9_read(0x06200010U, 16) == 0x1234U);
    assert(gpu.arm9_read(0x06208010U, 16) == 0x5678U);
    assert((gpu.io_read(0x04000248U, 16) & 0xFFFFU) == 0x8181U);
    // Banks share no storage: H holds 32 KiB, I starts after it.
    assert(gpu.arm9_read(0x06200010U + 0x8000U, 16) == 0x5678U);
}

void test_obj_priority() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    gpu.io_write(kVramB, 0x82U, 8);
    for (std::uint32_t offset = 0; offset < 32U; offset += 2U) {
        gpu.arm9_write(0x06400000U + offset, 0x1111U, 16);
    }
    for (std::uint32_t object = 1; object < 128U; ++object) {
        gpu.arm9_write(kOam + object * 8U, 0x0200U, 16); // Disable unused entries.
    }
    gpu.arm9_write(kOam, 0U, 16);
    gpu.arm9_write(kOam + 2U, 0U, 16);
    gpu.arm9_write(kOam + 4U, 0U, 16);
    gpu.arm9_write(kPalette + 0x202U, 0x7C00U, 16);
    gpu.io_write(kA, (1U << 16U) | (1U << 12U) | (1U << 8U), 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF0000FFU); // Equal priority: the OBJ draws over BG0.

    write16(gpu, kA + 0x08U, 0x0001U); // BG0 priority 1; OBJ priority 0 still wins.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF0000FFU);
}

void test_window_masking() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 13U) | (1U << 8U), 32);
    write16(gpu, kA + 0x40U, 0x0001U);
    write16(gpu, kA + 0x44U, 0x0001U);
    write16(gpu, kA + 0x48U, 0x0000U); // BG0 disabled inside WIN0.
    write16(gpu, kA + 0x4AU, 0x0001U); // BG0 enabled outside.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF0000FFU);
    assert(pixel(gpu.screen_top(), 1U) == 0xFFFF0000U);
}

void test_obj_window_masking() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    gpu.io_write(kVramB, 0x82U, 8);
    for (std::uint32_t offset = 0; offset < 32U; offset += 2U) {
        gpu.arm9_write(0x06400000U + offset, 0x1111U, 16);
    }
    gpu.arm9_write(0x06400000U, 0x0001U, 16); // Transparent pixels do not mask.
    for (std::uint32_t object = 1; object < 128U; ++object) {
        gpu.arm9_write(kOam + object * 8U, 0x0200U, 16);
    }
    gpu.arm9_write(kOam, 0x0800U, 16); // Object 0 is an OBJ-window sprite.
    gpu.arm9_write(kOam + 2U, 0U, 16);
    gpu.arm9_write(kOam + 4U, 0U, 16);
    gpu.io_write(kA, (1U << 16U) | (1U << 15U) | (1U << 12U) | (1U << 8U), 32);
    write16(gpu, kA + 0x4AU, 0x0021U); // BG0 outside OBJ window; no BG inside.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF0000FFU);
    assert(pixel(gpu.screen_top(), 1U) == 0xFFFF0000U);
    assert(pixel(gpu.screen_top(), 4U) == 0xFF0000FFU);
    assert(pixel(gpu.screen_top(), 8U) == 0xFFFF0000U);
}

void test_alpha_blending() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 9U), 32);
    write16(gpu, kA + 0x08U, 0x0004U);
    write16(gpu, kA + 0x0AU, 0x0108U); // BG1: character block 2, screen block 1.
    set_bg_palette(gpu, 0U, 0U);
    set_bg_palette(gpu, 1U, 0x001FU);
    set_bg_palette(gpu, 2U, 0x7C00U);
    gpu.arm9_write(kVram + 0x4000U, 0x1111U, 16);
    gpu.arm9_write(kVram + 0x8000U, 0x2222U, 16);
    gpu.io_write(kA + 0x50U, 0x0241U, 16); // BG0 first target, BG1 second target, alpha.
    gpu.io_write(kA + 0x52U, 0x0808U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF7B007BU); // (31 * 8) / 16 = 15 of 31 per channel.
}

void test_master_brightness_and_screen_swap() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu, 0x0010U);
    gpu.io_write(kA + 0x6CU, (1U << 14U) | 16U, 16); // Full brighten.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFFFFFFU);

    gpu.io_write(kA + 0x6CU, 0U, 16);
    gpu.io_write(kPow, 0x0203U, 16); // Swap bit clear: engine A to the lower screen.
    gpu.render_line(0);
    assert(pixel(gpu.screen_bottom(), 0U) == 0xFF830000U); // 16 / 31 expands to 131.
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFFFFFFU);
}

void test_display_modes_and_fifo() {
    {
        dsr::Gpu2D gpu;
        set_power(gpu);
        gpu.io_write(kA, 0U, 32);
        gpu.render_line(0);
        assert(pixel(gpu.screen_top(), 0U) == 0xFFFFFFFFU);
    }
    {
        dsr::Gpu2D gpu;
        setup_text_bg0(gpu);
        gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 7U), 32);
        gpu.render_line(0);
        assert(pixel(gpu.screen_top(), 0U) == 0xFFFFFFFFU);
    }
    {
        dsr::Gpu2D gpu;
        gpu.io_write(kVramA, 0x80U, 8);
        set_power(gpu);
        gpu.arm9_write(kLcdcA, 0x001FU, 16);
        gpu.io_write(kA, 2U << 16U, 32);
        gpu.render_line(0);
        assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
    }
    {
        dsr::Gpu2D gpu;
        set_power(gpu);
        gpu.io_write(kA + 0x68U, 0x03E0001FU, 32);
        gpu.io_write(kA, 3U << 16U, 32);
        gpu.render_line(0);
        assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
        assert(pixel(gpu.screen_top(), 1U) == 0xFF00FF00U);
    }
}

void test_capture_to_lcdc_bank() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x80U, 8);
    set_power(gpu);
    set_bg_palette(gpu, 0U, 0x001FU);
    gpu.io_write(kA, 1U << 16U, 32);
    gpu.io_write(kA + 0x64U, 0x80000000U | (3U << 20U), 32);
    gpu.render_line(0);
    assert(gpu.arm9_read(kLcdcA, 16) == 0x801FU);
    gpu.vblank_start();
    assert((gpu.io_read(kA + 0x64U, 32) & 0x80000000U) == 0U);
}

void test_vram_overlap_and_arm7_mapping() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    gpu.io_write(kVramB, 0x89U, 8); // Bank B at main BG slot 1.
    gpu.arm9_write(kVram, 0x00F0U, 16);
    gpu.arm9_write(kVram + 0x20000U, 0x0F00U, 16);
    gpu.io_write(kVramB, 0x81U, 8); // Now both banks overlap at slot 0.
    assert(gpu.arm9_read(kVram, 16) == 0x0FF0U);
    assert(gpu.arm9_vram_page(kVram) == nullptr);

    gpu.io_write(kVramA, 0U, 8);
    gpu.io_write(kVramB, 0U, 8);
    gpu.io_write(0x04000242U, 0x82U, 8); // Bank C allocated to ARM7.
    assert(gpu.vramstat() == 1U);
    assert(gpu.arm7_vram_page(kVram) != nullptr);
    gpu.arm7_write(kVram, 0x5AU, 8);
    assert(gpu.arm7_read(kVram, 8) == 0x5AU);
}

void test_extended_palette_lookup() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramE, 0x80U, 8);
    gpu.arm9_write(0x06880002U, 0x03E0U, 16); // Initialize extended palette in LCDC view.
    gpu.io_write(kVramA, 0x81U, 8);
    gpu.io_write(kVramE, 0x84U, 8); // Bank E: Engine A BG extended palette.
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 30U), 32);
    write16(gpu, kA + 0x08U, 0x0084U); // 8bpp text in BG0, extended palette.
    set_bg_palette(gpu, 1U, 0x001FU); // Standard palette is red; extended entry is green.
    gpu.arm9_write(kVram + 0x4000U, 0x0101U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF00FF00U);
}

void test_mode_six_does_not_force_bg0_3d() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D fake_gpu3d;
    fake_3d_line.fill((31U << 24U) | 63U);
    gpu.attach_gpu3d(&fake_gpu3d);
    set_power(gpu, 0x8203U | 4U);
    set_bg_palette(gpu, 0U, 0x7c00U);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | 6U, 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xff0000ffU);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 3U) | 6U, 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xffff0000U);
}

void test_3d_layer_alpha_blending() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D fake_gpu3d;
    fake_3d_line.fill((16U << 24U) | 63U); // Half-alpha red in packed native 3D format.
    gpu.attach_gpu3d(&fake_gpu3d);
    set_power(gpu, 0x8203U | 4U);
    set_bg_palette(gpu, 0U, 0x7C00U); // Blue backdrop.
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 3U), 32);
    gpu.io_write(kA + 0x50U, 0x2041U, 16); // BG0 over backdrop, per-pixel alpha.
    gpu.render_line(0);
    // alpha = 16 + 1: red (63 * 17) >> 6 = 16 of 31, blue ((31 << 1) * 15) >> 6 = 14 of 31.
    assert(pixel(gpu.screen_top(), 0U) == 0xFF830073U);
}

// A 3D pixel over a second-target pixel blends by polygon alpha even when BLDCNT selects no
// effect; with no second target the 3D pixel is its own 6-bit colour truncated to 5 bits.
void test_3d_layer_blends_over_second_target_without_effect() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D fake_gpu3d;
    fake_3d_line.fill((16U << 24U) | 63U);
    gpu.attach_gpu3d(&fake_gpu3d);
    set_power(gpu, 0x8203U | 4U);
    set_bg_palette(gpu, 0U, 0x7C00U);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 3U), 32);
    gpu.io_write(kA + 0x50U, 0x2000U, 16); // Backdrop is a second target, effect: none.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF830073U);

    gpu.io_write(kA + 0x50U, 0x0000U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U); // 6-bit 63 truncates to 5-bit 31.
}

// 2D blending and brightness work on 5-bit channels: 31 and 30 at 8/16 each average to 30,
// not to the 31 a widened 6-bit pipeline produces.
void test_blend_and_brightness_use_five_bit_channels() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 9U), 32);
    write16(gpu, kA + 0x08U, 0x0004U);
    write16(gpu, kA + 0x0AU, 0x0108U);
    set_bg_palette(gpu, 0U, 0U);
    set_bg_palette(gpu, 1U, 0x001FU); // red 31
    set_bg_palette(gpu, 2U, 0x001EU); // red 30
    gpu.arm9_write(kVram + 0x4000U, 0x1111U, 16);
    gpu.arm9_write(kVram + 0x8000U, 0x2222U, 16);
    gpu.io_write(kA + 0x50U, 0x0241U, 16);
    gpu.io_write(kA + 0x52U, 0x0808U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFF60000U); // 30 of 31 expands to 246.

    // Brightness up on BG0's red 31 with EVY = 8 lifts the empty channels: 0 + (31 * 8) / 16 = 15.
    gpu.io_write(kA + 0x50U, 0x0081U, 16);
    gpu.io_write(kA + 0x54U, 8U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF7B7BU);
    gpu.io_write(kA + 0x50U, 0x00C1U, 16); // Brightness down: 31 - (31 * 8) / 16 = 16.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFF830000U);
}

// Display colours expand 5 bits as (c * 255) / 31, e.g. 1 -> 8, 16 -> 131, 30 -> 246.
void test_five_bit_output_expansion() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    set_bg_palette(gpu, 0U, 0x0000U);
    gpu.io_write(kA, 1U << 16U, 32);
    for (const std::uint16_t raw : {0x0001U, 0x0010U, 0x03DEU}) {
        set_bg_palette(gpu, 0U, static_cast<std::uint16_t>(raw));
        gpu.render_line(0);
        const std::uint32_t r = (raw & 31U) * 255U / 31U;
        const std::uint32_t g = ((raw >> 5U) & 31U) * 255U / 31U;
        const std::uint32_t b = ((raw >> 10U) & 31U) * 255U / 31U;
        assert(pixel(gpu.screen_top(), 0U) == (0xFF000000U | (r << 16U) | (g << 8U) | b));
    }
}

void disable_objects(dsr::Gpu2D& gpu, bool is_a = true) {
    for (unsigned object = 0; object < 128U; ++object) {
        gpu.arm9_write(kOam + (is_a ? 0U : 0x400U) + object * 8U, 0x0200U, 16);
    }
}

void test_bg_mode_matrix() {
    // 0=text, 1=affine, 2=extended tile, 3=large bitmap, 4=invalid.
    constexpr unsigned types[7][4]{
        {0,0,0,0}, {0,0,0,1}, {0,0,1,1}, {0,0,0,2},
        {0,0,1,2}, {0,0,2,2}, {4,4,3,4}};
    for (bool is_a : {true, false}) {
        for (unsigned mode = 0; mode <= 6; ++mode) {
            for (unsigned bg = 0; bg < 4; ++bg) {
                dsr::Gpu2D gpu;
                set_power(gpu);
                const auto io = is_a ? kA : kA + 0x1000U;
                const auto vram = is_a ? kVram : kVram + 0x200000U;
                const auto palette = is_a ? kPalette : kPalette + 0x400U;
                gpu.io_write(is_a ? kVramA : 0x04000242U, is_a ? 0x81U : 0x84U, 8);
                gpu.arm9_write(palette, 0x7C00U, 16);
                gpu.arm9_write(palette + 2U, 0x001FU, 16);
                gpu.arm9_write(palette + 4U, 0x03E0U, 16);
                gpu.io_write(io, (1U << 16U) | (1U << (8U + bg)) | mode, 32);
                const auto type = types[mode][bg];
                write16(gpu, io + 8U + bg * 2U, type == 3 ? 0U : 4U);
                gpu.arm9_write(vram + 0x4000U, type == 0 ? 0x0021U : 0x0201U, 16);
                if (type == 3) gpu.arm9_write(vram, 0x0201U, 16);
                const auto param = bg == 2 ? 0x20U : 0x30U;
                write16(gpu, io + param, 0x100U);
                gpu.render_line(0);
                const auto frame = is_a ? gpu.screen_top() : gpu.screen_bottom();
                const bool invalid = type == 4 || (!is_a && mode == 6);
                assert(pixel(frame, 0) == (invalid ? 0xFF0000FFU : 0xFFFF0000U));
                if (!invalid) assert(pixel(frame, 1) == 0xFF00FF00U);
            }
        }
    }
}

void test_text_screen_blocks_and_flips() {
    for (unsigned size = 0; size < 4; ++size) {
        dsr::Gpu2D gpu;
        setup_text_bg0(gpu);
        write16(gpu, kA + 8U, static_cast<std::uint16_t>(4U | (size << 14U)));
        const unsigned x = (size & 1U) ? 256U : 0U;
        const unsigned y = (size & 2U) ? 256U : 0U;
        const unsigned block = (x / 256U) + (y / 256U) * ((size & 1U) ? 2U : 1U);
        gpu.arm9_write(kVram + block * 0x800U, 0x1C00U, 16); // Both flips, palette 1.
        gpu.arm9_write(kVram + 0x4000U + 31U - 1U, 0x2000U, 16); // Last pixel, high nibble 2.
        set_bg_palette(gpu, 18U, 0x03E0U);
        write16(gpu, kA + 0x10U, x);
        write16(gpu, kA + 0x12U, y);
        gpu.render_line(0);
        assert(pixel(gpu.screen_top(), 0) == 0xFF00FF00U);
    }
}

void test_text_extended_palette_pages_and_slots() {
    for (bool is_a : {true, false}) {
        for (unsigned bg = 0; bg < 4; ++bg) {
            for (unsigned mirror = 0; mirror < (bg < 2 ? 2U : 1U); ++mirror) {
                dsr::Gpu2D gpu;
                set_power(gpu);
                const auto io = is_a ? kA : kA + 0x1000U;
                const auto bank = is_a ? kVramE : 0x04000248U;
                const auto lcdc = is_a ? 0x06880000U : 0x06898000U;
                const unsigned slot = bg + mirror * 2U;
                gpu.io_write(bank, 0x80U, 8);
                gpu.arm9_write(lcdc + slot * 0x2000U + 3U * 512U + 2U, 0x03E0U, 16);
                gpu.io_write(bank, is_a ? 0x84U : 0x82U, 8);
                gpu.io_write(is_a ? kVramA : 0x04000242U, is_a ? 0x81U : 0x84U, 8);
                const auto vram = is_a ? kVram : kVram + 0x200000U;
                gpu.arm9_write(vram, 0x3000U, 16);
                gpu.arm9_write(vram + 0x4000U, 0x0101U, 16);
                gpu.io_write(io, (1U << 16U) | (1U << 30U) | (1U << (8U + bg)), 32);
                write16(gpu, io + 8U + bg * 2U, static_cast<std::uint16_t>(0x84U | (mirror << 13U)));
                gpu.render_line(0);
                assert(pixel(is_a ? gpu.screen_top() : gpu.screen_bottom(), 0) == 0xFF00FF00U);
            }
        }
    }
}

void test_bitmap_dimensions_bases_and_palette() {
    for (bool is_a : {true, false}) {
        for (bool direct : {true, false}) {
            for (unsigned size = 0; size < 4; ++size) {
                dsr::Gpu2D gpu;
                set_power(gpu);
                const auto io = is_a ? kA : kA + 0x1000U;
                const auto vram = is_a ? kVram : kVram + 0x200000U;
                gpu.io_write(is_a ? kVramA : 0x04000242U, is_a ? 0x81U : 0x84U, 8);
                gpu.arm9_write((is_a ? kPalette : kPalette + 0x400U) + 2U, 0x03E0U, 16);
                // Global tile screen base must not move bitmaps. Extended palettes do not apply.
                gpu.io_write(io, (1U << 16U) | (1U << 11U) | (1U << 30U) | (1U << 27U) | 3U, 32);
                write16(gpu, io + 0x0EU, static_cast<std::uint16_t>(0x180U | (direct ? 4U : 0U) | (size << 14U)));
                const unsigned width = size == 0 ? 128U : size == 1 ? 256U : 512U;
                const unsigned height = size >= 2 ? (size == 2 ? 256U : 512U) : width;
                gpu.io_write(io + 0x3CU, 1U << 8U, 32); // Source row 1.
                gpu.arm9_write(vram + 0x4000U + width * (direct ? 2U : 1U), direct ? 0x83E0U : 0x0101U, 16);
                gpu.render_line(0);
                assert(pixel(is_a ? gpu.screen_top() : gpu.screen_bottom(), 0) == 0xFF00FF00U);
                gpu.io_write(io + 0x3CU, height << 8U, 32);
                gpu.render_line(0);
                assert(pixel(is_a ? gpu.screen_top() : gpu.screen_bottom(), 0) == 0xFF000000U);
            }
        }
    }
}

void test_large_bitmap_ignores_tile_base_and_3d_requires_enable() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D fake;
    gpu.attach_gpu3d(&fake);
    fake_3d_line.fill((31U << 24U) | 63U);
    set_power(gpu, 0x8207U);
    gpu.io_write(kVramA, 0x81U, 8);
    set_bg_palette(gpu, 0, 0x7C00U);
    set_bg_palette(gpu, 1, 0x03E0U);
    gpu.arm9_write(kVram, 0x0101U, 16);
    gpu.io_write(kA, (1U << 16U) | (1U << 27U) | (1U << 8U) | (1U << 10U) | 6U, 32);
    write16(gpu, kA + 0x0CU, 0x100U);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFF00FF00U); // Not an implicit 3D BG0.
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 3U) | 6U, 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFFFF0000U);
}

void test_obj_mapping_boundaries_and_odd_tile() {
    for (bool is_a : {true, false}) {
        for (unsigned boundary = 0; boundary < 4; ++boundary) {
            for (bool affine : {true, false}) {
                dsr::Gpu2D gpu;
                set_power(gpu);
                disable_objects(gpu, is_a);
                const auto io = is_a ? kA : kA + 0x1000U;
                const auto obj = is_a ? 0x06400000U : 0x06600000U;
                const auto oam = kOam + (is_a ? 0U : 0x400U);
                gpu.io_write(is_a ? kVramB : 0x04000243U, is_a ? 0x82U : 0x84U, 8);
                gpu.arm9_write((is_a ? kPalette : kPalette + 0x400U) + 0x202U, 0x03E0U, 16);
                gpu.arm9_write(obj + (32U << boundary), 0x0101U, 16); // Odd tile 1 is valid.
                gpu.arm9_write(oam, 0x2000U | (affine ? 0x100U : 0U), 16);
                gpu.arm9_write(oam + 2U, 0U, 16);
                gpu.arm9_write(oam + 4U, 1U, 16);
                gpu.arm9_write(oam + 6U, 0x100U, 16);
                gpu.arm9_write(oam + 30U, 0x100U, 16);
                gpu.io_write(io, (1U << 16U) | (1U << 12U) | (1U << 4U) | (boundary << 20U), 32);
                gpu.render_line(0);
                assert(pixel(is_a ? gpu.screen_top() : gpu.screen_bottom(), 0) == 0xFF00FF00U);
            }
        }
    }
}

void test_bitmap_obj_alpha_and_mapping() {
    for (bool one_d : {true, false}) {
        for (bool wide : {true, false}) {
            dsr::Gpu2D gpu;
            set_power(gpu);
            disable_objects(gpu);
            gpu.io_write(kVramB, 0x82U, 8);
            set_bg_palette(gpu, 0U, 0x7C00U);
            const unsigned tile = 33U;
            const unsigned base = one_d ? tile * (wide ? 256U : 128U)
                : (tile & (wide ? 31U : 15U)) * 16U + (tile & ~(wide ? 31U : 15U)) * 128U;
            gpu.arm9_write(0x06400000U + base, 0x801FU, 16);
            gpu.arm9_write(kOam, 0x0C00U, 16);
            gpu.arm9_write(kOam + 2U, 0U, 16);
            gpu.arm9_write(kOam + 4U, tile | (7U << 12U), 16);
            gpu.io_write(kA, (1U << 16U) | (1U << 12U) | (one_d ? 1U << 6U : 0U) |
                (wide ? (one_d ? 1U << 22U : 1U << 5U) : 0U), 32);
            write16(gpu, kA + 0x50U, 1U << 13U); // Forced OBJ alpha over backdrop, effect disabled.
            write16(gpu, kA + 0x52U, 0x0010U); // Deliberately unrelated EVA/EVB.
            gpu.render_line(0);
            assert(pixel(gpu.screen_top(), 0) == 0xFF7B007BU); // Palette 7 -> EVA 8, EVB 8.
            gpu.arm9_write(kOam + 4U, tile, 16); // Alpha 0 disables the bitmap OBJ.
            gpu.render_line(0);
            assert(pixel(gpu.screen_top(), 0) == 0xFF0000FFU);
        }
    }
}

void test_bg_mosaic_holds_rendered_scanline() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    write16(gpu, kA + 8U, 0x44U);
    write16(gpu, kA + 0x4CU, 0x11U); // 2 x 2.
    gpu.render_line(0);
    set_bg_palette(gpu, 1U, 0x03E0U);
    gpu.render_line(1);
    assert(pixel(gpu.screen_top(), 0, 1) == 0xFFFF0000U);
    gpu.arm9_write(kVram + 0x4008U, 0x1111U, 16); // Tile row 2.
    gpu.render_line(2);
    assert(pixel(gpu.screen_top(), 0, 2) == 0xFF00FF00U);
}

void test_obj_mosaic_screen_grid() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    disable_objects(gpu);
    gpu.io_write(kVramB, 0x82U, 8);
    gpu.io_write(kA, (1U << 16U) | (1U << 12U), 32);
    write16(gpu, kA + 0x4CU, 1U << 8U);
    gpu.arm9_write(kOam, 0x1000U, 16);
    gpu.arm9_write(kOam + 2U, 1U, 16); // Positioned at x=1, not a mosaic block boundary.
    gpu.arm9_write(kOam + 4U, 0U, 16);
    gpu.arm9_write(kPalette + 0x202U, 0x001FU, 16);
    gpu.arm9_write(kPalette + 0x204U, 0x03E0U, 16);
    gpu.arm9_write(0x06400000U, 0x2221U, 16);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 2) == 0xFF00FF00U);
    assert(pixel(gpu.screen_top(), 3) == 0xFF00FF00U);
}

void test_affine_hblank_updates() {
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    gpu.io_write(kA, (1U << 16U) | (1U << 11U) | 3U, 32);
    write16(gpu, kA + 0x0EU, 0x80U);
    write16(gpu, kA + 0x36U, 0x100U);
    set_bg_palette(gpu, 1, 0x001FU);
    set_bg_palette(gpu, 2, 0x03E0U);
    set_bg_palette(gpu, 3, 0x7C00U);
    gpu.arm9_write(kVram, 0x0101U, 16);
    gpu.arm9_write(kVram + 128U, 0x0202U, 16);
    gpu.arm9_write(kVram + 384U, 0x0303U, 16);
    gpu.render_line(0);
    write16(gpu, kA + 0x36U, 0x200U); // HBlank: line 0 already advanced by old PD.
    gpu.render_line(1);
    assert(pixel(gpu.screen_top(), 0, 1) == 0xFF00FF00U);
    gpu.io_write(kA + 0x38U, 0U, 32); // Writing X must not reset the advanced Y.
    gpu.render_line(2);
    assert(pixel(gpu.screen_top(), 0, 2) == 0xFF0000FFU);
    // Ordinary scroll, window and master-brightness writes also affect only later lines.
    write16(gpu, kA + 0x6CU, 0x801FU);
    gpu.render_line(3);
    assert(pixel(gpu.screen_top(), 0, 3) == 0xFF000000U);
    assert(pixel(gpu.screen_top(), 0, 1) == 0xFF00FF00U);
}

void test_vram_mirrors() {
    dsr::Gpu2D gpu;
    gpu.io_write(0x04000245U, 0x81U, 8);
    gpu.arm9_write(kVram, 0x1234U, 16);
    assert(gpu.arm9_read(kVram + 0x8000U, 16) == 0x1234U);
    assert(gpu.arm9_read(kVram + 0x80000U, 16) == 0x1234U);
    assert(gpu.arm9_vram_page(kVram + 0x80000U) == gpu.arm9_vram_page(kVram));
    gpu.arm9_write(kVram + 0x88000U, 0x5678U, 16);
    assert(gpu.arm9_read(kVram, 16) == 0x5678U);
    gpu.io_write(0x04000248U, 0x81U, 8);
    gpu.io_write(0x04000249U, 0x81U, 8);
    gpu.arm9_write(0x06200000U, 0xAA55U, 16);
    gpu.arm9_write(0x06208000U, 0x55AAU, 16);
    assert(gpu.arm9_read(0x06210000U, 16) == 0xAA55U);
    for (auto addr : {0x0620C000U, 0x06218000U, 0x0621C000U, 0x06228000U}) {
        assert(gpu.arm9_read(addr, 16) == 0x55AAU);
    }
    gpu.io_write(0x04000249U, 0x82U, 8);
    gpu.arm9_write(0x06600000U, 0x4321U, 16);
    assert(gpu.arm9_read(0x06604000U, 16) == 0x4321U);
    assert(gpu.arm9_read(0x06620000U, 16) == 0x4321U);
    gpu.io_write(0x04000242U, 0x82U, 8); // Bank C for ARM7, all 16 MiB aliases.
    gpu.arm7_write(0x06800000U, 0x5AU, 8);
    for (auto addr : {0x06000000U, 0x06400000U, 0x06800000U, 0x06FC0000U}) {
        assert(gpu.arm7_read(addr, 8) == 0x5AU);
        assert(gpu.arm7_vram_page(addr) == gpu.arm7_vram_page(kVram));
    }
}

void test_capture_uses_compositor_not_display_mode() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        dsr::Gpu2D gpu;
        set_power(gpu);
        gpu.io_write(kVramA, 0x80U, 8);
        set_bg_palette(gpu, 0, 0x001FU);
        gpu.io_write(kA, mode << 16U, 32);
        gpu.io_write(kA + 0x6CU, 0x8010U, 16); // Capture is before master brightness.
        gpu.io_write(kA + 0x64U, 0x80340000U, 32); // A, 256x192, write offset 1.
        gpu.render_line(0);
        assert(gpu.arm9_read(kLcdcA + 0x8000U, 16) == 0x801FU);
    }
}

void test_capture_fifo_consumption_and_alpha() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.io_write(kVramA, 0x80U, 8);
    gpu.io_write(kA, 3U << 16U, 32);
    for (unsigned pair = 0; pair < 256U; ++pair) {
        gpu.io_write(kA + 0x68U, pair < 128U ? 0x83E0801FU : 0xFC00FC00U, 32);
    }
    gpu.io_write(kA + 0x64U, 0xA2300000U, 32); // Source B FIFO, 256x192.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFFFF0000U);
    assert(gpu.arm9_read(kLcdcA, 16) == 0xFC00U); // Display and capture each drain a line.
    assert(gpu.arm9_read(kLcdcA + 2U, 16) == 0xFC00U);
    gpu.render_line(1);
    assert(gpu.arm9_read(kLcdcA + 512U, 16) == 0U);
    gpu.vblank_start();
    gpu.io_write(kA, 1U << 16U, 32);
    for (unsigned pair = 0; pair < 256U; ++pair) {
        gpu.io_write(kA + 0x68U, pair < 128U ? 0x001F001FU : 0xFC00FC00U, 32);
    }
    gpu.io_write(kA + 0x64U, 0xA2000000U, 32); // 128x128 FIFO copy.
    gpu.render_line(0);
    assert(gpu.arm9_read(kLcdcA, 16) == 0x001FU);
    gpu.render_line(1);
    assert(gpu.arm9_read(kLcdcA + 256U, 16) == 0xFC00U); // 128-wide capture still drains 256 pixels.
}

void test_capture_vram_copy_sets_alpha() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.io_write(kVramA, 0x80U, 8);
    gpu.io_write(kVramB, 0x80U, 8);
    gpu.arm9_write(0x06828000U, 0x03E0U, 16);
    gpu.io_write(kA, (1U << 16U) | (1U << 18U), 32);
    gpu.io_write(kA + 0x64U, 0xA4000000U, 32); // Source B bank B, read offset 1.
    gpu.render_line(0);
    assert(gpu.arm9_read(kLcdcA, 16) == 0x83E0U);
}

void test_engine_b_display_mode_masks_reserved_bit() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.arm9_write(kPalette + 0x400U, 0x03E0U, 16);
    gpu.io_write(kA + 0x1000U, 3U << 16U, 32);
    gpu.render_line(0);
    assert(pixel(gpu.screen_bottom(), 0) == 0xFF00FF00U);
}

void test_small_bank_palette_slots_and_reserved_offsets() {
    for (unsigned bank = 5; bank <= 6; ++bank) {
        for (unsigned offset = 0; offset < 4; ++offset) {
            dsr::Gpu2D gpu;
            setup_text_bg0(gpu);
            const auto control = 0x04000240U + bank;
            const auto lcdc = bank == 5 ? 0x06890000U : 0x06894000U;
            gpu.io_write(control, 0x80U, 8);
            gpu.arm9_write(lcdc + 2U, 0x03E0U, 16);
            gpu.io_write(control, 0x84U | (offset << 3U), 8);
            gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 30U), 32);
            write16(gpu, kA + 8U, 0x84U | ((offset & 1U) << 13U));
            gpu.arm9_write(kVram + 0x4000U, 0x0101U, 16);
            gpu.render_line(0);
            assert(pixel(gpu.screen_top(), 0) == (offset < 2U ? 0xFF00FF00U : 0xFF000000U));
        }
    }
}

void test_obj_extended_palettes() {
    for (bool is_a : {true, false}) {
        dsr::Gpu2D gpu;
        set_power(gpu);
        disable_objects(gpu, is_a);
        const auto control = is_a ? 0x04000245U : 0x04000249U;
        const auto lcdc = is_a ? 0x06890000U : 0x068A0000U;
        const auto obj = is_a ? 0x06400000U : 0x06600000U;
        const auto oam = kOam + (is_a ? 0U : 0x400U);
        const auto io = kA + (is_a ? 0U : 0x1000U);
        gpu.io_write(control, 0x80U, 8);
        gpu.arm9_write(lcdc + 3U * 512U + 2U, 0x03E0U, 16);
        gpu.io_write(control, is_a ? 0x85U : 0x83U, 8);
        gpu.io_write(is_a ? kVramB : 0x04000243U, is_a ? 0x82U : 0x84U, 8);
        gpu.arm9_write(obj, 0x0101U, 16);
        gpu.arm9_write(oam, 0x2000U, 16);
        gpu.arm9_write(oam + 2U, 0U, 16);
        gpu.arm9_write(oam + 4U, 0x3000U, 16);
        gpu.io_write(io, (1U << 16U) | (1U << 12U) | (1U << 31U), 32);
        gpu.render_line(0);
        assert(pixel(is_a ? gpu.screen_top() : gpu.screen_bottom(), 0) == 0xFF00FF00U);
    }
}

void test_affine_double_size_and_obj_2d() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    disable_objects(gpu);
    gpu.io_write(kVramB, 0x82U, 8);
    gpu.io_write(kA, (1U << 16U) | (1U << 12U), 32); // 2D map.
    gpu.arm9_write(kPalette + 0x202U, 0x001FU, 16);
    gpu.arm9_write(kPalette + 0x204U, 0x03E0U, 16);
    gpu.arm9_write(0x06400000U, 0x1111U, 16);
    gpu.arm9_write(0x06400400U, 0x2222U, 16); // Second tile row has a 1024-byte stride.
    gpu.arm9_write(kOam, 0U, 16);
    gpu.arm9_write(kOam + 2U, 0x4000U, 16); // 16x16.
    gpu.render_line(8);
    assert(pixel(gpu.screen_top(), 0, 8) == 0xFF00FF00U);
    gpu.arm9_write(kOam, 0x0300U, 16); // Affine, double-size bounding box.
    gpu.arm9_write(kOam + 2U, 0U, 16); // 8x8 image in 16x16 bounding box.
    gpu.arm9_write(kOam + 6U, 0x100U, 16);
    gpu.arm9_write(kOam + 30U, 0x100U, 16);
    gpu.render_line(4);
    assert(pixel(gpu.screen_top(), 3, 4) == 0xFF000000U);
    assert(pixel(gpu.screen_top(), 4, 4) == 0xFFFF0000U);
}

void test_3d_scroll_window_and_blend_clamp() {
    dsr::Gpu2D gpu;
    dsr::Gpu3D fake;
    gpu.attach_gpu3d(&fake);
    fake_3d_line.fill((16U << 24U) | 63U);
    set_power(gpu, 0x8207U);
    set_bg_palette(gpu, 0, 0x7C00U);
    gpu.io_write(kA, (1U << 16U) | (1U << 8U) | (1U << 3U) | (1U << 13U), 32);
    write16(gpu, kA + 0x40U, 0x00FFU);
    write16(gpu, kA + 0x44U, 0x00C0U);
    write16(gpu, kA + 0x48U, 1U); // Window allows BG0 but forbids effects.
    write16(gpu, kA + 0x4AU, 1U);
    write16(gpu, kA + 0x50U, 1U << 13U); // Forced 3D blend ignores window effects flag.
    write16(gpu, kA + 0x10U, 1U);
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFF830073U);
    assert(pixel(gpu.screen_top(), 255) == 0xFF0000FFU); // 3D wrap is 512 pixels, not 256.
    write16(gpu, kA + 0x10U, 0U);
    write16(gpu, kA + 0x48U, 0U); // Masked 3D contributes nothing.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFF0000FFU);
    setup_text_bg0(gpu);
    write16(gpu, kA + 0x50U, 0x2041U);
    write16(gpu, kA + 0x52U, 0x1F1FU); // Factors clamp to 16, saturating red + blue.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0) == 0xFFFF00FFU);
}

// The original scalar colour/paint path, deliberately independent of the
// renderer's lookup tables and front-to-back selection.
static std::uint32_t reference_argb(std::uint16_t raw, std::uint16_t master) {
    const auto mode = (master >> 14U) & 3U;
    const unsigned factor = std::min<unsigned>(master & 31U, 16U);
    const auto channel = [&](unsigned value) {
        if (mode == 1U) {
            value += ((31U - value) * factor) / 16U;
        } else if (mode == 2U) {
            value -= (value * factor) / 16U;
        }
        return (value * 255U) / 31U;
    };
    return 0xFF000000U | channel(raw & 31U) << 16U |
        channel((raw >> 5U) & 31U) << 8U | channel((raw >> 10U) & 31U);
}

static std::uint16_t reference_effect(std::uint16_t source, std::uint16_t target,
                                     unsigned effect, unsigned a, unsigned b) {
    std::uint16_t result = 0U;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        unsigned value = (source >> shift) & 31U;
        const unsigned below = (target >> shift) & 31U;
        if (effect == 1U) {
            value = std::min((value * std::min(a, 16U) + below * std::min(b, 16U)) / 16U, 31U);
        } else if (effect == 2U) {
            value += (31U - value) * std::min(a, 16U) / 16U;
        } else if (effect == 3U) {
            value -= value * std::min(a, 16U) / 16U;
        }
        result |= static_cast<std::uint16_t>(value << shift);
    }
    return result;
}

struct ReferenceTextState {
    std::array<std::uint8_t, 0x8000> vram{};
    std::array<std::uint16_t, 256> palette{};
    std::array<std::uint16_t, 4> cnt{}, hofs{}, vofs{};
    std::array<std::uint16_t, 2> horizontal{}, vertical{};
    std::uint32_t dispcnt = 0U;
    std::uint16_t winin = 0U, winout = 0U, blend = 0U, alpha = 0U, evy = 0U, master = 0U;
};

static bool reference_range(unsigned coordinate, std::uint16_t bounds) {
    const unsigned start = bounds >> 8U;
    const auto end = bounds & 255U;
    return start <= end ? coordinate >= start && coordinate < end
        : coordinate >= start || coordinate < end;
}

static std::array<std::uint32_t, 256> reference_text_line(const ReferenceTextState& state, unsigned y) {
    std::array<std::uint32_t, 256> result{};
    const unsigned effect = (state.blend >> 6U) & 3U;
    const unsigned target1 = state.blend & 63U;
    const unsigned target2 = (state.blend >> 8U) & 63U;
    auto backdrop = state.palette[0];
    if ((target1 & 32U) != 0U && effect >= 2U) {
        backdrop = reference_effect(backdrop, 0U, effect, state.evy & 31U, 0U);
    }
    for (unsigned x = 0; x < 256; ++x) {
        unsigned mask = 63U;
        if ((state.dispcnt & 0xE000U) != 0U) {
            mask = state.winout & 63U;
            for (int window = 1; window >= 0; --window) {
                if ((state.dispcnt & (1U << (13 + window))) != 0U &&
                    reference_range(x, state.horizontal[window]) &&
                    reference_range(y, state.vertical[window])) {
                    mask = (state.winin >> (window * 8)) & 63U;
                }
            }
        }
        auto dst = backdrop;
        unsigned dst_layer = 5U;
        for (int priority = 3; priority >= 0; --priority) {
            for (int bg = 3; bg >= 0; --bg) {
                if ((state.dispcnt & (1U << (8 + bg))) == 0U ||
                    (mask & (1U << bg)) == 0U || (state.cnt[bg] & 3U) != unsigned(priority)) {
                    continue;
                }
                unsigned px = (x + state.hofs[bg]) & 255U;
                unsigned py = (y + state.vofs[bg]) & 255U;
                const unsigned map = bg * 0x800U + ((py / 8U) * 32U + px / 8U) * 2U;
                const unsigned entry = state.vram[map] | (state.vram[map + 1] << 8U);
                unsigned lx = px & 7U, ly = py & 7U;
                if ((entry & 0x400U) != 0U) lx = 7U - lx;
                if ((entry & 0x800U) != 0U) ly = 7U - ly;
                unsigned index = state.vram[0x4000U + (entry & 1023U) * 32U + ly * 4U + lx / 2U];
                index = (index >> ((lx & 1U) * 4U)) & 15U;
                if (index == 0U) continue;
                const auto color = state.palette[((entry >> 12U) & 15U) * 16U + index];
                const bool dst_target = dst_layer != unsigned(bg) && (target2 & (1U << dst_layer)) != 0U;
                if ((mask & 32U) != 0U && (target1 & (1U << bg)) != 0U &&
                    (effect >= 2U || (effect == 1U && dst_target))) {
                    dst = reference_effect(color, dst, effect,
                        effect == 1U ? state.alpha & 31U : state.evy & 31U, (state.alpha >> 8U) & 31U);
                } else {
                    dst = color;
                }
                dst_layer = unsigned(bg);
            }
        }
        result[x] = reference_argb(dst, state.master);
    }
    return result;
}

void test_random_text_reference() {
    std::mt19937 random(0xA211U);
    dsr::Gpu2D gpu;
    gpu.io_write(kVramA, 0x81U, 8);
    set_power(gpu);
    ReferenceTextState state;
    for (unsigned trial = 0; trial < 200; ++trial) {
        state.dispcnt = (1U << 16U) | ((random() & 15U) << 8U) | ((random() & 3U) << 13U);
        state.winin = random(); state.winout = random();
        state.blend = trial % 2 ? 0U : random();
        state.alpha = random(); state.evy = random(); state.master = random();
        gpu.io_write(kA, state.dispcnt, 32);
        write16(gpu, kA + 0x48U, state.winin); write16(gpu, kA + 0x4AU, state.winout);
        write16(gpu, kA + 0x50U, state.blend); write16(gpu, kA + 0x52U, state.alpha);
        write16(gpu, kA + 0x54U, state.evy); write16(gpu, kA + 0x6CU, state.master);
        for (unsigned window = 0; window < 2; ++window) {
            state.horizontal[window] = random(); state.vertical[window] = random();
            write16(gpu, kA + 0x40U + window * 2U, state.horizontal[window]);
            write16(gpu, kA + 0x44U + window * 2U, state.vertical[window]);
        }
        for (unsigned bg = 0; bg < 4; ++bg) {
            state.cnt[bg] = 4U | (bg << 8U) | (random() & 3U);
            state.hofs[bg] = random() & 511U; state.vofs[bg] = random() & 511U;
            write16(gpu, kA + 8U + bg * 2U, state.cnt[bg]);
            write16(gpu, kA + 0x10U + bg * 4U, state.hofs[bg]);
            write16(gpu, kA + 0x12U + bg * 4U, state.vofs[bg]);
            for (unsigned offset = bg * 0x800U; offset < (bg + 1) * 0x800U; offset += 2) {
                const unsigned entry = (random() & 31U) | (random() & 0xFC00U);
                state.vram[offset] = entry; state.vram[offset + 1] = entry >> 8U;
                gpu.arm9_write(kVram + offset, entry, 16);
            }
        }
        for (unsigned offset = 0x4000U; offset < 0x4400U; offset += 2) {
            const auto value = random() & 65535U;
            state.vram[offset] = value; state.vram[offset + 1] = value >> 8U;
            gpu.arm9_write(kVram + offset, value, 16);
        }
        for (unsigned index = 0; index < 256; ++index) {
            state.palette[index] = random() & 0x7FFFU;
            set_bg_palette(gpu, index, state.palette[index]);
        }
        const unsigned y = random() % 192U;
        const auto expected = reference_text_line(state, y);
        gpu.render_line(y);
        for (unsigned x = 0; x < 256; ++x) assert(pixel(gpu.screen_top(), x, y) == expected[x]);
    }
}

// Original per-pixel text sampling, without row loads or format specialization.
static std::uint16_t reference_text_sample(dsr::Gpu2D& gpu, unsigned x, unsigned y,
        std::uint32_t dispcnt, std::uint16_t cnt, unsigned hofs, unsigned vofs,
        const std::array<std::uint16_t, 256>& palette,
        const std::array<std::array<std::uint16_t, 4096>, 3>& extended, unsigned banks) {
    constexpr std::array<std::array<unsigned, 2>, 4> sizes{{{256, 256}, {512, 256},
                                                         {256, 512}, {512, 512}}};
    const auto size = sizes[cnt >> 14U];
    const unsigned px = (x + hofs) % size[0], py = (y + vofs) % size[1];
    const unsigned tile_x = px / 8U, tile_y = py / 8U;
    const unsigned block = tile_x / 32U + tile_y / 32U * (size[0] / 256U);
    const unsigned screen_base = ((cnt >> 8U) & 31U) * 0x800U +
                                 ((dispcnt >> 27U) & 7U) * 0x10000U;
    const auto entry = gpu.arm9_read(kVram + screen_base + block * 0x800U +
                                    ((tile_y % 32U) * 32U + tile_x % 32U) * 2U, 16);
    unsigned lx = px % 8U, ly = py % 8U;
    if ((entry & 0x400U) != 0U) lx = 7U - lx;
    if ((entry & 0x800U) != 0U) ly = 7U - ly;
    const bool color256 = (cnt & 0x80U) != 0U;
    const unsigned char_base = ((cnt >> 2U) & 15U) * 0x4000U +
                               ((dispcnt >> 24U) & 7U) * 0x10000U;
    auto index = gpu.arm9_read(kVram + char_base + (entry & 1023U) * (color256 ? 64U : 32U) +
                              ly * (color256 ? 8U : 4U) + lx / (color256 ? 1U : 2U), 8);
    if (!color256) index = (index >> ((lx % 2U) * 4U)) & 15U;
    if (index == 0U) return palette[0];
    const bool ext = color256 && (dispcnt & (1U << 30U)) != 0U;
    const unsigned palette_index = index + (color256 ? (ext ? entry >> 12U : 0U) * 256U
                                                                     : (entry >> 12U) * 16U);
    if (!ext) return palette[palette_index];
    std::uint16_t color = 0U;
    for (unsigned bank = 0; bank < banks; ++bank) color |= extended[bank][palette_index];
    return color;
}

void test_random_text_rows_reference() {
    std::mt19937 random(0x2D1210U);
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.io_write(kVramA, 0x80U, 8); gpu.io_write(kVramB, 0x80U, 8);
    for (unsigned offset = 0; offset < 0x20000U; offset += 4U) {
        gpu.arm9_write(kLcdcA + offset, random(), 32);
        gpu.arm9_write(0x06820000U + offset, random(), 32);
    }
    std::array<std::uint16_t, 256> palette{};
    for (unsigned i = 0; i < palette.size(); ++i) {
        palette[i] = random() & 0x7FFFU;
        set_bg_palette(gpu, i, palette[i]);
    }
    constexpr std::array<std::uint32_t, 3> controls{kVramE, 0x04000245U, 0x04000246U};
    constexpr std::array<std::uint32_t, 3> lcdc{0x06880000U, 0x06890000U, 0x06894000U};
    std::array<std::array<std::uint16_t, 4096>, 3> extended{};
    for (unsigned bank = 0; bank < 3; ++bank) {
        gpu.io_write(controls[bank], 0x80U, 8);
        for (unsigned i = 0; i < extended[bank].size(); ++i) {
            extended[bank][i] = random() & 0x7FFFU;
            gpu.arm9_write(lcdc[bank] + i * 2U, extended[bank][i], 16);
        }
    }
    for (unsigned trial = 0; trial < 384; ++trial) {
        const unsigned banks = trial % 4U;
        for (unsigned bank = 0; bank < 3; ++bank)
            gpu.io_write(controls[bank], bank < banks ? 0x84U : 0U, 8);
        gpu.io_write(kVramA, 0x81U, 8);
        gpu.io_write(kVramB, trial & 1U ? 0x81U : 0x89U, 8);
        const auto dispcnt = (1U << 16U) | (1U << 8U) | ((trial % 3U == 2U) ? 1U << 30U : 0U) |
            ((trial % 8U < 4U ? 0U : random() & 7U) << 24U) |
            ((trial % 8U < 4U ? 0U : random() & 7U) << 27U);
        const auto cnt = static_cast<std::uint16_t>(((trial / 3U % 4U) << 14U) |
            ((random() & 31U) << 8U) | ((trial % 3U != 0U) ? 0x80U : 0U) |
            ((trial / 12U % 16U) << 2U) | (random() & 3U));
        const unsigned hofs = random() & 511U, vofs = random() & 511U, y = random() % 192U;
        const auto master = static_cast<std::uint16_t>(random());
        gpu.io_write(kA, dispcnt, 32); write16(gpu, kA + 8U, cnt);
        write16(gpu, kA + 0x10U, hofs); write16(gpu, kA + 0x12U, vofs);
        write16(gpu, kA + 0x6CU, master);
        gpu.render_line(y);
        for (unsigned x = 0; x < 256; ++x) {
            const auto color = reference_text_sample(gpu, x, y, dispcnt, cnt, hofs, vofs,
                                                      palette, extended, banks);
            assert(pixel(gpu.screen_top(), x, y) == reference_argb(color, master));
        }
    }
}

void test_output_reference() {
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.io_write(kVramA, 0x80U, 8);
    gpu.io_write(kA, 2U << 16U, 32);
    // Exhaust all RGB555 colours for each legal brightness mode/factor.
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned factor = 0; factor < 32; ++factor) {
            const auto master = static_cast<std::uint16_t>((mode << 14U) | factor);
            write16(gpu, kA + 0x6CU, master);
            for (unsigned y = 0; y < 128; ++y) {
                for (unsigned x = 0; x < 256; ++x) {
                    gpu.arm9_write(kLcdcA + (y * 256U + x) * 2U, y * 256U + x, 16);
                }
                gpu.render_line(y);
                for (unsigned x = 0; x < 256; ++x) {
                    assert(pixel(gpu.screen_top(), x, y) == reference_argb(y * 256U + x, master));
                }
            }
        }
    }
}

// The old extended palette reader ORs every bank matching the selected slot.
static std::uint16_t reference_palette(const std::array<std::uint16_t, 3>& banks, unsigned count) {
    std::uint16_t value = 0U;
    for (unsigned i = 0; i < count; ++i) value |= banks[i];
    return value;
}

void test_random_palette_reference() {
    std::mt19937 random(0xA212U);
    constexpr std::array<std::uint32_t, 3> controls{kVramE, 0x04000245U, 0x04000246U};
    constexpr std::array<std::uint32_t, 3> lcdc{0x06880000U, 0x06890000U, 0x06894000U};
    for (bool object : {false, true}) {
        dsr::Gpu2D gpu;
        set_power(gpu);
        gpu.io_write(kVramA, 0x81U, 8);
        gpu.io_write(kVramB, 0x82U, 8);
        for (unsigned trial = 0; trial < 100; ++trial) {
            const unsigned count = object ? trial % 3U : trial % 4U;
            const unsigned page = random() & 15U;
            const auto master = static_cast<std::uint16_t>(random());
            const auto backdrop = static_cast<std::uint16_t>(random() & 0x7FFFU);
            set_bg_palette(gpu, 0U, backdrop);
            write16(gpu, kA + 0x6CU, master);
            gpu.io_write(kA, (1U << 16U) | (object ? 1U << 12U : 1U << 8U) |
                (count ? object ? 1U << 31U : 1U << 30U : 0U), 32);
            write16(gpu, kA + 8U, 0x0084U);
            for (unsigned offset = 0; offset < 0x800U; offset += 2U) {
                gpu.arm9_write(kVram + offset, page << 12U, 16);
            }
            for (unsigned sprite = 0; sprite < 128; ++sprite) {
                gpu.arm9_write(kOam + sprite * 8U, 0x0200U, 16);
            }
            gpu.arm9_write(kOam, 0x2000U, 16);
            gpu.arm9_write(kOam + 2U, 0U, 16);
            gpu.arm9_write(kOam + 4U, page << 12U, 16);
            std::array<std::uint8_t, 8> indices{};
            std::array<std::uint16_t, 256> regular{};
            std::array<std::array<std::uint16_t, 3>, 256> extended{};
            for (unsigned i = 0; i < 8; ++i) indices[i] = random() & 255U;
            // Ensure that transparency and cache hits are present in every trial.
            indices[0] = 0U; indices[2] = indices[1];
            for (unsigned i = 0; i < 256; ++i) {
                regular[i] = random() & 0x7FFFU;
                gpu.arm9_write(kPalette + (object ? 0x200U : 0U) + i * 2U, regular[i], 16);
                for (unsigned bank = 0; bank < 3; ++bank) extended[i][bank] = random() & 0x7FFFU;
            }
            // Restore the shared BG backdrop after writing BG palette entry zero.
            set_bg_palette(gpu, 0U, backdrop);
            for (unsigned bank = 0; bank < 3; ++bank) {
                gpu.io_write(controls[bank], 0x80U, 8);
                for (unsigned i = 0; i < 256; ++i) {
                    gpu.arm9_write(lcdc[bank] + (page * 256U + i) * 2U, extended[i][bank], 16);
                }
                const bool mapped = object ? bank > 0U && bank <= count : bank < count;
                gpu.io_write(controls[bank], mapped ? object ? 0x85U : 0x84U : 0U, 8);
            }
            for (unsigned i = 0; i < 8; i += 2) {
                gpu.arm9_write(object ? 0x06400000U + i : kVram + 0x4000U + i,
                    indices[i] | (indices[i + 1] << 8U), 16);
            }
            gpu.render_line(0);
            for (unsigned x = 0; x < 256; ++x) {
                const unsigned index = indices[x & 7U];
                auto color = backdrop;
                if (index != 0U && (!object || x < 8U)) {
                    auto values = extended[index];
                    if (object) values = {values[1], values[2], 0U};
                    color = count ? reference_palette(values, count) : regular[index];
                }
                assert(pixel(gpu.screen_top(), x) == reference_argb(color, master));
            }
        }
    }
}

static std::array<std::uint32_t, 256> reference_bitmap_line(
        const std::array<std::uint16_t, 65536>& a, const std::array<std::uint16_t, 65536>& b,
        bool overlap, bool wrap, int width, int height, std::int32_t ref_x, std::int32_t ref_y,
        std::int16_t pa, std::int16_t pc, std::uint16_t backdrop) {
    std::array<std::uint32_t, 256> line{};
    for (int x = 0; x < 256; ++x) {
        int sx = static_cast<int>((std::int64_t(ref_x) + std::int64_t(pa) * x) >> 8);
        int sy = static_cast<int>((std::int64_t(ref_y) + std::int64_t(pc) * x) >> 8);
        auto color = backdrop;
        if (wrap || (sx >= 0 && sy >= 0 && sx < width && sy < height)) {
            if (wrap) { sx = (sx % width + width) % width; sy = (sy % height + height) % height; }
            const unsigned index = static_cast<unsigned>(sy * width + sx);
            std::uint16_t raw = 0U;
            if (index < a.size()) raw = a[index] | (overlap ? b[index] : 0U);
            else if (!overlap && index < a.size() * 2U) raw = b[index - a.size()];
            if ((raw & 0x8000U) != 0U) color = raw & 0x7FFFU;
        }
        line[x] = reference_argb(color, 0U);
    }
    return line;
}

void test_random_bitmap_reference() {
    std::mt19937 random(0xA213U);
    dsr::Gpu2D gpu;
    set_power(gpu);
    gpu.io_write(kVramA, 0x80U, 8); gpu.io_write(kVramB, 0x80U, 8);
    std::array<std::uint16_t, 65536> a{}, b{};
    for (unsigned i = 0; i < a.size(); ++i) {
        a[i] = random(); b[i] = random();
        gpu.arm9_write(kLcdcA + i * 2U, a[i], 16);
        gpu.arm9_write(0x06820000U + i * 2U, b[i], 16);
    }
    constexpr std::array<std::array<int, 2>, 4> sizes{{{128, 128}, {256, 256}, {512, 256}, {512, 512}}};
    for (unsigned trial = 0; trial < 100; ++trial) {
        const bool overlap = trial & 1U, wrap = trial & 2U;
        const unsigned size = random() & 3U;
        const auto pa = static_cast<std::int16_t>(random()), pc = static_cast<std::int16_t>(random());
        const auto rx = (static_cast<std::int32_t>(random() % 1200U) - 600) * 256;
        const auto ry = (static_cast<std::int32_t>(random() % 1200U) - 600) * 256;
        const auto backdrop = static_cast<std::uint16_t>(random() & 0x7FFFU);
        set_bg_palette(gpu, 0U, backdrop);
        gpu.io_write(kVramA, 0x81U, 8); gpu.io_write(kVramB, overlap ? 0x81U : 0x89U, 8);
        gpu.io_write(kA, (1U << 16U) | (1U << 11U) | 3U, 32);
        write16(gpu, kA + 0x0EU, 0x84U | (size << 14U) | (wrap ? 0x2000U : 0U));
        write16(gpu, kA + 0x30U, static_cast<std::uint16_t>(pa));
        write16(gpu, kA + 0x34U, static_cast<std::uint16_t>(pc));
        gpu.io_write(kA + 0x38U, static_cast<std::uint32_t>(rx) & 0x0FFFFFFFU, 32);
        gpu.io_write(kA + 0x3CU, static_cast<std::uint32_t>(ry) & 0x0FFFFFFFU, 32);
        const auto expected = reference_bitmap_line(a, b, overlap, wrap, sizes[size][0], sizes[size][1],
                                                    rx, ry, pa, pc, backdrop);
        gpu.render_line(0);
        for (unsigned x = 0; x < 256; ++x) assert(pixel(gpu.screen_top(), x) == expected[x]);
    }
}

void test_scratch_clear_between_engines_and_lines() {
    dsr::Gpu2D gpu;
    setup_text_bg0(gpu);
    set_bg_palette(gpu, 0U, 0U);
    gpu.io_write(kVramB, 0x82U, 8);
    disable_objects(gpu);
    disable_objects(gpu, false);
    for (std::uint32_t offset = 0; offset < 32U; offset += 2U)
        gpu.arm9_write(0x06400000U + offset, 0x1111U, 16);
    gpu.arm9_write(kOam, 0U, 16);
    gpu.arm9_write(kOam + 2U, 8U, 16);
    gpu.arm9_write(kOam + 4U, 0U, 16);
    gpu.arm9_write(kPalette + 0x202U, 0x7C00U, 16);
    gpu.arm9_write(kPalette + 0x400U, 0x03E0U, 16);
    const auto display = (1U << 16U) | (1U << 12U) | (1U << 8U);
    gpu.io_write(kA, display, 32);
    gpu.io_write(kA + 0x1000U, display, 32); // Unmapped B layers must stay transparent.
    gpu.render_line(0);
    assert(pixel(gpu.screen_top(), 0U) == 0xFFFF0000U);
    assert(pixel(gpu.screen_top(), 8U) == 0xFF0000FFU);
    for (unsigned x = 0; x < 256; ++x)
        assert(pixel(gpu.screen_bottom(), x) == 0xFF00FF00U);

    for (std::uint32_t offset = 0; offset < 32U; offset += 2U)
        gpu.arm9_write(0x06400000U + offset, 0U, 16);
    gpu.render_line(1); // Text row one and the OBJ are now transparent.
    for (unsigned x = 0; x < 256; ++x) {
        assert(pixel(gpu.screen_top(), x, 1U) == 0xFF000000U);
        assert(pixel(gpu.screen_bottom(), x, 1U) == 0xFF00FF00U);
    }
}

void test_composition_order_matches_scalar() {
    std::mt19937 random(0x2046U);
    for (unsigned enabled = 0; enabled < 16; ++enabled) {
        for (unsigned packed = 0; packed < 256; ++packed) {
            std::array<std::uint8_t, 4> priorities{};
            for (unsigned bg = 0; bg < 4; ++bg) priorities[bg] = (packed >> (bg * 2)) & 3U;
            for (unsigned obj_priority = 0; obj_priority <= 4; ++obj_priority) {
                const auto actual = dsr::gpu2d_detail::composition_order(enabled, priorities, obj_priority);
                std::array<std::uint8_t, 5> expected{};
                unsigned count = 0;
                for (int priority = 3; priority >= 0; --priority) {
                    for (int bg = 3; bg >= 0; --bg) {
                        if ((enabled & (1U << bg)) && priorities[bg] == priority) expected[count++] = bg;
                    }
                    if (obj_priority == static_cast<unsigned>(priority)) expected[count++] = 4;
                }
                assert(actual.count == count);
                assert(std::equal(expected.begin(), expected.begin() + count, actual.layers.begin()));
                // Compare an order-sensitive synthetic blend with random window/opacity
                // masks; an omitted, duplicated, or reordered layer changes the result.
                const unsigned visible = random() & 31U;
                std::array<unsigned, 5> colors{};
                for (auto& color : colors) color = random() & 31U;
                unsigned scalar = random() & 31U;
                unsigned flattened = scalar;
                for (unsigned i = 0; i < count; ++i) {
                    const auto layer = expected[i];
                    if (visible & (1U << layer)) scalar = (scalar + 3U * colors[layer]) / 4U;
                }
                for (unsigned i = 0; i < actual.count; ++i) {
                    const auto layer = actual.layers[i];
                    if (visible & (1U << layer)) flattened = (flattened + 3U * colors[layer]) / 4U;
                }
                assert(flattened == scalar);
            }
        }
    }
}

} // namespace

namespace dsr {
Gpu3D::Gpu3D() : impl_(nullptr) {}
Gpu3D::~Gpu3D() = default;
const std::uint32_t* Gpu3D::line(int y) const {
    return y >= 0 && y < 192 ? fake_3d_line.data() : nullptr;
}
} // namespace dsr

int main(int argc, char** argv) {
    const struct { const char* name; void (*run)(); } tests[]{
        {"composition_order", test_composition_order_matches_scalar},
        {"mode6_3d_enable", test_mode_six_does_not_force_bg0_3d},
        {"text", test_text_bg_and_palette_write_rules},
        {"affine", test_affine_rotation},
        {"indexed_bitmap", test_extended_256_color_bitmap},
        {"direct_bitmap", test_extended_direct_color_bitmap},
        {"large_bitmap", test_large_bitmap_mode},
        {"display_off", test_mode_zero_renders_the_off_display_backdrop},
        {"engine_b", test_engine_b_text},
        {"banks_hi", test_bank_h_and_i_engine_b_bg_mapping},
        {"obj_priority", test_obj_priority},
        {"windows", test_window_masking},
        {"obj_window", test_obj_window_masking},
        {"alpha", test_alpha_blending},
        {"brightness_swap", test_master_brightness_and_screen_swap},
        {"display_modes", test_display_modes_and_fifo},
        {"capture", test_capture_to_lcdc_bank},
        {"vram_overlap", test_vram_overlap_and_arm7_mapping},
        {"extended_palette", test_extended_palette_lookup},
        {"3d_alpha", test_3d_layer_alpha_blending},
        {"3d_forced_alpha", test_3d_layer_blends_over_second_target_without_effect},
        {"color_arithmetic", test_blend_and_brightness_use_five_bit_channels},
        {"color_expansion", test_five_bit_output_expansion},
        {"bg_modes", test_bg_mode_matrix},
        {"text_sizes_flips", test_text_screen_blocks_and_flips},
        {"palette_pages_slots", test_text_extended_palette_pages_and_slots},
        {"bitmap_dimensions", test_bitmap_dimensions_bases_and_palette},
        {"large_base_3d", test_large_bitmap_ignores_tile_base_and_3d_requires_enable},
        {"obj_boundaries", test_obj_mapping_boundaries_and_odd_tile},
        {"bitmap_obj", test_bitmap_obj_alpha_and_mapping},
        {"bg_mosaic", test_bg_mosaic_holds_rendered_scanline},
        {"obj_mosaic", test_obj_mosaic_screen_grid},
        {"hblank", test_affine_hblank_updates},
        {"vram_mirrors", test_vram_mirrors},
        {"capture_display_source", test_capture_uses_compositor_not_display_mode},
        {"capture_fifo", test_capture_fifo_consumption_and_alpha},
        {"capture_vram_alpha", test_capture_vram_copy_sets_alpha},
        {"engine_b_mode", test_engine_b_display_mode_masks_reserved_bit},
        {"small_palette_slots", test_small_bank_palette_slots_and_reserved_offsets},
        {"obj_palette", test_obj_extended_palettes},
        {"double_size_2d", test_affine_double_size_and_obj_2d},
        {"3d_scroll_windows", test_3d_scroll_window_and_blend_clamp},
        {"random_text_reference", test_random_text_reference},
        {"random_text_rows_reference", test_random_text_rows_reference},
        {"output_reference", test_output_reference},
        {"random_palette_reference", test_random_palette_reference},
        {"random_bitmap_reference", test_random_bitmap_reference},
        {"scratch_clear", test_scratch_clear_between_engines_and_lines},
    };
    bool found = false;
    for (const auto& test : tests) {
        if (argc == 1 || std::strcmp(argv[1], test.name) == 0) {
            test.run();
            found = true;
        }
    }
    return found ? 0 : 1;
}
