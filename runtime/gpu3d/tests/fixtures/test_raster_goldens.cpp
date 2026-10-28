// Project-compatibility snapshots from the pre-refactor renderer, not hardware claims.
// Regenerate only from an isolated, explicitly identified baseline renderer.
#include "dsr/gpu3d.h"
#include "gpu3d_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
using dsr::Gpu3D;
using Access = dsr::gpu3d_detail::Gpu3DTestAccess;
struct Vertex { int x, y, z; };
void command(Gpu3D& gpu, unsigned code, std::initializer_list<std::uint32_t> words = {}) {
    const auto addr = 0x04000440U + (code - 0x10U) * 4U;
    if (words.size() == 0) gpu.io_write(addr, 0, 32);
    for (auto word : words) gpu.io_write(addr, word, 32);
}
void vertex(Gpu3D& gpu, Vertex v, unsigned i) {
    constexpr std::array<unsigned, 3> colors{0x001f, 0x03e0, 0x7c00};
    constexpr std::array<std::uint32_t, 3> uv{0xffb0ff90U, 0x007300d1U, 0x009dffd7U};
    command(gpu, 0x20, {colors[i % 3]});
    command(gpu, 0x22, {uv[i % 3]});
    command(gpu, 0x23, {static_cast<std::uint16_t>(v.x) | (std::uint32_t(static_cast<std::uint16_t>(v.y)) << 16), static_cast<std::uint16_t>(v.z)});
}
struct Texture {
    std::array<std::uint8_t, 0x20000> data{};
    Texture() {
        for (unsigned i = 0; i < 64; ++i) {
            const auto c = 0x8000U | ((i * 3U) & 31U) | (((i * 7U) & 31U) << 5U) | (((i * 11U) & 31U) << 10U);
            data[2*i] = static_cast<std::uint8_t>(c); data[2*i+1] = static_cast<std::uint8_t>(c >> 8);
        }
    }
    static const std::uint8_t* lookup(const void* ctx, int slot) {
        return slot == 0 ? static_cast<const Texture*>(ctx)->data.data() : nullptr;
    }
    static const std::uint8_t* palette(const void*, int) { return nullptr; }
};
// Input IDs: 0-15 signed fractional slopes; 16-27 six clip planes in
// both vertex orders; 28-31 horizontal extrema/integer and half pixels;
// 32-35 coincident vertices/lines; 36-39 one-pixel/subpixel triangles;
// 40-51 paired shared edges with reversed/cyclic orders; 52-59 viewports;
// 60-67 W=0 and sign/offset projections; 68-75 color/depth/UV gradients;
// 76-79 wire triangles; 80-83 one-dot polygons; 84-85 far-plane
// rejection; 86 retained homogeneous-origin W=0; 87-110 each member of
// shared-edge cases 40-51 alone; 111-118 varying-W depth (Z/W buffers);
// 119-126 wrapped/extreme viewport bytes; 127-130 subpixel quantization.
void scene(Gpu3D& gpu, Texture& texture, int fixture) {
    const bool single_shared = fixture >= 87 && fixture < 111;
    const int id = single_shared ? 40 + (fixture - 87) / 2 : fixture;
    const bool first_only = single_shared && ((fixture - 87) % 2 == 0);
    const bool second_only = single_shared && !first_only;
    Access::attach_texture_source(gpu, {&texture, &Texture::lookup, &Texture::palette});
    if (id >= 111 && id < 119) {
        // SWAP_BUFFERS selects the mode for the next geometry frame.
        command(gpu, 0x50, {(id & 1) ? 2U : 0U});
        gpu.run(100000); gpu.vblank_start(); gpu.render_start();
    }
    gpu.io_write(0x04000354U, 0x7fffU, 32);
    gpu.io_write(0x04000060U, 1U, 32);
    command(gpu, 0x2a, {(7U << 26) | (15U << 16)});
    if (id >= 52 && id < 60) {
        const unsigned left = (id - 52) * 7U, bottom = (id - 52) * 3U;
        command(gpu, 0x60, {(170U << 24) | (236U << 16) | (bottom << 8) | left});
    }
    if (id >= 119 && id < 127) {
        constexpr std::array<std::uint32_t, 8> viewports{
            0xff00ff00U, 0x00ff00ffU, 0xffffffffU, 0xc8ffc800U,
            0x00c00000U, 0xff01fffeU, 0xbf00bf00U, 0x000000ffU};
        command(gpu, 0x60, {viewports[id - 119]});
    }
    if ((id >= 60 && id < 68) || id == 86) {
        // Projection w = z * factor + constant; includes w=0 and sign crossings.
        const int factor = (id == 86 || (id & 1)) ? 4096 : -4096;
        const int constant = (id == 86 || id % 4 < 2) ? 0 : 2048;
        command(gpu, 0x10, {0});
        command(gpu, 0x16, {4096,0,0,0, 0,4096,0,0, 0,0,4096,static_cast<std::uint32_t>(factor), 0,0,0,static_cast<std::uint32_t>(constant)});
    }
    if (id >= 111 && id < 119) {
        // Positive, varying homogeneous W, with independent Z and W depths.
        command(gpu, 0x10, {0});
        command(gpu, 0x16, {4096,0,0,0, 0,4096,0,0,
            0,0,4096,2048, 0,0,0,4096});
    }
    std::array<Vertex, 3> v{{{-3000,-2800,-1000},{3000,-2300,1000},{11,3000,0}}};
    if (id < 16) {
        const int d = id % 8; const int sign = id < 8 ? 1 : -1;
        v = {{{-3500,-3000, -1300},{sign*(2700+d*37),-2600+d*19,1100},{sign*(17+d*31),3100-d*23,0}}};
    } else if (id < 28) {
        const int plane = (id - 16) / 2, direction = (id - 16) % 2;
        v = {{{-1800,-2000,-1000},{2400,-1500,1000},{0,2500,0}}};
        int* coord = plane / 2 == 0 ? &v[0].x : plane / 2 == 1 ? &v[0].y : &v[0].z;
        *coord = (plane % 2 ? 1 : -1) * 6000;
        if (direction) std::swap(v[0], v[1]);
    } else if (id < 40) {
        const int n = id - 28;
        const int x = (n % 4) * 16 + ((n & 1) ? 8 : 0);
        if (n < 4) v = {{{-3000,x,0},{3000,x,0},{x,3000,0}}};
        else if (n < 8) v = {{{x,x,0},{x,x,0},{x + (n-5)*16,x + 16,0}}};
        else v = {{{x,x,0},{x+((n&1)?1:16),x,0},{x,x+((n&2)?1:16),0}}};
    } else if (id < 52) {
        const int d = id - 40, shift = (d % 3) * 8;
        v = {{{-3000+shift,-3000,0},{3000+shift,-3000,500},{3000+shift,3000,-500}}};
    } else if (id >= 68) {
        const int d=id-68;
        v = {{{-4003+d,-4071,-1024+d*31},{4051,-4057+d,1024-d*29},{11+d,4083, d*17}}};
    }
    if (id >= 76 && id < 80) {
        const int delta = (id-76)*13;
        v = {{{-3600,-3200+delta,-1000},{3000,-2500-delta,1000},{delta,3200,0}}};
    } else if (id >= 80 && id < 84) {
        const int x=(id-80)*16;
        v = {{{x,x,0},{x,x,0},{x,x,0}}};
        gpu.io_write(0x04000610U, (id & 1) ? 8U : 4U, 32);
    } else if (id == 84 || id == 85) {
        v = {{{-1800,-2000,6000},{2400,-1500,1000},{0,2500,0}}};
        if (id == 85) std::swap(v[0], v[1]);
    } else if (id == 86) {
        v = {{{0,0,0},{2048,-2048,2048},{2048,2048,2048}}};
    }
    if (id >= 111 && id < 119) {
        const int d = (id - 111) / 2;
        v = {{{-3000+d*13,-2800,-2500},{3000,-2300+d*11,2200},{11,3000,500}}};
    } else if (id >= 127 && id < 131) {
        const int d = id - 127;
        v = {{{-16-d,-16,0},{16+d,-16,0},{-16,16+d,0}}};
    }
    std::array<Vertex,3> shared{};
    if (id >= 40 && id < 52) {
        shared = {{v[0], v[2], {-3000+(id-40)%3*8,3000,-500}}};
        if (id & 1) { std::swap(v[0],v[2]); std::swap(shared[0],shared[2]); }
        if (id & 2) {
            const auto rotate = [](std::array<Vertex,3>& a) {
                const auto t=a[0]; a[0]=a[1]; a[1]=a[2]; a[2]=t;
            };
            rotate(v); rotate(shared);
        }
    }
    const auto attr = id >= 76 && id < 80 ? 0U : (31U << 16);
    command(gpu, 0x29, {attr | (1U << 24) | 0xc0U |
        ((id >= 80 && id < 84) ? 1U << 13 : 0U) |
        ((id == 26 || id == 27) ? 1U << 12 : 0U)});
    if (!second_only) {
        command(gpu, 0x40, {0});
        for (unsigned i=0;i<3;++i)
            vertex(gpu,v[i], (id >= 16 && id < 28 && (id & 1) && i < 2) ? 1-i : i);
        command(gpu, 0x41);
    }
    if (id >= 40 && id < 52 && !first_only) {
        auto b = shared;
        command(gpu, 0x29, {(31U << 16) | (2U << 24) | 0xc0U});
        command(gpu, 0x40, {0});
        for (unsigned i=0;i<3;++i) vertex(gpu,b[i],i);
        command(gpu, 0x41);
    }
    command(gpu, 0x50, {(id >= 111 && id < 119 && (id & 1)) ? 2U : 0U});
    gpu.run(100000); gpu.vblank_start(); gpu.render_start();
    if ((id >= 60 && id < 68) || id == 86) {
        const auto view = gpu.geometry_view();
        if (view.vertex_count != 3) { std::cerr << "missing W fixture vertices\n"; std::exit(1); }
        const float factor = (id == 86 || (id & 1)) ? 1.0F : -1.0F;
        const float constant = (id == 86 || id % 4 < 2) ? 0.0F : 0.5F;
        for (unsigned i = 0; i < 3; ++i)
            if (view.vertices[i].clip[3] != factor * (static_cast<float>(v[i].z) / 4096.0F) + constant) {
                std::cerr << "unexpected W fixture transform, case " << id << '\n'; std::exit(1);
            }
    }
    if (id == 86) {
        const auto view = gpu.geometry_view();
        std::array<std::array<float,4>,3> positions{};
        for (unsigned i=0;i<3;++i) positions[i] = view.vertices[i].clip;
        Gpu3D clip_gpu;
        std::array<std::array<float,4>,16> clipped{};
        const auto count = Access::clip_positions(clip_gpu, positions, clipped);
        const std::array<float,4> origin{};
        if (count < 3 || std::find(clipped.begin(), clipped.begin()+count, origin) == clipped.begin()+count) {
            std::cerr << "homogeneous origin was not retained by clipping\n"; std::exit(1);
        }
    }
}
// Two independent 64-bit streams, canonical little-endian encoding of every
// field (never struct padding); both streams cover the entire output.
struct Hash {
    std::uint64_t a=14695981039346656037ULL, b=7809847782465536322ULL;
    void byte(std::uint8_t x) { a=(a^x)*1099511628211ULL; b=(b^x)*14029467366897019727ULL; }
    void word(std::uint32_t x) { for (int i=0;i<4;++i) byte(static_cast<std::uint8_t>(x>>(8*i))); }
};
Hash snapshot(const Gpu3D& gpu, int scale, bool capture) {
    Hash h; const auto view=gpu.raster_view(); const auto geometry=gpu.geometry_view();
    h.word(static_cast<std::uint32_t>(view.output_pixel_count));
    for (unsigned i=0;i<256U*192U;++i) {
        h.word(view.output[i]);
        const auto pixel = Access::framebuffer_pixel(gpu,i);
        for (auto x : pixel) h.word(x);
        if (capture) {
            h.byte(view.opaque_only_mask[i]); h.byte(view.capture_causes[i]);
            // ID capture is a build option; metadata contains the same ID.
            const auto id = static_cast<std::uint8_t>(pixel[2]);
            if (view.capture_polygon_ids && view.capture_polygon_ids[i] != id) {
                std::cerr << "captured polygon ID differs from framebuffer\n"; std::exit(1);
            }
            h.byte(id);
        }
    }
    h.word(static_cast<std::uint32_t>(geometry.polygon_count));
    if (capture) for (unsigned i=0;i<geometry.polygon_count;++i) h.word(view.capture_polygon_touches[i]);
    if (scale > 1) for (int y=0;y<192*scale;++y) for (int x=0;x<256*scale;++x) {
        h.word(gpu.scaled_line(y)[x]);
        for (auto value : Access::scaled_framebuffer_pixel(gpu, static_cast<unsigned>(y*256*scale+x))) h.word(value);
    }
    return h;
}
// Clip output count and ordered homogeneous coordinates, independent of pixels.
Hash clip_snapshot(int id) {
    std::array<std::array<float, 4>, 3> input{{
        {{-0.4F,-0.3F,-0.2F,1.0F}},
        {{0.6F,-0.2F,0.3F,1.0F}},
        {{0.1F,0.7F,0.1F,1.0F}}}};
    const int plane = id / 2;
    input[0][plane / 2] = (plane & 1) ? 1.5F : -1.5F;
    if (id & 1) std::swap(input[0], input[1]);
    Gpu3D gpu;
    std::array<std::array<float, 4>, 16> output{};
    const auto count = Access::clip_positions(gpu, input, output);
    const int axis = plane / 2;
    const float sign = (plane & 1) ? 1.0F : -1.0F;
    unsigned outside = 0, on_plane = 0;
    for (const auto& vertex : input) outside += sign * vertex[axis] > vertex[3];
    for (std::size_t i = 0; i < count; ++i) {
        const float distance = output[i][3] - sign * output[i][axis];
        if (distance < -0.00001F) {
            std::cerr << "clipped vertex outside plane, case " << id << '\n'; std::exit(1);
        }
        if (distance < 0.00001F) ++on_plane;
    }
    if (outside != 1 || count != 4 || on_plane != 2) {
        std::cerr << "clip fixture did not cross plane, case " << id << '\n'; std::exit(1);
    }
    Hash h; h.word(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) for (float value : output[i]) {
        std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); h.word(bits);
    }
    return h;
}
}
int main(int argc, char** argv) {
    const bool generate = argc == 3 && std::string(argv[1]) == "--generate";
    if (argc != (generate ? 3 : 2)) { std::cerr << "usage: test_raster_goldens [--generate] FILE\n"; return 2; }
    const char* path=argv[generate ? 2 : 1];
    std::ifstream expected; std::ofstream output;
    if (generate) output.open(path); else expected.open(path);
    if (generate ? !output : !expected) { std::cerr << "cannot open " << path << '\n'; return 2; }
    std::array<Hash, 16> w_depth_hashes{};
    for (int id=0;id<131;++id) for (int threads : {0,6}) for (int scale : {1,2,3,4})
    for (int capture : {0,1}) {
        Texture texture; Gpu3D gpu; gpu.set_render_threads(threads); gpu.set_output_scale(scale); gpu.set_capture_opaque_only(capture != 0);
        scene(gpu,texture,id); const auto h=snapshot(gpu,scale,capture != 0);
        if (id >= 111 && id < 119) {
            const auto geometry = gpu.geometry_view();
            if (geometry.vertex_count != 3 ||
                geometry.vertices[0].clip[3] == geometry.vertices[1].clip[3]) {
                std::cerr << "W-depth fixture lacks varying W\n"; return 1;
            }
            Hash depth;
            for (unsigned i=0; i<256U*192U; ++i)
                depth.word(Access::framebuffer_pixel(gpu, i)[1]);
            const int slot = (threads == 0 ? 0 : 8) + (scale - 1) * 2 + capture;
            if (id & 1) w_depth_hashes[slot] = depth;
            else if (w_depth_hashes[slot].a == depth.a && w_depth_hashes[slot].b == depth.b) {
                std::cerr << "Z/W depth snapshots unexpectedly identical case=" << id
                          << " threads=" << threads << " scale=" << scale
                          << " capture=" << capture << '\n'; return 1;
            }
        }
        if (generate) output << id << ' ' << threads << ' ' << scale << ' ' << capture << ' ' << std::hex << h.a << ' ' << h.b << std::dec << '\n';
        else {
            int ei,et,es,ec; std::uint64_t a,b;
            if (!(expected >> ei >> et >> es >> ec >> std::hex >> a >> b >> std::dec) || ei!=id || et!=threads || es!=scale || ec!=capture || a!=h.a || b!=h.b) {
                std::cerr << "golden mismatch case=" << id << " threads=" << threads << " scale=" << scale << " capture=" << capture << " actual=" << std::hex << h.a << ' ' << h.b << '\n'; return 1;
            }
        }
    }
    for (int id=0; id<12; ++id) {
        const auto h = clip_snapshot(id);
        if (generate) output << "clip " << id << ' ' << std::hex << h.a << ' ' << h.b << std::dec << '\n';
        else {
            std::string label; int ei; std::uint64_t a,b;
            if (!(expected >> label >> ei >> std::hex >> a >> b >> std::dec) ||
                label != "clip" || ei != id || a != h.a || b != h.b) {
                std::cerr << "clip golden mismatch case=" << id << '\n'; return 1;
            }
        }
    }
    std::string extra; if (!generate && expected >> extra) { std::cerr << "extra fixture records\n"; return 1; }
    if (generate && !output) return 2;
    std::cout << "131 scenes x 2 threading modes x 4 scales x 2 capture modes: full outputs\n";
}
