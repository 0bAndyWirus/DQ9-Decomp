#pragma once

#include <stddef.h>
#include <stdint.h>

/* The Godot extension imports these symbols from the session DLL on Windows.
 * DSR_SESSION_BUILD is private to that DLL's compilation. */
#if defined(_WIN32)
#if defined(DSR_SESSION_BUILD)
#define DSR_SESSION_API __declspec(dllexport)
#else
#define DSR_SESSION_API __declspec(dllimport)
#endif
#else
#define DSR_SESSION_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsr_session_handle dsr_session_handle;

typedef struct dsr_input_state {
    uint16_t buttons;
    int32_t pen_x;
    int32_t pen_y;
    uint8_t pen_down;
    uint8_t lid_closed;
} dsr_input_state;

typedef struct dsr_session_config {
    /* UTF-8 paths on every platform, including Windows. */
    const char* rom_path;
    const char* save_path;
    uint8_t language;
    uint8_t deterministic_rtc;
    int64_t rtc_epoch_seconds;
    int32_t rtc_timezone_offset_minutes;
} dsr_session_config;


/* Returns NULL on failure. Error text is copied to error_out when provided. */
DSR_SESSION_API dsr_session_handle* dsr_session_create(const dsr_session_config* config,
                                        char* error_out, size_t error_capacity);
DSR_SESSION_API void dsr_session_destroy(dsr_session_handle* session);
/* Returns 0 on success, nonzero on failure. */
DSR_SESSION_API int dsr_session_step(dsr_session_handle* session, const dsr_input_state* input,
                     char* error_out, size_t error_capacity);
DSR_SESSION_API int dsr_session_set_input(dsr_session_handle* session, const dsr_input_state* input,
                          char* error_out, size_t error_capacity);
/* Borrowed until the next step or destroy. Screens are 256x192 uint32 ARGB. */
DSR_SESSION_API const uint32_t* dsr_session_get_screen(const dsr_session_handle* session, int screen);
/* Borrowed interleaved stereo int16 PCM, valid until next step/destroy. */
DSR_SESSION_API const int16_t* dsr_session_get_audio(const dsr_session_handle* session,
                                     size_t* frame_count, double* sample_rate);
DSR_SESSION_API uint64_t dsr_session_frame_count(const dsr_session_handle* session);
DSR_SESSION_API uint64_t dsr_session_ticks(const dsr_session_handle* session);
DSR_SESSION_API void dsr_session_save(dsr_session_handle* session);

#ifdef __cplusplus
}
#endif
