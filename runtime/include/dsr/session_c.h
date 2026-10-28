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
    /* Appended later; NULL, "" or "auto" keep the ABI config / save-file-size choice. Names as --backup-type. */
    const char* backup_type;
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
/* Flushes pending save data. Returns 0 on success (including no pending writes),
 * nonzero on write failure or a NULL session. Failed writes remain pending for retry.
 * Call on the session's owner thread, like step/destroy. */
DSR_SESSION_API int dsr_session_save(dsr_session_handle* session);
/* Owner-thread only. Copies canonical main RAM or allowlisted ARM9 TCM backing,
 * without bus side effects. TCM must be enabled and mapped at the address.
 * Returns 0 on success, nonzero on failure. */
DSR_SESSION_API int dsr_session_copy_snapshot_ram(const dsr_session_handle* session,
                                                   uint32_t address, void* destination, size_t size);
/* Save inspection and restore are owner-thread-only. Capacity is the effective chip's raw
 * byte count, or zero for NULL/no cartridge. Listing visits managed regular, non-symlink
 * backups newest filename first, regardless of mtime or capacity. path is UTF-8, borrowed
 * only for the callback; size is the raw file byte count. The callback returns zero to
 * accept an entry or nonzero to fail listing, and must not reenter/destroy the session.
 * Listing returns 0 on success, nonzero with error text on failure. */
typedef int (*dsr_save_backup_callback)(void* context, const char* path, uint64_t size);
DSR_SESSION_API uint64_t dsr_session_get_save_capacity(dsr_session_handle* session);
DSR_SESSION_API int dsr_session_get_save_backups(dsr_session_handle* session,
                                 dsr_save_backup_callback callback, void* context,
                                 char* error_out, size_t error_capacity);
/* Restore is allowed only before any step attempt, including a session now paused/stopped.
 * NULL/empty source selects the newest managed backup. Uses exact raw chip capacity;
 * never changes chip type, pads/truncates bytes or converts DSV. Preserves the old target.
 * Returns 0 on success, nonzero with error text on failure. */
DSR_SESSION_API int dsr_session_restore_save(dsr_session_handle* session, const char* source,
                              char* error_out, size_t error_capacity);
/* Nonzero once the program has read the cartridge backup like an EEPROM while a flash chip is emulated. */
DSR_SESSION_API int dsr_session_chip_mismatch_suspected(dsr_session_handle* session);

#ifdef __cplusplus
}
#endif
