#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

#include <cstdint>

namespace dsr {
namespace {

const std::uint8_t* texture_lookup(const void* context, int slot) {
    const auto* gpu2d = static_cast<const Gpu2D*>(context);
    return gpu2d == nullptr ? nullptr : gpu2d->texture_slot(slot);
}

const std::uint8_t* palette_lookup(const void* context, int slot) {
    const auto* gpu2d = static_cast<const Gpu2D*>(context);
    return gpu2d == nullptr ? nullptr : gpu2d->texpal_slot(slot);
}

std::uint8_t texture_read(const void* context, std::uint32_t address) {
    const auto* gpu2d = static_cast<const Gpu2D*>(context);
    return gpu2d == nullptr ? 0 : gpu2d->texture_byte(address);
}

std::uint8_t palette_read(const void* context, std::uint32_t address) {
    const auto* gpu2d = static_cast<const Gpu2D*>(context);
    return gpu2d == nullptr ? 0 : gpu2d->texpal_byte(address);
}

std::uint64_t texture_generation(const void* context) {
    const auto* gpu2d = static_cast<const Gpu2D*>(context);
    return gpu2d == nullptr ? 0 : gpu2d->vram_generation();
}

} // namespace

void Gpu3D::attach_gpu2d(const Gpu2D* gpu2d) {
    set_texture_source(gpu2d, &texture_lookup, &palette_lookup, &texture_read, &palette_read,
                       &texture_generation);
}

} // namespace dsr
