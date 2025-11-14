#ifndef SEQUENCE_DOWNLOAD_H
#define SEQUENCE_DOWNLOAD_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of sequence slots
#define SEQUENCE_MAX_SLOTS 16

// Maximum MIDI file size (16 KB)
#define SEQUENCE_MAX_FILE_SIZE (16 * 1024)

// Chunk size for downloads (256 bytes raw data)
#define SEQUENCE_DOWNLOAD_CHUNK_SIZE 256

// Download session timeout (30 seconds of inactivity)
#define SEQUENCE_DOWNLOAD_TIMEOUT_SECONDS 30

// Download state for a sequence slot
typedef enum {
    DOWNLOAD_STATE_IDLE = 0,
    DOWNLOAD_STATE_ACTIVE,
    DOWNLOAD_STATE_COMPLETE,
    DOWNLOAD_STATE_ERROR
} DownloadState;

// Download session data
typedef struct {
    DownloadState state;
    uint8_t slot;
    uint8_t program;           // Program/pad assignment for this track
    uint16_t total_chunks;
    uint16_t file_size;
    uint8_t *buffer;           // File data buffer
    time_t last_activity;      // Timestamp of last chunk request
} DownloadSession;

// Initialize sequence download system
void sequence_download_init(void);

// Start a new download session
// Loads file from slot and prepares for chunked transmission
// Returns 0 on success, -1 on error (file not found, invalid slot, etc.)
int sequence_download_start(uint8_t slot, uint8_t *out_program, uint16_t *out_total_chunks, uint16_t *out_file_size);

// Get a chunk of data (7-bit encoded)
// Returns encoded length on success, 0 on error
size_t sequence_download_get_chunk(uint8_t slot, uint8_t chunk_num, uint8_t *encoded_buffer, size_t buffer_size);

// Complete download session and cleanup
void sequence_download_complete(uint8_t slot);

// Abort current download session
void sequence_download_abort(uint8_t slot);

// Check all sessions for timeouts and auto-abort stale downloads
void sequence_download_check_timeouts(void);

// Get download session for a slot
DownloadSession* sequence_download_get_session(uint8_t slot);

// Encode 8-bit data to 7-bit for MIDI SysEx
// For every 7 bytes of input, produces 8 bytes of output
void encode_8bit_to_7bit(const uint8_t *data, uint8_t *encoded, size_t num_blocks);

#ifdef __cplusplus
}
#endif

#endif // SEQUENCE_DOWNLOAD_H
