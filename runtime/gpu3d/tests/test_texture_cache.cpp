#include "texture.h"
#include "band_pool.h"
#include "gpu3d_test.h"
#include "../../gpu2d/src/gpu2d_test.h"
#include "dsr/gpu2d.h"
#include "dsr/machine.h"
#include "dsr/mem.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

namespace {
using namespace dsr::gpu3d_detail;

bool check(bool condition, std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
bool same(Texel a, Texel b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool test_integer_texture_floor() {
    const auto matches = [](float value) {
        return floor_texcoord(value) == static_cast<int>(std::floor(value));
    };
    bool ok = true;
    // Both adjacent floats at every texel boundary include signed zero and
    // the subnormals around it, exposing negative-fraction truncation errors.
    for (int integer = -65536; integer <= 65536; ++integer) {
        const float value = static_cast<float>(integer);
        ok &= matches(value);
        ok &= matches(std::nextafter(value, -std::numeric_limits<float>::infinity()));
        ok &= matches(std::nextafter(value, std::numeric_limits<float>::infinity()));
    }
    ok &= matches(-0.0F);
    ok &= matches(static_cast<float>(std::numeric_limits<int>::min()));
    ok &= matches(std::nextafter(static_cast<float>(std::numeric_limits<int>::max()), 0.0F));
    std::mt19937 random(0xF10012U);
    for (unsigned i = 0; i < 200000; ++i) {
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        if (!std::isfinite(value)) continue;
        const double floored = std::floor(static_cast<double>(value));
        if (floored < std::numeric_limits<int>::min() ||
            floored > std::numeric_limits<int>::max()) continue;
        ok &= matches(value);
    }
    return check(ok, "integer texture floor matches scalar floor at boundaries and random floats");
}

struct Storage {
    std::vector<std::uint8_t> textures = std::vector<std::uint8_t>(0x80000U);
    std::vector<std::uint8_t> palettes = std::vector<std::uint8_t>(0x18000U);
    unsigned texture_mask = 15U;
    unsigned palette_mask = 63U;
    unsigned texture_overlap = 0U;
    unsigned palette_overlap = 0U;
    mutable std::size_t lookups = 0;

    static const std::uint8_t* texture_lookup(const void* context, int slot) {
        const auto& self = *static_cast<const Storage*>(context);
        ++self.lookups;
        return (self.texture_mask & (1U << slot)) != 0U &&
               (self.texture_overlap & (1U << slot)) == 0U
            ? self.textures.data() + static_cast<std::size_t>(slot) * 0x20000U : nullptr;
    }
    static const std::uint8_t* palette_lookup(const void* context, int slot) {
        const auto& self = *static_cast<const Storage*>(context);
        ++self.lookups;
        return (self.palette_mask & (1U << slot)) != 0U &&
               (self.palette_overlap & (1U << slot)) == 0U
            ? self.palettes.data() + static_cast<std::size_t>(slot) * 0x4000U : nullptr;
    }
    static std::uint8_t texture_read(const void* context, std::uint32_t address) {
        const auto& self = *static_cast<const Storage*>(context);
        const auto slot = (address >> 17U) & 3U;
        if ((self.texture_mask & (1U << slot)) == 0U) return 0;
        const auto offset = address & 0x1ffffU;
        const auto value = self.textures[slot * 0x20000U + offset];
        return (self.texture_overlap & (1U << slot)) != 0U
            ? static_cast<std::uint8_t>(value | self.textures[((slot + 1U) & 3U) * 0x20000U + offset])
            : value;
    }
    static std::uint8_t palette_read(const void* context, std::uint32_t address) {
        const auto& self = *static_cast<const Storage*>(context);
        auto slot = (address >> 14U) & 7U;
        if (slot > 5U) slot -= 5U;
        if ((self.palette_mask & (1U << slot)) == 0U) return 0;
        const auto offset = address & 0x3fffU;
        const auto value = self.palettes[slot * 0x4000U + offset];
        return (self.palette_overlap & (1U << slot)) != 0U
            ? static_cast<std::uint8_t>(value | self.palettes[((slot + 1U) % 6U) * 0x4000U + offset])
            : value;
    }
    TextureSource view(bool fallback_only = false) const {
        return {this, fallback_only ? nullptr : &texture_lookup,
                fallback_only ? nullptr : &palette_lookup, &texture_read, &palette_read};
    }
};

// The original whole-image loop: decode each texel independently using the
// unchanged scalar decoder. Compare named components, not struct padding.
static bool reference_image(const TextureSource& source, std::uint32_t param,
                            std::uint32_t palette, const Texel* actual) {
    const int width = 8 << ((param >> 20U) & 7U);
    const int height = 8 << ((param >> 23U) & 7U);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (!same(actual[y * width + x], decode_texel(source, param, palette, x, y)))
                return false;
    return true;
}

bool test_bulk_reference() {
    Storage storage;
    std::mt19937 random(0x83ba9201U);
    for (auto& byte : storage.textures) byte = static_cast<std::uint8_t>(random());
    for (auto& byte : storage.palettes) byte = static_cast<std::uint8_t>(random());
    std::vector<Texel> actual;
    std::size_t compared = 0;
    for (unsigned format = 0; format <= 7; ++format) {
        for (unsigned iteration = 0; iteration < 16U; ++iteration) {
            const unsigned size_s = iteration == 0 ? 7U : iteration == 1 ? 0U : random() % 8U;
            const unsigned size_t = iteration == 0 ? 7U : iteration == 1 ? 0U : random() % 8U;
            const auto address = iteration < 4U ? 0xffffU - iteration : random() & 0xffffU;
            const auto param = (format << 26U) | (size_s << 20U) | (size_t << 23U) |
                address | (random() & 0xe00f0000U);
            const auto palette = iteration < 4U ? 0x1fffU - iteration : random();
            storage.texture_mask = iteration < 4U ? 15U : random() & 15U;
            storage.palette_mask = iteration < 4U ? 63U : random() & 63U;
            storage.texture_overlap = iteration < 4U ? 0U : random() & 15U;
            storage.palette_overlap = iteration < 4U ? 0U : random() & 63U;
            actual.resize(static_cast<std::size_t>(8U << size_s) * (8U << size_t));
            const auto source = storage.view((iteration & 1U) != 0U);
            decode_texture(source, param, palette, actual.data());
            if (!check(reference_image(source, param, palette, actual.data()),
                       "bulk decoder matches scalar for all formats, sizes, aliases and overlapping slots"))
                return false;
            compared += actual.size();
        }
    }
    std::cout << "Bulk/scalar texels compared: " << compared << '\n';
    return true;
}

bool test_cache_reuse_invalidation_and_lru() {
    Storage storage;
    const auto source = storage.view();
    constexpr auto a = 7U << 26U;
    constexpr auto b = a | 1U;
    constexpr auto c = a | 2U;
    storage.textures[0] = 0x1fU;
    storage.textures[1] = 0x80U;
    DecodedTextureCache cache(512U, 2U);
    cache.begin_frame(10U);
    const auto* first = cache.acquire(source, a, 0U);
    const auto reads = storage.lookups;
    bool ok = check(first && first[0].r == 63U && first[0].a == 31U, "cache decodes first image");
    ok &= check(cache.acquire(source, a, 0U) == first && storage.lookups == reads,
                "polygons sharing a key do not fetch VRAM again");
    cache.begin_frame(10U);
    ok &= check(cache.acquire(source, a, 0U) == first && storage.lookups == reads,
                "unchanged generations reuse the decoded block across frames");
    const auto* second = cache.acquire(source, b, 0U);
    ok &= check(second && cache.acquire(source, c, 0U) == nullptr &&
                cache.memory_bytes() == 512U && cache.entry_count() == 2U,
                "both payload and entry bounds hold; pinned images cannot be evicted");
    cache.begin_frame(10U);
    cache.acquire(source, a, 0U); // A is MRU and pinned; B is the LRU victim.
    ok &= check(cache.acquire(source, c, 0U) && cache.find(b, 0U) == nullptr &&
                cache.find(a, 0U) == first, "LRU evicts the least recently used unpinned image");
    storage.textures[0] = 0U;
    storage.textures[1] = 0xfcU;
    cache.begin_frame(11U);
    ok &= check(cache.find(a, 0U) == nullptr && cache.find(c, 0U) == nullptr,
                "generation change invalidates every texture immediately without decoding");
    const auto* changed = cache.acquire(source, a, 0U);
    ok &= check(changed == first && changed[0].b == 63U && changed[0].r == 0U &&
                storage.lookups > reads, "new generation redecodes into the same allocation");
    ok &= check(cache.find(a, 1U) == nullptr && cache.acquire(source, a, 1U) &&
                cache.entry_count() == 2U, "palette base participates in identity even for direct color");
    cache.begin_frame(11U);
    ok &= check(cache.acquire(source, a | (7U << 20U) | (7U << 23U), 0U) == nullptr &&
                cache.memory_bytes() == 512U, "oversized textures leave the bounded cache unchanged");
    cache.clear();
    ok &= check(cache.memory_bytes() == 0U && cache.entry_count() == 0U && cache.find(a, 0U) == nullptr,
                "reset/source replacement releases all storage and identities");
    DecodedTextureCache entry_bounded(4096U, 1U);
    entry_bounded.begin_frame(0U);
    ok &= check(entry_bounded.acquire(source, a, 0U) && !entry_bounded.acquire(source, b, 0U),
                "metadata entries are bounded separately from payload bytes");
    entry_bounded.begin_frame(0U);
    ok &= check(entry_bounded.acquire(source, b, 0U) && !entry_bounded.find(a, 0U) &&
                entry_bounded.entry_count() == 1U, "entry-bound eviction is LRU between snapshots");
    return ok;
}

bool test_shared_immutable_snapshot() {
    Storage storage;
    std::mt19937 random(0xa3760U);
    for (auto& byte : storage.textures) byte = static_cast<std::uint8_t>(random());
    for (auto& byte : storage.palettes) byte = static_cast<std::uint8_t>(random());
    DecodedTextureCache cache;
    const auto source = storage.view();
    const auto param = (5U << 26U) | (3U << 20U) | (2U << 23U);
    cache.begin_frame(4U);
    const auto* image = cache.acquire(source, param, 123U);
    if (!check(image && reference_image(source, param, 123U, image), "compressed snapshot matches scalar"))
        return false;
    std::array<std::uint64_t, 8> sums{};
    dsr::BandPool pool(6);
    const auto lookups = storage.lookups;
    pool.run(8, [&](int band) {
        for (unsigned repeat = 0; repeat < 100U; ++repeat) {
            const auto* shared = cache.find(param, 123U);
            for (unsigned i = static_cast<unsigned>(band); i < 64U * 32U; i += 8U)
                sums[band] += shared[i].r + shared[i].g + shared[i].b + shared[i].a;
        }
    });
    bool ok = check(storage.lookups == lookups && cache.find(param, 123U) == image,
                    "band workers never fetch VRAM or mutate decoded storage");
    for (unsigned band = 0; band < 8U; ++band) {
        std::uint64_t expected = 0;
        for (unsigned i = band; i < 64U * 32U; i += 8U) {
            const auto texel = decode_texel(source, param, 123U, i % 64U, i / 64U);
            expected += texel.r + texel.g + texel.b + texel.a;
        }
        ok &= check(sums[band] == expected * 100U, "all bands sample the same complete immutable image");
    }
    return ok;
}
Texel sample_machine(dsr::Machine& machine, std::uint32_t param, std::uint32_t palette = 0) {
    dsr::Gpu3DPolygon polygon{};
    polygon.texture = param;
    polygon.palette = palette;
    const std::array<std::array<float, 2>, 64> coordinates{};
    return Gpu3DTestAccess::sample_texels(machine.gpu3d(), polygon, coordinates)[0];
}

bool test_live_guest_write_paths() {
    using dsr::CpuId;
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu2d();
    auto& cpu9 = machine.cpu(CpuId::Arm9);
    auto& cpu7 = machine.cpu(CpuId::Arm7);
    machine.gpu3d().io_write(0x04000060U, 1U, 32);
    constexpr std::array<std::uint32_t, 7> controls{
        0x04000240U, 0x04000241U, 0x04000242U, 0x04000243U,
        0x04000244U, 0x04000245U, 0x04000246U};
    constexpr std::array<std::uint32_t, 7> bases{
        0x06800000U, 0x06820000U, 0x06840000U, 0x06860000U,
        0x06880000U, 0x06890000U, 0x06894000U};
    const auto map = [&](unsigned bank, unsigned mode) {
        machine.write8(CpuId::Arm9, controls[bank], static_cast<std::uint8_t>(0x80U | mode));
    };
    const auto matches = [&](std::uint32_t param, Texel expected) {
        return same(sample_machine(machine, param), expected);
    };
    constexpr auto indexed = 4U << 26U;
    constexpr auto direct = 7U << 26U;
    constexpr Texel red{63, 0, 0, 31}, green{0, 63, 0, 31}, blue{0, 0, 63, 31};
    map(0, 0);
    dsr::wr16(&cpu9, bases[0], 0x0101U); // Recompiler direct-page upload.
    map(0, 3);
    bool ok = true;
    for (unsigned bank : {4U, 5U, 6U}) {
        map(bank, 0);
        dsr::wr32(&cpu9, bases[bank], 0x001f0000U);
        map(bank, 3);
        ok &= check(matches(indexed, red), "E/F/G palette CPU word upload invalidates decoded texels");
        map(bank, 0);
        dsr::wr16(&cpu9, bases[bank] + 2U, 0x03e0U);
        map(bank, 3);
        ok &= check(matches(indexed, green), "mid-frame palette halfword upload is visible on retained sampling");
        const auto generation = gpu.vram_generation();
        dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(gpu, bank, 2U, 0U);
        dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(gpu, bank, 3U, 0x7cU);
        ok &= check(gpu.vram_generation() == generation + 2U && matches(indexed, blue),
                    "every physical texture-palette byte write invalidates without a remap");
        machine.write8(CpuId::Arm9, controls[bank], 0U);
    }
    machine.write8(CpuId::Arm9, controls[0], 0U);
    map(2, 2); // C/D may be ARM7-owned OR image VRAM, never both.
    dsr::wr8(&cpu7, 0x06000000U, 0x1fU);
    dsr::wr8(&cpu7, 0x06000001U, 0x80U);
    map(2, 3);
    ok &= check(matches(direct, red), "ARM7 byte upload invalidates when C becomes texture VRAM");
    map(2, 2);
    dsr::wr16(&cpu7, 0x06000000U, 0x83e0U);
    map(2, 3);
    ok &= check(matches(direct, green), "ARM7 direct-page halfword uploads cannot retain stale textures");
    map(2, 2);
    machine.write16(CpuId::Arm7, 0x02000000U, 0xfc00U);
    machine.write32(CpuId::Arm7, 0x040000b0U, 0x02000000U);
    machine.write32(CpuId::Arm7, 0x040000b4U, 0x06000000U);
    machine.write32(CpuId::Arm7, 0x040000b8U, 0x80000001U);
    map(2, 3);
    ok &= check(matches(direct, blue), "ARM7 immediate DMA upload invalidates on the texture remap");
    const auto blocked_generation = gpu.vram_generation();
    dsr::wr32(&cpu9, bases[2], 0x83e083e0U);
    dsr::wr16(&cpu7, 0x06000000U, 0x801fU);
    ok &= check(gpu.vram_generation() == blocked_generation && matches(direct, blue),
                "CPU writes to texture-mapped banks are blocked and cannot bypass invalidation");
    machine.write8(CpuId::Arm9, controls[2], 0U);
    map(0, 0);
    machine.write32(CpuId::Arm9, 0x02000000U, 0x83e083e0U);
    machine.write32(CpuId::Arm9, 0x040000d4U, 0x02000000U);
    machine.write32(CpuId::Arm9, 0x040000d8U, bases[0]);
    machine.write32(CpuId::Arm9, 0x040000dcU, 0x84000001U);
    dsr::wr8(&cpu9, bases[0], 0xffU); // ARM9's VRAM byte port ignores this.
    map(0, 3);
    ok &= check(matches(direct, green), "ARM9 direct-page DMA word uploads invalidate and byte writes stay ignored");
    const auto image_generation = gpu.vram_generation();
    dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(gpu, 0, 0U, 0x1fU);
    dsr::gpu2d_detail::Gpu2DTestAccess::write_bank_byte(gpu, 0, 1U, 0x80U);
    ok &= check(gpu.vram_generation() == image_generation + 2U && matches(direct, red),
                "every physical image-bank write invalidates already decoded data");
    // Display capture writes only LCDC banks. The remap out of and back into
    // texture mode invalidates even though capture itself cannot write an image bank.
    map(0, 0);
    gpu.io_write(0x04000304U, 0x820fU, 16);
    gpu.arm9_write(0x05000000U, 0x7c00U, 16);
    gpu.io_write(0x04000000U, 1U << 16U, 32);
    gpu.io_write(0x04000064U, 0x80300000U, 32);
    gpu.render_line(0);
    ok &= check(gpu.arm9_read(bases[0], 16) == 0xfc00U, "capture actually replaces LCDC image bytes");
    map(0, 3);
    ok &= check(matches(direct, blue), "display capture followed by a mid-frame remap invalidates decoded images");
    const auto capture_generation = gpu.vram_generation();
    gpu.render_line(1);
    ok &= check(gpu.vram_generation() == capture_generation && matches(direct, blue),
                "capture cannot mutate a texture-mapped destination between snapshots");
    return ok;
}

} // namespace

int main() {
    const bool ok = test_integer_texture_floor() & test_bulk_reference() &
                    test_cache_reuse_invalidation_and_lru() &
                    test_shared_immutable_snapshot() & test_live_guest_write_paths();
    if (!ok) return EXIT_FAILURE;
    std::cout << "All decoded texture cache tests passed\n";
    return EXIT_SUCCESS;
}
