// Behavioral snapshots from the pristine pre-refactor renderer, plus interface mutation checks.
// Hexadecimal constants preserve captured binary32 values.
#include "dsr/gpu3d.h"
#include "rasterizer.h"
#include "rasterizer_impl.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "rasterizer_simd.h"
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

using namespace dsr::gpu3d_detail;

namespace {
void expect(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        std::exit(1);
    }
}

bool equal(const RasterInputVertex& a, const RasterInputVertex& b) {
    return a.clip == b.clip && a.u == b.u && a.v == b.v && a.color == b.color;
}
std::uint32_t bits(float x) {
    std::uint32_t value;
    std::memcpy(&value, &x, sizeof value);
    return value;
}

void planes_and_box_edges() {
    RasterInputVertex point{};
    point.clip = {0.25F, -0.5F, 0.75F, 1.0F};
    constexpr std::array<std::uint32_t, 6> distances{{
        0x3fa00000, 0x3f400000, 0x3f000000, 0x3fc00000, 0x3fe00000, 0x3e800000}};
    for (int plane = 0; plane < 6; ++plane)
        expect(bits(raster_plane_distance(point, plane)) == distances[plane], "plane numbering");
    RasterInputVertex a{{0.125F, -0.25F, 0.375F, 1.3333F}, 0.1F, -4.75F, {3.1F, 31.7F, -2.2F}};
    RasterInputVertex b{{-0.45F, 0.875F, -4.75F, 3.14159F}, 7.3F, 1.125F, {29.2F, -8.25F, 15.75F}};
    const auto saved_a = a, saved_b = b;
    const auto da = raster_plane_distance(a, 4), db = raster_plane_distance(b, 4);
    const auto edge = interpolate_raster_clip(a, b, da / (da - db));
    const std::array<std::uint32_t, 9> expected{{
        0xbe2f4438, 0x3ea8ac7a, 0xc010f070, 0x4010f071, 0x4073bd2f,
        0xbfdcad0c, 0x41845820, 0x4131f97c, 0x40e1731e}};
    const std::array<float, 9> values{{edge.clip[0], edge.clip[1], edge.clip[2], edge.clip[3],
        edge.u, edge.v, edge.color[0], edge.color[1], edge.color[2]}};
    for (std::size_t i = 0; i < values.size(); ++i)
        expect(bits(values[i]) == expected[i], "BOX_TEST edge interpolation");
    expect(equal(a, saved_a) && equal(b, saved_b), "interpolation does not mutate inputs");
}


void clipping_contract() {
    std::array<RasterInputVertex, 16> input{}, output{};
    input[0].clip = {-2, 0, 0, 1};
    input[1].clip = {0, -0.5F, 0, 1};
    input[2].clip = {0, 0.5F, 0, 1};
    input[0].u = 2; input[1].u = 4; input[2].u = 6;
    const auto count = clip_raster_polygon(input, 3, 1U << 12, output);
    expect(count == 4, "clipping retained count");
    for (std::size_t i = 0; i < count; ++i)
        expect(equal(input[i], output[i]), "clipping writes both returned prefixes");
    const std::array<std::array<float, 4>, 4> expected{{
        {-1, 0.25F, 0, 1}, {-1, -0.25F, 0, 1},
        {0, -0.5F, 0, 1}, {0, 0.5F, 0, 1}}};
    const std::array<float, 4> u{{4, 3, 4, 6}};
    for (std::size_t i = 0; i < count; ++i) {
        expect(output[i].clip == expected[i], "clipped position/order");
        expect(output[i].u == u[i], "clipped attribute");
    }
    input[0].clip = {0, 0, 2, 1};
    const auto before_input = input;
    const auto before_output = output;
    expect(clip_raster_polygon(input, 3, 0, output) == 0, "far rejection count");
    for (std::size_t i = 0; i < input.size(); ++i) {
        expect(equal(input[i], before_input[i]), "far rejection input unchanged");
        expect(equal(output[i], before_output[i]), "far rejection output unchanged");
    }
    expect(clip_raster_polygon(input, 0, 1U << 12, output) == 0, "empty polygon count");
    input = {};
    input[0].clip = {-2, -2, 0, 1};
    input[1].clip = {2, 0, 2, 1};
    input[2].clip = {0, 2, 0, 1};
    const auto multi_count = clip_raster_polygon(input, 3, 1U << 12, output);
    constexpr std::array<std::array<float, 4>, 8> multi{{
        {-0.5F, 1, 0, 1}, {-1, 0, 0, 1}, {-1, -1, 0x1.555556p-2F, 1},
        {0, -1, 1, 1}, {0, -1, 1, 1}, {1, 1, 1, 1},
        {1, 1, 1, 1}, {1, 1, 1, 1}}};
    expect(multi_count == multi.size(), "multi-plane clipped count");
    for (std::size_t i = 0; i < multi_count; ++i) {
        expect(output[i].clip == multi[i], "multi-plane ordered clipping");
        expect(equal(input[i], output[i]), "multi-plane mutable input prefix");
    }
}

// Captured using the pristine renderer's clip_polygon with boundary vertices
// at x=-w and the outside vertex at x=-2w. The divisor is exactly w.
void clipping_divisor_threshold() {
    constexpr float divisors[] = {0x1.79ca0ep-67F, 0x1.79ca1p-67F, 0x1.79ca12p-67F};
    constexpr std::array<std::array<float, 5>, 2> attributes{{
        {{4, 13, 21, 31, 41}}, {{6, 16, 22, 32, 42}}}};
    constexpr std::array<std::array<int, 4>, 3> order{{
        {{0, 1, 0, 0}}, {{0, 1, 0, 0}}, {{1, 0, 0, 1}}}};
    for (std::size_t c = 0; c < 3; ++c) {
        const float d = divisors[c];
        std::array<RasterInputVertex, 16> input{}, output{};
        input[0] = {{-2 * d, 0, 0, d}, 2, 10, {20, 30, 40}};
        input[1] = {{-d, 0, 0, d}, 4, 13, {21, 31, 41}};
        input[2] = {{-d, 0, 0, d}, 6, 16, {22, 32, 42}};
        const auto count = clip_raster_polygon(input, 3, 1U << 12, output);
        expect(count == (c == 2 ? 4U : 2U), "clipping divisor threshold count");
        for (std::size_t i = 0; i < count; ++i) {
            const auto& v = output[i];
            const auto& a = attributes[order[c][i]];
            expect(v.clip == std::array<float, 4>{-d, 0, 0, d} &&
                   v.u == a[0] && v.v == a[1] &&
                   v.color == std::array<float, 3>{a[2], a[3], a[4]},
                   "clipping divisor threshold ordered attributes");
        }
    }
}

// The pristine span computes one delta*skip before adding to the start.
void left_clipped_span_prestep() {
    RasterBoundary left{}, right{};
    left.x = -2; right.x = 2;
    left.value.fill(1.0F);
    right.value.fill(0x1.000004p+0F);
    RasterSpan span{};
    expect(prepare_raster_span(left, right, 256, span), "left-clipped span exists");
    expect(span.x == 0 && span.width == 2, "left-clipped span geometry");
    for (std::size_t i = 0; i < span.value.size(); ++i) {
        expect(bits(span.delta[i]) == 0x33800000U, "left-clipped span delta");
        expect(bits(span.value[i]) == 0x3f800001U, "left-clipped span prestep");
    }
}

#if defined(__x86_64__) || defined(_M_X64)
void simd_four_sample_scalar_equivalence() {
    constexpr std::array<float, 4> initial{{1.0F, -1.0F, 0.5F, 3.0F}};
    constexpr std::array<float, 4> step{{0x1p-24F, -0x1p-24F, 0x1.8p-24F, -0x1.8p-22F}};
    for (bool more : {false, true}) {
        __m128 current = _mm_loadu_ps(initial.data());
        __m128 a, b, c, d;
        raster_four_samples(current, _mm_loadu_ps(step.data()), more, a, b, c, d);
        std::array<std::array<float, 4>, 4> samples{};
        _mm_storeu_ps(samples[0].data(), a);
        _mm_storeu_ps(samples[1].data(), b);
        _mm_storeu_ps(samples[2].data(), c);
        _mm_storeu_ps(samples[3].data(), d);
        std::array<float, 4> final{};
        _mm_storeu_ps(final.data(), current);
        for (std::size_t lane = 0; lane < initial.size(); ++lane) {
            float scalar = initial[lane];
            for (const auto& sample : samples) {
                expect(bits(sample[lane]) == bits(scalar), "four-sample sequential scalar equivalence");
                scalar += step[lane];
            }
            expect(bits(final[lane]) == bits(more ? scalar : initial[lane]),
                   "four-sample current update");
        }
    }
}
#endif

void projection_contract() {
    constexpr std::array<float, 3> color{{0.25F, 0.5F, 1}};
    for (float w : {0.0F, 1e-21F, -1e-21F}) {
        auto s = project_raster_screen({0, 0, 0, w}, 2, -3, color, 0xbfff0000U, 1);
        expect(s.x == 0 && s.y == 0 && s.z == 0 && s.inv_w == 0 &&
               s.u_over_w == 0 && s.v_over_w == 0 && s.color_over_w == std::array<float, 3>{},
               "screen near-zero W yields zeros");
    }
    for (float w : {1e-20F, -1e-20F}) {
        auto s = project_raster_screen({0, 0, 0, w}, 2, -3, color, 0xbfff0000U, 1);
        expect(s.x == 128 && s.y == 96 && s.inv_w == 1.0F / w,
               "screen threshold retains signed W");
    }
    struct ViewCase { std::array<float, 4> p; std::uint32_t viewport; int scale; float x, y; int fx, fy; };
    constexpr ViewCase cases[] = {
        {{0.125F,-0.25F,0.5F,1}, 0xbfff0000U, 1, 0x1.2p+7F, 0x1.ep+6F, 2304, 1920},
        {{0.125F,-0.25F,0.5F,1}, 0x0305faf0U, 1, 0x1.f8cp+7F, 0x1.828p+7F, 4038, 3092},
        {{0.125F,-0.25F,0.5F,1}, 0xbf7fbf00U, 1, 0x1.2p+6F, 0x1.4p-1F, 1152, 10},
        {{0.125F,-0.25F,0.5F,1}, 0xc07fc000U, 1, 0x1.2p+6F, 0x1.fd4p+7F, 1152, 4074},
        {{-0.999F,0.999F,0.1F,1}, 0xbfff0000U, 3, 0x1.8936p-2F, 0x1.26e4p-2F, 6, 4},
        {{-1.001F,1.001F,0.1F,1}, 0xbfff0000U, 1, -0x1.0628p-3F, -0x1.893p-4F, -2, -1},
    };
    for (const auto& c : cases) {
        auto s = project_raster_screen(c.p, 2, -3, color, c.viewport, c.scale);
        expect(s.x == c.x && s.y == c.y, "screen viewport packing and scale");
        std::array<RasterInputVertex, 3> in{};
        for (auto& v : in) v = {c.p, 2, -3, color};
        std::array<RasterVertex, 10> out{};
        bool front = false;
        expect(prepare_raster_polygon(in.data(), in.size(), c.viewport, 0xc0U, c.scale, out, front),
               "prepare viewport fixture");
        expect(out[0].x == c.fx && out[0].y == c.fy, "fixed projection quantization");
    }
    for (float w : {0.0F, 1e-21F, -1e-21F}) {
        std::array<RasterInputVertex, 3> in{};
        for (auto& v : in) v = {{0, 0, 0, w}, 2, -3, color};
        std::array<RasterVertex, 10> out{};
        bool front = false;
        expect(prepare_raster_polygon(in.data(), in.size(), 0xbfff0000U, 0xc0U, 1, out, front),
               "prepare near-zero W fixture");
        expect(out[0].x == 2048 && out[0].y == 1536 &&
               out[0].attribute[0] == (w == 0 ? 0x1.7d784p+26F : 1.0F / w) &&
               out[0].attribute[1] == 0.5F, "prepare zero substitution and signed tiny W");
    }
}

void wire_dot_contract() {
    const std::array<float, 3> color{{1, 1, 1}};
    const auto a = project_raster_screen({-0.03125F, -0.03125F, -0.5F, 0.75F},
                                         0, 0, color, 0xbfff0000U, 1);
    const auto b = project_raster_screen({0.03125F, -0.03125F, 0.5F, 1.25F},
                                         0, 0, color, 0xbfff0000U, 1);
    const auto c = project_raster_screen({0, 0.03125F, 0, 1}, 0, 0, color, 0xbfff0000U, 1);
    for (bool w_depth : {false, true}) {
        std::vector<RasterFragment> fragments;
        traverse_wire_triangle(a, b, c, 0x010000c0U, w_depth, 256, 0, 192,
                               [&](const RasterFragment& f) { fragments.push_back(f); });
        expect(fragments.size() == 2, "wire coverage count");
        for (std::size_t i = 0; i < fragments.size(); ++i) {
            expect(fragments[i].x == 130 && fragments[i].y == 97 + static_cast<int>(i),
                   "wire coverage order");
            expect(fragments[i].depth == (w_depth ? 4854U : 11010048U), "wire depth");
        }
        const RasterInputVertex vertex{{0.00390625F, 0.00390625F, 0.25F, 1.125F},
                                       2, -3, color};
        const auto screen = project_raster_screen(vertex.clip, vertex.u, vertex.v,
                                                   vertex.color, 0xbfff0000U, 1);
        RasterFragment dot{};
        expect(prepare_raster_dot(screen, vertex, w_depth, 256, 192, 0, 192, dot),
               "dot coverage");
        expect(dot.x == 128 && dot.y == 95 && dot.depth == (w_depth ? 4608U : 10252740U),
               "dot coordinates and depth");
        expect(dot.u == 2 && dot.v == -3 && dot.color == color, "dot unweighted attributes");
        expect(!prepare_raster_dot(screen, vertex, w_depth, 256, 192, 96, 192, dot),
               "dot row band exclusion");
    }
}

void send(dsr::Gpu3D& gpu, std::uint8_t code, std::initializer_list<std::uint32_t> words) {
    const auto address = 0x04000440U + (static_cast<std::uint32_t>(code) - 0x10U) * 4U;
    for (auto word : words) gpu.io_write(address, word, 32);
}

void box_test_contract() {
    dsr::Gpu3D gpu;
    send(gpu, 0x70, {0xf000f000U, 8192U << 16, 8192U | (8192U << 16)});
    gpu.run(1000);
    expect((gpu.io_read(0x04000600U, 32) & 2U) != 0, "BOX_TEST crossing frustum");
    send(gpu, 0x70, {8192U, 0U, 0U});
    gpu.run(1000);
    expect((gpu.io_read(0x04000600U, 32) & 2U) == 0, "BOX_TEST outside frustum");
}
}

int main() {
    planes_and_box_edges();
    clipping_contract();
    clipping_divisor_threshold();
    left_clipped_span_prestep();
#if defined(__x86_64__) || defined(_M_X64)
    simd_four_sample_scalar_equivalence();
#endif
    projection_contract();
    wire_dot_contract();
    box_test_contract();
}
