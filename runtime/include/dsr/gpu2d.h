// 2D graphics engines A/B, VRAM banks A-I and their mapping, palette, OAM,
// POWCNT1, display capture and master brightness. Software renderer.
//
// Owner: runtime/gpu2d. The core bus routes these ARM9 IO addresses here:
//   0x04000000-0x0400005F, 0x04000064-0x0400006F (engine A; 0x04000060 is
//   DISP3DCNT and belongs to Gpu3D), 0x04001000-0x0400106F (engine B),
//   0x04000240-0x04000246, 0x04000248-0x04000249 (VRAMCNT A-I; 0x04000247 is
//   WRAMCNT and belongs to the core), 0x04000304 (POWCNT1).
// DISPSTAT/VCOUNT and timing live in the core.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace dsr {

class Gpu3D;
namespace gpu2d_detail { struct Gpu2DTestAccess; }

struct Gpu2DEngineView {
    const std::uint32_t* registers = nullptr;
    std::size_t register_count = 0;
    std::int64_t affine_x[2]{};
    std::int64_t affine_y[2]{};
    int last_affine_line = -1;
};

struct Gpu2DPhysicalBankView {
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0;
};

struct Gpu2DTextureSlotView {
    bool mapped = false;
    std::uint8_t physical_bank = 0xff;
    std::uint32_t bank_offset = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0;
};

struct Gpu2DFrameView {
    Gpu2DEngineView engines[2]{};
    const std::uint8_t* vramcnt = nullptr;
    std::size_t vramcnt_count = 0;
    std::uint16_t powcnt1 = 0;
    bool capture_pending = false;
    bool capture_active = false;
    const std::uint8_t* palettes = nullptr;
    std::size_t palette_byte_count = 0;
    const std::uint8_t* oam = nullptr;
    std::size_t oam_byte_count = 0;
    Gpu2DPhysicalBankView banks[9]{};
    Gpu2DTextureSlotView texture_slots[4]{};
    Gpu2DTextureSlotView texture_palette_slots[6]{};
    std::uint64_t mapping_generation = 0;
};

// End-of-frame registers and memory state; no per-line replay history.
// Borrowed data is valid only on the owner thread until the next step/mutation.


class Gpu2D {
public:
    Gpu2D();
    ~Gpu2D();
    Gpu2D(const Gpu2D&) = delete;
    Gpu2D& operator=(const Gpu2D&) = delete;

    void reset();

    // ARM9 IO. width is 8, 16 or 32; addr is naturally aligned.
    void io_write(std::uint32_t addr, std::uint32_t value, int width);

    // ARM9 IO register readback, width is 8, 16 or 32; addr is aligned.
    std::uint32_t io_read(std::uint32_t addr, int width);

    // Fast-path page lookup for the bus (16 KiB pages). Return a host pointer
    // to the start of the page that guest `addr` falls into, or nullptr if the
    // page is unmapped, overlaps several banks, or needs slow-path handling.
    std::uint8_t* arm9_vram_page(std::uint32_t addr);      // 0x06000000-0x06FFFFFF
    std::uint8_t* arm7_vram_page(std::uint32_t addr);      // ARM7 0x06000000-0x06FFFFFF (banks C/D)
    // Invoked after any VRAMCNT write so the core can rebuild page tables.
    std::function<void()> on_vram_remap;

    // Slow-path memory access. Palette 0x05000000, VRAM 0x06000000, OAM
    // 0x07000000 (ARM9 view) and ARM7 VRAM. 8-bit writes to palette/OAM/VRAM
    // follow hardware rules (ignored for palette/OAM and ARM9 VRAM).
    std::uint32_t arm9_read(std::uint32_t addr, int width);
    void arm9_write(std::uint32_t addr, std::uint32_t value, int width);
    std::uint32_t arm7_read(std::uint32_t addr, int width);
    void arm7_write(std::uint32_t addr, std::uint32_t value, int width);

    // VRAMSTAT (0x04000240 read on ARM7) bits for banks C/D mapped to ARM7.
    std::uint8_t vramstat() const;

    // Contiguous single-bank texture/palette slots; nullptr if unmapped or
    // overlapping. Byte accessors below OR overlapping banks without snapshots.
    const std::uint8_t* texture_slot(int slot) const;
    const std::uint8_t* texpal_slot(int slot) const;
    std::uint8_t texture_byte(std::uint32_t address) const;
    std::uint8_t texpal_byte(std::uint32_t address) const;
    // Borrowed end-of-frame state, not per-scanline replay data. Valid only
    // on the Machine owner thread until the next step or direct mutation.
    Gpu2DFrameView frame_view() const;
    // Texture-cache invalidation token: reset to zero by reset(), incremented
    // by VRAMCNT writes and writes into texture image/palette bank storage.
    std::uint64_t vram_generation() const;

    // Rendering, driven by the core timing:
    //   render_line(y) for y in 0..191 at the start of HBlank of that line;
    //   line_3d is the 3D engine's 256-pixel output for line y (may be
    //   nullptr if 3D is disabled), format defined by Gpu3D::line().
    void attach_gpu3d(Gpu3D* gpu3d);
    void render_line(int y);
    void vblank_start();

    // Final 256x192 frames for the top and bottom LCDs after POWCNT1
    // screen swap and master brightness, 0xFFRRGGBB per pixel.
    const std::uint32_t* screen_top() const;
    const std::uint32_t* screen_bottom() const;
    // Engine A's row-major mask: 1 only for opaque, unblended, unmodified BG0
    // 3D pixels in normal display. Brightness-transformed or scrolled lines
    // stay software (0); the GPU overlay does not replay per-line 2D effects.
    const std::uint8_t* three_d_mask() const;

    struct Impl;

private:
    friend struct gpu2d_detail::Gpu2DTestAccess;
    Impl* impl_;
};

} // namespace dsr
