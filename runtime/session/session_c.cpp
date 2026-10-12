#include "dsr/session_c.h"
#include "dsr/host_path.h"

#include "dsr/session.h"
#include "dsr/session_render_frame.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <exception>

struct dsr_session_handle {
    explicit dsr_session_handle(const dsr::SessionConfig& config) : session(config) {}
    dsr::Session session;
};

namespace {
void set_error(char* out, size_t capacity, const char* message) noexcept {
    if (!out || capacity == 0) return;
    const size_t size = std::min(capacity - 1, std::strlen(message));
    std::memcpy(out, message, size);
    out[size] = '\0';
}
}

extern "C" int dsr_session_set_input(dsr_session_handle* session, const dsr_input_state* input,
                                      char* error_out, size_t error_capacity) {
    if (!session || !input) {
        set_error(error_out, error_capacity, "invalid session input arguments");
        return 1;
    }
    try {
        dsr::InputState native;
        native.buttons = input->buttons;
        native.pen_x = input->pen_x;
        native.pen_y = input->pen_y;
        native.pen_down = input->pen_down != 0;
        native.lid_closed = input->lid_closed != 0;
        session->session.machine().set_input(native);
        return 0;
    } catch (const std::exception& error) {
        set_error(error_out, error_capacity, error.what());
    } catch (...) {
        set_error(error_out, error_capacity, "unknown input failure");
    }
    return 1;
}
extern "C" dsr_session_handle* dsr_session_create(const dsr_session_config* config,
                                                   char* error_out, size_t error_capacity) {
    if (!config || !config->rom_path || !config->save_path) {
        set_error(error_out, error_capacity, "invalid session config");
        return nullptr;
    }
    try {
        dsr::SessionConfig native;
        native.rom_path = dsr::host::path_from_utf8(config->rom_path);
        native.save_path = dsr::host::path_from_utf8(config->save_path);
        native.language = config->language;
        native.deterministic_rtc = config->deterministic_rtc != 0;
        native.rtc_epoch_seconds = config->rtc_epoch_seconds;
        native.rtc_timezone_offset_minutes = config->rtc_timezone_offset_minutes;
        return new dsr_session_handle(native);
    } catch (const std::exception& error) {
        set_error(error_out, error_capacity, error.what());
    } catch (...) {
        set_error(error_out, error_capacity, "unknown session creation failure");
    }
    return nullptr;
}

extern "C" void dsr_session_destroy(dsr_session_handle* session) { delete session; }

extern "C" int dsr_session_step(dsr_session_handle* session, const dsr_input_state* input,
                                  char* error_out, size_t error_capacity) {
    if (!session || !input) {
        set_error(error_out, error_capacity, "invalid session step arguments");
        return 1;
    }
    try {
        dsr::InputState native;
        native.buttons = input->buttons;
        native.pen_x = input->pen_x;
        native.pen_y = input->pen_y;
        native.pen_down = input->pen_down != 0;
        native.lid_closed = input->lid_closed != 0;
        session->session.step(native);
        return 0;
    } catch (const std::exception& error) {
        set_error(error_out, error_capacity, error.what());
    } catch (...) {
        set_error(error_out, error_capacity, "unknown session step failure");
    }
    return 1;
}

extern "C" const uint32_t* dsr_session_get_screen(const dsr_session_handle* session, int screen) {
    if (!session) return nullptr;
    if (screen == 0) return session->session.screen_top();
    if (screen == 1) return session->session.screen_bottom();
    return nullptr;
}

extern "C" const int16_t* dsr_session_get_audio(const dsr_session_handle* session,
                                                  size_t* frame_count, double* sample_rate) {
    if (!session) return nullptr;
    if (frame_count) *frame_count = session->session.audio_frames();
    if (sample_rate) *sample_rate = dsr::Session::audio_rate();
    return session->session.audio_data();
}

extern "C" uint64_t dsr_session_frame_count(const dsr_session_handle* session) {
    return session ? session->session.frame_count() : 0;
}
extern "C" uint64_t dsr_session_ticks(const dsr_session_handle* session) {
    return session ? session->session.ticks() : 0;
}
extern "C" void dsr_session_save(dsr_session_handle* session) {
    if (session) session->session.flush_save();
}

bool dsr::capture_session_render_frame(const dsr_session_handle* session,
                                       RenderFrame& destination) noexcept {
    if (!session) return false;
    capture_render_frame(session->session.machine(), session->session.frame_count(), destination);
    return true;
}

void dsr::set_session_render_callback(dsr_session_handle* session,
                                      std::function<void()> callback) {
    if (session) session->session.machine().gpu3d().on_render_complete = std::move(callback);
}

void dsr::set_session_capture_opaque_only(dsr_session_handle* session, bool enabled) {
    if (session) session->session.machine().gpu3d().set_capture_opaque_only(enabled);
}
