#pragma once

namespace dsr {
class Gpu3D;

namespace detail {
// Start 3D rendering on scanline 214; do nothing on other scanlines.
// Call once per line start, after updating display status and its IRQs.
void process_render_boundary(unsigned scanline, Gpu3D& gpu3d);
} // namespace detail
} // namespace dsr
