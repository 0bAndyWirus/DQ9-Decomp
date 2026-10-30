#include "dsr/gpu3d.h"
#include "gpu3d_test.h"
#include "capture_policy.h"
#include "cpu_features.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <memory>
#include <iostream>
#include <string_view>

namespace {

using dsr::Gpu3D;
using dsr::gpu3d_detail::TextureSource;

bool check(bool condition, std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void test_avx2_dispatch_requirements() {
    using namespace dsr::gpu3d_detail;
    bool ok = check(avx2_state_enabled(kAvxStateFeatures, 1U << 5U, 6U),
                    "AVX2 dispatch permits fully enabled CPU/OS state");
    for (unsigned bit : {26U, 27U, 28U})
        ok &= check(!avx2_state_enabled(kAvxStateFeatures & ~(1U << bit), 1U << 5U, 6U),
                    "AVX2 dispatch rejects missing XSAVE/OSXSAVE/AVX");
    ok &= check(!avx2_state_enabled(kAvxStateFeatures, 0, 6U),
                "AVX2 dispatch rejects an AVX-only CPU");
    for (std::uint64_t state : {0U, 2U, 4U})
        ok &= check(!avx2_state_enabled(kAvxStateFeatures, 1U << 5U, state),
                    "AVX2 dispatch rejects disabled XMM/YMM state");
    if (!ok) std::exit(1);
}

void test_clipped_positions_match_reference_arithmetic() {
    Gpu3D gpu;
    bool ok = true;
    for (int plane = 0; plane < 6; ++plane) {
        const auto axis = static_cast<std::size_t>(plane / 2);
        const float sign = (plane & 1) != 0 ? 1.0F : -1.0F;
        std::array<std::array<float, 4>, 3> positions{{
            {0.125F, 0.25F, 0.375F, 1.3333F},
            {-0.25F, 0.5F, -0.125F, 3.14159F},
            {0.5F, -0.5F, 0.25F, 2.71828F},
        }};
        positions[0][axis] = sign * 0.75F;
        positions[1][axis] = -sign * 0.25F;
        positions[2][axis] = sign * 4.2F;
        std::array<std::array<float, 4>, 16> expected{};
        std::size_t expected_count = 0;
        auto previous = positions.back();
        bool previous_inside = sign * previous[axis] <= previous[3];
        for (const auto& current : positions) {
            const bool current_inside = sign * current[axis] <= current[3];
            if (current_inside != previous_inside) {
                const auto& inside = previous_inside ? previous : current;
                const auto& outside = previous_inside ? current : previous;
                const float inside_w = sign * inside[3];
                const float outside_w = sign * outside[3];
                const float ratio = (inside[axis] - inside_w) /
                    ((outside_w - inside_w) - (outside[axis] - inside[axis]));
                auto& vertex = expected[expected_count++];
                for (std::size_t i = 0; i < vertex.size(); ++i)
                    vertex[i] = inside[i] + (outside[i] - inside[i]) * ratio;
                vertex[axis] = sign * vertex[3];
            }
            if (current_inside) expected[expected_count++] = current;
            previous = current;
            previous_inside = current_inside;
        }
        std::array<std::array<float, 4>, 16> actual{};
        const auto count = dsr::gpu3d_detail::Gpu3DTestAccess::clip_positions(gpu, positions, actual);
        ok &= check(count == 4 && count == expected_count,
                    "one-plane triangle clipping retains four vertices");
        for (std::size_t i = 0; i < std::min(count, expected_count); ++i)
            ok &= check(actual[i] == expected[i],
                        "clipped XYZW follows inside-to-outside reference arithmetic");
        ok &= check(actual[0][axis] == sign * actual[0][3] &&
                    actual[3][axis] == sign * actual[3][3],
                    "both crossing directions snap exactly onto the active clip plane");
    }
    if (!ok) std::exit(1);
}

void port(Gpu3D& gpu, std::uint8_t code, std::uint32_t value = 0) {
    const auto addr = 0x04000440U + (static_cast<std::uint32_t>(code) - 0x10U) * 4U;
    gpu.io_write(addr, value, 32);
}

void send(Gpu3D& gpu, std::uint8_t code, std::initializer_list<std::uint32_t> values = {}) {
    if (values.size() == 0U) {
        port(gpu, code);
        return;
    }
    for (const auto value : values) port(gpu, code, value);
}

void position16(Gpu3D& gpu, std::int16_t x, std::int16_t y, std::int16_t z) {
    const auto xy = static_cast<std::uint16_t>(x) |
                    (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16);
    port(gpu, 0x23, xy);
    port(gpu, 0x23, static_cast<std::uint16_t>(z));
}

void triangle(Gpu3D& gpu, std::uint16_t color, std::uint32_t attr,
              std::int16_t za, std::int16_t zb, std::int16_t zc) {
    send(gpu, 0x20, {color});
    send(gpu, 0x29, {attr | 0xc0U});
    send(gpu, 0x40, {0});
    // These shorthand coordinates are in whole world units; VTX_16 stores Q4.12.
    position16(gpu, -4096, -4096, static_cast<std::int16_t>(za * 256));
    position16(gpu, 4096, -4096, static_cast<std::int16_t>(zb * 256));
    position16(gpu, 0, 4096, static_cast<std::int16_t>(zc * 256));
    send(gpu, 0x41);
}

void quad(Gpu3D& gpu, std::uint16_t color, std::uint32_t attr, std::int16_t z = 0) {
    send(gpu, 0x20, {color});
    send(gpu, 0x29, {attr | 0xc0U});
    send(gpu, 0x40, {1U});
    position16(gpu, -4096, -4096, z);
    position16(gpu, 4096, -4096, z);
    position16(gpu, 4096, 4096, z);
    position16(gpu, -4096, 4096, z);
    send(gpu, 0x41);
}

void shadow_scene(Gpu3D& gpu, bool include_mask, std::uint8_t shadow_id) {
    gpu.io_write(0x04000060U, 1U << 3, 32);
    triangle(gpu, 0x03e0U, (31U << 16) | (7U << 24), 0, 0, 0);
    if (include_mask)
        triangle(gpu, 0x7fffU, (31U << 16) | (3U << 4), 0, 0, 0);
    triangle(gpu, 0U, (16U << 16) | (3U << 4) |
                 (static_cast<std::uint32_t>(shadow_id) << 24), -1, -1, -1);
}

void edge_triangle(Gpu3D& gpu) {
    send(gpu, 0x20, {0x03e0U});
    send(gpu, 0x29, {(31U << 16) | (8U << 24) | 0xc0U});
    send(gpu, 0x40, {0U});
    position16(gpu, -512, -512, 0);
    position16(gpu, 512, -512, 0);
    position16(gpu, 0, 512, 0);
    send(gpu, 0x41);
}

void submit_frame(Gpu3D& gpu, std::uint32_t sort_mode = 0) {
    send(gpu, 0x50, {sort_mode});
    gpu.run(100000);
    gpu.vblank_start();
    gpu.render_start();
}

struct TestTextureSource {
    // Heap-backed: ~600 KiB per source overflows the 1 MiB default Windows stack
    // when a test holds two or more on the stack.
    std::vector<std::array<std::uint8_t, 0x20000>> textures =
        std::vector<std::array<std::uint8_t, 0x20000>>(4);
    std::vector<std::array<std::uint8_t, 0x4000>> palettes =
        std::vector<std::array<std::uint8_t, 0x4000>>(6);
    std::uint8_t mapped_textures = 0x0fU;
    std::uint8_t overlapping_textures = 0U;
    bool overlapping_palettes = false;
    mutable std::atomic<std::size_t> texture_slot_zero_lookups{0};
    std::uint64_t generation = 0;

    static const std::uint8_t* texture_lookup(const void* context, int slot) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        if (slot == 0) source.texture_slot_zero_lookups.fetch_add(1, std::memory_order_relaxed);
        return readonly_texture_lookup(context, slot);
    }
    static const std::uint8_t* readonly_texture_lookup(const void* context, int slot) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        return slot >= 0 && slot < static_cast<int>(source.textures.size()) &&
               (source.mapped_textures & (1U << slot)) != 0U &&
               (source.overlapping_textures & (1U << slot)) == 0U
            ? source.textures[static_cast<std::size_t>(slot)].data() : nullptr;
    }
    static const std::uint8_t* palette_lookup(const void* context, int slot) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        return slot >= 0 && slot < static_cast<int>(source.palettes.size())
            ? source.palettes[static_cast<std::size_t>(slot)].data() : nullptr;
    }
    TextureSource view() const { return {this, &texture_lookup, &palette_lookup}; }
    static const std::uint8_t* readonly_palette_lookup(const void* context, int slot) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        if (slot == 0 && source.overlapping_palettes) return nullptr;
        return palette_lookup(context, slot);
    }
    static std::uint8_t texture_read(const void* context, std::uint32_t address) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        const auto slot = (address >> 17U) & 3U;
        if ((source.mapped_textures & (1U << slot)) == 0U) return 0;
        const auto offset = address & 0x1ffffU;
        const auto value = source.textures[slot][offset];
        return (source.overlapping_textures & (1U << slot)) != 0U
            ? static_cast<std::uint8_t>(value | source.textures[(slot + 1U) & 3U][offset])
            : value;
    }
    static std::uint8_t palette_read(const void* context, std::uint32_t address) {
        const auto& source = *static_cast<const TestTextureSource*>(context);
        auto slot = (address >> 14U) & 7U;
        if (slot > 5U) slot -= 5U;
        const auto offset = address & 0x3fffU;
        const auto value = source.palettes[slot][offset];
        return slot == 0 && source.overlapping_palettes
            ? static_cast<std::uint8_t>(value | source.palettes[1][offset]) : value;
    }
    // These callbacks never mutate the diagnostic counter or populate caches.
    TextureSource readonly_view() const {
        return {this, &readonly_texture_lookup, &readonly_palette_lookup,
                &texture_read, &palette_read};
    }
    static std::uint64_t generation_lookup(const void* context) {
        return static_cast<const TestTextureSource*>(context)->generation;
    }
    TextureSource versioned_view() const {
        return {this, &texture_lookup, &readonly_palette_lookup, &texture_read, &palette_read,
                &generation_lookup};
    }
};

void palette_color(TestTextureSource& source, std::size_t offset, std::uint16_t color) {
    source.palettes[0][offset] = static_cast<std::uint8_t>(color);
    source.palettes[0][offset + 1U] = static_cast<std::uint8_t>(color >> 8);
}

void test_geometry_reference_command_semantics() {
    bool ok = true;
    {
        Gpu3D gpu;
        send(gpu, 0x40, {0U});
        position16(gpu, 32760, -32760, 0);
        send(gpu, 0x28, {20U | (1004U << 10) | (511U << 20)});
        submit_frame(gpu);
        const auto view = gpu.geometry_view();
        ok &= check(view.vertices[1].clip[0] == -32756.0F / 4096.0F &&
                    view.vertices[1].clip[1] == 32756.0F / 4096.0F &&
                    view.vertices[1].clip[2] == 511.0F / 4096.0F,
                    "VTX_DIFF adds signed Q0.12 deltas and wraps each signed 16-bit coordinate");
    }
    for (unsigned mode : {1U, 2U, 3U}) {
        Gpu3D gpu;
        send(gpu, 0x10, {3U});
        send(gpu, 0x16, {
            65536U, 32768U, 0U, 0U,
            0U, 4096U, 0U, 0U,
            0U, 0U, 4096U, 0U,
            8192U, 12288U, 0U, 4096U});
        send(gpu, 0x2a, {mode << 30});
        send(gpu, 0x22, {16U | (32U << 16)});
        if (mode == 2U) send(gpu, 0x21, {256U});
        send(gpu, 0x40, {0U});
        position16(gpu, 2048, 0, 0);
        submit_frame(gpu);
        const auto vertex = gpu.geometry_view().vertices[0];
        ok &= check(vertex.u == (mode == 1U ? 16.125F : 1.5F) &&
                    vertex.v == (mode == 1U ? 10.1875F : 2.25F),
                    "texgen uses mode-specific shifts and ignores matrix translation for normal/vertex sources");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {3U});
        send(gpu, 0x1b, {8192U, 4096U, 4096U});
        send(gpu, 0x2a, {1U << 30});
        send(gpu, 0x22, {0x7fffU});
        gpu.run(10000);
        ok &= check(dsr::gpu3d_detail::Gpu3DTestAccess::transformed_texcoord(gpu)[0] ==
                    65534 * 256, "texture transform retains the signed 32-bit result rather than wrapping at 16 bits");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {2U});
        send(gpu, 0x16, {
            4096U, 0U, 0U, 4096U,
            0U, 4096U, 0U, 0U,
            0U, 0U, 4096U, 0U,
            8192U, 0U, 0U, 4096U});
        send(gpu, 0x18, {
            4096U, 0U, 0U, 4096U,
            0U, 4096U, 0U, 0U,
            0U, 0U, 4096U, 0U,
            0U, 0U, 0U, 4096U});
        gpu.run(10000);
        ok &= check(gpu.io_read(0x04000680U, 32) == 12288U,
                    "position/vector mode retains all sixteen matrix components across multiplication");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {1U});
        send(gpu, 0x1b, {0x7fffffffU, 4096U, 4096U});
        send(gpu, 0x1b, {8192U, 4096U, 4096U});
        gpu.run(10000);
        ok &= check(gpu.io_read(0x04000640U, 32) == 0xfffffffeU,
                    "matrix products truncate to 32 bits, unlike saturating vertex transforms");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {2U});
        send(gpu, 0x1c, {8192U, 0U, 0U});
        send(gpu, 0x13, {1U});
        send(gpu, 0x15);
        send(gpu, 0x11);
        send(gpu, 0x12, {63U});
        gpu.run(10000);
        ok &= check(gpu.io_read(0x04000670U, 32) == 0U,
                    "PUSH stores only its current slot, without overwriting the next slot");
        send(gpu, 0x12, {1U});
        gpu.run(10000);
        ok &= check(gpu.io_read(0x04000670U, 32) == 8192U,
                    "negative POP addresses a pre-stored slot");
        send(gpu, 0x12, {2U});
        gpu.run(10000);
        ok &= check(((gpu.io_read(0x04000600U, 32) >> 8) & 31U) == 31U &&
                    (gpu.io_read(0x04000600U, 32) & (1U << 15)) != 0U,
                    "stack underflow wraps the six-bit pointer and reports error");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {0U});
        send(gpu, 0x1c, {4096U, 0U, 0U});
        send(gpu, 0x11);
        send(gpu, 0x1c, {4096U, 0U, 0U});
        send(gpu, 0x11);
        send(gpu, 0x15);
        send(gpu, 0x12, {1U});
        gpu.run(10000);
        ok &= check(gpu.io_read(0x04000670U, 32) == 8192U &&
                    (gpu.io_read(0x04000600U, 32) & (1U << 13)) != 0U,
                    "projection overflow overwrites its one slot and toggles the pointer");
    }
    {
        Gpu3D gpu;
        triangle(gpu, 31U, 31U << 16, 0, 0, 0);
        send(gpu, 0x60, {0xbf3f0000U});
        submit_frame(gpu);
        ok &= check((gpu.line(96)[128] & 63U) == 63U,
                    "VIEWPORT is captured per polygon, not applied retroactively at swap");
    }
    {
        Gpu3D gpu;
        send(gpu, 0x10, {3U});
        send(gpu, 0x1b, {134217728U, 4096U, 4096U});
        send(gpu, 0x2a, {1U << 30});
        send(gpu, 0x22, {32767U});
        send(gpu, 0x40, {0U});
        position16(gpu, 0, 0, 0);
        submit_frame(gpu);
        ok &= check(gpu.geometry_view().vertices[0].u == 67106816.0F,
                    "texgen retains the full signed 32-bit 12.4 output before converting to float");
    }
    if (!ok) std::exit(1);
}

void test_matrix_stack() {
    Gpu3D gpu;
    send(gpu, 0x10, {1});
    send(gpu, 0x15);
    send(gpu, 0x1c, {4096, 0, 0});
    send(gpu, 0x11);
    send(gpu, 0x1c, {8192, 0, 0});
    send(gpu, 0x71, {0, 0});
    gpu.run(10000);
    bool ok = check(gpu.io_read(0x04000620U, 32) == 12288U, "matrix translation and POS_TEST");
    send(gpu, 0x12, {1});
    send(gpu, 0x71, {0, 0});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000620U, 32) == 4096U, "matrix stack push/pop restore");
    if (!ok) std::exit(1);
}

void test_matrix_mode_semantics() {
    Gpu3D gpu;
    send(gpu, 0x10, {2});
    send(gpu, 0x17, {
        0U, 4096U, 0U,
        0xfffff000U, 0U, 0U,
        0U, 0U, 4096U,
        8192U, 12288U, 16384U});
    send(gpu, 0x71, {0U, 0U});
    gpu.run(10000);
    bool ok = check(gpu.io_read(0x04000620U, 32) == 8192U &&
                    gpu.io_read(0x04000624U, 32) == 12288U &&
                    gpu.io_read(0x04000628U, 32) == 16384U,
                    "4x3 load expands the translation row and transforms positions");
    ok &= check(gpu.io_read(0x04000644U, 32) == 4096U &&
                gpu.io_read(0x04000670U, 32) == 8192U,
                "clip matrix follows position-then-projection row-vector convention");
    ok &= check(gpu.io_read(0x04000680U, 32) == 0U &&
                gpu.io_read(0x04000684U, 32) == 4096U &&
                gpu.io_read(0x0400068cU, 32) == 0xfffff000U,
                "matrix mode 2 loads both position and directional matrices");

    send(gpu, 0x10, {1});
    send(gpu, 0x16, {
        4096U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        4096U, 0U, 0U, 4096U});
    send(gpu, 0x71, {0U, 0U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000620U, 32) == 4096U &&
                gpu.io_read(0x04000684U, 32) == 4096U &&
                gpu.io_read(0x0400068cU, 32) == 0xfffff000U,
                "matrix mode 1 replaces position without changing directional matrix");

    send(gpu, 0x10, {2});
    send(gpu, 0x1b, {8192U, 4096U, 4096U});
    send(gpu, 0x71, {0U, 0U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000640U, 32) == 8192U &&
                gpu.io_read(0x04000670U, 32) == 4096U &&
                gpu.io_read(0x04000684U, 32) == 4096U,
                "matrix mode 2 scale changes position only");

    send(gpu, 0x10, {1});
    send(gpu, 0x15);
    send(gpu, 0x1c, {4096U, 0U, 0U});
    send(gpu, 0x1b, {8192U, 4096U, 4096U});
    send(gpu, 0x71, {0U, 0U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000620U, 32) == 4096U,
                "matrix multiply commands prepend transforms to the current matrix");
    if (!ok) std::exit(1);
}

void test_geometry_test_results() {
    Gpu3D gpu;
    send(gpu, 0x10, {1U});
    send(gpu, 0x70, {0U, 0x10000000U, 0x10001000U});
    gpu.run(10000);
    bool ok = check((gpu.io_read(0x04000600U, 32) & 2U) != 0U,
                    "completed BOX_TEST publishes its result");
    send(gpu, 0x71, {0U, 0U});
    gpu.run(10000);
    ok &= check((gpu.io_read(0x04000600U, 32) & 2U) == 0U,
                "position test clears the previous box result");

    send(gpu, 0x16, {
        1U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 4096U});
    send(gpu, 0x71, {0xffffU, 0U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000620U, 32) == 0U,
                "POS_RESULT truncates negative fractional output toward zero");
    ok &= check(gpu.io_read(0x0400062cU, 32) == 4096U,
                "POS_RESULT includes the fourth homogeneous coordinate");

    send(gpu, 0x10, {2U});
    send(gpu, 0x16, {
        65536U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 4096U});
    send(gpu, 0x72, {511U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000630U, 16) == 0xff80U,
                "VEC_RESULT wraps a transformed component instead of saturating");
    send(gpu, 0x15);
    send(gpu, 0x72, {512U}); // -1.0 is representable; +1.0 below must wrap.
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000630U, 16) == 0xf000U,
                "VEC_RESULT sign-extends a negative Q0.12 component");
    // Load the directional scale explicitly: mode-2 SCALE affects position only.
    send(gpu, 0x16, {
        8192U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 4096U});
    send(gpu, 0x72, {256U});
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000630U, 16) == 0xf000U,
                "VEC_RESULT wraps +1.0 to -1.0 and replicates the sign in bits 13-15");

    Gpu3D inherited;
    send(inherited, 0x71, {0U, 2048U});
    send(inherited, 0x40, {0U});
    send(inherited, 0x25, {0xf800f800U});
    send(inherited, 0x25, {0xf8000800U});
    send(inherited, 0x25, {0x08000000U});
    send(inherited, 0x41);
    submit_frame(inherited);
    const auto inherited_geometry = inherited.geometry_view();
    ok &= check(inherited_geometry.vertex_count == 3U &&
                inherited_geometry.vertices[0].clip[2] == 0.5F &&
                inherited_geometry.vertices[2].clip[2] == 0.5F,
                "POS_TEST overwrites VTX coordinate latches used by partial vertices");
    if (!ok) std::exit(1);
}

void test_geometry_status_ack_and_matrix_readback() {
    Gpu3D gpu;
    send(gpu, 0x10, {3U});
    send(gpu, 0x11);
    send(gpu, 0x10, {0U});
    send(gpu, 0x12, {1U});
    gpu.run(10000);
    gpu.io_write(0x04000603U, 0x40U, 8);
    bool ok = check((gpu.io_read(0x04000600U, 32) & (1U << 15)) != 0U,
                    "IRQ mode byte write does not acknowledge a previous stack error");
    gpu.io_write(0x04000601U, 0x80U, 8);
    send(gpu, 0x10, {3U});
    send(gpu, 0x12, {1U});
    gpu.run(10000);
    ok &= check((gpu.io_read(0x04000600U, 32) & (1U << 15)) == 0U,
                "stack-error acknowledgement resets projection but not texture stack");

    send(gpu, 0x10, {1U});
    send(gpu, 0x16, {
        0x7fffffffU, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        8192U, 12288U, 16384U, 4096U});
    send(gpu, 0x10, {0U});
    send(gpu, 0x1b, {8192U, 4096U, 4096U});
    ok &= check(gpu.io_read(0x04000640U, 32) == 4096U,
                "matrix readback observes executed matrices, not queued modifications");
    gpu.run(10000);
    ok &= check(gpu.io_read(0x04000640U, 32) == 0xfffffffeU &&
                gpu.io_read(0x04000670U, 32) == 16384U &&
                gpu.io_read(0x04000674U, 32) == 12288U &&
                gpu.io_read(0x04000678U, 32) == 16384U,
                "clip readback is position followed by projection, Q20.12 wrapping not saturation");
    gpu.io_write(0x04000640U, 0U, 32);
    ok &= check(gpu.io_read(0x04000640U, 32) == 0xfffffffeU,
                "matrix readback registers are read-only");
    if (!ok) std::exit(1);
}

void test_matrix_result_register_layout() {
    Gpu3D gpu;
    constexpr std::array<std::uint32_t, 16> matrix{
        4096U, 0xfffff234U, 2U, 3U,
        4U, 4096U, 6U, 7U,
        8U, 9U, 4096U, 11U,
        12U, 13U, 14U, 4096U};
    send(gpu, 0x10, {2U});
    for (const auto value : matrix) port(gpu, 0x16, value);
    gpu.run(10000);
    bool ok = true;
    for (std::size_t index = 0; index < matrix.size(); ++index)
        ok &= check(gpu.io_read(0x04000640U + static_cast<std::uint32_t>(index * 4U), 32) ==
                    matrix[index], "all clip matrix readback words preserve column-major Q20.12 values");
    for (std::size_t index = 0; index < 9U; ++index)
        ok &= check(gpu.io_read(0x04000680U + static_cast<std::uint32_t>(index * 4U), 32) ==
                    matrix[(index / 3U) * 4U + index % 3U],
                    "all directional matrix readback words omit the fourth row and column");
    ok &= check(gpu.io_read(0x04000644U, 16) == 0xf234U &&
                gpu.io_read(0x04000646U, 16) == 0xffffU &&
                gpu.io_read(0x04000645U, 8) == 0xf2U &&
                gpu.io_read(0x04000685U, 8) == 0xf2U,
                "matrix readback byte and halfword access exposes fixed-point little-endian slices");
    if (!ok) std::exit(1);
}

void test_box_test_preserves_fractional_clip_coordinates() {
    Gpu3D gpu;
    send(gpu, 0x10, {1U});
    send(gpu, 0x16, {
        1U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 1U});
    send(gpu, 0x10, {0U});
    send(gpu, 0x16, {
        4095U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 4095U});
    send(gpu, 0x70, {0xffffU, 0U, 0U});
    gpu.run(10000);
    if (!check((gpu.io_read(0x04000600U, 32) & 2U) != 0U,
               "BOX_TEST retains a point on a fractional homogeneous clip boundary")) std::exit(1);
}

void test_swap_waits_for_vblank_and_blocks_following_commands() {
    Gpu3D gpu;
    triangle(gpu, 0x001fU, 31U << 16, 0, 0, 0);
    send(gpu, 0x50, {3U});
    send(gpu, 0x10, {1U});
    send(gpu, 0x1c, {4096U, 0U, 0U});
    send(gpu, 0x71, {0U, 0U});
    gpu.run(100000);
    bool ok = check(gpu.geometry_view().generation == 0U &&
                    (gpu.io_read(0x04000604U, 32) & 0xfffU) == 1U,
                    "SWAP_BUFFERS retains submission RAM until VBlank");
    ok &= check(gpu.io_read(0x04000620U, 32) == 0U &&
                gpu.io_read(0x04000670U, 32) == 0U,
                "geometry commands after swap remain queued until VBlank");
    gpu.vblank_start();
    ok &= check(gpu.geometry_view().generation == 1U &&
                gpu.geometry_view().swap_mode == 0U &&
                gpu.geometry_view().polygon_count == 1U &&
                gpu.io_read(0x04000604U, 32) == 0U,
                "VBlank publishes pending geometry and clears submission RAM");
    gpu.run(100000);
    ok &= check(gpu.io_read(0x04000620U, 32) == 4096U,
                "commands after swap resume in the next geometry frame");
    submit_frame(gpu, 0U);
    ok &= check(gpu.geometry_view().swap_mode == 3U,
                "swap sort/depth parameters apply to following geometry, not preceding geometry");
    if (!ok) std::exit(1);
}

void test_packed_fifo() {
    Gpu3D gpu;
    gpu.io_write(0x04000400U, 0x00232340U, 32); // BEGIN followed by two VTX_16 commands
    gpu.io_write(0x04000400U, 0U, 32); // BEGIN parameter
    gpu.io_write(0x04000400U, 0xffff0000U, 32); // VTX_16 #1 X/Y
    gpu.io_write(0x04000400U, 0U, 32); // VTX_16 #1 Z
    gpu.io_write(0x04000400U, 0x00000000U, 32); // VTX_16 #2 X/Y
    gpu.io_write(0x04000400U, 0U, 32); // VTX_16 #2 Z
    gpu.io_write(0x04000400U, 0x00000041U, 32); // next command word: END
    const auto status = gpu.io_read(0x04000600U, 32);
    bool ok = check(((status >> 16) & 0x1ffU) == 7U, "packed FIFO queue occupancy");
    gpu.run(10000);
    const auto counts = gpu.io_read(0x04000604U, 32);
    ok &= check((counts >> 16) == 2U, "packed FIFO command decoding and vertex RAM count");
    ok &= check((counts & 0xfffU) == 0U, "incomplete primitive does not allocate a polygon");
    if (!ok) std::exit(1);
}

void test_queued_test_and_stack_busy_flags() {
    constexpr std::uint32_t kStatus = 0x04000600U;
    constexpr std::uint32_t kFifo = 0x04000400U;
    bool ok = true;
    for (const auto code : {0x70U, 0x71U, 0x72U}) {
        Gpu3D gpu;
        port(gpu, static_cast<std::uint8_t>(code), 0U);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "direct test asserts busy as soon as its parameters arrive");
        if (code != 0x72U) port(gpu, static_cast<std::uint8_t>(code), 0U);
        if (code == 0x70U) port(gpu, 0x70U, 0U);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "queued direct test is busy before the engine starts");
        gpu.run(1);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "test remains busy during execution");
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U,
                    "test busy clears only after execution completes");
    }
    {
        Gpu3D gpu;
        // A matrix command's data can look like a packed test command.
        gpu.io_write(kFifo, 0x1cU, 32);
        gpu.io_write(kFifo, 0x00727170U, 32);
        gpu.io_write(kFifo, 0U, 32);
        gpu.io_write(kFifo, 0U, 32);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U,
                    "FIFO parameters are not mistaken for test commands");
        gpu.run(1000);
        gpu.io_write(kFifo, 0x007170U, 32); // BOX_TEST followed by POS_TEST
        for (unsigned i = 0; i < 5; ++i) gpu.io_write(kFifo, 0U, 32);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "packed queued tests assert busy before execution");
        gpu.run(12);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "completing one test does not clear another queued test");
        gpu.run(12);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U,
                    "packed test busy clears after the last test");
        send(gpu, 0x11);
        send(gpu, 0x12, {1U});
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 14)) != 0U,
                    "queued push/pop assert matrix-stack busy immediately");
        gpu.run(2);
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 14)) != 0U,
                    "matrix-stack busy remains set for the queued pop");
        gpu.run(2);
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 14)) == 0U,
                    "matrix-stack busy clears after the queued pop");
        send(gpu, 0x71, {0U, 0U});
        gpu.io_write(kStatus, 1U << 29, 32);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U,
                    "FIFO clear discards queued-test busy accounting");
        gpu.io_write(kFifo, 0x70U, 32);
        gpu.reset();
        ok &= check((gpu.io_read(kStatus, 32) & (1U | (1U << 14))) == 0U,
                    "reset clears queued test and stack busy accounting");
    }
    {
        Gpu3D gpu;
        // Fill the FIFO so this test waits in the direct-command staging queue.
        for (unsigned i = 0; i < 128; ++i) {
            gpu.io_write(kFifo, 0x10U, 32);
            gpu.io_write(kFifo, 0U, 32);
        }
        send(gpu, 0x71, {0U, 0U});
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "deferred direct test is busy even before it enters the FIFO");
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U,
                    "deferred test is accounted once and clears after execution");
    }
    if (!ok) std::exit(1);
}

void test_mixed_packed_and_direct_busy_retirement() {
    constexpr std::uint32_t kFifo = 0x04000400U;
    constexpr std::uint32_t kStatus = 0x04000600U;
    constexpr std::uint32_t kBusy = 1U | (1U << 14);
    bool ok = true;
    for (const auto code : {0x11U, 0x12U, 0x70U, 0x71U, 0x72U}) {
        Gpu3D gpu;
        gpu.io_write(kFifo, 0x00001110U, 32); // MODE, then PUSH; MODE's argument comes later.
        if (code == 0x11U) send(gpu, 0x11U);
        else if (code == 0x12U) send(gpu, 0x12U, {0U});
        else if (code == 0x70U) send(gpu, 0x70U, {0U, 0U, 0U});
        else if (code == 0x71U) send(gpu, 0x71U, {0U, 0U});
        else send(gpu, 0x72U, {0U});
        gpu.io_write(kFifo, 1U, 32);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) != 0U,
                    "mixed packed/direct commands assert busy before retirement");
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "direct commands between packed arguments retire both busy counters");
        if (code == 0x11U) {
            const auto status = gpu.io_read(kStatus, 32);
            ok &= check(((status >> 8) & 31U) == 1U && (status & (1U << 13)) != 0U,
                        "direct PUSH runs in projection mode before packed MODE and position PUSH");
        }
    }
    {
        Gpu3D gpu;
        gpu.io_write(kFifo, 0x1110U, 32);
        send(gpu, 0x11U);
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 14)) == 0U &&
                    (gpu.io_read(kStatus, 32) & (1U << 13)) != 0U,
                    "direct PUSH retires while packed MODE still waits for its argument");
        gpu.io_write(kFifo, 1U, 32);
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U &&
                    ((gpu.io_read(kStatus, 32) >> 8) & 31U) == 1U,
                    "packed command resumes when its delayed argument arrives");
    }
    {
        Gpu3D gpu;
        gpu.io_write(kFifo, 0x71U, 32);
        gpu.io_write(kFifo, 4096U, 32);
        send(gpu, 0x72U, {0U});
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) != 0U,
                    "retiring a direct test leaves the incomplete packed test busy");
        gpu.io_write(kFifo, 0U, 32);
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & 1U) == 0U &&
                    gpu.io_read(0x04000620U, 32) == 4096U,
                    "suspended packed test preserves its earlier arguments and retires once");
    }
    for (const auto discard : {0U, 1U, 2U}) {
        Gpu3D gpu;
        gpu.io_write(kFifo, 0x00701110U, 32);
        send(gpu, 0x50U, {0U});
        gpu.io_write(kFifo, 1U, 32);
        for (unsigned i = 0; i < 3; ++i) gpu.io_write(kFifo, 0U, 32);
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == kBusy,
                    "direct swap stalls the suspended packed stack/test stream");
        if (discard == 1U) gpu.io_write(kStatus, 1U << 29, 32);
        else if (discard == 2U) gpu.reset();
        gpu.vblank_start();
        gpu.run(1000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "swap release, FIFO clear and reset retire/discard suspended packed accounting");
    }
    if (!ok) std::exit(1);
}

void test_busy_retirement_across_swap_clear_and_stack_errors() {
    constexpr std::uint32_t kFifo = 0x04000400U;
    constexpr std::uint32_t kStatus = 0x04000600U;
    constexpr std::uint32_t kBusy = 1U | (1U << 14);
    bool ok = true;
    for (const bool clear : {false, true}) {
        Gpu3D gpu;
        send(gpu, 0x50, {0U});
        gpu.io_write(kFifo, 0x00721211U, 32); // PUSH, POP, VEC_TEST.
        gpu.io_write(kFifo, 0U, 32);
        gpu.io_write(kFifo, 0U, 32);
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == kBusy,
                    "swap-stalled stack and test commands remain busy");
        if (clear) {
            gpu.io_write(kStatus, 1U << 29, 32);
            ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                        "FIFO clear discards both counters while swap is stalled");
        }
        gpu.vblank_start();
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "swap release retires busy commands without counter underflow");
        send(gpu, 0x11U);
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "stack accounting remains usable after swap or FIFO clear");
    }
    for (const auto mode : {0U, 1U, 2U, 3U}) {
        Gpu3D gpu;
        send(gpu, 0x10U, {mode});
        for (unsigned i = 0; i < 34; ++i) send(gpu, 0x11U);
        send(gpu, 0x12U, {63U});
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 15)) != 0U,
                    "stack overflow/underflow sets the stack-error flag");
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "stack-error commands still retire their busy accounting in every mode");
    }
    for (const auto code : {0x13U, 0x14U}) {
        Gpu3D gpu;
        send(gpu, static_cast<std::uint8_t>(code), {0U});
        ok &= check((gpu.io_read(kStatus, 32) & (1U << 14)) == 0U,
                    "STORE/RESTORE do not assert push/pop stack busy, matching the reference FIFO");
        gpu.run(10000);
    }
    {
        Gpu3D gpu;
        gpu.io_write(kFifo, 0x007111U, 32); // PUSH followed by incomplete POS_TEST.
        gpu.run(10000);
        gpu.io_write(kStatus, 1U << 29, 32);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "FIFO clear discards an active test waiting for parameters");
        send(gpu, 0x72U, {0U});
        gpu.run(10000);
        ok &= check((gpu.io_read(kStatus, 32) & kBusy) == 0U,
                    "test accounting remains usable after clearing an incomplete packed command");
    }
    if (!ok) std::exit(1);
}

void test_full_direct_staging_busy_retirement() {
    constexpr std::uint32_t kFifo = 0x04000400U;
    constexpr std::uint32_t kStatus = 0x04000600U;
    bool ok = true;
    for (const auto code : {0x12U, 0x70U, 0x71U, 0x72U}) {
        Gpu3D gpu;
        for (unsigned i = 0; i < 128; ++i) {
            gpu.io_write(kFifo, 0x10U, 32);
            gpu.io_write(kFifo, 0U, 32);
        }
        for (unsigned i = 0; i < 256; ++i) send(gpu, 0x72U, {0U});
        if (code == 0x12U) send(gpu, 0x12U, {0U});
        else if (code == 0x70U) send(gpu, 0x70U, {0U, 0U, 0U});
        else if (code == 0x71U) send(gpu, 0x71U, {0U, 0U});
        else send(gpu, 0x72U, {0U});
        const auto mask = code == 0x12U ? (1U << 14) : 1U;
        ok &= check((gpu.io_read(kStatus, 32) & mask) != 0U,
                    "complete direct command held behind two full queues asserts busy");
        gpu.run(100000);
        ok &= check((gpu.io_read(kStatus, 32) & (1U | (1U << 14))) == 0U,
                    "full direct staging retries and retires when queue capacity returns");
    }
    if (!ok) std::exit(1);
}

void test_gxfifo_mirror_write_widths() {
    constexpr std::array<int, 3> kWidths{8, 16, 32};
    constexpr std::array<std::uint32_t, 3> kOffsets{3U, 2U, 0U};
    constexpr std::array<std::uint32_t, 3> kTranslations{32U, 4096U, 4096U};
    bool ok = true;
    for (std::size_t test = 0; test < kWidths.size(); ++test) {
        Gpu3D gpu;
        const std::array<std::uint32_t, 4> packet{
            0x1cU, kTranslations[test], 0U, 0U};
        for (std::size_t word = 0; word < packet.size(); ++word) {
            const auto addr = 0x04000400U + kOffsets[test] +
                              static_cast<std::uint32_t>(word * 4U);
            gpu.io_write(addr, packet[word], kWidths[test]);
        }
        gpu.run(1000);
        ok &= check((gpu.io_read(0x04000600U, 32) & (1U << 27)) == 0U,
                    "GXFIFO mirror accepts 8/16/32-bit command words");
        ok &= check(gpu.io_read(0x04000670U, 32) == kTranslations[test],
                    "GXFIFO mirror preserves command parameters at each width");
    }
    if (!ok) std::exit(1);
}

void test_gxfifo_stmia_burst() {
    constexpr std::uint32_t kFifo = 0x04000400U;
    constexpr std::array<std::uint32_t, 16> kMatrix{
        4096U, 0U, 0U, 0U,
        0U, 4096U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        0U, 0U, 0U, 4096U};
    Gpu3D gpu;
    gpu.io_write(kFifo, 0x16U, 32);
    for (std::size_t word = 0; word < kMatrix.size(); ++word) {
        gpu.io_write(kFifo + static_cast<std::uint32_t>(word * 4U),
                     kMatrix[word], 32);
    }
    gpu.run(10000);
    bool ok = check((gpu.io_read(0x04000600U, 32) & (1U << 27)) == 0U,
                    "STMIA mirror burst completes a 4x4 matrix command");
    for (std::size_t element = 0; element < kMatrix.size(); ++element) {
        ok &= check(gpu.io_read(0x04000640U + static_cast<std::uint32_t>(element * 4U), 32) ==
                        kMatrix[element],
                    "STMIA mirror burst delivers every matrix word in order");
    }
    if (!ok) std::exit(1);
}


void test_vtx16_q4_12_coordinates() {
    Gpu3D gpu;
    send(gpu, 0x20, {0x001fU});
    send(gpu, 0x29, {0xc0U | (31U << 16) | (7U << 24)});
    send(gpu, 0x40, {0});
    position16(gpu, -2048, -2048, 0);
    position16(gpu, 2048, -2048, 0);
    position16(gpu, 0, 2048, 0);
    send(gpu, 0x41);
    send(gpu, 0x71, {0x1000U, 0U});
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) == 63U && ((pixel >> 24) & 31U) == 31U,
                    "VTX_16 raw values are signed Q4.12 coordinates");
    ok &= check(gpu.io_read(0x04000620U, 32) == 4096U,
                "POS_TEST uses the same Q4.12 coordinate scale");
    if (!ok) std::exit(1);
}


void test_polygon_surface_enable_bits() {
    const auto render_center = [](std::uint32_t surfaces, bool reverse_winding) {
        Gpu3D gpu;
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {surfaces | (31U << 16) | (3U << 24)});
        send(gpu, 0x40, {0});
        position16(gpu, -2048, -2048, 0);
        if (reverse_winding) {
            position16(gpu, 0, 2048, 0);
            position16(gpu, 2048, -2048, 0);
        } else {
            position16(gpu, 2048, -2048, 0);
            position16(gpu, 0, 2048, 0);
        }
        send(gpu, 0x41);
        submit_frame(gpu);
        return ((gpu.line(96)[128] >> 24) & 31U) != 0U;
    };

    bool ok = check(!render_center(0U, false) && !render_center(0U, true),
                    "polygon with neither surface enabled is hidden");
    ok &= check(render_center(1U << 7, false) && !render_center(1U << 6, false),
                "front-surface flag controls front-facing polygons");
    ok &= check(render_center(1U << 6, true) && !render_center(1U << 7, true),
                "back-surface flag controls back-facing polygons");
    ok &= check(render_center(0xc0U, false) && render_center(0xc0U, true),
                "both surface flags render either winding");
    if (!ok) std::exit(1);
}


void test_triangle_raster_and_ram_counts() {
    Gpu3D gpu;
    triangle(gpu, 0x001fU, (31U << 16) | (7U << 24), 0, 0, 0);
    gpu.run(100000);
    const auto counts = gpu.io_read(0x04000604U, 32);
    bool ok = check((counts & 0xfffU) == 1U, "polygon RAM count");
    ok &= check((counts >> 16) == 3U, "vertex RAM count");
    submit_frame(gpu);
    const auto* line = gpu.line(96);
    ok &= check(line != nullptr, "valid 3D scanline is available");
    if (line != nullptr) {
        const auto pixel = line[128];
        ok &= check((pixel & 63U) == 63U && ((pixel >> 6) & 63U) == 0U &&
                    ((pixel >> 12) & 63U) == 0U && ((pixel >> 24) & 31U) == 31U,
                    "flat triangle coverage and RGB666 color");
        ok &= check(line[0] == 0U, "outside triangle retains transparent default clear plane");
    }
    if (!ok) std::exit(1);
}

void test_texture_formats() {
    TestTextureSource source;
    const auto texture_source = source.view();
    bool ok = true;
    const auto fmt = [](std::uint32_t format) { return format << 26; };

    source.textures[0][0] = 0xe3U;
    palette_color(source, 6, 0x001fU);
    auto texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(1), 0, 0, 0);
    ok &= check(texel.r == 63U && texel.a == 31U, "A3I5 decode");

    source.textures[0][0] = 3U;
    palette_color(source, 14, 0x03e0U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(2), 1U, 0, 0);
    ok &= check(texel.g == 63U && texel.a == 31U, "2-bit indexed decode and 8-byte palette base");

    source.textures[0][0] = 4U;
    palette_color(source, 8, 0x7c00U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(3), 0, 0, 0);
    ok &= check(texel.b == 63U && texel.a == 31U, "4-bit indexed decode");

    source.textures[0][0] = 7U;
    palette_color(source, 14, 0x001fU);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(4), 0, 0, 0);
    ok &= check(texel.r == 63U && texel.a == 31U, "8-bit indexed decode");
    source.textures[0][0] = 2U;
    source.textures[1][0] = 1U;
    source.textures[1][1] = 0x80U;
    palette_color(source, 24, 0x03e0U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.g == 63U && texel.a == 31U, "4x4 compressed metadata and PLTT_BASE offsets");

    source.textures[0][0] = 3U;
    source.textures[1][1] = 0U;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.a == 0U, "4x4 compressed transparent index");
    source.textures[0][0] = 2U;
    source.textures[1][1] = 0x40U;
    palette_color(source, 20, 0x001fU);
    palette_color(source, 22, 0x7c00U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.r == 31U && texel.b == 31U && texel.a == 31U,
                "4x4 compressed mode-one midpoint in RGB666");
    source.textures[1][1] = 0xc0U;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.r == 39U && texel.b == 23U && texel.a == 31U,
                "4x4 compressed mode-three weighted RGB666 colors");
    source.textures[0][0] = 3U;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.r == 23U && texel.b == 39U && texel.a == 31U,
                "4x4 compressed mode-three reversed weighted RGB666 colors");

    source.textures[0][0] = 2U;
    source.textures[1][1] = 0x40U;
    palette_color(source, 20, 0x0001U);
    palette_color(source, 22, 0U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.r == 1U, "4x4 compressed mode-one blends in cache precision");
    source.textures[1][1] = 0xc0U;
    palette_color(source, 20, 0U);
    palette_color(source, 22, 0x000bU);
    source.textures[0][0] = 3U;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(5), 1U, 0, 0);
    ok &= check(texel.r == 15U, "4x4 compressed mode-three blends expanded channels");
    source.textures[1][1] = 0xc0U;
    source.textures[0][0] = 2U;

    source.textures[0][0] = 0xfaU;
    palette_color(source, 4, 0x7c00U);
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(6), 0, 0, 0);
    ok &= check(texel.b == 63U && texel.a == 31U, "A5I3 decode");

    source.textures[0][0] = 0x1fU;
    source.textures[0][1] = 0x80U;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(7), 0, 0, 0);
    ok &= check(texel.r == 63U && texel.a == 31U, "direct-color decode");
    source.textures[0][0] = 1U;
    source.palettes[1][2] = 0x1fU;
    source.palettes[1][3] = 0U;
    source.palettes[4][2] = 0U;
    source.palettes[4][3] = 0x7cU;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(4), 0x1800U, 0, 0);
    ok &= check(texel.r == 63U && texel.b == 0U,
                "texture palette address slot 6 mirrors slot 1");
    source.palettes[2][2] = 0xe0U;
    source.palettes[2][3] = 0x03U;
    source.palettes[5][2] = 0U;
    source.palettes[5][3] = 0x7cU;
    texel = dsr::gpu3d_detail::decode_texel(texture_source, fmt(4), 0x1c00U, 0, 0);
    ok &= check(texel.g == 63U && texel.b == 0U,
                "texture palette address slot 7 mirrors slot 2");

    if (!ok) std::exit(1);
}

void test_texture_addressing_and_transparency() {
    TestTextureSource source;
    bool ok = true;
    for (unsigned format : {2U, 3U, 4U}) {
        palette_color(source, 0U, 0x001fU);
        const auto opaque = dsr::gpu3d_detail::decode_texel(source.view(), format << 26, 0U, 0, 0);
        const auto transparent = dsr::gpu3d_detail::decode_texel(
            source.view(), (format << 26) | (1U << 29), 0U, 0, 0);
        ok &= check(opaque.r == 63U && opaque.a == 31U && transparent.a == 0U,
                    "colour zero transparency applies to all indexed formats");
    }
    constexpr std::array<std::uint8_t, 8> alpha3{0, 4, 8, 13, 17, 22, 26, 31};
    constexpr std::uint32_t alpha_param = (1U << 26U) | (1U << 20U) |
                                         (1U << 23U) | (1U << 29U);
    for (unsigned intensity = 0; intensity < 32; ++intensity) {
        palette_color(source, intensity * 2U, static_cast<std::uint16_t>(
            intensity | (intensity << 5U) | (intensity << 10U)));
        for (unsigned alpha = 0; alpha < alpha3.size(); ++alpha)
            source.textures[0][alpha * 32U + intensity] =
                static_cast<std::uint8_t>((alpha << 5U) | intensity);
    }
    std::array<dsr::gpu3d_detail::Texel, 256> bulk{};
    dsr::gpu3d_detail::decode_texture(source.view(), alpha_param, 0U, bulk.data());
    bool scalar_matches = true;
    bool bulk_matches = true;
    for (unsigned alpha = 0; alpha < alpha3.size(); ++alpha) {
        for (unsigned intensity = 0; intensity < 32; ++intensity) {
            const auto offset = alpha * 32U + intensity;
            const auto scalar = dsr::gpu3d_detail::decode_texel(
                source.view(), alpha_param, 0U, offset % 16U, offset / 16U);
            const auto rgb = intensity == 0U ? 0U : intensity * 2U + 1U;
            scalar_matches &= scalar.r == rgb && scalar.g == rgb && scalar.b == rgb &&
                              scalar.a == alpha3[alpha];
            const auto& cached = bulk[offset];
            bulk_matches &= cached.r == scalar.r && cached.g == scalar.g &&
                            cached.b == scalar.b && cached.a == alpha3[alpha];
        }
    }
    ok &= check(scalar_matches, "A3I5 scalar RGB/alpha matches all 32 intensities x 8 truncated alpha levels");
    ok &= check(bulk_matches, "A3I5 bulk RGB/alpha matches scalar and ignores colour-zero transparency");
    constexpr std::uint32_t alpha5_param = (6U << 26U) | (1U << 20U) |
                                          (1U << 23U) | (1U << 29U);
    for (unsigned byte = 0; byte < bulk.size(); ++byte)
        source.textures[0][byte] = static_cast<std::uint8_t>(byte);
    dsr::gpu3d_detail::decode_texture(source.view(), alpha5_param, 0U, bulk.data());
    scalar_matches = true;
    bulk_matches = true;
    for (unsigned byte = 0; byte < bulk.size(); ++byte) {
        const auto scalar = dsr::gpu3d_detail::decode_texel(
            source.view(), alpha5_param, 0U, byte % 16U, byte / 16U);
        const auto intensity = byte & 7U;
        const auto rgb = intensity == 0U ? 0U : intensity * 2U + 1U;
        scalar_matches &= scalar.r == rgb && scalar.g == rgb && scalar.b == rgb &&
                          scalar.a == (byte >> 3U);
        bulk_matches &= bulk[byte].r == scalar.r && bulk[byte].g == scalar.g &&
                        bulk[byte].b == scalar.b && bulk[byte].a == scalar.a;
    }
    ok &= check(scalar_matches && bulk_matches,
                "A5I3 scalar and bulk preserve all 32 stored alpha levels x 8 indices");
    source.textures[2][8] = 2U;
    source.textures[1][0x10004] = 1U;
    source.textures[1][0x10005] = 0x80U;
    palette_color(source, 24U, 0x03e0U);
    const auto compressed = dsr::gpu3d_detail::decode_texel(
        source.view(), (5U << 26) | 0x8001U, 1U, 0, 0);
    ok &= check(compressed.g == 63U && compressed.a == 31U,
                "compressed slot-two data addresses slot-one upper metadata half");
    source.textures[1][0] = 1U;
    palette_color(source, 2U, 0x7c00U);
    const auto crossing = dsr::gpu3d_detail::decode_texel(
        source.view(), (4U << 26) | 0x3fffU, 0U, 0, 1);
    ok &= check(crossing.b == 63U && crossing.a == 31U,
                "texture rows carry into the next 128-KiB slot");
    if (!ok) std::exit(1);
}

void test_negative_texture_coordinate_wrap() {
    TestTextureSource source;
    source.textures[0][7] = 1U;
    palette_color(source, 0U, 0x7c00U);
    palette_color(source, 2U, 0x001fU);
    const auto render = [&](std::uint32_t wrap) {
        Gpu3D gpu;
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
        gpu.io_write(0x04000060U, 1U, 32);
        send(gpu, 0x2a, {(4U << 26) | wrap});
        send(gpu, 0x22, {0xffffU});
        triangle(gpu, 0x7fffU, 31U << 16, 0, 0, 0);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto repeated = render(1U << 16);
    const auto flipped = render((1U << 16) | (1U << 18));
    const auto clamped = render(1U << 18);
    bool ok = check((repeated & 63U) == 63U && ((repeated >> 12) & 63U) == 0U,
                    "negative fractional S floors before power-of-two repeat wrapping");
    ok &= check((flipped & 63U) == 0U && ((flipped >> 12) & 63U) == 63U &&
                clamped == flipped,
                "negative flipped repeat reflects texels, flip without repeat clamps");
    if (!ok) std::exit(1);
}

void test_textured_renderer_injection() {
    TestTextureSource source;
    source.textures[0][0] = 0x1fU;
    source.textures[0][1] = 0x80U;
    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    gpu.io_write(0x04000060U, 1U, 32);
    send(gpu, 0x2a, {7U << 26});
    triangle(gpu, 0x7fffU, (31U << 16) | (5U << 24), 0, 0, 0);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) == 63U && ((pixel >> 6) & 63U) == 0U &&
                    ((pixel >> 12) & 63U) == 0U && ((pixel >> 24) & 31U) == 31U,
                    "injected texture source reaches software rasterizer");
    TestTextureSource modulated_source;
    const auto direct_color = static_cast<std::uint16_t>(
        0x8000U | 3U | (13U << 5U) | (29U << 10U));
    modulated_source.textures[0][0] = static_cast<std::uint8_t>(direct_color);
    modulated_source.textures[0][1] = static_cast<std::uint8_t>(direct_color >> 8U);
    Gpu3D modulated_gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(
        modulated_gpu, modulated_source.view());
    modulated_gpu.io_write(0x04000060U, 1U, 32);
    send(modulated_gpu, 0x2a, {7U << 26U});
    triangle(modulated_gpu, 11U | (21U << 5U) | (7U << 10U),
             (31U << 16) | (4U << 24), 0, 0, 0);
    submit_frame(modulated_gpu);
    const auto modulated_pixel = modulated_gpu.line(96)[128];
    ok &= check((modulated_pixel & 63U) == 2U &&
                ((modulated_pixel >> 6U) & 63U) == 19U &&
                ((modulated_pixel >> 12U) & 63U) == 14U,
                "modulation expands both five-bit colors before its truncated six-bit product");
    Gpu3D decal_gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(decal_gpu, source.view());
    decal_gpu.io_write(0x04000060U, 1U, 32);
    send(decal_gpu, 0x2a, {7U << 26});
    triangle(decal_gpu, 0x7c00U, (31U << 16) | (1U << 4) | (6U << 24), 0, 0, 0);
    submit_frame(decal_gpu);
    const auto decal_pixel = decal_gpu.line(96)[128];
    ok &= check((decal_pixel & 63U) == 63U && ((decal_pixel >> 12) & 63U) == 0U,
                "opaque decal texture selects texture color exactly");

    source.textures[0][1] = 0U;
    Gpu3D transparent_gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(transparent_gpu, source.view());
    transparent_gpu.io_write(0x04000060U, 1U, 32);
    send(transparent_gpu, 0x2a, {7U << 26});
    triangle(transparent_gpu, 0x7c00U, (31U << 16) | (1U << 4) | (7U << 24), 0, 0, 0);
    submit_frame(transparent_gpu);
    const auto transparent_pixel = transparent_gpu.line(96)[128];
    ok &= check((transparent_pixel & 63U) == 0U && ((transparent_pixel >> 12) & 63U) == 63U,
                "transparent decal texture selects vertex color exactly");
    TestTextureSource translucent_decal_source;
    translucent_decal_source.textures[0][0] = static_cast<std::uint8_t>(3U << 5U);
    palette_color(translucent_decal_source, 0U, 0x001fU);
    Gpu3D translucent_decal_gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(
        translucent_decal_gpu, translucent_decal_source.view());
    translucent_decal_gpu.io_write(0x04000060U, 1U, 32);
    send(translucent_decal_gpu, 0x2a, {1U << 26U});
    triangle(translucent_decal_gpu, 0U, (31U << 16) | (1U << 4) | (4U << 24), 0, 0, 0);
    submit_frame(translucent_decal_gpu);
    const auto translucent_decal_pixel = translucent_decal_gpu.line(96)[128];
    ok &= check((translucent_decal_pixel & 63U) == 25U &&
                ((translucent_decal_pixel >> 6U) & 63U) == 0U &&
                ((translucent_decal_pixel >> 12U) & 63U) == 0U &&
                ((translucent_decal_pixel >> 24U) & 31U) == 31U,
                "decal blending uses the texture's raw five-bit alpha and truncates at bit five");
    if (!ok) std::exit(1);
}

void test_depth_test() {
    Gpu3D gpu;
    triangle(gpu, 0x001fU, (31U << 16) | (1U << 24), -8, -8, -8);
    triangle(gpu, 0x03e0U, (31U << 16) | (2U << 24), 8, 8, 8);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    check((pixel & 63U) == 63U && ((pixel >> 6) & 63U) == 0U,
          "nearer polygon wins z-buffer test");
}

void test_depth_rejection_skips_texture_sampling() {
    for (unsigned alpha : {0U, 31U}) {
        TestTextureSource source;
        source.textures[0][0] = 0x1fU;
        source.textures[0][1] = 0x80U;
        Gpu3D gpu;
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
        gpu.io_write(0x04000060U, 1U, 32);
        gpu.io_write(0x04000354U, 0U, 32);
        send(gpu, 0x2a, {7U << 26});
        triangle(gpu, 0x7fffU, (alpha << 16) | (5U << 24), 0, 0, 0);
        submit_frame(gpu);
        bool ok = check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 0,
                        "depth-rejected filled and wireframe fragments do not sample textures");
        gpu.io_write(0x04000354U, 0x7fffU, 32);
        gpu.render_start();
        ok &= check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) > 0,
                    "visible filled and wireframe fragments still sample textures");
        if (!ok) std::exit(1);
    }
}

void test_depth_equal_requires_matching_depth() {
    const auto render_overlay = [](std::int16_t depth_step) {
        Gpu3D gpu;
        triangle(gpu, 0x001fU, (31U << 16) | (1U << 24), 0, 0, 0);
        triangle(gpu, 0x03e0U,
                 (31U << 16) | (1U << 14) | (2U << 24),
                 depth_step, depth_step, depth_step);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto matching_depth = render_overlay(0);
    const auto different_depth = render_overlay(1);
    bool ok = check((matching_depth & 63U) == 0U &&
                    ((matching_depth >> 6) & 63U) == 63U,
                    "depth-equal accepts matching 24-bit depth");
    ok &= check((different_depth & 63U) == 63U &&
                ((different_depth >> 6) & 63U) == 0U,
                "depth-equal rejects a depth beyond the tolerance");
    if (!ok) std::exit(1);
}

void test_fog_density_interpolation() {
    Gpu3D gpu;
    gpu.io_write(0x04000060U, 1U << 7, 32);
    gpu.io_write(0x04000358U, (31U << 16) | 0x001fU, 32);
    for (std::uint32_t index = 0; index < 8U; ++index) {
        gpu.io_write(0x04000360U + index * 4U, index == 5U ? 0x7f000000U : 0U, 32);
    }
    send(gpu, 0x20, {0x7fffU});
    send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 15) | (8U << 24)});
    send(gpu, 0x40, {0});
    const auto vertex = [](int x, int y, int z) {
        return (static_cast<std::uint32_t>(x) & 0x3ffU) |
               ((static_cast<std::uint32_t>(y) & 0x3ffU) << 10) |
               ((static_cast<std::uint32_t>(z) & 0x3ffU) << 20);
    };
    send(gpu, 0x24, {vertex(-16, -16, 30), vertex(16, -16, 30), vertex(0, 16, 30)});
    send(gpu, 0x41);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    const auto green = (pixel >> 6) & 63U;
    const auto blue = (pixel >> 12) & 63U;
    bool ok = check(green >= 30U && green <= 33U && blue >= 30U && blue <= 33U,
                    "fog linearly interpolates adjacent density entries");
    if (!ok) std::exit(1);
}
void test_fog_descending_density_uses_weighted_truncation() {
    Gpu3D gpu;
    gpu.io_write(0x04000060U, 1U << 7, 32);
    gpu.io_write(0x04000358U, 31U << 16, 32);
    gpu.io_write(0x04000360U + 22U, 125U, 8);
    gpu.io_write(0x04000360U + 23U, 0U, 8);
    send(gpu, 0x20, {0x7fffU});
    send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 15) | (8U << 24)});
    send(gpu, 0x40, {0});
    const auto vertex = [](int x, int y, int z) {
        return (static_cast<std::uint32_t>(x) & 0x3ffU) |
               ((static_cast<std::uint32_t>(y) & 0x3ffU) << 10) |
               ((static_cast<std::uint32_t>(z) & 0x3ffU) << 20);
    };
    send(gpu, 0x24, {vertex(-16, -16, 30), vertex(16, -16, 30), vertex(0, 16, 30)});
    send(gpu, 0x41);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    bool ok = check(((pixel >> 6U) & 63U) == 32U && ((pixel >> 12U) & 63U) == 32U,
                    "descending fog densities use DeSmuME's weighted-sum truncation");
    if (!ok) std::exit(1);
}

void test_one_dot_depth_boundary() {
    const auto render_dot = [](std::uint32_t limit) {
        Gpu3D gpu;
        gpu.io_write(0x04000350U, (31U << 16) | 0x03e0U, 32);
        gpu.io_write(0x04000610U, limit, 32);
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 13) | (9U << 24)});
        send(gpu, 0x40, {0});
        position16(gpu, 0, 0, 0);
        position16(gpu, 0, 0, 0);
        position16(gpu, 0, 0, 0);
        send(gpu, 0x41);
        submit_frame(gpu);
        return gpu.line(96)[128]; // NDC origin maps to the inclusive viewport's centre.
    };
    const auto hidden = render_dot(4U);
    const auto visible = render_dot(8U);
    bool ok = check((hidden & 63U) == 0U && ((hidden >> 6) & 63U) == 63U,
                    "one-dot polygon is hidden beyond W boundary");
    ok &= check((visible & 63U) == 63U && ((visible >> 6) & 63U) == 0U,
                "one-dot polygon is visible at inclusive W boundary");
    if (!ok) std::exit(1);
}

void test_clipped_slivers_do_not_become_one_dot_polygons() {
    // Both original triangles span many pixels. Only their clipped fragments
    // fit within one pixel, crossing the left or top viewport boundary.
    constexpr std::array<std::array<std::int16_t, 2>, 3> left{{
        {-4608, 2048}, {-4080, 1131}, {-4608, 1024}}};
    constexpr std::array<std::array<std::int16_t, 2>, 3> top{{
        {-2560, 4779}, {-1872, 4075}, {-1792, 4779}}};
    constexpr std::uint32_t red = (31U << 24) | 63U;
    bool ok = true;
    for (int threads : {0, 2, 3}) {
        for (unsigned side = 0; side < 2; ++side) {
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            gpu.io_write(0x04000350U, (31U << 16) | 0x03e0U, 32);
            gpu.io_write(0x04000354U, 0x7fffU, 32);
            send(gpu, 0x20, {0x001fU});
            send(gpu, 0x29, {0xc0U | (31U << 16)});
            send(gpu, 0x40, {0});
            for (const auto& position : side == 0 ? left : top)
                position16(gpu, position[0], position[1], 0);
            send(gpu, 0x41);
            submit_frame(gpu);
            const unsigned x = side == 0 ? 0 : 69;
            const unsigned y = side == 0 ? 69 : 0;
            ok &= check(gpu.line(y)[x] == red,
                        "clipped subpixel sliver retains its filled boundary pixel");
            const auto view = gpu.raster_view();
            ok &= check(std::count(view.output, view.output + view.output_pixel_count, red) == 1,
                        "clipped sliver produces exactly one filled pixel");
        }
    }
    if (!ok) std::exit(1);
}

void test_shadow_requires_depth_mask_and_different_polygon_id() {
    const auto render_shadow = [](bool include_mask, std::uint8_t shadow_id) {
        Gpu3D gpu;
        shadow_scene(gpu, include_mask, shadow_id);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto no_mask = render_shadow(false, 1U);
    const auto same_id = render_shadow(true, 7U);
    const auto shadowed = render_shadow(true, 1U);
    bool ok = check(((no_mask >> 6) & 63U) == 63U,
                    "shadow color polygon without a depth-failed mask is discarded");
    ok &= check(((same_id >> 6) & 63U) == 63U,
                "shadow color polygon over the same polygon ID is discarded");
    ok &= check(((shadowed >> 6) & 63U) == 29U,
                "shadow color polygon darkens only the different-ID masked fragment");
    if (!ok) std::exit(1);
}

void test_shadow_preserves_vertex_color() {
    TestTextureSource source;
    source.textures[0][0] = 0x1fU;
    source.textures[0][1] = 0x80U;
    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    gpu.io_write(0x04000060U, 1U | (1U << 3), 32);
    triangle(gpu, 0x03e0U, (31U << 16) | (7U << 24), 0, 0, 0);
    triangle(gpu, 0U, (31U << 16) | (3U << 4), 0, 0, 0);
    send(gpu, 0x2a, {7U << 26});
    triangle(gpu, 0x7c00U, (16U << 16) | (3U << 4) | (1U << 24), -1, -1, -1);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    const bool ok = check((pixel & 63U) == 0U && ((pixel >> 6) & 63U) == 29U &&
                          ((pixel >> 12) & 63U) == 33U,
                          "shadow colour uses vertex RGB and ignores texture RGB");
    if (!ok) std::exit(1);
}

void test_translucent_id_rejection_ignores_blend_enable_and_depth_write() {
    const auto render = [](bool blending, bool third_polygon) {
        Gpu3D gpu;
        gpu.io_write(0x04000060U, blending ? 1U << 3 : 0U, 32);
        triangle(gpu, 0x001fU, (16U << 16) | (1U << 24), 0, 0, 0);
        triangle(gpu, 0x03e0U, (16U << 16) | (1U << 24) | (1U << 11), -2, -2, -2);
        if (third_polygon)
            triangle(gpu, 0x7c00U, (16U << 16) | (2U << 24), -1, -1, -1);
        submit_frame(gpu, 1U);
        return gpu.line(96)[128];
    };
    const auto no_blend = render(false, false);
    const auto depth = render(true, true);
    bool ok = check((no_blend & 63U) == 63U && ((no_blend >> 6) & 63U) == 0U,
                    "same translucent ID is rejected even with alpha blending disabled");
    ok &= check(((depth >> 12) & 63U) == 33U,
                "rejected translucent fragment does not update depth even with bit eleven set");
    if (!ok) std::exit(1);
}

void test_translucent_blending() {
    Gpu3D gpu;
    gpu.io_write(0x04000350U, 31U << 16, 32); // opaque clear plane for blend test
    gpu.io_write(0x04000060U, 1U << 3, 32);
    triangle(gpu, 0x001fU, (16U << 16) | (1U << 24), 0, 0, 0);
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) == 33U, "translucent RGB blend coefficient");
    ok &= check(((pixel >> 24) & 31U) == 31U, "translucent blend preserves opaque clear alpha");
    if (!ok) std::exit(1);
}
void test_edge_marking_uses_neighbor_color_and_antialias_alpha() {
    Gpu3D plain;
    Gpu3D marked;
    Gpu3D marked_without_aa;
    marked.io_write(0x04000060U, (1U << 4) | (1U << 5), 32);
    marked_without_aa.io_write(0x04000060U, 1U << 5, 32);
    marked.io_write(0x04000330U, 0x001f7c00U, 32); // ID 0 blue, ID 8 red
    marked_without_aa.io_write(0x04000330U, 0x001f7c00U, 32);
    plain.io_write(0x04000354U, 0x7fffU, 32);
    marked.io_write(0x04000354U, 0x7fffU, 32);
    marked_without_aa.io_write(0x04000354U, 0x7fffU, 32);
    edge_triangle(plain);
    edge_triangle(marked);
    edge_triangle(marked_without_aa);
    submit_frame(plain);
    submit_frame(marked);
    submit_frame(marked_without_aa);

    bool marked_outside = false;
    bool correct_aa_edge = false;
    bool correct_opaque_edge = false;
    bool wrong_edge_color = false;
    bool marked_inside = false;
    for (int y = 0; y < 192; ++y) {
        const auto* plain_line = plain.line(y);
        const auto* marked_line = marked.line(y);
        const auto* opaque_line = marked_without_aa.line(y);
        for (int x = 0; x < 256; ++x) {
            const auto baseline = plain_line[x];
            const auto edge = marked_line[x];
            if (((baseline >> 24) & 31U) == 0U) {
                if (((edge >> 24) & 31U) != 0U) {
                    marked_outside = true;
                    wrong_edge_color |= (edge & 63U) != 63U ||
                        ((edge >> 6U) & 63U) != 0U || ((edge >> 12U) & 63U) != 0U;
                    correct_aa_edge |= ((edge >> 24U) & 31U) == 16U &&
                        (opaque_line[x] & 63U) == 63U &&
                        ((opaque_line[x] >> 24U) & 31U) == 31U;
                    correct_opaque_edge |= (opaque_line[x] & 63U) == 63U &&
                        ((opaque_line[x] >> 24U) & 31U) == 31U;
                }
                continue;
            }
            marked_inside |= edge != baseline;
        }
    }
    bool ok = check(marked_outside,
                    "edge marking paints the farther clear-plane pixel");
    ok &= check(!wrong_edge_color,
                "edge marking uses the nearer neighbor's polygon edge color");
    ok &= check(!marked_inside,
                "edge marking does not paint the nearer opaque pixel");
    ok &= check(correct_aa_edge,
                "antialiased edge blending preserves destination alpha on transparent pixels");
    ok &= check(correct_opaque_edge,
                "edge marking without anti-aliasing writes an opaque neighbor edge color");
    if (!ok) std::exit(1);
}
void test_ambient_lighting_sums_enabled_lights() {
    Gpu3D gpu;
    send(gpu, 0x30, {8U << 16});
    send(gpu, 0x31, {0U});
    send(gpu, 0x32, {0U});
    send(gpu, 0x33, {0x001fU});
    send(gpu, 0x32, {1U << 30});
    send(gpu, 0x33, {(1U << 30) | 0x001fU});
    send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 24) | 3U});
    send(gpu, 0x40, {0U});
    send(gpu, 0x21, {0U});
    position16(gpu, -4096, -4096, 0);
    position16(gpu, 4096, -4096, 0);
    position16(gpu, 0, 4096, 0);
    send(gpu, 0x41);
    submit_frame(gpu);

    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) == 29U,
                    "ambient contribution is accumulated once for each enabled light");
    ok &= check(((pixel >> 6) & 63U) == 0U && ((pixel >> 12) & 63U) == 0U,
                "ambient-light test keeps unused color channels clear");
    if (!ok) std::exit(1);
}

void test_specular_lighting_uses_reference_half_vector_normalization() {
    Gpu3D gpu;
    send(gpu, 0x30, {0U});
    send(gpu, 0x31, {31U});
    send(gpu, 0x32, {0x20000000U}); // light points along -Z
    send(gpu, 0x33, {0x001fU});
    send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 24) | 1U});
    send(gpu, 0x40, {0U});
    send(gpu, 0x21, {0x1ff00000U}); // normal points along +Z
    position16(gpu, -4096, -4096, 0);
    position16(gpu, 4096, -4096, 0);
    position16(gpu, 0, 4096, 0);
    send(gpu, 0x41);
    submit_frame(gpu);

    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) != 0U,
                    "specular term is lit under the reference unit-length half vector");
    ok &= check(((pixel >> 6) & 63U) == 0U && ((pixel >> 12) & 63U) == 0U,
                "specular-light test keeps unused color channels clear");
    if (!ok) std::exit(1);
}

void test_light_half_vector_matches_reference_normalization() {
    // Light along -Z maps (0, 0, -8192) to (0, 0, -4096) in Q4.12.
    // Project compatibility target: a reference emulator's observable output.
    Gpu3D gpu;
    send(gpu, 0x32, {0x20000000U});
    gpu.run(100);
    const auto half = dsr::gpu3d_detail::Gpu3DTestAccess::light_half_vector(gpu, 0);
    bool ok = check(half[0] == 0 && half[1] == 0 && half[2] == -4096,
                    "light half vector is normalized to unit length (reference scale)");
    if (!ok) std::exit(1);
}




void test_no_swap_renders_retained_geometry_and_clear_plane() {
    Gpu3D gpu;
    gpu.io_write(0x04000350U, (31U << 16) | 0x7c00U, 32); // opaque blue clear plane
    triangle(gpu, 0x001fU, (31U << 16) | (7U << 24), 0, 0, 0);
    submit_frame(gpu);

    bool ok = check(gpu.line(0)[0] == ((31U << 24) | (63U << 12)),
                    "opaque rear-plane color is rendered");
    ok &= check(gpu.line(96)[128] == ((31U << 24) | 63U),
                "swapped geometry is rendered");

    gpu.io_write(0x04000350U, 0x03e0U, 32); // transparent green clear plane
    gpu.vblank_start(); // no new SWAP_BUFFERS
    gpu.render_start();
    ok &= check(gpu.line(0)[0] == (63U << 6),
                "no-swap frame uses live clear color and zero alpha");
    ok &= check(gpu.line(96)[128] == ((31U << 24) | 63U),
                "no-swap frame retains active geometry over transparent clear");

    submit_frame(gpu); // the empty geometry list now replaces the retained triangle
    ok &= check(gpu.line(96)[128] == (63U << 6),
                "next buffer swap replaces retained geometry");
    if (!ok) std::exit(1);
}

void test_box_test_and_near_clipping() {
    Gpu3D box_gpu;
    send(box_gpu, 0x70, {
        static_cast<std::uint16_t>(-4096) |
            (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-4096)) << 16),
        8192U << 16, 8192U | (8192U << 16)});
    box_gpu.run(1000);
    bool ok = check((box_gpu.io_read(0x04000600U, 32) & (1U << 1)) != 0U,
                    "box test intersects view volume");
    send(box_gpu, 0x70, {8192U, 0U, 0U});
    box_gpu.run(1000);
    ok &= check((box_gpu.io_read(0x04000600U, 32) & (1U << 1)) == 0U,
                "box test rejects outside volume");
    const auto pack_box_xy = [](std::int16_t x, std::int16_t y) {
        return static_cast<std::uint16_t>(x) |
               (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16);
    };
    Gpu3D wrapped_box_gpu;
    send(wrapped_box_gpu, 0x70, {
        pack_box_xy(30720, -4096), pack_box_xy(-4096, 4096), pack_box_xy(8192, 8192)});
    wrapped_box_gpu.run(1000);
    ok &= check((wrapped_box_gpu.io_read(0x04000600U, 32) & (1U << 1)) != 0U,
                "box test wraps extent endpoints in Q4.12");
    Gpu3D rotated_box_gpu;
    send(rotated_box_gpu, 0x10, {1});
    send(rotated_box_gpu, 0x16, {
        2896U, static_cast<std::uint32_t>(-2896), 0U, 0U,
        2896U, 2896U, 0U, 0U,
        0U, 0U, 4096U, 0U,
        5325U, 5325U, 0U, 4096U});
    send(rotated_box_gpu, 0x70, {
        pack_box_xy(-4096, -410), pack_box_xy(-410, 8192), pack_box_xy(819, 819)});
    rotated_box_gpu.run(1000);
    ok &= check((rotated_box_gpu.io_read(0x04000600U, 32) & (1U << 1)) == 0U,
                "box test rejects a rotated box outside a frustum corner");

    Gpu3D clip_gpu;
    triangle(clip_gpu, 0x7c00U, (31U << 16) | (3U << 24), -32, 0, 0);
    submit_frame(clip_gpu);
    const auto center = clip_gpu.line(96)[128];
    ok &= check(((center >> 24) & 31U) == 31U && ((center >> 12) & 63U) != 0U,
                "polygon crossing near plane is clipped and rasterized");
    if (!ok) std::exit(1);
}

std::uint32_t packed10(int x, int y, int z) {
    return (static_cast<std::uint32_t>(x) & 0x3ffU) |
           ((static_cast<std::uint32_t>(y) & 0x3ffU) << 10) |
           ((static_cast<std::uint32_t>(z) & 0x3ffU) << 20);
}

void set_bitmap_color(TestTextureSource& source, std::size_t slot,
                      std::size_t x, std::size_t y, std::uint16_t color) {
    const auto offset = (y * 256U + x) * 2U;
    source.textures[slot][offset] = static_cast<std::uint8_t>(color);
    source.textures[slot][offset + 1U] = static_cast<std::uint8_t>(color >> 8);
}

void test_matrix_store_restore_and_gxstat_stack_bits() {
    Gpu3D projection;
    send(projection, 0x10, {0U});
    send(projection, 0x15);
    send(projection, 0x1c, {4096U, 0U, 0U});
    send(projection, 0x11);
    projection.run(10000);
    bool ok = check((projection.io_read(0x04000600U, 32) & (1U << 13)) != 0U,
                    "projection matrix push exposes its stack pointer in GXSTAT");

    send(projection, 0x15);
    send(projection, 0x12, {0U});
    send(projection, 0x71, {0U, 0U});
    projection.run(10000);
    ok &= check(projection.io_read(0x04000620U, 32) == 4096U &&
                (projection.io_read(0x04000600U, 32) & (1U << 13)) == 0U,
                "projection pop ignores its parameter, restores the matrix, and clears the pointer");

    send(projection, 0x11);
    send(projection, 0x11);
    projection.run(10000);
    ok &= check((projection.io_read(0x04000600U, 32) & (1U << 15)) != 0U,
                "projection stack overflow sets GXSTAT bit 15");
    projection.io_write(0x04000600U, 1U << 15, 32);
    ok &= check((projection.io_read(0x04000600U, 32) & ((1U << 13) | (1U << 15))) == 0U,
                "acknowledging a matrix stack error clears the error and projection pointer");

    Gpu3D projection_store;
    send(projection_store, 0x10, {0U});
    send(projection_store, 0x15);
    send(projection_store, 0x1c, {4096U, 0U, 0U});
    send(projection_store, 0x13, {31U});
    send(projection_store, 0x15);
    send(projection_store, 0x14, {31U});
    send(projection_store, 0x71, {0U, 0U});
    projection_store.run(10000);
    ok &= check(projection_store.io_read(0x04000620U, 32) == 4096U &&
                (projection_store.io_read(0x04000600U, 32) & (1U << 15)) == 0U,
                "projection store/restore ignores the address and uses its single matrix slot");

    Gpu3D coordinate;
    send(coordinate, 0x10, {1U});
    send(coordinate, 0x15);
    send(coordinate, 0x1c, {4096U, 0U, 0U});
    send(coordinate, 0x13, {31U});
    send(coordinate, 0x15);
    send(coordinate, 0x14, {31U});
    send(coordinate, 0x71, {0U, 0U});
    coordinate.run(10000);
    ok &= check((coordinate.io_read(0x04000600U, 32) & (1U << 15)) != 0U &&
                coordinate.io_read(0x04000620U, 32) == 4096U,
                "coordinate-stack slot 31 stores and restores data while reporting its reserved-slot error");
    coordinate.io_write(0x04000600U, 1U << 15, 32);
    ok &= check((coordinate.io_read(0x04000600U, 32) & (1U << 15)) == 0U,
                "writing one to GXSTAT bit 15 acknowledges the stack error");
    if (!ok) std::exit(1);
}

void test_matrix_busy_bit_only_covers_push_pop() {
    Gpu3D gpu;
    send(gpu, 0x10, {1U});
    gpu.run(1);
    bool ok = check((gpu.io_read(0x04000600U, 32) & (1U << 14)) == 0U,
                    "matrix-mode command does not assert the matrix-stack busy bit");
    ok &= check((gpu.io_read(0x04000600U, 32) & (1U << 27)) != 0U,
                "GXSTAT geometry busy remains asserted while a command is pending");
    gpu.run(1);

    send(gpu, 0x11);
    gpu.run(1);
    ok &= check((gpu.io_read(0x04000600U, 32) & (1U << 14)) != 0U,
                "matrix-stack busy bit is asserted while a push is executing");
    gpu.run(1);
    ok &= check((gpu.io_read(0x04000600U, 32) & (1U << 14)) == 0U,
                "matrix-stack busy bit clears when the push completes");
    if (!ok) std::exit(1);
}

std::array<std::int64_t, 2> transform_texture_coordinates(unsigned mode,
                                                       std::uint32_t coordinate = 16U) {
    Gpu3D gpu;
    send(gpu, 0x10, {3U});
    send(gpu, 0x15);
    send(gpu, 0x1c, {8192U, 0U, 0U});
    send(gpu, 0x13, {0U});
    send(gpu, 0x15);
    send(gpu, 0x14, {0U});
    send(gpu, 0x2a, {mode << 30});
    send(gpu, 0x22, {coordinate});
    if (mode == 2U) send(gpu, 0x21, {0U});
    if (mode == 3U) send(gpu, 0x24, {packed10(1, 0, 0)});
    gpu.run(10000);
    return dsr::gpu3d_detail::Gpu3DTestAccess::transformed_texcoord(gpu);
}

void test_texture_matrix_store_restore_and_coordinate_modes() {
    const auto untransformed = transform_texture_coordinates(0U);
    const auto texture_source = transform_texture_coordinates(1U);
    const auto normal_source = transform_texture_coordinates(2U);
    const auto vertex_source = transform_texture_coordinates(3U);
    bool ok = check(untransformed[0] == 4096,
                    "texture mode zero keeps the signed 4.12 texture coordinate");
    ok &= check(texture_source[0] == 4608,
                "texture mode one applies stored matrix translation at one-sixteenth scale");
    ok &= check(normal_source[0] == 4096 && vertex_source[0] == 4096,
                "texture modes two and three add TEXCOORD and ignore the matrix translation row");
    ok &= check(transform_texture_coordinates(1U, 256U)[0] == 66048,
                "texture transforms preserve coordinates above eight texels");
    ok &= check(transform_texture_coordinates(1U, 0x7ffdU)[0] == 0x7fff00,
                "texture transform preserves signed values near the 12.4 input maximum");
    ok &= check(transform_texture_coordinates(1U, 0x8000U)[0] == -0x7ffe00,
                "texture transform sign-extends the negative 12.4 range");
    if (!ok) std::exit(1);
}

void test_material_diffuse_sets_vertex_color() {
    Gpu3D gpu;
    // DIF_AMB bit 15 copies its diffuse RGB to COLOR; subsequent COLOR
    // commands remain independent of the material register.
    send(gpu, 0x30, {(1U << 15U) | 0x7c00U});
    send(gpu, 0x29, {0xc0U | (31U << 16U)});
    send(gpu, 0x40, {0U});
    position16(gpu, -4096, -4096, 0);
    send(gpu, 0x20, {0x001fU});
    position16(gpu, 4096, -4096, 0);
    position16(gpu, 0, 4096, 0);
    send(gpu, 0x41);
    submit_frame(gpu);
    const auto geometry = gpu.geometry_view();
    bool ok = check(geometry.vertex_count == 3U &&
                    geometry.vertices[0].color == std::array<float, 3>{0.0F, 0.0F, 63.0F} &&
                    geometry.vertices[1].color == std::array<float, 3>{63.0F, 0.0F, 0.0F},
                    "DIF_AMB bit 15 sets diffuse RGB as COLOR; COLOR then writes an independent value");
    if (!ok) std::exit(1);
}


void test_vertex_color_interpolates_in_six_bit_framebuffer_space() {
    Gpu3D gpu;
    send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 24)});
    send(gpu, 0x40, {0U});
    send(gpu, 0x20, {0x001fU});
    position16(gpu, -4096, -4096, 0);
    send(gpu, 0x20, {0U});
    position16(gpu, 4096, -4096, 0);
    position16(gpu, 0, 4096, 0);
    send(gpu, 0x41);
    submit_frame(gpu);

    const auto red = gpu.line(96)[128] & 63U;
    bool ok = check(red == 16U,
                    "vertex color interpolation keeps six-bit precision between 5-bit vertices");
    if (!ok) std::exit(1);
}
void test_vertex_packed_coordinate_commands() {
    const auto render = [](unsigned command) {
        Gpu3D gpu;
        if (command == 0x26U) {
            send(gpu, 0x10, {1U});
            send(gpu, 0x16, {
                4096U, 0U, 0U, 0U,
                0U, 0U, 0U, 0U,
                0U, 4096U, 0U, 0U,
                0U, 0U, 0U, 4096U});
        } else if (command == 0x27U) {
            send(gpu, 0x10, {1U});
            send(gpu, 0x16, {
                0U, 0U, 0U, 0U,
                4096U, 0U, 0U, 0U,
                0U, 4096U, 0U, 0U,
                0U, 0U, 0U, 4096U});
        }
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (2U << 24)});
        send(gpu, 0x40, {0U});
        if (command == 0x24U) {
            send(gpu, 0x24, {packed10(-32, -32, 0)});
            send(gpu, 0x24, {packed10(32, -32, 0)});
            send(gpu, 0x24, {packed10(0, 32, 0)});
        } else if (command == 0x25U) {
            position16(gpu, -2048, -2048, 0);
            send(gpu, 0x25, {static_cast<std::uint16_t>(4096) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-2048)) << 16)});
            send(gpu, 0x25, {static_cast<std::uint32_t>(static_cast<std::uint16_t>(0)) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(2048)) << 16)});
        } else if (command == 0x26U) {
            send(gpu, 0x26, {static_cast<std::uint16_t>(-2048) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-2048)) << 16)});
            send(gpu, 0x26, {static_cast<std::uint16_t>(2048) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-2048)) << 16)});
            send(gpu, 0x26, {static_cast<std::uint32_t>(0) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(2048)) << 16)});
        } else if (command == 0x27U) {
            send(gpu, 0x27, {static_cast<std::uint16_t>(-2048) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-2048)) << 16)});
            send(gpu, 0x27, {static_cast<std::uint16_t>(2048) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-2048)) << 16)});
            send(gpu, 0x27, {static_cast<std::uint32_t>(0) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(2048)) << 16)});
        } else {
            position16(gpu, -256, -256, 0);
            send(gpu, 0x28, {packed10(511, 0, 0)});
            send(gpu, 0x28, {packed10(-255, 511, 0)});
        }
        send(gpu, 0x41);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };

    constexpr std::array<unsigned, 5> commands{0x24U, 0x25U, 0x26U, 0x27U, 0x28U};
    bool ok = true;
    for (const auto command : commands) {
        const auto pixel = render(command);
        ok &= check((pixel & 63U) == 63U && ((pixel >> 24) & 31U) == 31U,
                    "packed vertex command emits signed coordinates and preserves inherited axes");
    }
    if (!ok) std::exit(1);
}

void test_toon_alpha_test_and_wireframe() {
    Gpu3D toon;
    toon.io_write(0x04000390U, 0x7c00U, 16);
    send(toon, 0x20, {8U});
    send(toon, 0x29, {0xc0U | (2U << 4) | (31U << 16) | (3U << 24)});
    send(toon, 0x40, {0U});
    position16(toon, -2048, -2048, 0);
    position16(toon, 2048, -2048, 0);
    position16(toon, 0, 2048, 0);
    send(toon, 0x41);
    submit_frame(toon);
    const auto toon_pixel = toon.line(96)[128];
    bool ok = check((toon_pixel & 63U) == 0U && ((toon_pixel >> 12) & 63U) == 63U,
                    "polygon mode two selects and expands the toon-table entry by red index");
    const auto render_highlight = [](bool highlight, bool texturing) {
        TestTextureSource source;
        source.textures[0][0] = 0xffU;
        source.textures[0][1] = 0xffU;
        Gpu3D gpu;
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
        gpu.io_write(0x04000060U, (texturing ? 1U : 0U) | (highlight ? 1U << 1 : 0U), 32);
        gpu.io_write(0x04000390U, 0x0842U, 16);
        send(gpu, 0x2a, {7U << 26});
        send(gpu, 0x20, {8U});
        send(gpu, 0x29, {0xc0U | (2U << 4) | (31U << 16) | (3U << 24)});
        send(gpu, 0x40, {0U});
        position16(gpu, -2048, -2048, 0);
        position16(gpu, 2048, -2048, 0);
        position16(gpu, 0, 2048, 0);
        send(gpu, 0x41);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto toon_texture = render_highlight(false, true);
    const auto highlight_texture = render_highlight(true, true);
    const auto highlight_untextured = render_highlight(true, false);
    ok &= check((toon_texture & 63U) == 5U && ((toon_texture >> 6U) & 63U) == 5U &&
                ((toon_texture >> 12U) & 63U) == 5U,
                "toon shading modulates the texture with the toon-table entry");
    ok &= check((highlight_texture & 63U) == 22U &&
                ((highlight_texture >> 6U) & 63U) == 22U &&
                ((highlight_texture >> 12U) & 63U) == 22U,
                "highlight shading adds toon color to modulation of the source red channel");
    ok &= check((highlight_untextured & 63U) == 22U &&
                ((highlight_untextured >> 6U) & 63U) == 22U &&
                ((highlight_untextured >> 12U) & 63U) == 22U,
                "highlight shading uses white as the texture when texturing is disabled");

    const auto alpha_pixel = [](std::uint32_t alpha) {
        Gpu3D gpu;
        gpu.io_write(0x04000060U, 1U << 2, 32);
        gpu.io_write(0x04000340U, 16U, 8);
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {0xc0U | (alpha << 16) | (4U << 24)});
        send(gpu, 0x40, {0U});
        position16(gpu, -2048, -2048, 0);
        position16(gpu, 2048, -2048, 0);
        position16(gpu, 0, 2048, 0);
        send(gpu, 0x41);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto below_reference = alpha_pixel(15U);
    const auto at_reference = alpha_pixel(16U);
    const auto above_reference = alpha_pixel(17U);
    ok &= check(((below_reference >> 24) & 31U) == 0U &&
                ((at_reference >> 24) & 31U) == 16U &&
                ((above_reference >> 24) & 31U) == 17U,
                "alpha test rejects only values below the reference value");

    Gpu3D wire;
    send(wire, 0x20, {0x001fU});
    send(wire, 0x29, {0xc0U | (5U << 24)});
    send(wire, 0x40, {0U});
    position16(wire, -2048, -2048, 0);
    position16(wire, 2048, -2048, 0);
    position16(wire, 0, 2048, 0);
    send(wire, 0x41);
    submit_frame(wire);
    bool found_wire_edge = false;
    for (int y = 0; y < 192 && !found_wire_edge; ++y) {
        const auto* line = wire.line(y);
        for (int x = 0; x < 256; ++x) {
            if ((line[x] & 63U) != 0U) {
                found_wire_edge = true;
                break;
            }
        }
    }
    ok &= check(((wire.line(96)[128] >> 24) & 31U) == 0U && found_wire_edge,
                "zero-alpha polygon renders only its wireframe edges as opaque pixels");
    if (!ok) std::exit(1);
}


void test_clear_image_bitmap_scroll_depth_and_fog() {
    TestTextureSource source;
    source.textures[3].fill(0xffU);
    set_bitmap_color(source, 2U, 1U, 1U, 0x801fU);
    set_bitmap_color(source, 2U, 129U, 97U, 0x801fU);
    const auto center_depth = (97U * 256U + 129U) * 2U;
    source.textures[3][center_depth] = 0U;
    source.textures[3][center_depth + 1U] = 0x80U;

    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    gpu.io_write(0x04000060U, 1U << 14, 32);
    gpu.io_write(0x04000356U, 0x0101U, 16);
    gpu.vblank_start();
    gpu.render_start();
    bool ok = check((gpu.line(0)[0] & 63U) == 63U &&
                    ((gpu.line(0)[0] >> 24) & 31U) == 31U,
                    "bitmap clear plane applies the wrapped rear-image scroll offsets");

    gpu.io_write(0x04000060U, (1U << 14) | (1U << 7), 32);
    gpu.io_write(0x04000358U, (31U << 16) | 0x001fU, 32);
    gpu.io_write(0x04000360U, 0x7fU, 32);
    triangle(gpu, 0x03e0U, (31U << 16) | (6U << 24), 0, 0, 0);
    submit_frame(gpu);
    const auto center = gpu.line(96)[128];
    ok &= check((center & 63U) == 63U && ((center >> 6) & 63U) == 0U,
                "bitmap rear depth occludes geometry and bitmap fog enables rear-plane fog");
    if (!ok) std::exit(1);
}

void test_clear_image_unmapped_slots_are_independently_zero() {
    TestTextureSource source;
    set_bitmap_color(source, 2U, 128U, 96U, 0x801fU);
    source.textures[3].fill(0xffU);
    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    gpu.io_write(0x04000060U, 1U << 14, 32);
    gpu.io_write(0x04000350U, (31U << 16) | 0x03e0U, 32);
    gpu.io_write(0x04000354U, 0x7fffU, 32);
    source.mapped_textures = 1U << 2;
    triangle(gpu, 0x7c00U, (31U << 16) | (7U << 24), 0, 0, 0);
    submit_frame(gpu);
    bool ok = check(gpu.line(96)[128] == ((31U << 24) | 63U),
                    "unmapped clear depth is zero while mapped clear colour remains visible");
    source.mapped_textures = 1U << 3;
    gpu.vblank_start();
    gpu.render_start();
    ok &= check(gpu.line(0)[0] == 0U,
                "unmapped clear colour is transparent black, not CLEAR_COLOR");
    ok &= check(gpu.line(96)[128] == ((31U << 24) | (63U << 12)),
                "mapped clear depth remains usable when clear colour is unmapped");
    source.mapped_textures = 0U;
    gpu.vblank_start();
    gpu.render_start();
    ok &= check(gpu.line(96)[128] == 0U,
                "both unmapped clear image slots produce zero colour and zero depth");
    if (!ok) std::exit(1);
}

void test_polygon_vertex_ram_overflow_flags() {
    Gpu3D gpu;
    send(gpu, 0x40, {2U});
    gpu.run(16);
    for (std::size_t vertex = 0; vertex < 2053U; ++vertex) {
        port(gpu, 0x24, {0U});
        gpu.run(8);
    }
    const auto polygon_overflow = gpu.io_read(0x04000604U, 32);
    bool ok = check((polygon_overflow & 0xfffU) == 2048U &&
                    (polygon_overflow >> 16) == 2053U &&
                    (gpu.io_read(0x04000060U, 32) & (1U << 13)) != 0U,
                    "polygon RAM overflow saturates polygon count and raises DISP3DCNT bit 13");

    gpu.io_write(0x04000060U, 1U << 13, 32);
    ok &= check((gpu.io_read(0x04000060U, 32) & (1U << 13)) == 0U,
                "writing one to DISP3DCNT bit 13 acknowledges RAM overflow");
    for (std::size_t vertex = 2053U; vertex < 6145U; ++vertex) {
        port(gpu, 0x24, {0U});
        gpu.run(8);
    }
    const auto vertex_overflow = gpu.io_read(0x04000604U, 32);
    ok &= check((vertex_overflow >> 16) == 6144U &&
                (gpu.io_read(0x04000060U, 32) & (1U << 13)) != 0U,
                "vertex RAM overflow saturates vertex count and raises DISP3DCNT bit 13");
    if (!ok) std::exit(1);
}

void test_enhanced_geometry_preserves_hardware_status() {
    bool ok = true;
    for (const bool enhanced : {false, true}) {
        Gpu3D gpu;
        gpu.set_enhanced_geometry(enhanced);
        send(gpu, 0x40, {2U}); // triangle strip
        gpu.run(16);
        for (std::size_t vertex = 0; vertex < 2053U; ++vertex) {
            port(gpu, 0x24, 0U);
            gpu.run(8);
        }
        const auto count = gpu.io_read(0x04000604U, 32);
        ok &= check((count & 0xfffU) == 2048U && (count >> 16) == 2053U &&
                    (gpu.io_read(0x04000060U, 32) & (1U << 13)) != 0U,
                    "enhanced polygon capacity retains hardware count and overflow");
        send(gpu, 0x50, {0U});
        gpu.run(100000);
        gpu.vblank_start();
        const auto view = gpu.geometry_view();
        ok &= check(view.polygon_count == (enhanced ? 2051U : 2048U) &&
                    view.polygon_overflow,
                    "enhanced strip retains extra polygons without hiding overflow");
    }
    {
        Gpu3D gpu;
        gpu.set_enhanced_geometry(true);
        send(gpu, 0x40, {1U}); // independent quads
        gpu.run(16);
        for (std::size_t vertex = 0; vertex < 6148U; ++vertex) {
            port(gpu, 0x24, 0U);
            gpu.run(8);
        }
        const auto count = gpu.io_read(0x04000604U, 32);
        ok &= check((count & 0xfffU) == 1536U && (count >> 16) == 6144U &&
                    (gpu.io_read(0x04000060U, 32) & (1U << 13)) != 0U,
                    "extra vertices cannot advance the hardware polygon count");
        send(gpu, 0x50, {0U});
        gpu.run(100000);
        gpu.vblank_start();
        const auto view = gpu.geometry_view();
        ok &= check(view.vertex_count == 6148U && view.polygon_count == 1537U &&
                    view.vertex_overflow && !view.polygon_overflow,
                    "enhanced vertices retain extra quads with hardware overflow flags");
    }
    if (!ok) std::exit(1);
}

void test_disabling_enhanced_geometry_preserves_oversized_lists() {
    Gpu3D gpu;
    gpu.set_render_threads(0);
    gpu.set_enhanced_geometry(true);
    send(gpu, 0x40, {2U}); // triangle strip, still open at the transition
    gpu.run(16);
    for (std::size_t i = 0; i < 2053U; ++i) {
        port(gpu, 0x24, 0U);
        gpu.run(8);
    }
    const auto count = gpu.io_read(0x04000604U, 32);
    const auto status = gpu.io_read(0x04000060U, 32);
    gpu.set_enhanced_geometry(false);
    bool ok = check(gpu.io_read(0x04000604U, 32) == count &&
                    gpu.io_read(0x04000060U, 32) == status,
                    "disabling enhanced geometry preserves guest RAM counts and overflow");
    port(gpu, 0x24, 0U);
    gpu.run(8);
    ok &= check((gpu.io_read(0x04000604U, 32) >> 16) == 2054U,
                "open primitive continues after disabling enhanced geometry");
    send(gpu, 0x50, {0U});
    gpu.run(100000);
    gpu.vblank_start();
    auto view = gpu.geometry_view();
    ok &= check(view.vertex_count == 2054U && view.polygon_count == 2051U &&
                view.polygon_overflow,
                "oversized submission survives the transition and swap");
    gpu.set_enhanced_geometry(false);
    view = gpu.geometry_view();
    ok &= check(view.vertex_count == 2054U && view.polygon_count == 2051U,
                "oversized render list survives a repeated disable while paused");
    send(gpu, 0x50, {0U});
    gpu.run(100000);
    gpu.vblank_start();
    view = gpu.geometry_view();
    ok &= check(view.vertex_count == 0U && view.polygon_count == 0U,
                "subsequent empty swap retires the oversized render list");
    if (!ok) std::exit(1);
}

void test_enhanced_geometry_capacity_overflow_lifecycle() {
    constexpr std::size_t capacity = 12288U;
    bool ok = true;
    for (const bool enhanced : {false, true}) {
        Gpu3D gpu;
        gpu.set_enhanced_geometry(enhanced);
        const auto vertices = [&gpu](std::size_t count) {
            send(gpu, 0x40, {1U}); // independent quads
            gpu.run(16);
            for (std::size_t i = 0; i < count; ++i) {
                port(gpu, 0x24, 0U);
                gpu.run(8);
            }
        };
        const auto swap = [&gpu]() {
            send(gpu, 0x50, {0U});
            gpu.run(100000);
            gpu.vblank_start();
        };

        vertices(capacity);
        swap();
        auto view = gpu.geometry_view();
        ok &= check(view.vertex_count == (enhanced ? capacity : 6144U) &&
                    !view.enhanced_overflow,
                    "exact enhanced vertex capacity is not enhanced overflow, including classic mode");

        vertices(capacity + 1U);
        ok &= check(!gpu.geometry_view().enhanced_overflow,
                    "submission overflow is not published before swap");
        send(gpu, 0x50, {0U});
        gpu.run(100000);
        ok &= check(!gpu.geometry_view().enhanced_overflow,
                    "pending swap does not publish overflow before VBlank");
        gpu.vblank_start();
        view = gpu.geometry_view();
        ok &= check(view.vertex_count == (enhanced ? capacity : 6144U) &&
                    view.enhanced_overflow == enhanced,
                    "exceeding enhanced vertex capacity publishes overflow only in enhanced mode");
        gpu.vblank_start();
        ok &= check(gpu.geometry_view().enhanced_overflow == enhanced,
                    "overflow remains latched on VBlank without a swap");
        swap();
        ok &= check(!gpu.geometry_view().enhanced_overflow,
                    "next empty swap clears enhanced overflow");
    }
    if (!ok) std::exit(1);
}

void test_enhanced_geometry_keeps_guest_raster_hardware_limited() {
    std::array<std::uint32_t, 256 * 192> classic{};
    bool ok = true;
    for (int mode = 0; mode < 3; ++mode) {
        Gpu3D gpu;
        gpu.set_render_threads(0);
        gpu.set_enhanced_geometry(mode != 0);
        gpu.set_capture_opaque_only(mode == 2);
        send(gpu, 0x40, {2U});
        gpu.run(16);
        for (std::size_t vertex = 0; vertex < 2050U; ++vertex) {
            port(gpu, 0x24, 0U);
            gpu.run(8);
        }
        triangle(gpu, 0x001fU, 31U << 16, 0, 0, 0);
        submit_frame(gpu);
        const auto geometry = gpu.geometry_view();
        ok &= check(geometry.polygon_count == (mode == 0 ? 2048U : 2049U),
                    "enhanced presentation retains polygon beyond hardware capacity");
        for (int y = 0; y < 192; ++y) {
            const auto* row = gpu.line(y);
            if (mode == 0) std::copy_n(row, 256, classic.begin() + y * 256);
            else ok &= check(std::equal(row, row + 256, classic.begin() + y * 256),
                             "guest raster excludes enhanced-only polygon");
        }
        if (mode == 2) {
            const auto raster = gpu.raster_view();
            ok &= check(raster.opaque_only_mask != nullptr && raster.enhanced_output != nullptr,
                        "enhanced presentation capture and raster remain available");
            if (raster.enhanced_output) {
                std::size_t changed_pixels = 0;
                for (std::size_t i = 0; i < classic.size(); ++i)
                    changed_pixels += raster.enhanced_output[i] != classic[i];
                ok &= check(changed_pixels > 0,
                            "enhanced-only polygon changes presentation without changing guest raster");
            }
        }
    }
    if (!ok) std::exit(1);
}

void test_widescreen_projection_fov_equality() {
    Gpu3D classic;
    Gpu3D wide;
    for (auto* gpu : {&classic, &wide}) {
        send(*gpu, 0x10, {0U});
        const std::uint32_t x_scale = gpu == &classic ? 4096U : 3072U;
        send(*gpu, 0x16, {
            x_scale, 0U, 0U, 0U,
            0U, 4096U, 0U, 0U,
            0U, 0U, 4096U, 0U,
            0U, 0U, 0U, 4096U});
        send(*gpu, 0x29, {(31U << 16) | 0xc0U});
        send(*gpu, 0x40, {0U});
        position16(*gpu, -3072, -2048, 1024);
        position16(*gpu, 2048, -1024, 512);
        position16(*gpu, 1024, 3072, -512);
        send(*gpu, 0x41);
        submit_frame(*gpu);
    }
    const auto a = classic.geometry_view();
    const auto b = wide.geometry_view();
    bool ok = check(a.vertex_count == 3U && b.vertex_count == 3U,
                    "both projection matrices produce a triangle");
    if (a.vertex_count == 3U && b.vertex_count == 3U) {
        for (std::size_t i = 0; i < 3U; ++i) {
            const auto& picture = a.vertices[i].clip;
            const auto& full = b.vertices[i].clip;
            ok &= check(picture[0] * 0.75F == full[0] &&
                        picture[1] == full[1] && picture[2] == full[2] &&
                        picture[3] == full[3],
                        "Picture correction and Full projection yield the same clip-space FOV");
        }
    }
    if (!ok) std::exit(1);
}

void test_swap_sort_and_z_w_depth_modes() {
    const auto render_sort = [](std::uint32_t sort_mode) {
        Gpu3D gpu;
        submit_frame(gpu, sort_mode);
        gpu.io_write(0x04000060U, 1U << 3, 32);
        gpu.io_write(0x04000350U, 31U << 16, 32);
        const auto draw = [&gpu](std::uint16_t color, std::uint8_t id, std::int16_t y_offset) {
            send(gpu, 0x20, {color});
            send(gpu, 0x29, {0xc0U | (16U << 16) |
                             (static_cast<std::uint32_t>(id) << 24)});
            send(gpu, 0x40, {0U});
            position16(gpu, -2048, static_cast<std::int16_t>(-2048 + y_offset), 0);
            position16(gpu, 2048, static_cast<std::int16_t>(-2048 + y_offset), 0);
            position16(gpu, 0, static_cast<std::int16_t>(2048 + y_offset), 0);
            send(gpu, 0x41);
        };
        draw(0x001fU, 1U, 0);
        draw(0x03e0U, 2U, 512);
        submit_frame(gpu, sort_mode);
        return gpu.line(96)[128];
    };
    const auto automatic_sort = render_sort(0U);
    const auto manual_sort = render_sort(1U);
    bool ok = check((automatic_sort & 63U) > ((automatic_sort >> 6) & 63U) &&
                    ((manual_sort >> 6) & 63U) > (manual_sort & 63U),
                    "SWAP_BUFFERS bit zero selects automatic versus command-order translucent sorting");

    const auto render_depth = [](std::uint32_t swap_mode) {
        Gpu3D gpu;
        submit_frame(gpu, swap_mode);
        send(gpu, 0x16, {
            4096U, 0U, 0U, 0U,
            0U, 4096U, 0U, 0U,
            0U, 0U, 0xfffff000U, 2048U,
            0U, 0U, 2048U, 4096U});
        gpu.io_write(0x04000354U, 0x7fffU, 32);
        const auto draw = [&gpu](std::uint16_t color, std::int16_t z) {
            send(gpu, 0x20, {color});
            send(gpu, 0x29, {0xc0U | (31U << 16) | (3U << 24)});
            send(gpu, 0x40, {0U});
            position16(gpu, -4096, -4096, z);
            position16(gpu, 4096, -4096, z);
            position16(gpu, 0, 4096, z);
            send(gpu, 0x41);
        };
        draw(0x001fU, 0);
        draw(0x03e0U, 4096);
        submit_frame(gpu, swap_mode);
        return gpu.line(96)[128];
    };
    const auto z_buffered = render_depth(0U);
    const auto w_buffered = render_depth(2U);
    ok &= check((z_buffered & 63U) == 0U && ((z_buffered >> 6) & 63U) == 63U &&
                (w_buffered & 63U) == 63U && ((w_buffered >> 6) & 63U) == 0U,
                "SWAP_BUFFERS bit one selects Z depth or W depth comparisons");
    if (!ok) std::exit(1);
}

void test_opaque_polygons_draw_in_ascending_screen_y_order() {
    // Both triangles contain the centre pixel at identical depth. The one whose lowest
    // vertex is higher on screen (green, submitted second) is drawn first, and the equal
    // depth of the other (red) then fails the less-than test.
    Gpu3D gpu;
    const auto draw = [&gpu](std::uint16_t color, std::uint8_t id, std::int16_t bottom, std::int16_t top) {
        send(gpu, 0x20, {color});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (static_cast<std::uint32_t>(id) << 24)});
        send(gpu, 0x40, {0U});
        position16(gpu, -2048, bottom, 0);
        position16(gpu, 2048, bottom, 0);
        position16(gpu, 0, top, 0);
        send(gpu, 0x41);
    };
    draw(0x001fU, 1U, -2048, 2048);  // red: NDC y -0.5..0.5, lowest edge at screen row 144
    draw(0x03e0U, 2U, -1024, 3072);  // green: NDC y -0.25..0.75, lowest edge at screen row 120
    submit_frame(gpu);
    const auto pixel = gpu.line(96)[128];
    bool ok = check((pixel & 63U) == 0U && ((pixel >> 6) & 63U) == 63U,
                    "opaque polygons are drawn bottom-edge-highest first, not in submission order");
    if (!ok) std::exit(1);
}

void test_front_facing_passes_equal_depth_over_back_facing() {
    const auto render = [](bool first_is_back_facing) {
        Gpu3D gpu;
        const auto draw = [&gpu](std::uint16_t color, std::uint8_t id, bool reverse_winding) {
            send(gpu, 0x20, {color});
            send(gpu, 0x29, {0xc0U | (31U << 16) | (static_cast<std::uint32_t>(id) << 24)});
            send(gpu, 0x40, {0U});
            position16(gpu, -2048, -2048, 0);
            if (reverse_winding) {
                position16(gpu, 0, 2048, 0);
                position16(gpu, 2048, -2048, 0);
            } else {
                position16(gpu, 2048, -2048, 0);
                position16(gpu, 0, 2048, 0);
            }
            send(gpu, 0x41);
        };
        draw(0x001fU, 1U, first_is_back_facing);
        draw(0x03e0U, 2U, false);
        submit_frame(gpu);
        return gpu.line(96)[128];
    };
    const auto over_back = render(true);
    const auto over_front = render(false);
    bool ok = check((over_back & 63U) == 0U && ((over_back >> 6) & 63U) == 63U,
                    "front-facing fragment replaces an opaque back-facing one at equal depth");
    ok &= check((over_front & 63U) == 63U && ((over_front >> 6) & 63U) == 0U,
                "equal depth between two front-facing fragments keeps the first");
    if (!ok) std::exit(1);
}

void test_inclusive_viewport_coverage() {
    const auto render = [](std::uint32_t viewport) {
        Gpu3D gpu;
        gpu.io_write(0x04000350U, 0x7c00U | (31U << 16), 32);
        send(gpu, 0x60, {viewport});
        quad(gpu, 0x001fU, (31U << 16) | (1U << 24));
        submit_frame(gpu);
        std::array<bool, 256U * 192U> covered{};
        for (std::size_t y = 0; y < 192U; ++y) {
            for (std::size_t x = 0; x < 256U; ++x) {
                covered[y * 256U + x] = (gpu.line(static_cast<int>(y))[x] & 63U) == 63U;
            }
        }
        return covered;
    };
    const auto full = render(0xbfff0000U);
    bool ok = true;
    std::size_t full_count = 0;
    for (const bool covered : full) full_count += covered;
    ok &= check(full_count == 256U * 192U,
                "inclusive full-screen viewport covers all 49152 pixels, including last row/column");
    const auto inset = render(0x3f3f2020U); // x=32..63, y=32..63 (bottom-origin).
    bool inset_exact = true;
    for (std::size_t y = 0; y < 192U; ++y) {
        for (std::size_t x = 0; x < 256U; ++x) {
            const bool expected = x >= 32U && x <= 63U && y >= 128U && y <= 159U;
            inset_exact &= inset[y * 256U + x] == expected;
        }
    }
    ok &= check(inset_exact, "inclusive inset viewport uses 192-y and covers exactly its 32x32 rectangle");
    if (!ok) std::exit(1);
}

// References below retain the pre-optimization rounding, coordinate wrapping,
// shading and span arithmetic, independently of the optimized template paths.
static std::uint8_t reference_round(float value, int maximum) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, maximum));
}

void test_color_rounding_matches_lround() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    bool ok = true;
    for (int maximum : {31, 63}) {
        for (int i = -65536; i <= 65536; ++i) {
            const float value = static_cast<float>(i) / 512.0F;
            ok &= Access::rounded_color(value, maximum == 63) == reference_round(value, maximum);
        }
        for (int i = 0; i < maximum; ++i) {
            const float half = static_cast<float>(i) + 0.5F;
            for (float value : {std::nextafter(half, 0.0F), half, std::nextafter(half, 100.0F)})
                ok &= Access::rounded_color(value, maximum == 63) == reference_round(value, maximum);
        }
        std::mt19937 random(0x34923);
        for (int i = 0; i < 1000000; ++i) {
            const float value = std::uniform_real_distribution<float>(-256.0F, 256.0F)(random);
            ok &= Access::rounded_color(value, maximum == 63) == reference_round(value, maximum);
        }
    }
    if (!check(ok, "integer color rounding matches old lround across color ranges and half boundaries")) std::exit(1);
}

static int reference_wrap(int value, int size, bool repeat, bool flip) {
    if (!repeat) return std::clamp(value, 0, size - 1);
    const int period = size * (flip ? 2 : 1);
    int result = value % period;
    if (result < 0) result += period;
    return flip && result >= size ? period - 1 - result : result;
}

static dsr::gpu3d_detail::Texel reference_sample(const TextureSource& source,
    const dsr::Gpu3DPolygon& polygon, float u, float v) {
    const int width = 8 << ((polygon.texture >> 20U) & 7U);
    const int height = 8 << ((polygon.texture >> 23U) & 7U);
    const int x = reference_wrap(static_cast<int>(std::floor(u)), width,
        (polygon.texture & (1U << 16U)) != 0U, (polygon.texture & (1U << 18U)) != 0U);
    const int y = reference_wrap(static_cast<int>(std::floor(v)), height,
        (polygon.texture & (1U << 17U)) != 0U, (polygon.texture & (1U << 19U)) != 0U);
    return dsr::gpu3d_detail::decode_texel(source, polygon.texture, polygon.palette, x, y);
}

void test_prepared_texture_and_polygon_cache_reference() {
    TestTextureSource source;
    std::mt19937 random(0x29b43);
    for (auto& bank : source.textures) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    Gpu3D gpu;
    gpu.io_write(0x04000060U, 1U, 16);
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    bool ok = true;
    for (unsigned format = 1; format <= 7; ++format) {
        for (int iteration = 0; iteration < 128; ++iteration) {
            dsr::Gpu3DPolygon polygon{};
            polygon.texture = (format << 26U) | (random() & 0x23ffffffU);
            polygon.palette = random();
            std::array<std::array<float, 2>, 64> coordinates{};
            for (std::size_t i = 0; i < coordinates.size(); ++i) {
                coordinates[i] = i >= 16 ? coordinates[i % 16] : std::array<float, 2>{
                    std::uniform_real_distribution<float>(-2048, 2048)(random),
                    std::uniform_real_distribution<float>(-2048, 2048)(random)};
            }
            source.mapped_textures = static_cast<std::uint8_t>(random() & 15U);
            const auto actual = dsr::gpu3d_detail::Gpu3DTestAccess::sample_texels(gpu, polygon, coordinates);
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const auto expected = reference_sample(source.view(), polygon, coordinates[i][0], coordinates[i][1]);
                ok &= actual[i].r == expected.r && actual[i].g == expected.g &&
                      actual[i].b == expected.b && actual[i].a == expected.a;
            }
        }
    }
    // Alternating samples hit the multi-entry cache, unlike the old last-texel cache.
    source.mapped_textures = 15;
    dsr::Gpu3DPolygon polygon{};
    polygon.texture = 7U << 26U;
    std::array<std::array<float, 2>, 64> coordinates{};
    for (std::size_t i = 0; i < coordinates.size(); ++i) coordinates[i] = {static_cast<float>(i & 1U), 0};
    source.texture_slot_zero_lookups.store(0, std::memory_order_relaxed);
    dsr::gpu3d_detail::Gpu3DTestAccess::sample_texels(gpu, polygon, coordinates);
    ok &= source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 4U;
    if (!check(ok, "prepared decoders/cache match old sampling and reuse nonconsecutive addresses")) std::exit(1);
}

void test_decoded_sampling_matches_reference() {
    TestTextureSource source;
    std::mt19937 random(0x238ecaU);
    for (auto& bank : source.textures)
        for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes)
        for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    Gpu3D gpu;
    gpu.io_write(0x04000060U, 1U, 16);
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.versioned_view());
    bool ok = true;
    for (unsigned format = 1; format <= 7; ++format) {
        for (unsigned flags = 0; flags < 16U; ++flags) {
            dsr::Gpu3DPolygon polygon{};
            polygon.texture = (format << 26U) | (flags << 16U) |
                ((random() % 5U) << 20U) | ((random() % 5U) << 23U) |
                (random() & 0x2000ffffU);
            polygon.palette = random();
            source.mapped_textures = static_cast<std::uint8_t>(random() & 15U);
            source.overlapping_textures = static_cast<std::uint8_t>(random() & 15U);
            source.overlapping_palettes = (random() & 1U) != 0U;
            ++source.generation;
            std::array<std::array<float, 2>, 64> coordinates{};
            for (auto& coordinate : coordinates) {
                coordinate = {std::uniform_real_distribution<float>(-4096, 4096)(random),
                              std::uniform_real_distribution<float>(-4096, 4096)(random)};
            }
            coordinates[0] = {-0.25F, -0.25F};
            coordinates[1] = {-1024.0F, 1024.0F};
            const auto actual = dsr::gpu3d_detail::Gpu3DTestAccess::sample_texels(gpu, polygon, coordinates);
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const auto expected = reference_sample(source.versioned_view(), polygon,
                                                       coordinates[i][0], coordinates[i][1]);
                ok &= actual[i].r == expected.r && actual[i].g == expected.g &&
                      actual[i].b == expected.b && actual[i].a == expected.a;
            }
        }
    }
    if (!check(ok, "decoded sampling matches old floor/clamp/repeat/flip for all seven formats"))
        std::exit(1);
}

void test_decoded_cache_renderer_lifetime() {
    for (int threads : {0, 1, 2, 6}) {
        TestTextureSource source;
        source.textures[0][0] = 1U;
        palette_color(source, 2U, 0x03e0U);
        Gpu3D gpu;
        gpu.set_render_threads(threads);
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.versioned_view());
        gpu.io_write(0x04000060U, 1U, 32);
        gpu.io_write(0x04000354U, 0x7fffU, 32);
        send(gpu, 0x2a, {4U << 26U});
        triangle(gpu, 0x7fffU, (31U << 16) | (1U << 24), 0, 0, 0);
        triangle(gpu, 0x7fffU, (31U << 16) | (2U << 24), 0, 0, 0);
        submit_frame(gpu);
        const auto lookups = source.texture_slot_zero_lookups.load(std::memory_order_relaxed);
        bool ok = check(lookups == 1U &&
                        gpu.line(96)[128] == ((31U << 24) | (63U << 6)),
                        "all polygons and bands share one decoded texture");
        gpu.render_start();
        ok &= check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == lookups,
                    "retained geometry reuses the decoded image across frames");
        source.palettes[0][2] = 0x1fU;
        source.palettes[0][3] = 0U;
        ++source.generation;
        gpu.render_start();
        ok &= check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == lookups + 1U &&
                    gpu.line(96)[128] == ((31U << 24) | 63U),
                    "palette-only guest rewrite invalidates before every band starts");
        source.textures[0][0] = 2U;
        palette_color(source, 4U, 0x7c00U);
        ++source.generation;
        gpu.render_start();
        ok &= check(gpu.line(96)[128] == ((31U << 24) | (63U << 12)),
                    "texture guest rewrite is visible without a geometry resubmit");
        TestTextureSource replacement;
        replacement.generation = source.generation;
        palette_color(replacement, 0U, 0x03e0U);
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, replacement.versioned_view());
        gpu.render_start();
        ok &= check(gpu.line(96)[128] == ((31U << 24) | (63U << 6)),
                    "source replacement with equal generation cannot reuse stale bytes");
        gpu.reset();
        gpu.io_write(0x04000060U, 1U, 32);
        gpu.io_write(0x04000354U, 0x7fffU, 32);
        palette_color(replacement, 0U, 0x001fU);
        send(gpu, 0x2a, {4U << 26U});
        triangle(gpu, 0x7fffU, 31U << 16, 0, 0, 0);
        submit_frame(gpu);
        ok &= check(gpu.line(96)[128] == ((31U << 24) | 63U),
                    "renderer reset clears cached images even when the source token is unchanged");
        if (!ok) std::exit(1);
    }
}

void test_inline_uses_one_full_frame_cache() {
    for (int threads : {0, 1}) {
        TestTextureSource source;
        source.textures[0][0] = 1U;
        palette_color(source, 2U, 0x03e0U);
        Gpu3D gpu;
        gpu.set_render_threads(threads);
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
        gpu.io_write(0x04000060U, 1U, 32);
        gpu.io_write(0x04000354U, 0x7fffU, 32);
        send(gpu, 0x2a, {4U << 26U});
        triangle(gpu, 0x7fffU, (31U << 16) | (1U << 24), 0, 0, 0);
        submit_frame(gpu);
        if (!check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 1U,
                   "inline mode samples a constant-UV polygon once, not once per band"))
            std::exit(1);
        if (!check(gpu.raster_view().output[96U * 256U + 128U] == ((31U << 24) | (63U << 6)),
                   "full-frame cached texture renders the expected green pixel"))
            std::exit(1);
        source.palettes[0][2] = 0x1fU;
        source.palettes[0][3] = 0U;
        gpu.render_start();
        if (!check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 2U &&
                   gpu.raster_view().output[96U * 256U + 128U] == ((31U << 24) | 63U),
                   "inline rerender invalidates texture keys and observes the live palette"))
            std::exit(1);
    }
}

static std::array<std::uint8_t, 4> reference_shade(const TextureSource& source,
    const dsr::Gpu3DPolygon& polygon, const std::array<float, 3>& color, float u, float v,
    bool highlight, const std::array<std::uint16_t, 32>& toon_table) {
    const auto mode = (polygon.attr >> 4U) & 3U;
    const bool textured = ((polygon.texture >> 26U) & 7U) != 0U;
    const auto texel = textured ? reference_sample(source, polygon, u, v) :
        dsr::gpu3d_detail::Texel{0, 0, 0, 31};
    const std::array<std::uint8_t, 3> vertex{
        reference_round(color[0], 63), reference_round(color[1], 63), reference_round(color[2], 63)};
    const auto expand = [](unsigned value) { return value == 0 ? 0 : value * 2U + 1U; };
    std::array<std::uint8_t, 3> toon = vertex;
    if (mode == 2U) for (unsigned channel = 0; channel < 3; ++channel)
        toon[channel] = static_cast<std::uint8_t>(expand((toon_table[vertex[0] >> 1U] >> (channel * 5U)) & 31U));
    highlight = highlight && mode == 2U;
    auto shade = mode == 2U && !highlight ? toon : vertex;
    auto alpha = static_cast<std::uint8_t>((polygon.attr >> 16U) & 31U);
    if (alpha == 0U) alpha = 31U;
    if (mode != 3U) {
        const std::array<std::uint8_t, 3> texture = textured ?
            std::array<std::uint8_t, 3>{texel.r, texel.g, texel.b} :
            std::array<std::uint8_t, 3>{63, 63, 63};
        if (mode == 0U || mode == 2U) for (unsigned channel = 0; channel < 3; ++channel) {
            const auto modulation = highlight ? vertex[0] : shade[channel];
            const auto modulated = ((texture[channel] + 1U) * (modulation + 1U) - 1U) / 64U;
            shade[channel] = static_cast<std::uint8_t>(
                highlight ? std::min(63U, modulated + toon[channel]) : modulated);
        }
        else if (mode == 1U && textured) for (unsigned channel = 0; channel < 3; ++channel)
            shade[channel] = (texel.a == 0U || texel.a == 31U) ?
                (texel.a == 0U ? vertex[channel] : texture[channel]) :
                static_cast<std::uint8_t>((texture[channel] * texel.a + vertex[channel] * (31U - texel.a)) >> 5U);
        if (textured && mode != 1U) {
            const auto alpha6 = ((expand(texel.a) + 1U) * (expand(alpha) + 1U) - 1U) / 64U;
            alpha = static_cast<std::uint8_t>(std::min(alpha6 / 2U, 31U));
        }
    }
    return {shade[0], shade[1], shade[2], alpha};
}

void test_shade_pixel_matches_reference() {
    TestTextureSource source;
    std::mt19937 random(0x236ad);
    for (auto& bank : source.textures) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    std::array<std::uint16_t, 32> toon{};
    for (unsigned i = 0; i < toon.size(); ++i) {
        toon[i] = static_cast<std::uint16_t>(random());
        gpu.io_write(0x04000380U + i * 2U, toon[i], 16);
    }
    bool ok = true;
    for (int i = 0; i < 4096; ++i) {
        dsr::Gpu3DPolygon polygon{};
        polygon.attr = random();
        polygon.texture = random();
        polygon.palette = random();
        const bool highlight = (random() & 1U) != 0;
        gpu.io_write(0x04000060U, 1U | (highlight ? 2U : 0U), 16);
        std::array<float, 3> color{};
        for (auto& channel : color) channel = std::uniform_real_distribution<float>(-1, 64)(random);
        const auto u = std::uniform_real_distribution<float>(-1024, 1024)(random);
        const auto v = std::uniform_real_distribution<float>(-1024, 1024)(random);
        ok &= dsr::gpu3d_detail::Gpu3DTestAccess::shade_pixel(gpu, polygon, color, u, v) ==
              reference_shade(source.view(), polygon, color, u, v, highlight, toon);
    }
    if (!check(ok, "generic shading matches original across all modes, alphas and texture formats")) std::exit(1);
}


static std::array<std::uint32_t, 256> reference_span(const TextureSource& source,
    const dsr::Gpu3DPolygon& polygon, int x, int x_right,
    std::array<float, 7> value, const std::array<float, 7>& right) {
    std::array<std::uint32_t, 256> output{};
    int width = x_right - x;
    if (width <= 0) return output;
    const float inverse_width = 1.0F / static_cast<float>(width);
    std::array<float, 7> delta{};
    for (std::size_t i = 0; i < delta.size(); ++i) delta[i] = (right[i] - value[i]) * inverse_width;
    if (x < 0) {
        const float skipped = static_cast<float>(-x);
        for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i] * skipped;
        width += x; x = 0;
    }
    width = std::min(width, 256 - x);
    for (; width > 0; --width, ++x) {
        const float w = 1.0F / value[0];
        std::array<std::uint8_t, 3> color{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float scaled = value[4 + channel] * w + 0.5F;
            color[channel] = reference_round(static_cast<float>(std::clamp(
                static_cast<int>(std::max(scaled, 0.0F)), 0, 63)), 63);
        }
        const auto texel = polygon.texture == 0 ? dsr::gpu3d_detail::Texel{63, 63, 63, 31} :
            reference_sample(source, polygon, value[2] * w, value[3] * w);
        const std::array<std::uint8_t, 3> texture{texel.r, texel.g, texel.b};
        for (std::size_t channel = 0; channel < 3; ++channel)
            color[channel] = static_cast<std::uint8_t>(((texture[channel] + 1U) * (color[channel] + 1U) - 1U) / 64U);
        const auto alpha6 = ((static_cast<unsigned>(texel.a == 0 ? 0 : texel.a * 2U + 1U) + 1U) * 64U - 1U) / 64U;
        const auto alpha = std::min(alpha6 / 2U, 31U);
        const auto depth = static_cast<std::uint32_t>(std::floor(std::clamp(value[1], 0.0F, 1.0F) * 4194303.0F)) << 2;
        if (alpha != 0 && depth < 0xffffffU)
            output[static_cast<std::size_t>(x)] = color[0] | (color[1] << 6U) | (color[2] << 12U) | (alpha << 24U);
        if (width > 1) for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i];
    }
    return output;
}

void test_specialized_spans_match_reference() {
    TestTextureSource source;
    std::mt19937 random(0x374ad);
    for (auto& bank : source.textures) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    Gpu3D gpu;
    gpu.io_write(0x04000060U, 1U, 16);
    gpu.io_write(0x04000354U, 0x7fffU, 32);
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.view());
    bool ok = true;
    for (unsigned format : {0U, 2U, 3U, 4U, 7U}) for (int iteration = 0; iteration < 256; ++iteration) {
        dsr::Gpu3DPolygon polygon{};
        polygon.attr = 31U << 16U;
        polygon.texture = format == 0 ? 0 : (format << 26U) | (random() & 0x23ffffffU);
        polygon.palette = random();
        std::array<float, 7> left{}, right{};
        for (auto* edge : {&left, &right}) {
            (*edge)[0] = std::uniform_real_distribution<float>(0.25F, 4.0F)(random);
            (*edge)[1] = std::uniform_real_distribution<float>(0, 1)(random);
            (*edge)[2] = std::uniform_real_distribution<float>(-128, 128)(random);
            (*edge)[3] = std::uniform_real_distribution<float>(-128, 128)(random);
            for (int channel = 4; channel < 7; ++channel)
                (*edge)[channel] = std::uniform_real_distribution<float>(0, 63)(random) * (*edge)[0];
        }
        const int x = static_cast<int>(random() % 64U) - 32;
        const int end = 1 + static_cast<int>(random() % 288U);
        ok &= dsr::gpu3d_detail::Gpu3DTestAccess::draw_span(gpu, polygon, x, end, left, right) ==
              reference_span(source.view(), polygon, x, end, left, right);
    }
    if (!check(ok, "specialized spans match original rounding/interpolation/sampling/shading")) std::exit(1);
}

// Original fragment/blend arithmetic, with independent storage so the batch
// test checks every metadata field rather than just the packed RGB output.
struct ReferenceSpanPixel {
    std::uint8_t r, g, b, alpha;
    std::uint32_t depth;
    std::uint8_t polygon_id;
    bool fog;
    std::uint8_t translucent_polygon_id;
    bool is_translucent_polygon, stencil;
    std::uint8_t facing;
};

using SpanAccess = dsr::gpu3d_detail::Gpu3DTestAccess;

static ReferenceSpanPixel reference_span_pixel(const SpanAccess::SpanPixel& state) {
    return {
        static_cast<std::uint8_t>(state[0] & 63U),
        static_cast<std::uint8_t>((state[0] >> 6U) & 63U),
        static_cast<std::uint8_t>((state[0] >> 12U) & 63U),
        static_cast<std::uint8_t>((state[0] >> 24U) & 31U), state[1],
        static_cast<std::uint8_t>(state[2] & 63U), (state[3] & 1U) != 0U,
        static_cast<std::uint8_t>((state[2] >> 8U) & 255U),
        (state[3] & 4U) != 0U, (state[3] & 2U) != 0U,
        static_cast<std::uint8_t>((state[3] >> 3U) & 3U)};
}

static SpanAccess::SpanPixel reference_span_state(const ReferenceSpanPixel& pixel) {
    return {
        static_cast<std::uint32_t>(
            pixel.r | (pixel.g << 6U) | (pixel.b << 12U) | (pixel.alpha << 24U)),
        pixel.depth, static_cast<std::uint32_t>(
            pixel.polygon_id | (pixel.translucent_polygon_id << 8U)),
        static_cast<std::uint32_t>(pixel.fog) | (static_cast<std::uint32_t>(pixel.stencil) << 1U) |
        (static_cast<std::uint32_t>(pixel.is_translucent_polygon) << 2U) | (pixel.facing << 3U)};
}

static void reference_blend_pixel(ReferenceSpanPixel& pixel, const std::array<std::uint8_t, 4>& source,
    std::uint8_t polygon_id, bool is_translucent_polygon, bool fog_enabled, std::uint32_t depth,
    std::uint32_t attr, bool front_facing, std::uint32_t disp3d, unsigned alpha_ref) {
    const auto shading_mode = (attr >> 4) & 3U;
    if (shading_mode == 3U) {
        if (polygon_id == 0U) return;
        if (!pixel.stencil || pixel.polygon_id == polygon_id) return;
        pixel.stencil = false;
    }
    if (source[3] == 0U) return;
    if ((disp3d & (1U << 2)) != 0U && source[3] < alpha_ref) return;
    const bool translucent = source[3] < 31U;
    if (translucent && pixel.translucent_polygon_id == polygon_id) return;
    const bool blend = translucent && (disp3d & (1U << 3)) != 0U && pixel.alpha != 0U;
    if (blend) {
        const auto alpha = static_cast<unsigned>(source[3]);
        const auto inverse = 31U - alpha;
        pixel.r = static_cast<std::uint8_t>((source[0] * (alpha + 1U) + pixel.r * inverse) / 32U);
        pixel.g = static_cast<std::uint8_t>((source[1] * (alpha + 1U) + pixel.g * inverse) / 32U);
        pixel.b = static_cast<std::uint8_t>((source[2] * (alpha + 1U) + pixel.b * inverse) / 32U);
        pixel.alpha = std::max(pixel.alpha, source[3]);
    } else {
        pixel.r = source[0]; pixel.g = source[1]; pixel.b = source[2]; pixel.alpha = source[3];
    }
    pixel.facing = front_facing ? 1U : 2U;
    if (translucent) pixel.translucent_polygon_id = polygon_id;
    else {
        pixel.polygon_id = polygon_id;
        pixel.is_translucent_polygon = is_translucent_polygon;
    }
    pixel.fog = translucent ? (pixel.fog && fog_enabled) : fog_enabled;
    if (!translucent || (attr & (1U << 11)) != 0U) pixel.depth = depth;
}

static void reference_write_fragment(ReferenceSpanPixel& destination, const TextureSource& source,
    const dsr::Gpu3DPolygon& polygon, bool translucent_polygon, bool front_facing,
    const std::array<float, 3>& color, float u, float v, std::uint32_t depth,
    std::uint32_t disp3d, unsigned alpha_ref, const std::array<std::uint16_t, 32>& toon) {
    const auto polygon_id = static_cast<std::uint8_t>((polygon.attr >> 24) & 63U);
    const bool depth_equal = (polygon.attr & (1U << 14)) != 0U;
    const auto depth_delta = depth > destination.depth
        ? depth - destination.depth : destination.depth - depth;
    const bool less_equal = front_facing && destination.facing == 2U && destination.alpha == 31U;
    const bool depth_fail = depth_equal ? depth_delta > 255U
        : less_equal ? depth > destination.depth : depth >= destination.depth;
    if (depth_fail) {
        if (((polygon.attr >> 4) & 3U) == 3U && polygon_id == 0U) destination.stencil = true;
        return;
    }
    const auto shaded = reference_shade(source, polygon, color, u, v, (disp3d & 2U) != 0U, toon);
    reference_blend_pixel(destination, shaded, polygon_id, translucent_polygon,
        (polygon.attr & (1U << 15)) != 0U, depth, polygon.attr, front_facing, disp3d, alpha_ref);
}

static SpanAccess::SpanState reference_span_fragments(const TextureSource& source,
    const dsr::Gpu3DPolygon& polygon, int x, int x_right, std::array<float, 7> value,
    const std::array<float, 7>& right, SpanAccess::SpanState output, bool front_facing,
    bool translucent_polygon, bool w_depth, std::uint32_t disp3d, unsigned alpha_ref,
    const std::array<std::uint16_t, 32>& toon) {
    int width = x_right - x;
    if (width <= 0) return output;
    const float inverse_width = 1.0F / static_cast<float>(width);
    std::array<float, 7> delta{};
    for (std::size_t i = 0; i < delta.size(); ++i) delta[i] = (right[i] - value[i]) * inverse_width;
    if (x < 0) {
        const float skipped = static_cast<float>(-x);
        for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i] * skipped;
        width += x; x = 0;
    }
    if (x + width > 256) width = 256 - x;
    for (; width > 0; --width, ++x) {
        const float w = 1.0F / value[0];
        std::array<float, 3> color{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float scaled = value[4 + channel] * w + 0.5F;
            color[channel] = static_cast<float>(std::clamp(static_cast<int>(std::max(scaled, 0.0F)), 0, 63));
        }
        const auto depth = w_depth ? static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(std::floor(w * 4096.0F)), 0, 0x00ffffff)) :
            static_cast<std::uint32_t>(std::floor(std::clamp(value[1], 0.0F, 1.0F) * 4194303.0F)) << 2;
        auto pixel = reference_span_pixel(output[x]);
        reference_write_fragment(pixel, source, polygon, translucent_polygon, front_facing, color,
            value[2] * w, value[3] * w, depth, disp3d, alpha_ref, toon);
        output[x] = reference_span_state(pixel);
        if (width > 1) for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i];
    }
    return output;
}

void test_batch_spans_match_original_fragments() {
    TestTextureSource source;
    std::mt19937 random(0xa39258);
    for (auto& bank : source.textures) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes) for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    Gpu3D gpu;
    SpanAccess::attach_texture_source(gpu, source.view());
    std::array<std::uint16_t, 32> toon{};
    for (unsigned i = 0; i < toon.size(); ++i) {
        toon[i] = static_cast<std::uint16_t>(random());
        gpu.io_write(0x04000380U + i * 2U, toon[i], 16);
    }
    for (unsigned iteration = 0; iteration < 4096; ++iteration) {
        const unsigned format = iteration % 8U;
        const unsigned mode = (iteration / 8U) % 4U;
        dsr::Gpu3DPolygon polygon{};
        polygon.attr = (mode << 4U) | ((iteration % 3U == 0U ? 31U : random() % 32U) << 16U) |
            ((random() % 64U) << 24U) | (random() & ((1U << 11U) | (1U << 14U) | (1U << 15U)));
        if (mode == 3U && (iteration & 32U) == 0U) polygon.attr &= ~(63U << 24U);
        polygon.texture = format == 0U ? 0U : (format << 26U) | (random() & 0x23ffffffU);
        polygon.palette = random();
        const std::uint32_t disp3d = 1U | (random() & 14U);
        const unsigned alpha_ref = random() % 32U;
        gpu.io_write(0x04000060U, disp3d, 16);
        gpu.io_write(0x04000340U, alpha_ref, 32);
        const bool front = (random() & 1U) != 0U;
        const bool translucent = (random() & 1U) != 0U;
        const bool w_depth = (iteration & 64U) != 0U;
        std::array<float, 7> left{}, right{};
        for (auto* edge : {&left, &right}) {
            (*edge)[0] = std::uniform_real_distribution<float>(0.0001F, 4.0F)(random);
            (*edge)[1] = std::uniform_real_distribution<float>(-0.125F, 1.125F)(random);
            (*edge)[2] = std::uniform_real_distribution<float>(-2048.0F, 2048.0F)(random);
            (*edge)[3] = std::uniform_real_distribution<float>(-2048.0F, 2048.0F)(random);
            for (unsigned channel = 4; channel < 7; ++channel)
                (*edge)[channel] = std::uniform_real_distribution<float>(-1.0F, 64.0F)(random) * (*edge)[0];
        }
        if ((iteration & 2U) == 0U) {
            left[0] = right[0] = 0.5F;
            left[1] = right[1] = 0.5F;
        }
        SpanAccess::SpanState initial{};
        const auto polygon_id = (polygon.attr >> 24U) & 63U;
        for (unsigned lane = 0; lane < initial.size(); ++lane) {
            auto& state = initial[lane];
            state[0] = random() & 0x1f03ffffU;
            state[1] = random() & 0xffffffU;
            if ((iteration & 2U) == 0U) {
                constexpr std::array<int, 9> offset{-256, -255, -1, 0, 1, 255, 256, -512, 512};
                state[1] = (w_depth ? 8192 : 8388604) + offset[lane % offset.size()];
            }
            state[2] = (lane % 3U == 0U ? polygon_id : random() % 64U) |
                ((lane % 4U == 0U ? polygon_id : random() % 64U) << 8U);
            state[3] = (random() & 7U) | ((random() % 3U) << 3U);
            if (lane % 4U == 0U) {
                state[0] |= 31U << 24U;
                state[3] = (state[3] & 7U) | (2U << 3U);
            }
        }
        const int x = static_cast<int>(random() % 336U) - 48;
        const int end = iteration % 5U == 0U ? x + static_cast<int>(iteration % 9U) :
            x + static_cast<int>(random() % 336U);
        const auto expected = reference_span_fragments(source.view(), polygon, x, end, left, right,
            initial, front, translucent, w_depth, disp3d, alpha_ref, toon);
        for (bool batch : {false, true}) {
            const auto actual = SpanAccess::draw_span_state(
                gpu, polygon, x, end, left, right, initial, front, translucent, w_depth, batch);
            if (actual == expected) continue;
            std::cerr << "FAIL: span fragment reference iteration=" << iteration
                      << " format=" << format << " mode=" << mode << " batch=" << batch << '\n';
            for (unsigned lane = 0; lane < actual.size(); ++lane) {
                if (actual[lane] == expected[lane]) continue;
                std::cerr << "pixel=" << lane << " actual=";
                for (auto field : actual[lane]) std::cerr << std::hex << field << ' ';
                std::cerr << "expected=";
                for (auto field : expected[lane]) std::cerr << std::hex << field << ' ';
                std::cerr << std::dec << '\n';
                break;
            }
            std::exit(1);
        }
    }
    std::cout << "PASS: batch/scalar spans match original shading/depth/blend, 4096 randomized cases\n";
}

void test_pack_pixels_matches_scalar() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    Gpu3D gpu;
    std::mt19937 random(0x9ac405U);
    constexpr std::array<unsigned, 12> counts{0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 64, 253};
    for (unsigned fog_config = 0; fog_config < 5; ++fog_config) {
        constexpr std::array<unsigned, 5> shifts{0, 0, 4, 10, 15};
        gpu.io_write(0x04000060U, fog_config == 0U ? 0U :
            (1U << 7U) | (shifts[fog_config] << 8U) | (fog_config == 2U ? 1U << 6U : 0U), 16);
        gpu.io_write(0x04000358U, random(), 32);
        gpu.io_write(0x0400035cU, random(), 16);
        for (unsigned i = 0; i < 32; ++i)
            gpu.io_write(0x04000360U + i, random() & 127U, 8);
        for (bool capture : {false, true}) {
            for (unsigned fog_mask = 0; fog_mask < 16; ++fog_mask) {
                for (unsigned first = 0; first < 4; ++first) {
                    for (unsigned count : counts) {
                        Access::PackState initial{};
                        for (unsigned i = 0; i < initial.size(); ++i) {
                            initial[i] = {static_cast<std::uint32_t>(random()),
                                static_cast<std::uint32_t>(random()) & 0xffffffU,
                                static_cast<std::uint32_t>(random()),
                                (static_cast<std::uint32_t>(random()) & 30U) |
                                    ((fog_mask >> ((i - first) & 3U)) & 1U),
                                static_cast<std::uint32_t>(random())};
                        }
                        const auto expected = Access::pack_pixels(gpu, initial, first, count, capture, true);
                        const auto actual = Access::pack_pixels(gpu, initial, first, count, capture, false);
                        if (actual == expected) continue;
                        std::cerr << "FAIL: packing scalar reference fog_config=" << fog_config
                                  << " capture=" << capture << " mask=" << fog_mask
                                  << " first=" << first << " count=" << count << '\n';
                        std::exit(1);
                    }
                }
            }
        }
    }
}

static std::array<std::uint32_t, 4> reference_clear_pixel(std::uint32_t color, std::uint32_t depth) {
    const auto expand = [](unsigned value) { return value == 0 ? 0 : value * 2U + 1U; };
    return {expand(color & 31U) | (expand((color >> 5U) & 31U) << 6U) |
        (expand((color >> 10U) & 31U) << 12U) | (((color >> 16U) & 31U) << 24U),
        ((depth & 0x7fffU) << 9U) | 0x1ffU, ((color >> 24U) & 63U) | 0xff00U,
        (color >> 15U) & 1U};
}

void test_uniform_clear_matches_reference() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    Gpu3D gpu;
    std::mt19937 random(0x21c3);
    bool ok = true;
    for (int i = 0; i < 256; ++i) {
        Access::SpanState stale{};
        for (std::size_t lane = 0; lane < stale.size(); ++lane) {
            stale[lane] = {static_cast<std::uint32_t>(random()) & 0x1f03ffffU,
                static_cast<std::uint32_t>(random()) & 0xffffffU,
                static_cast<std::uint32_t>(random() % 64U) |
                    (static_cast<std::uint32_t>(random() % 255U) << 8U),
                static_cast<std::uint32_t>(lane & 7U) |
                    (static_cast<std::uint32_t>(lane % 3U) << 3U)};
        }
        // A zero-width span seeds all named fields without drawing a fragment.
        Access::draw_span_state(gpu, {}, 0, 0, {}, {}, stale, false, false, false, false);
        const auto color = random(), depth = random();
        gpu.io_write(0x04000350U, color, 32);
        gpu.io_write(0x04000354U, depth, 32);
        const auto expected = reference_clear_pixel(color, depth);
        ok &= Access::clear_pixel(gpu, random() % (256U * 192U)) == expected;
        for (unsigned lane = 0; lane < stale.size(); ++lane)
            ok &= Access::framebuffer_pixel(gpu, lane) == expected;
        for (unsigned index : {256U, 6143U, 6144U, 49151U})
            ok &= Access::framebuffer_pixel(gpu, index) == expected;
    }
    if (!check(ok, "uniform fill preserves every framebuffer field from original clear")) std::exit(1);
}

void test_uniform_clear_targets_match_reference() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    std::mt19937 random(0x63ac91);
    bool ok = true;
    for (int threads : {0, 2, 6}) {
        for (bool capture : {false, true}) {
            for (int scale : {1, 2, 4}) {
                Gpu3D gpu;
                gpu.set_render_threads(threads);
                gpu.set_capture_opaque_only(capture);
                gpu.set_output_scale(scale);
                for (int frame = 0; frame < 2; ++frame) {
                    const auto color = random(), depth = random();
                    const auto expected = reference_clear_pixel(color, depth);
                    gpu.io_write(0x04000350U, color, 32);
                    gpu.io_write(0x04000354U, depth, 32);
                    gpu.render_start();
                    const auto raster = gpu.raster_view();
                    for (unsigned index = 0; index < 256U * 192U; ++index) {
                        ok &= Access::framebuffer_pixel(gpu, index) == expected;
                        ok &= raster.output[index] == expected[0];
                        if (capture) {
                            ok &= raster.opaque_only_mask[index] == 1U;
                            ok &= raster.capture_causes[index] == 0U;
                        }
                    }
                    if (scale > 1) {
                        for (int y = 0; y < 192 * scale; ++y) {
                            const auto* row = gpu.scaled_line(y);
                            if (!check(row != nullptr, "uniform hi-res clear publishes every row"))
                                std::exit(1);
                            ok &= std::all_of(row, row + 256 * scale,
                                [&](std::uint32_t pixel) { return pixel == expected[0]; });
                        }
                    }
                }
            }
        }
    }
    if (!check(ok, "uniform clear matches scalar fields/output across bands, capture and scales"))
        std::exit(1);
}

enum class BandScene { Gradient, Textures, EdgesFog, Shadow, Translucent, Facing, WireDotsClip, RearPlane, RowBounds };

constexpr std::array<std::string_view, 9> kBandSceneNames{
    "gradient/DDA", "textures/cache", "edges/fog", "shadow", "translucent IDs",
    "equal-depth facing", "wire/dots/clipping", "rear plane", "prepared row bounds"};
constexpr std::array<int, 9> kBandSceneVariants{4, 21, 2, 3, 4, 2, 2, 3, 8};

void fill_band_textures(TestTextureSource& source) {
    source.mapped_textures = 0x0fU;
    source.overlapping_textures = 0U;
    source.overlapping_palettes = false;
    std::mt19937 random(0xbad324);
    for (auto& bank : source.textures)
        for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (auto& bank : source.palettes)
        for (auto& byte : bank) byte = static_cast<std::uint8_t>(random());
    for (std::size_t i = 0; i < 64U; ++i) {
        const auto color = static_cast<std::uint16_t>(
            0x8000U | ((i * 3U) & 31U) | (((i * 7U) & 31U) << 5U) | (((i * 11U) & 31U) << 10U));
        source.textures[0][i * 2U] = static_cast<std::uint8_t>(color);
        source.textures[0][i * 2U + 1U] = static_cast<std::uint8_t>(color >> 8U);
    }
}

void gradient_triangle(Gpu3D& gpu, std::uint32_t attr, std::int16_t z_offset = 0) {
    send(gpu, 0x29, {attr | 0xc0U});
    send(gpu, 0x40, {0U});
    send(gpu, 0x20, {0x001fU});
    send(gpu, 0x22, {0xffb0ff90U});
    position16(gpu, -4003, -4071, static_cast<std::int16_t>(-1024 + z_offset));
    send(gpu, 0x20, {0x03e0U});
    send(gpu, 0x22, {0x007300d1U});
    position16(gpu, 4051, -4057, static_cast<std::int16_t>(1024 + z_offset));
    send(gpu, 0x20, {0x7c00U});
    send(gpu, 0x22, {0x009dffd7U});
    position16(gpu, 11, 4083, z_offset);
    send(gpu, 0x41);
}

void band_scene(Gpu3D& gpu, TestTextureSource& source, BandScene scene, int variant) {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    const auto sort_mode = scene == BandScene::Gradient ? static_cast<std::uint32_t>(variant) :
        scene == BandScene::Translucent ? 1U : 0U;
    // A SWAP selects the mode for the next geometry frame, not the current one.
    submit_frame(gpu, sort_mode);
    Access::attach_texture_source(gpu, source.readonly_view());
    gpu.io_write(0x04000354U, 0x7fffU, 32);
    switch (scene) {
    case BandScene::Gradient:
        quad(gpu, 0x001fU, (31U << 16) | (1U << 24), 3072);
        gpu.io_write(0x04000060U, 1U | (1U << 3), 32);
        send(gpu, 0x10, {0U});
        send(gpu, 0x16, {
            4096U, 0U, 0U, 0U, 0U, 4096U, 0U, 0U,
            0U, 0U, 4096U, 256U, 0U, 0U, 0U, 4096U});
        send(gpu, 0x2a, {(7U << 26) | (15U << 16)});
        gradient_triangle(gpu, (31U << 16) | (2U << 24));
        gradient_triangle(gpu, (17U << 16) | (3U << 24), -256);
        gradient_triangle(gpu, (9U << 16) | (4U << 24), -512);
        break;
    case BandScene::Textures: {
        const auto format = static_cast<std::uint32_t>(variant % 7 + 1);
        source.overlapping_textures = variant >= 7 && variant < 14 ? 1U : 0U;
        source.overlapping_palettes = variant >= 7 && variant < 14;
        source.mapped_textures = variant >= 14 ? 0x0eU : 0x0fU;
        if (format == 4U) {
            std::fill_n(source.textures[0].begin(), 64, 1U);
            palette_color(source, 2U, 0x03e0U);
            palette_color(source, 18U, 0x7c00U);
        }
        gpu.io_write(0x04000060U, 1U | (1U << 3), 32);
        send(gpu, 0x2a, {(format << 26U) | (15U << 16U)});
        send(gpu, 0x22, {0xff80ff90U}); // Negative wrapped/flipped UVs.
        send(gpu, 0x2b, {0U});
        triangle(gpu, 0x7fffU, (31U << 16) | (4U << 24), -5, -5, -5);
        send(gpu, 0x2b, {1U});
        // Same UVs, different palette and nearer depth: reset the polygon cache.
        triangle(gpu, 0x7fffU, (31U << 16) | (5U << 24), -6, -6, -6);
        send(gpu, 0x2a, {(7U << 26U) | 0x4000U | (15U << 16U)});
        triangle(gpu, 0x7fffU, (15U << 16) | (6U << 24), 0, 0, 0);
        break;
    }
    case BandScene::EdgesFog:
        gpu.io_write(0x04000060U, (1U << 5) | (1U << 7) | (variant ? 1U << 4 : 0U), 32);
        gpu.io_write(0x04000358U, (31U << 16) | 0x7c00U, 32);
        for (std::uint32_t i = 0; i < 32U; ++i) {
            gpu.io_write(0x04000360U + i, 127U - i * 4U, 8);
            gpu.io_write(0x04000380U + i * 2U, i | (i << 5U) | (i << 10U), 16);
        }
        for (std::uint32_t i = 0; i < 8U; ++i)
            gpu.io_write(0x04000330U + i * 2U, (i * 3U) | (31U << 5U) | ((31U - i * 3U) << 10U), 16);
        quad(gpu, 0x03e0U, (31U << 16) | (24U << 24) | (1U << 15), 1024);
        for (int y = 24; y < 192; y += 24) {
            const auto viewport = (static_cast<std::uint32_t>(193 - y) << 24U) |
                (223U << 16U) | (static_cast<std::uint32_t>(189 - y) << 8U) | 32U;
            send(gpu, 0x60, {viewport});
            quad(gpu, 0x001fU, (31U << 16) | (8U << 24) | (1U << 15), -512);
            // Four nearer neighbours around a farther pixel: preserve cardinal
            // priority (right/down/left/up), including at each band boundary.
            constexpr std::array<int, 4> dx{1, 0, -1, 0}, dy{0, 1, 0, -1};
            for (std::size_t n = 0; n < dx.size(); ++n) {
                const auto x = static_cast<std::uint32_t>(128 + dx[n]);
                const auto bottom_y = static_cast<std::uint32_t>(191 - y - dy[n]);
                send(gpu, 0x60, {(bottom_y << 24U) | (x << 16U) | (bottom_y << 8U) | x});
                quad(gpu, 0x7fffU, (31U << 16) | (static_cast<std::uint32_t>(8U + 8U * n) << 24U) |
                     (1U << 15), -1024);
            }
        }
        send(gpu, 0x60, {0xbfff0000U});
        edge_triangle(gpu); // Same setup as the pixel-pinned edge test.
        break;
    case BandScene::Shadow:
        shadow_scene(gpu, variant != 0, variant == 1 ? 7U : 1U);
        break;
    case BandScene::Translucent:
        gpu.io_write(0x04000060U, (variant & 1) != 0 ? 1U << 3 : 0U, 32);
        triangle(gpu, 0x001fU, (16U << 16) | (1U << 24), 0, 0, 0);
        triangle(gpu, 0x03e0U, (16U << 16) | (1U << 24) | (1U << 11), -2, -2, -2);
        if ((variant & 2) != 0)
            triangle(gpu, 0x7c00U, (16U << 16) | (2U << 24), -1, -1, -1);
        break;
    case BandScene::Facing:
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (1U << 24)});
        send(gpu, 0x40, {0U});
        position16(gpu, -4096, -4096, 0);
        position16(gpu, variant ? 0 : 4096, variant ? 4096 : -4096, 0);
        position16(gpu, variant ? 4096 : 0, variant ? -4096 : 4096, 0);
        send(gpu, 0x41);
        triangle(gpu, 0x03e0U, (31U << 16) | (2U << 24), 0, 0, 0);
        break;
    case BandScene::WireDotsClip:
        triangle(gpu, 0x001fU, 5U << 24, 0, 0, 0);
        triangle(gpu, 0x7c00U, (31U << 16) | (3U << 24) | (1U << 12), -32, 0, 32);
        gpu.io_write(0x04000610U, 8U, 32);
        for (int y : {23, 24, 167, 168, -1, 192}) {
            const auto bottom_y = static_cast<std::uint32_t>(191 - y) & 0xffU;
            send(gpu, 0x60, {(bottom_y << 24U) | (96U << 16U) | (bottom_y << 8U) | 96U});
            send(gpu, 0x20, {0x03e0U});
            send(gpu, 0x29, {0xc0U | (31U << 16) | (9U << 24) | (1U << 13)});
            send(gpu, 0x40, {0U});
            for (int i = 0; i < 3; ++i) position16(gpu, 0, 0, -3072);
            send(gpu, 0x41);
        }
        if (variant != 0) {
            send(gpu, 0x60, {0xbf0f00f0U}); // Wrapped horizontal viewport.
            triangle(gpu, 0x7fffU, (31U << 16) | (12U << 24), 0, 0, 0);
        }
        break;
    case BandScene::RowBounds: {
        quad(gpu, 0x001fU, (31U << 16) | (1U << 24), 3072);
        const int row = variant * 24;
        const auto bottom_y = static_cast<std::uint32_t>(190 - row) & 0xffU;
        const auto top_y = static_cast<std::uint32_t>(192 - row) & 0xffU;
        // Three-row viewports straddle every band boundary, including a
        // negative top row. Filled and wire paths have different row bounds.
        send(gpu, 0x60, {(top_y << 24U) | (223U << 16U) | (bottom_y << 8U) | 32U});
        send(gpu, 0x20, {0x03e0U});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (2U << 24)});
        send(gpu, 0x40, {0U});
        position16(gpu, -6144, -6144, -512);
        position16(gpu, 6144, -6144, -512);
        position16(gpu, 0, 6144, -512);
        send(gpu, 0x41);
        triangle(gpu, 0x7fffU, 3U << 24, -4, -4, -4);
        const auto dot_y = static_cast<std::uint32_t>(191 - row) & 0xffU;
        send(gpu, 0x60, {(dot_y << 24U) | (96U << 16U) | (dot_y << 8U) | 96U});
        send(gpu, 0x20, {0x7c00U});
        send(gpu, 0x29, {0xc0U | (31U << 16) | (4U << 24)});
        send(gpu, 0x40, {0U});
        // The original polygon already fits one pixel in this tiny viewport.
        // Clipping must not move its outside first vertex (x=95, y=row+1).
        position16(gpu, -6144, -6144, -3072);
        position16(gpu, 0, 0, -3072);
        position16(gpu, 512, 512, -3072);
        send(gpu, 0x41);
        break;
    }
    case BandScene::RearPlane:
        source.mapped_textures = variant == 0 ? 0x0fU : variant == 1 ? 1U << 2 : 1U << 3;
        gpu.io_write(0x04000060U, (1U << 14) | (1U << 7), 32);
        gpu.io_write(0x04000356U, 0xf3e7U, 16);
        gpu.io_write(0x04000358U, (31U << 16) | 0x001fU, 32);
        for (std::uint32_t i = 0; i < 32U; ++i)
            gpu.io_write(0x04000360U + i, 127U - i * 4U, 8);
        triangle(gpu, 0x03e0U, (31U << 16) | (6U << 24), 0, 0, 0);
        break;
    }
    submit_frame(gpu, sort_mode);
}

void test_pack_targets_match_scalar() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    for (int threads : {0, 2, 6}) {
        for (bool capture : {false, true}) {
            for (int scale : {1, 2, 4}) {
                for (auto scene : {BandScene::Gradient, BandScene::EdgesFog}) {
                    TestTextureSource source;
                    fill_band_textures(source);
                    Gpu3D gpu;
                    gpu.set_render_threads(threads);
                    gpu.set_capture_opaque_only(capture);
                    gpu.set_output_scale(scale);
                    band_scene(gpu, source, scene, 0);
                    for (unsigned frame = 0; frame < 2; ++frame) {
                        if (frame != 0U) {
                            gpu.io_write(0x04000350U, (13U << 16) | 0x03e0U | (19U << 24), 32);
                            gpu.io_write(0x04000358U, (7U << 16) | 0x7c00U, 32);
                            gpu.render_start();
                        }
                        if (!check(Access::packed_targets_match_scalar(gpu),
                                   "all native/scaled pixels and capture IDs match scalar packing"))
                            std::exit(1);
                    }
                }
            }
        }
    }
}

struct BandSnapshot {
    std::array<std::uint32_t, 256U * 192U> output{};
    std::array<std::array<std::uint32_t, 4>, 256U * 192U> pixels{};
};

void band_snapshot(const Gpu3D& gpu, BandSnapshot& snapshot) {
    const auto view = gpu.raster_view();
    if (!check(view.output_pixel_count == snapshot.output.size(), "band snapshot covers all 49152 pixels"))
        std::exit(1);
    std::copy_n(view.output, snapshot.output.size(), snapshot.output.begin());
    for (unsigned i = 0; i < snapshot.pixels.size(); ++i)
        snapshot.pixels[i] = dsr::gpu3d_detail::Gpu3DTestAccess::framebuffer_pixel(gpu, i);
}

bool band_snapshot_equal(const BandSnapshot& expected, const BandSnapshot& actual,
                         std::size_t scene, int variant, int threads, int frame) {
    for (std::size_t i = 0; i < actual.output.size(); ++i) {
        if (expected.output[i] == actual.output[i] && expected.pixels[i] == actual.pixels[i]) continue;
        std::cerr << "FAIL: band identity scene=" << kBandSceneNames[scene] << " variant=" << variant
                  << " threads=" << threads << " frame=" << frame << " x=" << i % 256U
                  << " y=" << i / 256U << " expected=0x" << std::hex << expected.output[i]
                  << " actual=0x" << actual.output[i] << std::dec << " (output or framebuffer fields)\n";
        return false;
    }
    return true;
}

void test_band_parallel_identity() {
    constexpr std::array<int, 4> thread_counts{0, 1, 2, 6};
    auto expected = std::make_unique<std::array<BandSnapshot, 3>>();
    auto actual = std::make_unique<BandSnapshot>();
    auto retained = std::make_unique<BandSnapshot>();
    std::size_t scenes = 0;
    for (std::size_t scene = 0; scene < kBandSceneNames.size(); ++scene) {
        for (int variant = 0; variant < kBandSceneVariants[scene]; ++variant) {
            ++scenes;
            for (int threads : thread_counts) {
                TestTextureSource source;
                fill_band_textures(source);
                Gpu3D gpu;
                gpu.set_render_threads(threads);
                band_scene(gpu, source, static_cast<BandScene>(scene), variant);
                for (int frame = 0; frame < 3; ++frame) {
                    if (frame == 1) {
                        gpu.io_write(0x04000350U, (13U << 16) | 0x03e0U | (1U << 15) | (19U << 24), 32);
                        gpu.io_write(0x04000358U, (7U << 16) | 0x7c00U, 32);
                        source.textures[0][0] ^= 0x3fU;
                        source.palettes[0][18] ^= 0x17U;
                        gpu.render_start(); // No swap: clear, fog and texture reads are live.
                    } else if (frame == 2) {
                        // Reconfigure after live use, then reset without recreating workers.
                        gpu.set_render_threads(threads);
                        gpu.reset();
                        fill_band_textures(source);
                        band_scene(gpu, source, static_cast<BandScene>(scene), variant);
                    }
                    band_snapshot(gpu, *actual);
                    if (scene == 0U && frame != 1) {
                        // An independent expectation, not another banded render:
                        // the far red quad covers every row in the final column.
                        for (unsigned y = 0; y < 192U; ++y) {
                            const auto word = actual->output[y * 256U + 255U];
                            if (word == ((31U << 24) | 63U)) continue;
                            std::cerr << "FAIL: band coverage threads=" << threads << " x=255 y=" << y
                                      << " expected=0x1f00003f actual=0x" << std::hex << word << std::dec << '\n';
                            std::exit(1);
                        }
                    }
                    if (scene == 8U && frame != 1) {
                        const auto index = static_cast<std::size_t>(variant * 24 + 1) * 256U + 95U;
                        if (!check(actual->output[index] == ((31U << 24) | (63U << 12)),
                                   "prepared bounds retain the original unclipped one-dot row"))
                            std::exit(1);
                    }
                    if (threads == 0) {
                        (*expected)[frame] = *actual;
                    } else if (!band_snapshot_equal((*expected)[frame], *actual, scene, variant, threads, frame)) {
                        std::exit(1);
                    }
                    gpu.render_start();
                    band_snapshot(gpu, *retained);
                    if (!band_snapshot_equal(*actual, *retained, scene, variant, threads, frame)) std::exit(1);
                    if (frame == 0 && scene == 3U) {
                        const auto green = (actual->output[96U * 256U + 128U] >> 6U) & 63U;
                        if (!check(green == (variant == 2 ? 29U : 63U), "pinned shadow across band boundaries"))
                            std::exit(1);
                    }
                    if (frame == 0 && scene == 1U && variant == 3) {
                        if (!check(actual->output[72U * 256U + 128U] == ((31U << 24) | (63U << 12)),
                                   "consecutive indexed polygons reset the palette/texel cache")) std::exit(1);
                    }
                }
                if (!check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 0U,
                           "identity fixtures use stateless texture callbacks")) std::exit(1);
            }
        }
    }
    std::cout << "PASS: band identity " << scenes << " scenes x 4 thread modes x 3 frames x 49152 pixels"
              << " (all output words and framebuffer fields; retained rerenders also identical)\n";
}

void test_hires_full_viewport_quad() {
    constexpr std::uint16_t color = 0x56b2U;
    const auto expected = reference_clear_pixel((31U << 16) | color, 0U)[0];
    bool ok = true;
    for (int threads : {0, 6}) {
        for (int scale : {2, 3}) {
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            gpu.set_output_scale(scale);
            quad(gpu, color, (31U << 16) | (7U << 24));
            submit_frame(gpu);
            const auto* first = gpu.scaled_line(0);
            ok &= check(first != nullptr, "hi-res full-viewport quad has an output buffer");
            for (int y = 0; y < 192 * scale; ++y) {
                const auto* row = gpu.scaled_line(y);
                ok &= check(row == first + y * 256 * scale,
                            "hi-res rows use the scaled width as their stride");
                if (!check(std::all_of(row, row + 256 * scale,
                                      [expected](std::uint32_t pixel) { return pixel == expected; }),
                           "hi-res quad covers every pixel with exact RGB6 and alpha5"))
                    std::exit(1);
            }
            ok &= check(gpu.scaled_line(-1) == nullptr &&
                        gpu.scaled_line(192 * scale) == nullptr,
                        "hi-res row bounds reject negative and past-end rows");
        }
    }
    if (!ok) std::exit(1);
}

void test_hires_clipped_offset_viewport() {
    bool ok = true;
    for (int scale : {2, 3}) {
        Gpu3D gpu;
        gpu.set_output_scale(scale);
        send(gpu, 0x60, {(170U << 24) | (236U << 16) | (23U << 8) | 17U});
        send(gpu, 0x20, {0x001fU});
        send(gpu, 0x29, {(31U << 16) | 0xc0U});
        send(gpu, 0x40, {1U});
        position16(gpu, -8192, -8192, 0);
        position16(gpu, 8192, -8192, 0);
        position16(gpu, 8192, 8192, 0);
        position16(gpu, -8192, 8192, 0);
        send(gpu, 0x41);
        submit_frame(gpu);
        for (int y = 0; y < 192 * scale; ++y) {
            const auto* row = gpu.scaled_line(y);
            for (int x = 0; x < 256 * scale; ++x) {
                const bool inside = x >= 17 * scale && x < 237 * scale &&
                                    y >= 21 * scale && y < 169 * scale;
                ok &= row[x] == (inside ? ((31U << 24U) | 63U) : 0U);
            }
        }
    }
    if (!check(ok, "clipping and inclusive offset viewport bounds scale in output-pixel units"))
        std::exit(1);
}

void test_hires_clear_bitmap_sampling() {
    TestTextureSource source;
    fill_band_textures(source);
    Gpu3D gpu;
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.readonly_view());
    gpu.io_write(0x04000060U, 1U << 14, 32);
    gpu.io_write(0x04000356U, 0xf3e7U, 16);
    bool ok = true;
    for (int scale : {2, 3}) {
        gpu.set_output_scale(scale);
        gpu.render_start();
        for (int y = 0; y < 192 * scale; ++y) {
            const auto* row = gpu.scaled_line(y);
            const auto* native = gpu.line(y / scale);
            for (int x = 0; x < 256 * scale; ++x) ok &= row[x] == native[x / scale];
        }
    }
    if (!check(ok, "hi-res rear-plane color/depth bitmap samples retain native texel coordinates"))
        std::exit(1);
}

void test_hires_triangle_coverage() {
    bool ok = true;
    for (int threads : {0, 6}) {
        Gpu3D gpu;
        gpu.set_render_threads(threads);
        gpu.set_output_scale(2);
        triangle(gpu, 0x001fU, (31U << 16) | (7U << 24), 0, 0, 0);
        submit_frame(gpu);
        const auto covered = [](std::uint32_t pixel) { return (pixel >> 24U) != 0U; };
        const auto native = std::count_if(gpu.line(0), gpu.line(0) + 256U * 192U, covered);
        const auto hires = std::count_if(gpu.scaled_line(0),
                                        gpu.scaled_line(0) + 512U * 384U, covered);
        ok &= check(native > 0 && hires * 10 >= native * 36 && hires * 10 <= native * 44,
                    "scale-2 triangle covers between 3.6 and 4.4 times the native pixels");
    }
    if (!ok) std::exit(1);
}

void test_hires_native_identity_and_capture() {
    using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
    bool ok = true;
    for (int threads : {0, 6}) {
        for (bool capture : {false, true}) {
            Gpu3D native;
            Gpu3D scaled;
            int native_callbacks = 0;
            int scaled_callbacks = 0;
            native.on_render_complete = [&] { ++native_callbacks; };
            scaled.on_render_complete = [&] { ++scaled_callbacks; };
            scaled.set_output_scale(2);
            for (auto* gpu : {&native, &scaled}) {
                gpu->set_render_threads(threads);
                gpu->set_capture_opaque_only(capture);
                gpu->io_write(0x04000060U, (1U << 3) | (1U << 4) | (1U << 5), 32);
                gpu->io_write(0x04000330U, 0x001f7c00U, 32);
                quad(*gpu, 0x7c00U, (31U << 16) | (24U << 24), 2048);
                edge_triangle(*gpu);
                triangle(*gpu, 0x001fU, (17U << 16) | (2U << 24), -1, -1, -1);
                gpu->run(100000);
                const auto counts = gpu->io_read(0x04000604U, 32);
                ok &= check((counts & 0xfffU) == 3U && (counts >> 16U) == 10U,
                            "hi-res configuration does not change polygon/vertex RAM counts");
                submit_frame(*gpu);
            }
            for (int scale : {2, 1, 2, 3, 1, 4, 2, 1}) {
                scaled.set_output_scale(scale);
                native.render_start();
                scaled.render_start();
                ok &= check(std::equal(native.line(0), native.line(0) + 256U * 192U, scaled.line(0)),
                            "mixed opaque/translucent/edge native output survives scale changes");
                bool metadata_equal = true;
                for (unsigned i = 0; i < 256U * 192U; ++i)
                    metadata_equal &= Access::framebuffer_pixel(native, i) ==
                                      Access::framebuffer_pixel(scaled, i);
                ok &= check(metadata_equal, "hi-res pass leaves all native framebuffer metadata untouched");
                const auto a = native.geometry_view(), b = scaled.geometry_view();
                ok &= check(a.generation == b.generation && a.vertex_count == b.vertex_count &&
                            a.polygon_count == b.polygon_count && a.swap_mode == b.swap_mode &&
                            native.io_read(0x04000604U, 32) == scaled.io_read(0x04000604U, 32) &&
                            native.io_read(0x04000600U, 32) == scaled.io_read(0x04000600U, 32) &&
                            native_callbacks == scaled_callbacks,
                            "hi-res pass does not double-count GX state or render callbacks");
                const auto order_a = native.draw_order_view(), order_b = scaled.draw_order_view();
                ok &= check(order_a.opaque_count == order_b.opaque_count &&
                            order_a.translucent_count == order_b.translucent_count &&
                            std::equal(order_a.opaque, order_a.opaque + order_a.opaque_count, order_b.opaque) &&
                            std::equal(order_a.translucent, order_a.translucent + order_a.translucent_count,
                                       order_b.translucent),
                            "hi-res preparation preserves native draw order");
                if (capture) {
                    const auto raster_a = native.raster_view(), raster_b = scaled.raster_view();
                    ok &= check(raster_a.opaque_only_mask != nullptr && raster_b.opaque_only_mask != nullptr &&
                                std::equal(raster_a.opaque_only_mask, raster_a.opaque_only_mask + 256U * 192U,
                                           raster_b.opaque_only_mask) &&
                                std::equal(raster_a.capture_causes, raster_a.capture_causes + 256U * 192U,
                                           raster_b.capture_causes) &&
                                std::equal(raster_a.capture_polygon_touches,
                                           raster_a.capture_polygon_touches + a.polygon_count,
                                           raster_b.capture_polygon_touches),
                                "hi-res pass neither overwrites nor double-counts native capture scratch");
                    if (raster_a.capture_polygon_ids != nullptr)
                        ok &= check(raster_b.capture_polygon_ids != nullptr &&
                                    std::equal(raster_a.capture_polygon_ids,
                                               raster_a.capture_polygon_ids + 256U * 192U,
                                               raster_b.capture_polygon_ids),
                                    "hi-res pass preserves captured native polygon IDs");
                }
                ok &= check(scale != 1 || scaled.scaled_line(0) == nullptr,
                            "returning to native scale disables hi-res rows");
            }
        }
    }
    if (!ok) std::exit(1);
}

void test_hires_scale_clamp() {
    Gpu3D gpu;
    bool ok = check(gpu.output_scale() == 1 && gpu.scaled_line(0) == nullptr,
                    "hi-res rendering defaults off");
    gpu.set_output_scale(9);
    ok &= check(gpu.output_scale() == 4, "hi-res scale clamps above four");
    gpu.set_output_scale(0);
    ok &= check(gpu.output_scale() == 1 && gpu.scaled_line(0) == nullptr,
                "hi-res zero scale clamps to one");
    gpu.set_output_scale(-3);
    ok &= check(gpu.output_scale() == 1, "hi-res negative scale clamps to one");
    gpu.set_output_scale(2);
    quad(gpu, 0x001fU, 31U << 16);
    submit_frame(gpu);
    gpu.reset();
    ok &= check(gpu.output_scale() == 2 && gpu.scaled_line(0) == nullptr,
                "reset retains host scale configuration but invalidates hi-res output");
    gpu.render_start();
    ok &= check(gpu.scaled_line(0) != nullptr &&
                std::all_of(gpu.scaled_line(0), gpu.scaled_line(0) + 512U * 384U,
                            [](std::uint32_t pixel) { return pixel == 0U; }),
                "next render publishes the reset clear plane at the retained scale");
    if (!ok) std::exit(1);
}

void test_hires_scale_change_invalidates_rows() {
    Gpu3D gpu;
    gpu.set_output_scale(4);
    bool ok = check(gpu.scaled_line(0) == nullptr,
                    "enabling hi-res waits for the first completed render");
    quad(gpu, 0x001fU, 31U << 16);
    submit_frame(gpu);
    ok &= check(gpu.scaled_line(0) != nullptr &&
                gpu.scaled_line(767) == gpu.scaled_line(0) + 767U * 1024U,
                "completed scale-four output has a 1024-pixel row stride");
    gpu.set_output_scale(2);
    for (int y = -1; y <= 768; ++y)
        ok &= check(gpu.scaled_line(y) == nullptr,
                    "scale change never exposes stale scale-four rows at scale two");
    gpu.render_start();
    for (int y = 0; y < 384; ++y) {
        const auto* row = gpu.scaled_line(y);
        ok &= check(row != nullptr && row == gpu.scaled_line(0) + y * 512U &&
                    std::all_of(row, row + 512U,
                                [](std::uint32_t pixel) { return pixel == ((31U << 24U) | 63U); }),
                    "next completed render publishes valid 512-wide scale-two rows");
    }
    gpu.set_output_scale(4);
    gpu.set_output_scale(2);
    ok &= check(gpu.scaled_line(0) == nullptr,
                "changing away and back before rendering still invalidates borrowed rows");
    if (!ok) std::exit(1);
}

void test_hires_shared_decoded_texture_state() {
    bool ok = true;
    for (int threads : {0, 6}) {
        TestTextureSource source;
        source.textures[0][0] = 1U;
        palette_color(source, 2U, 0x03e0U);
        Gpu3D gpu;
        gpu.set_render_threads(threads);
        gpu.set_output_scale(2);
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source.versioned_view());
        gpu.io_write(0x04000060U, 1U, 32);
        send(gpu, 0x2a, {4U << 26U});
        triangle(gpu, 0x7fffU, (31U << 16) | (1U << 24), 0, 0, 0);
        submit_frame(gpu);
        ok &= check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 1U &&
                    gpu.scaled_line(192)[256] == ((31U << 24) | (63U << 6)),
                    "native and hi-res passes share one pinned decoded texture snapshot");
        palette_color(source, 2U, 0x001fU);
        ++source.generation;
        gpu.render_start();
        ok &= check(source.texture_slot_zero_lookups.load(std::memory_order_relaxed) == 2U &&
                    gpu.line(96)[128] == ((31U << 24) | 63U) &&
                    gpu.scaled_line(192)[256] == ((31U << 24) | 63U),
                    "one texture invalidation updates both passes without a second cache preparation");
    }
    if (!ok) std::exit(1);
}

void test_hires_band_parallel_identity() {
    constexpr int scale = 2;
    constexpr std::array<int, 9> variants{3, 10, 1, 2, 3, 1, 1, 1, 4};
    constexpr std::size_t pixels = 256U * 192U * scale * scale;
    std::array<std::vector<std::uint32_t>, 2> expected{
        std::vector<std::uint32_t>(pixels), std::vector<std::uint32_t>(pixels)};
    bool ok = true;
    for (std::size_t scene = 0; scene < kBandSceneNames.size(); ++scene) {
        for (int threads : {0, 2, 6}) {
            TestTextureSource source;
            fill_band_textures(source);
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            gpu.set_output_scale(scale);
            band_scene(gpu, source, static_cast<BandScene>(scene), variants[scene]);
            for (int frame = 0; frame < 2; ++frame) {
                if (frame != 0) {
                    source.textures[0][0] ^= 0x3fU;
                    source.palettes[0][18] ^= 0x17U;
                    gpu.io_write(0x04000350U, (13U << 16) | 0x03e0U | (19U << 24), 32);
                    gpu.render_start();
                }
                const auto* output = gpu.scaled_line(0);
                if (threads == 0) std::copy_n(output, pixels, expected[frame].begin());
                else if (!std::equal(expected[frame].begin(), expected[frame].end(), output)) {
                    std::cerr << "FAIL: hi-res band identity scene=" << kBandSceneNames[scene]
                              << " threads=" << threads << " frame=" << frame << '\n';
                    std::exit(1);
                }
                gpu.render_start();
                ok &= check(std::equal(expected[frame].begin(), expected[frame].end(), gpu.scaled_line(0)),
                            "retained hi-res scene matches inline and threaded renders");
            }
        }
    }
    if (!ok) std::exit(1);
    std::cout << "PASS: hi-res band identity 9 scenes x 3 thread modes x 2 frames x 196608 pixels\n";
}

void opaque_mask_quad(Gpu3D& gpu, std::uint32_t attr, std::int16_t z,
                      std::uint32_t polygon_id = 9U) {
    send(gpu, 0x20, {0x001fU});
    send(gpu, 0x29, {attr | 0xc0U | (polygon_id << 24U)});
    send(gpu, 0x40, {1U});
    for (const auto [x, y] : std::array<std::array<std::int16_t, 2>, 4>{
             std::array<std::int16_t, 2>{-1024, -1024}, {1024, -1024},
             {1024, 1024}, {-1024, 1024}})
        position16(gpu, x, y, z);
    send(gpu, 0x41);
}

// Independent branch-by-branch policy reference. The old capture excluded
// every partial touch; the new shader can represent only this subset.
static bool single_layer_reference(std::uint8_t alpha, std::uint32_t attr,
                                   std::uint32_t disp, std::uint8_t dst_alpha,
                                   bool dst_fog, bool& seen) {
    if (((attr >> 4) & 3) == 3 || ((attr >> 16) & 31) == 0) return false;
    if (alpha == 0) return true;
    if (alpha == 31) return !seen;
    const bool first = !seen;
    seen = true;
    if (!first || dst_alpha != 31) return false;
    if ((disp & 8) == 0) return false;
    if (attr & (1U << 11)) return false;
    if (attr & (1U << 14)) return false;
    if ((disp & 128) && dst_fog && !(attr & (1U << 15))) return false;
    return true;
}

void test_single_layer_capture_policy() {
    std::mt19937 random(0x9d517);
    for (unsigned trial = 0; trial < 100000; ++trial) {
        bool reference_seen = false, actual_seen = false;
        for (unsigned fragment = 0; fragment < 8; ++fragment) {
            const auto alpha = static_cast<std::uint8_t>(random() & 31U);
            const auto attr = random();
            const auto disp = random();
            const auto destination_alpha = static_cast<std::uint8_t>(random() & 31U);
            const bool fog = (random() & 1U) != 0U;
            const bool reference = single_layer_reference(alpha, attr, disp,
                destination_alpha, fog, reference_seen);
            const bool actual = dsr::gpu3d_detail::capture_single_layer(alpha, attr,
                disp, destination_alpha, fog, actual_seen);
            if (!check(reference == actual && reference_seen == actual_seen,
                       "random single-layer history matches independent reference")) std::exit(1);
        }
    }
    std::cout << "PASS: 800000 randomized single-layer history decisions\n";
}

// Original capture_fragment's classification, retained as a reference for
// raw diagnostics. Eligibility intentionally expands only through the policy.
static bool old_capture_excludes(unsigned polygon_alpha, unsigned mode, unsigned texel_alpha) {
    if (polygon_alpha != 31U || mode == 3U) return true;
    return texel_alpha != 0U && texel_alpha != 31U;
}

void test_capture_fragment_random_reference() {
    std::mt19937 random(0x9a1fa);
    for (unsigned trial = 0; trial < 128; ++trial) {
        Gpu3D gpu;
        gpu.set_render_threads(0);
        gpu.set_capture_opaque_only(true);
        auto source = std::make_unique<TestTextureSource>();
        const unsigned polygon_alpha = 1U + random() % 31U;
        const unsigned texel_alpha = random() % 32U;
        const unsigned mode = random() % 4U;
        source->textures[0].fill(static_cast<std::uint8_t>((texel_alpha << 3U) | 1U));
        dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source->readonly_view());
        gpu.io_write(0x04000060U, 9U, 32);
        quad(gpu, 0x03e0U, (31U << 16U) | (1U << 24U), 0);
        send(gpu, 0x2a, {6U << 26U});
        opaque_mask_quad(gpu, (polygon_alpha << 16U) | (mode << 4U), -1024);
        submit_frame(gpu);
        const auto raster = gpu.raster_view();
        constexpr unsigned pixel = 90U * 256U + 112U;
        if (!check((raster.capture_causes[pixel] != 0) ==
                   old_capture_excludes(polygon_alpha, mode, texel_alpha),
                   "raw touched classification matches original on random inputs")) std::exit(1);
        dsr::Gpu3DPolygon polygon{};
        polygon.attr = (polygon_alpha << 16U) | (mode << 4U) | 0xc0U;
        polygon.texture = 6U << 26U;
        const auto shaded = dsr::gpu3d_detail::Gpu3DTestAccess::shade_pixel(
            gpu, polygon, {63.0F, 63.0F, 63.0F}, 0.0F, 0.0F);
        bool seen = false;
        const bool eligible = single_layer_reference(shaded[3], polygon.attr, 9U, 31U, false, seen);
        if (!check(raster.opaque_only_mask[pixel] == static_cast<unsigned>(eligible),
                   "captured eligibility agrees with native shade alpha, not raw texel alpha")) std::exit(1);
    }
    std::cout << "PASS: 128 randomized capture fragments against original/native references\n";
}

void test_capture_depth_rejected_history() {
    constexpr unsigned pixel = 90U * 256U + 112U;
    constexpr std::size_t pixels = 256U * 192U;
    for (int threads : {0, 2, 6}) {
        std::uint32_t behind_color = 0;
        for (int scene = 0; scene < 6; ++scene) {
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            gpu.io_write(0x04000060U, 1U << 3U, 32);
            quad(gpu, 0x03e0U, (31U << 16U) | (1U << 24U), 0);
            if (scene != 2) {
                const auto attr = (16U << 16U) | (scene == 5 ? (1U << 14U) : 0U);
                opaque_mask_quad(gpu, attr, scene == 4 ? 0 : 1024, 9U);
            }
            if (scene != 0)
                opaque_mask_quad(gpu, 16U << 16U, -1024, 10U);
            if (scene == 2)
                opaque_mask_quad(gpu, 16U << 16U, -2048, 11U);
            if (scene == 3)
                opaque_mask_quad(gpu, 16U << 16U, 2048, 11U);
            submit_frame(gpu);
            const auto without_capture = gpu.raster_view().output;
            std::vector<std::uint32_t> output(without_capture, without_capture + pixels);
            gpu.set_capture_opaque_only(true);
            gpu.render_start();
            const auto raster = gpu.raster_view();
            bool ok = check(raster.opaque_only_mask != nullptr &&
                            raster.capture_causes != nullptr &&
                            raster.capture_polygon_touches != nullptr,
                            "depth history capture publishes mask and diagnostics");
            if (!ok) std::exit(1);
            ok &= check(std::equal(output.begin(), output.end(), raster.output),
                        "depth history capture preserves native output");
            ok &= check(raster.opaque_only_mask[pixel] ==
                            ((scene == 2 || scene == 4 || scene == 5) ? 0U : 1U),
                        "occluded partials are ignored except ties and depth-equal mode");
            if (scene == 0) {
                behind_color = raster.output[pixel];
                ok &= check(raster.capture_causes[pixel] != 0U &&
                            raster.capture_polygon_touches[1] != 0U,
                            "occluded partial remains in raw capture diagnostics");
            } else {
                ok &= check(raster.output[pixel] != behind_color,
                            "later accepted partial changes the native framebuffer");
            }
            if (!ok) std::exit(1);
        }
        {
            Gpu3D edge;
            edge.set_render_threads(threads);
            edge.set_capture_opaque_only(true);
            edge.io_write(0x04000060U, 1U << 3U, 32);
            opaque_mask_quad(edge, 31U << 16U, 0, 1U);
            opaque_mask_quad(edge, 16U << 16U, 1024, 9U);
            submit_frame(edge);
            const auto raster = edge.raster_view();
            constexpr unsigned inside = 90U * 256U + 112U;
            constexpr unsigned boundary = 90U * 256U + 96U;
            bool ok = check(raster.opaque_only_mask != nullptr && raster.capture_causes != nullptr,
                            "occluder edge capture publishes buffers");
            if (!ok) std::exit(1);
            ok &= check(raster.capture_causes[boundary] != 0U &&
                        raster.opaque_only_mask[boundary] == 0U,
                        "rejected partial at an opaque edge remains conservative");
            ok &= check(raster.opaque_only_mask[inside] == 1U,
                        "rejected partial inside an opaque neighborhood stays eligible");
            if (!ok) std::exit(1);
        }
        {
            Gpu3D dot;
            dot.set_render_threads(threads);
            dot.set_capture_opaque_only(true);
            dot.io_write(0x04000060U, 1U << 3U, 32);
            dot.io_write(0x04000610U, 8U, 32);
            quad(dot, 0x03e0U, (31U << 16U) | (1U << 24U), 0);
            send(dot, 0x20, {0x001fU});
            send(dot, 0x29, {0xc0U | (16U << 16U) | (1U << 13U) | (9U << 24U)});
            send(dot, 0x40, {0U});
            position16(dot, 0, 0, 1024);
            position16(dot, 0, 0, 1024);
            position16(dot, 0, 0, 1024);
            send(dot, 0x41);
            submit_frame(dot);
            const auto raster = dot.raster_view();
            constexpr unsigned center = 96U * 256U + 128U;
            bool ok = check(raster.opaque_only_mask != nullptr && raster.capture_causes != nullptr,
                            "one-dot capture publishes buffers");
            if (!ok) std::exit(1);
            ok &= check(raster.capture_causes[center] != 0U &&
                        raster.opaque_only_mask[center] == 1U,
                        "safely occluded one-dot partial is diagnostic-only");
            ok &= check(((dot.line(96)[128] >> 6U) & 63U) == 63U,
                        "depth-rejected one-dot leaves opaque framebuffer intact");
            if (!ok) std::exit(1);
        }
        {
            Gpu3D transparent;
            transparent.set_render_threads(threads);
            transparent.set_capture_opaque_only(true);
            auto source = std::make_unique<TestTextureSource>();
            source->textures[0].fill(1U); // A5I3 alpha zero, palette index one.
            dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(
                transparent, source->readonly_view());
            transparent.io_write(0x04000060U, 1U | (1U << 3U), 32);
            opaque_mask_quad(transparent, 31U << 16U, 0, 1U);
            send(transparent, 0x2a, {6U << 26U});
            opaque_mask_quad(transparent, 16U << 16U, 1024, 9U);
            submit_frame(transparent);
            const auto raster = transparent.raster_view();
            constexpr unsigned boundary = 90U * 256U + 96U;
            bool ok = check(raster.opaque_only_mask != nullptr && raster.capture_causes != nullptr,
                            "transparent rejected fragment capture publishes buffers");
            if (!ok) std::exit(1);
            ok &= check(raster.capture_causes[boundary] != 0U &&
                        raster.opaque_only_mask[boundary] == 1U,
                        "zero-effective-alpha rejected partial does not exclude an opaque edge");
            if (!ok) std::exit(1);
        }
        {
            Gpu3D dots;
            dots.set_render_threads(threads);
            dots.set_capture_opaque_only(true);
            dots.io_write(0x04000060U, 1U << 3U, 32);
            dots.io_write(0x04000610U, 8U, 32);
            quad(dots, 0x03e0U, (31U << 16U) | (1U << 24U), 1024);
            send(dots, 0x20, {0x001fU});
            send(dots, 0x29, {0xc0U | (31U << 16U) | (1U << 13U) | (2U << 24U)});
            for (std::int16_t y : {-96, -64, 0, 32, 64}) {
                for (std::int16_t x : {-64, -32, 0, 32, 64}) {
                    send(dots, 0x40, {0U});
                    for (int vertex = 0; vertex < 3; ++vertex)
                        position16(dots, x, y, -1024);
                    send(dots, 0x41);
                }
            }
            opaque_mask_quad(dots, 16U << 16U, 0, 9U);
            opaque_mask_quad(dots, 16U << 16U, 0, 10U);
            submit_frame(dots);
            const auto raster = dots.raster_view();
            constexpr unsigned center = 96U * 256U + 128U;
            bool ok = check(raster.opaque_only_mask != nullptr && raster.capture_causes != nullptr,
                            "native-only occluder capture publishes buffers");
            if (!ok) std::exit(1);
            for (unsigned y = 94; y <= 98; ++y)
                for (unsigned x = 126; x <= 130; ++x)
                    ok &= check((dots.line(y)[x] & 63U) == 63U,
                                "native-only dots cover the complete occlusion neighborhood");
            ok &= check((dots.line(96)[128] & 63U) == 63U &&
                        raster.capture_causes[center] != 0U,
                        "opaque one-dot fragment rejects the partial at the center");
            ok &= check(raster.opaque_only_mask[center] == 0U,
                        "native-only opaque dot cannot prove GPU occlusion");
            if (!ok) std::exit(1);
        }
        Gpu3D shadow;
        shadow.set_render_threads(threads);
        shadow.set_capture_opaque_only(true);
        shadow_scene(shadow, true, 1U);
        submit_frame(shadow);
        const auto raster = shadow.raster_view();
        constexpr unsigned shadow_pixel = 96U * 256U + 128U;
        bool ok = check(raster.opaque_only_mask != nullptr &&
                        raster.opaque_only_mask[shadow_pixel] == 0U,
                        "depth-failed shadow mask remains conservatively ineligible");
        ok &= check(((shadow.line(96)[128] >> 6U) & 63U) == 29U,
                    "depth-failed shadow mask still sets stencil for shadow color");
        if (!ok) std::exit(1);
    }
    std::cout << "PASS: depth-aware capture history and shadow stencil, 3 thread modes\n";
}

void test_capture_opaque_depth_provenance() {
    constexpr unsigned pixel = 90U * 256U + 112U;
    for (int threads : {0, 2, 6}) {
        for (int scene = 0; scene < 5; ++scene) {
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            gpu.set_capture_opaque_only(true);
            auto source = std::make_unique<TestTextureSource>();
            // Every native sample sees opaque index 1, even in the potentially
            // cutout case. Eligibility must depend on format, not this sample.
            source->textures[0].fill(scene == 2 ? 0xf9U : scene == 3 ? 0xffU : 0x55U);
            source->palettes[0][2] = 31U;
            dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source->readonly_view());
            gpu.io_write(0x04000060U, 1U | (1U << 3U), 32);
            const auto texture = scene == 2 ? 6U << 26U :
                                 scene == 3 ? 7U << 26U :
                                 (2U << 26U) | (scene == 1 || scene == 4 ? 1U << 29U : 0U);
            send(gpu, 0x2a, {texture});
            quad(gpu, 0x7fffU, (31U << 16U) | (1U << 24U), 0);
            send(gpu, 0x2a, {0U});
            if (scene == 4)
                quad(gpu, 0x7fffU, (31U << 16U) | (2U << 24U), -512);
            opaque_mask_quad(gpu, 16U << 16U, 1024, 9U);
            opaque_mask_quad(gpu, 16U << 16U, 2048, 10U);
            submit_frame(gpu);
            const auto raster = gpu.raster_view();
            bool ok = check(raster.opaque_only_mask != nullptr && raster.capture_causes != nullptr,
                            "opaque depth provenance capture publishes buffers");
            if (!ok) std::exit(1);
            ok &= check(raster.capture_causes[pixel] != 0U,
                        "rejected partial layers reach provenance guard");
            ok &= check(raster.opaque_only_mask[pixel] == (scene == 0 || scene == 4 ? 1U : 0U),
                        "only reliably opaque final depth protects rejected partials");
            if (!ok) std::exit(1);
        }
    }
}

void test_opaque_only_capture() {
    constexpr std::size_t pixels = 256U * 192U;
    auto expected = std::make_unique<std::array<std::uint8_t, pixels>>();
    auto output = std::make_unique<std::array<std::uint32_t, pixels>>();
    auto expected_causes = std::make_unique<std::array<std::uint8_t, pixels>>();
#ifdef DSR_GPU_CAPTURE_IDS
    auto expected_ids = std::make_unique<std::array<std::uint8_t, pixels>>();
#endif
    std::array<std::uint32_t, 2048> expected_touches{};
    bool ok = true;
    // Opaque, translucent, depth-rejected translucent, partial texel,
    // depth-rejected partial texel, partial decal texel, binary texel,
    // shadow mask, wireframe, duplicate translucent ID.
    for (int scene = 0; scene < 10; ++scene) {
        for (int threads : {0, 2, 6}) {
            Gpu3D gpu;
            gpu.set_render_threads(threads);
            auto source = std::make_unique<TestTextureSource>();
            source->textures[0].fill(scene == 6 ? 0xf9U : 0x79U); // A5I3 alpha31/15, index1.
            source->palettes[0][2] = 31U;
            dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, source->readonly_view());
            gpu.io_write(0x04000060U, 1U | (1U << 3U) | (1U << 5U) | (1U << 7U), 32);
            quad(gpu, 0x03e0U, (31U << 16U) | (1U << 24U), 0);
            if (scene >= 3 && scene <= 6) send(gpu, 0x2a, {6U << 26U});
            const unsigned alpha = scene == 8 ? 0U : scene == 1 || scene == 2 || scene == 9 ? 16U : 31U;
            const unsigned mode = scene == 5 ? 1U : scene == 7 ? 3U : 0U;
            // Shadow ID0 sets stencil on depth failure; normal opaque polygons
            // never consult that stencil, so only its touched footprint matters.
            if (scene == 7) {
                send(gpu, 0x20, {0x001fU});
                send(gpu, 0x29, {(31U << 16U) | (3U << 4U) | 0xc0U});
                send(gpu, 0x40, {1U});
                for (const auto [x, y] : std::array<std::array<std::int16_t, 2>, 4>{
                         std::array<std::int16_t, 2>{-1024, -1024}, {1024, -1024},
                         {1024, 1024}, {-1024, 1024}})
                    position16(gpu, x, y, 1024);
                send(gpu, 0x41);
            } else {
                opaque_mask_quad(gpu, (alpha << 16U) | (mode << 4U),
                                 scene == 2 || scene == 4 ? 1024 : -1024);
            }
            if (scene == 9) opaque_mask_quad(gpu, 16U << 16U, -2048);
            submit_frame(gpu);
            ok &= check(gpu.raster_view().opaque_only_mask == nullptr &&
                        gpu.raster_view().capture_polygon_ids == nullptr, "capture is off by default");
            std::copy_n(gpu.raster_view().output, pixels, output->begin());
            gpu.set_capture_opaque_only(true);
            ok &= check(gpu.raster_view().opaque_only_mask == nullptr &&
                        gpu.raster_view().capture_polygon_ids == nullptr, "enabling waits for a completed render");
            gpu.render_start();
            const auto raster = gpu.raster_view();
            ok &= check(raster.opaque_only_mask != nullptr &&
                        std::equal(output->begin(), output->end(), raster.output),
                        "capture never changes software colour/alpha output");
            if (!raster.opaque_only_mask) std::exit(1);
            if (threads == 0) std::copy_n(raster.opaque_only_mask, pixels, expected->begin());
            else ok &= check(std::equal(expected->begin(), expected->end(), raster.opaque_only_mask),
                             "opaque-only capture/dilation is identical with 0/2/6 threads");
            ok &= check(raster.capture_causes && raster.capture_polygon_touches,
                        "completed capture publishes diagnostic buffers");
#ifdef DSR_GPU_CAPTURE_IDS
            ok &= check(raster.capture_polygon_ids != nullptr, "GPU test build captures native polygon IDs");
#else
            ok &= check(raster.capture_polygon_ids == nullptr, "production render skips test-only polygon-ID capture");
#endif
            const auto count = gpu.geometry_view().polygon_count;
            if (threads == 0) {
                std::copy_n(raster.capture_causes, pixels, expected_causes->begin());
#ifdef DSR_GPU_CAPTURE_IDS
                std::copy_n(raster.capture_polygon_ids, pixels, expected_ids->begin());
#endif
                std::copy_n(raster.capture_polygon_touches, count, expected_touches.begin());
            } else {
                ok &= check(std::equal(expected_causes->begin(), expected_causes->end(), raster.capture_causes) &&
#ifdef DSR_GPU_CAPTURE_IDS
                            std::equal(expected_ids->begin(), expected_ids->end(), raster.capture_polygon_ids) &&
#endif
                            std::equal(expected_touches.begin(), expected_touches.begin() + count,
                                       raster.capture_polygon_touches),
                            "capture causes and per-polygon reductions are band-independent");
            }
            std::size_t excluded = 0;
            for (int y = 0; y < 192; ++y) {
                for (int x = 0; x < 256; ++x) {
                    const auto actual = raster.opaque_only_mask[y * 256 + x];
#ifdef DSR_GPU_CAPTURE_IDS
                    ok &= check(raster.capture_polygon_ids[y * 256 + x] ==
                                (dsr::gpu3d_detail::Gpu3DTestAccess::framebuffer_pixel(gpu, y * 256 + x)[2] & 63U),
                                "captured polygon IDs belong to the same final native framebuffer");
#endif
                    excluded += actual == 0;
                    if (scene == 8) continue;
                    const bool footprint = (scene == 7 || scene == 9) &&
                                           x >= 95 && x <= 160 && y >= 71 && y <= 120;
                    if (actual == static_cast<std::uint8_t>(!footprint)) continue;
                    std::cerr << "FAIL: opaque mask scene=" << scene << " threads=" << threads
                              << " x=" << x << " y=" << y << '\n';
                    std::exit(1);
                }
            }
            if (scene == 8)
                ok &= check(excluded > 0 && excluded < 3300 &&
                            raster.opaque_only_mask[90U * 256U + 112U] == 1,
                            "wireframe excludes touched edges, not its interior");
            gpu.set_capture_opaque_only(false);
            ok &= check(gpu.raster_view().opaque_only_mask == nullptr &&
                        gpu.raster_view().capture_polygon_ids == nullptr, "disabling invalidates capture buffers");
            gpu.set_capture_opaque_only(true);
            gpu.reset();
            ok &= check(gpu.raster_view().opaque_only_mask == nullptr &&
                        gpu.raster_view().capture_polygon_ids == nullptr, "reset invalidates capture buffers");
            gpu.render_start();
            ok &= check(std::all_of(gpu.raster_view().opaque_only_mask,
                                   gpu.raster_view().opaque_only_mask + pixels,
                                   [](auto value) { return value == 1; }),
                        "clear initializes every captured pixel to one");
        }
    }
    if (!ok) std::exit(1);
    std::cout << "PASS: opaque-only capture 10 scenes x 3 thread modes x 49152 pixels\n";
}

} // namespace

int main() {
    test_avx2_dispatch_requirements();
    test_clipped_positions_match_reference_arithmetic();
    test_capture_fragment_random_reference();
    test_single_layer_capture_policy();
    test_opaque_only_capture();
    test_capture_depth_rejected_history();
    test_capture_opaque_depth_provenance();
    test_band_parallel_identity();
    test_color_rounding_matches_lround();
    test_prepared_texture_and_polygon_cache_reference();
    test_decoded_sampling_matches_reference();
    test_decoded_cache_renderer_lifetime();
    test_inline_uses_one_full_frame_cache();
    test_specialized_spans_match_reference();
    test_batch_spans_match_original_fragments();
    test_uniform_clear_matches_reference();
    test_uniform_clear_targets_match_reference();
    test_pack_pixels_matches_scalar();
    test_pack_targets_match_scalar();
    test_shade_pixel_matches_reference();
    test_geometry_reference_command_semantics();
    test_inclusive_viewport_coverage();
    test_matrix_stack();
    test_matrix_mode_semantics();
    test_geometry_test_results();
    test_geometry_status_ack_and_matrix_readback();
    test_matrix_result_register_layout();
    test_box_test_preserves_fractional_clip_coordinates();
    test_swap_waits_for_vblank_and_blocks_following_commands();
    test_packed_fifo();
    test_queued_test_and_stack_busy_flags();
    test_mixed_packed_and_direct_busy_retirement();
    test_busy_retirement_across_swap_clear_and_stack_errors();
    test_full_direct_staging_busy_retirement();
    test_gxfifo_mirror_write_widths();
    test_gxfifo_stmia_burst();
    test_box_test_and_near_clipping();
    test_vtx16_q4_12_coordinates();
    test_triangle_raster_and_ram_counts();
    test_hires_full_viewport_quad();
    test_hires_clipped_offset_viewport();
    test_hires_clear_bitmap_sampling();
    test_hires_triangle_coverage();
    test_hires_native_identity_and_capture();
    test_hires_scale_clamp();
    test_hires_scale_change_invalidates_rows();
    test_hires_shared_decoded_texture_state();
    test_hires_band_parallel_identity();
    test_polygon_surface_enable_bits();
    test_texture_formats();
    test_texture_addressing_and_transparency();
    test_negative_texture_coordinate_wrap();
    test_textured_renderer_injection();
    test_depth_test();
    test_depth_rejection_skips_texture_sampling();
    test_depth_equal_requires_matching_depth();
    test_fog_density_interpolation();
    test_fog_descending_density_uses_weighted_truncation();
    test_one_dot_depth_boundary();
    test_clipped_slivers_do_not_become_one_dot_polygons();
    test_shadow_requires_depth_mask_and_different_polygon_id();
    test_shadow_preserves_vertex_color();
    test_translucent_id_rejection_ignores_blend_enable_and_depth_write();
    test_translucent_blending();
    test_edge_marking_uses_neighbor_color_and_antialias_alpha();
    test_no_swap_renders_retained_geometry_and_clear_plane();
    test_specular_lighting_uses_reference_half_vector_normalization();
    test_light_half_vector_matches_reference_normalization();
    test_matrix_store_restore_and_gxstat_stack_bits();
    test_matrix_busy_bit_only_covers_push_pop();
    test_texture_matrix_store_restore_and_coordinate_modes();
    test_vertex_color_interpolates_in_six_bit_framebuffer_space();
    test_vertex_packed_coordinate_commands();
    test_toon_alpha_test_and_wireframe();
    test_clear_image_bitmap_scroll_depth_and_fog();
    test_clear_image_unmapped_slots_are_independently_zero();
    test_polygon_vertex_ram_overflow_flags();
    test_enhanced_geometry_preserves_hardware_status();
    test_disabling_enhanced_geometry_preserves_oversized_lists();
    test_enhanced_geometry_capacity_overflow_lifecycle();
    test_enhanced_geometry_keeps_guest_raster_hardware_limited();
    test_widescreen_projection_fov_equality();
    test_swap_sort_and_z_w_depth_modes();
    test_opaque_polygons_draw_in_ascending_screen_y_order();
    test_front_facing_passes_equal_depth_over_back_facing();
    test_material_diffuse_sets_vertex_color();
}
