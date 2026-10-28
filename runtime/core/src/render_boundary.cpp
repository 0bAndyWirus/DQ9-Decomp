#include "render_boundary.h"
#include "dsr/gpu3d.h"

namespace dsr::detail {

// Synchronous whole-frame render at the line-214 boundary, after display
// status updates: docs/cleanroom/render-timing.md.
void process_render_boundary(unsigned scanline, Gpu3D& gpu3d) {
    if (scanline == 214) {
        gpu3d.render_start();
    }
}

} // namespace dsr::detail
