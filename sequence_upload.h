#ifndef SEQUENCE_UPLOAD_H
#define SEQUENCE_UPLOAD_H

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

// Chunk size for uploads (256 bytes raw data)
#define SEQUENCE_CHUNK_SIZE 256

// Upload session timeout (30 seconds of inactivity)
#define SEQUENCE_UPLOAD_TIMEOUT_SECONDS 30

// Upload state for a sequence slot
typedef enum {
    UPLOAD_STATE_IDLE = 0,
    UPLOAD_STATE_RECEIVING,
    UPLOAD_STATE_COMPLETE,
    UPLOAD_STATE_ERROR
} UploadState;

// Upload session data
typedef struct {
    UploadState state;
    uint8_t slot;
    uint8_t program;           // Target program number (0-31)
    uint16_t total_chunks;
    uint16_t file_size;
    uint16_t chunks_received;
    uint8_t *buffer;           // Reassembly buffer
    uint16_t buffer_pos;       // Current write position
    time_t last_activity;      // Timestamp of last received data (for timeout detection)
} UploadSession;

// Initialize sequence upload system
void sequence_upload_init(void);

// Start a new upload session
// Returns 0 on success, -1 on error
int sequence_upload_start(uint8_t slot, uint8_t program, uint16_t total_chunks, uint16_t file_size);

// Receive a chunk of data (7-bit encoded)
// Returns 0 on success, -1 on error
int sequence_upload_chunk(uint8_t slot, uint8_t chunk_num, const uint8_t *encoded_data, size_t encoded_len);

// Complete upload and save to file
// Returns 0 on success, -1 on error
int sequence_upload_complete(uint8_t slot);

// Abort current upload session
void sequence_upload_abort(uint8_t slot);

// Check all sessions for timeouts and auto-abort stale uploads
// Call this periodically (e.g., from main loop or timer)
void sequence_upload_check_timeouts(void);

// Get upload session for a slot
UploadSession* sequence_upload_get_session(uint8_t slot);

// Decode 7-bit encoded data to 8-bit
// For every 9 bytes of input, produces 8 bytes of output
// Byte 0:   MSBs (top bit of each of the 8 output bytes)
// Bytes 1-8: Lower 7 bits of each output byte
void decode_7bit_to_8bit(const uint8_t *encoded, uint8_t *decoded, size_t num_blocks);

#ifdef __cplusplus
}
#endif

#endif // SEQUENCE_UPLOAD_H
