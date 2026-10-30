#pragma once

#include "dsr/render_frame.h"
#include "dsr/session_c.h"

namespace dsr {

// C++ presentation seam for the opaque C session. Call only on its owner thread;
// RenderFrame views are borrowed until the next step or session destruction.
DSR_SESSION_API bool capture_session_render_frame(const dsr_session_handle* session,
                                  RenderFrame& destination) noexcept;

// Install/remove only on the session owner thread. The callback runs just after
// scanline-214 rendering, before guest code can mutate texture VRAM again.
DSR_SESSION_API void set_session_render_callback(dsr_session_handle* session,
                                 std::function<void()> callback);

// Owner-thread-only optional fragment classification for hybrid GPU display.
DSR_SESSION_API void set_session_capture_opaque_only(dsr_session_handle* session, bool enabled);

// Owner-thread-only retained geometry capacity; does not change guest-visible
// hardware counts or overflow flags.
DSR_SESSION_API void set_session_enhanced_geometry(dsr_session_handle* session, bool enabled);

// Configure separate presentation composition without changing guest LCD output.
// Full maps the wide-camera raster; Picture uses native 3D coordinates.
DSR_SESSION_API void set_session_enhanced_presentation(dsr_session_handle* session, bool enabled, bool full);

// Set before stepping; affects only Full-mode presentation composition.
DSR_SESSION_API void set_session_presentation_scale(dsr_session_handle* session, int scale);

} // namespace dsr
