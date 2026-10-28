#include "dsr/machine.h"
#include "dsr/gpu3d.h"
#include "dsr/render_frame.h"
#include <array>

#include <cstdint>
#include <iostream>

namespace {
constexpr auto arm9 = dsr::CpuId::Arm9;
constexpr std::uint64_t line_ticks = 2130;
int failures = 0;
void expect(bool value, const char* rule) {
    if (!value) { ++failures; std::cerr << "FAIL: " << rule << '\n'; }
}
void command(dsr::Machine& machine, unsigned code, std::uint32_t value) {
    machine.write32(arm9, 0x04000440U + (code - 0x10U) * 4U, value);
}
void triangle(dsr::Machine& machine) {
    command(machine, 0x2a, 7U << 26); // direct RGB555; all texture coordinates 0
    command(machine, 0x29, 0xc0U | (31U << 16));
    command(machine, 0x20, 0x7fff);
    command(machine, 0x40, 0);
    command(machine, 0x23, 0xf000f000); command(machine, 0x23, 0);
    command(machine, 0x23, 0xf0001000); command(machine, 0x23, 0);
    command(machine, 0x23, 0x10000000); command(machine, 0x23, 0);
    command(machine, 0x41, 0); command(machine, 0x50, 0);
}
void test_render_boundary_and_recurrence() {
    dsr::Machine machine({});
    unsigned captures = 0;
    machine.write16(arm9, 0x04000004, (214U << 8) | (1U << 5));
    machine.gpu3d().on_render_complete = [&] {
        ++captures;
        const auto status = machine.read16(arm9, 0x04000004);
        expect(machine.read16(arm9, 0x04000006) == 214,
               "render completion observes line 214");
        expect((status & 7U) == 5U,
               "line compare and VBlank are set and HBlank cleared before render");
        expect((machine.read32(arm9, 0x04000214) & (1U << 2)) != 0,
               "line compare IRQ is raised before render completion");
    };
    expect(machine.read16(arm9, 0x04000006) == 0, "reset starts at line 0");
    for (unsigned frame = 0; frame < 3; ++frame) {
        machine.run_ticks(213 * line_ticks);
        expect(captures == frame, "no render through start of line 213");
        expect(machine.read16(arm9, 0x04000006) == 213, "reaches line 213");
        machine.run_ticks(1605);
        expect((machine.read16(arm9, 0x04000004) & 2U) == 0,
               "line 213 HBlank is clear one tick before boundary");
        machine.run_ticks(1);
        expect((machine.read16(arm9, 0x04000004) & 2U) != 0,
               "line 213 HBlank starts at tick 1606");
        expect(captures == frame, "HBlank does not trigger the 3D render");
        machine.run_ticks(line_ticks - 1606 - 1);
        expect(captures == frame, "render not completed one tick before line 214");
        machine.run_ticks(1);
        expect(captures == frame + 1, "one render completes at line 214");
        machine.write32(arm9, 0x04000214, 1U << 2);
        machine.run_ticks(1);
        expect(captures == frame + 1, "render does not repeat after the boundary");
        machine.run_ticks(49 * line_ticks - 1);
        expect(machine.read16(arm9, 0x04000006) == 0, "frame wraps to line 0");
        expect(captures == frame + 1, "no extra render before the next frame");
    }
}

void test_texture_upload_after_swap() {
    dsr::Machine machine({});
    machine.write8(arm9, 0x04000240, 0x80);
    machine.write16(arm9, 0x06800000, 0x801f); // initial red texture
    machine.write8(arm9, 0x04000240, 0x83);
    machine.write32(arm9, 0x04000060, 1);
    machine.write16(arm9, 0x04000354, 0x7fff);
    triangle(machine);
    machine.run_ticks(192 * line_ticks); // reset processes HStart0 before execution; now192
    expect(machine.read16(arm9, 0x04000006) == 192, "texture test reaches swap scanline192");
    expect(machine.gpu3d().geometry_view().polygon_count == 1, "swap flush publishes geometry by192");
    expect(machine.gpu3d().line(96)[128] == 0, "swap flush does not rasterize before VBlank uploads");
    machine.write8(arm9, 0x04000240, 0x80); // guest remaps to LCDC for uploading
    machine.run_ticks(21 * line_ticks); // now213, last complete upload line
    machine.write16(arm9, 0x06800000, 0x83e0); // green replaces red
    machine.write8(arm9, 0x04000240, 0x83);
    machine.run_ticks(line_ticks - 1);
    expect(machine.gpu3d().line(96)[128] == 0, "render waits until scanline214 boundary");
    machine.run_ticks(1);
    expect(machine.read16(arm9, 0x04000006) == 214, "texture render starts on214");
    expect(machine.gpu3d().line(96)[128] == ((31U << 24) | (63U << 6)),
           "same render sees restored mapping and texture upload during192-213");
    machine.write8(arm9, 0x04000240, 0x80);
    machine.write16(arm9, 0x06800000, 0xfc00); // late blue upload
    machine.write8(arm9, 0x04000240, 0x83);
    expect(machine.gpu3d().line(96)[128] == ((31U << 24) | (63U << 6)),
           "upload after render start does not retroactively alter output");
    machine.run_ticks(263 * line_ticks);
    expect(machine.gpu3d().line(96)[128] == ((31U << 24) | (63U << 12)),
           "next render invalidates last-texel cache without another swap");
}
void test_clear_image_upload_after_swap() {
    dsr::Machine machine({});
    machine.write8(arm9, 0x04000240, 0x80);
    machine.write8(arm9, 0x04000241, 0x80);
    machine.write16(arm9, 0x06800000, 0x801f);
    machine.write16(arm9, 0x06820000, 0x7fff);
    machine.write8(arm9, 0x04000240, 0x93); // clear colour texture slot2
    machine.write8(arm9, 0x04000241, 0x9b); // clear depth texture slot3
    machine.write32(arm9, 0x04000060, 1U << 14);
    machine.run_ticks(192 * line_ticks);
    machine.write8(arm9, 0x04000240, 0x80);
    machine.write16(arm9, 0x06800000, 0x83e0);
    machine.write8(arm9, 0x04000240, 0x93);
    machine.run_ticks(22 * line_ticks);
    expect(machine.gpu3d().line(0)[0] == ((31U << 24) | (63U << 6)),
           "clear image reads VBlank-uploaded VRAM at render start, not swap");
    machine.run_ticks(241 * line_ticks); // next swap/flush at192
    machine.write8(arm9, 0x04000241, 0x80);
    machine.write16(arm9, 0x06820000, 0xffff); // depth image now enables fog
    machine.write8(arm9, 0x04000241, 0x9b);
    machine.write32(arm9, 0x04000060, (1U << 14) | (1U << 7));
    machine.write32(arm9, 0x04000358, (31U << 16) | 0x7c00U);
    for (unsigned off = 0; off < 32; off += 4)
        machine.write32(arm9, 0x04000360 + off, 0x7f7f7f7f);
    machine.run_ticks(22 * line_ticks);
    expect(machine.gpu3d().line(0)[0] == ((31U << 24) | (63U << 12)),
           "clear depth slot3 sees VBlank upload of fog flag before render start");
}
void test_lcd_uses_previous_render_epoch() {
    dsr::Machine machine({});
    auto& gpu = machine.gpu3d();
    machine.gpu2d().io_write(0x04000304U, 0x820fU, 16);
    machine.gpu2d().io_write(0x04000000U, (1U << 16U) | (1U << 8U) | (1U << 3U), 32);
    gpu.io_write(0x04000354U, 0x7fffU, 16);
    int captures = 0;
    float captured_x = 0;
    gpu.on_render_complete = [&] {
        dsr::RenderFrame frame{};
        dsr::capture_render_frame(machine, captures, frame);
        ++captures;
        expect(machine.read16(arm9, 0x04000006U) == 214, "capture runs at render214, not frame end");
        expect(frame.draw_order.opaque_count == 1, "capture sees complete sorted draw indices");
        captured_x = frame.geometry.vertices[0].clip[0];
    };
    const auto quad = [&](int left, int bottom) {
        const auto send = [&](unsigned code, std::uint32_t value) {
            command(machine, code, value);
            gpu.run(10000);
        };
        send(0x29, (31U << 16U) | 0xc0U); send(0x20, 0x7fffU); send(0x40, 1);
        for (const auto [x, y] : std::array<std::array<int, 2>, 4>{
                 std::array<int, 2>{left, bottom}, {left + 2048, bottom},
                 {left + 2048, bottom + 2048}, {left, bottom + 2048}}) {
            send(0x23, static_cast<std::uint16_t>(x) |
                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U));
            send(0x23, 0);
        }
        send(0x41, 0); send(0x50, 0);
    };
    quad(-3072, 0);
    machine.run_frame();
    expect(captures == 1 && captured_x < 0, "first future scene is left quad");
    expect(machine.gpu2d().three_d_mask()[64 * 256 + 64] == 0, "first LCD frame has no previous scene");
    quad(1024, -2048);
    machine.run_frame();
    expect(captures == 2 && captured_x > 0, "second future scene is right quad");
    expect(machine.gpu2d().three_d_mask()[64 * 256 + 64] == 1 &&
           machine.gpu2d().three_d_mask()[112 * 256 + 192] == 0,
           "current LCD mask belongs to prior left scene, not end-of-step right geometry");
    quad(-3072, 0);
    machine.run_frame();
    expect(captured_x < 0 && machine.gpu2d().three_d_mask()[112 * 256 + 192] == 1 &&
           machine.gpu2d().three_d_mask()[64 * 256 + 64] == 0,
           "alternating LCD mask tracks prior right scene without a stale epoch");
}
}
int main() {
    test_render_boundary_and_recurrence();
    test_texture_upload_after_swap(); test_clear_image_upload_after_swap();
    test_lcd_uses_previous_render_epoch();
    std::cout << "3D render timing failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
