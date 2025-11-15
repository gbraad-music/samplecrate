#include "sequence_upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <direct.h>  // for _mkdir
#else
    #include <sys/stat.h>  // for mkdir
#endif

// Upload sessions for each slot
static UploadSession upload_sessions[SEQUENCE_MAX_SLOTS];

// Initialize sequence upload system
void sequence_upload_init(void) {
    for (int i = 0; i < SEQUENCE_MAX_SLOTS; i++) {
        upload_sessions[i].state = UPLOAD_STATE_IDLE;
        upload_sessions[i].slot = i;
        upload_sessions[i].program = 0;
        upload_sessions[i].total_chunks = 0;
        upload_sessions[i].file_size = 0;
        upload_sessions[i].chunks_received = 0;
        upload_sessions[i].buffer = nullptr;
        upload_sessions[i].buffer_pos = 0;
        upload_sessions[i].last_activity = 0;
    }
}

// Start a new upload session
int sequence_upload_start(uint8_t slot, uint8_t program, uint16_t total_chunks, uint16_t file_size) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        printf("[SequenceUpload] ERROR: Invalid slot %d\n", slot);
        return -1;
    }

    if (file_size > SEQUENCE_MAX_FILE_SIZE) {
        printf("[SequenceUpload] ERROR: File size %d exceeds maximum %d\n",
               file_size, SEQUENCE_MAX_FILE_SIZE);
        return -1;
    }

    UploadSession *session = &upload_sessions[slot];

    // RECOVERY: Abort any existing session (handles abandoned/incomplete uploads)
    if (session->state != UPLOAD_STATE_IDLE) {
        printf("[SequenceUpload] WARNING: Aborting previous session on slot %d (state=%d)\n",
               slot, session->state);
        sequence_upload_abort(slot);
    }

    // Allocate buffer for reassembly
    session->buffer = (uint8_t*)malloc(file_size);
    if (!session->buffer) {
        printf("[SequenceUpload] ERROR: Failed to allocate %d bytes for slot %d\n",
               file_size, slot);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    // Initialize session
    session->state = UPLOAD_STATE_RECEIVING;
    session->program = program;  // Store target program number
    session->total_chunks = total_chunks;
    session->file_size = file_size;
    session->chunks_received = 0;
    session->buffer_pos = 0;
    session->last_activity = time(NULL);  // Track when session started

    printf("[SequenceUpload] Started upload to slot %d (program %d): %d chunks, %d bytes\n",
           slot, program, total_chunks, file_size);

    return 0;
}

// Decode 7-bit encoded data to 8-bit
// For every 8 bytes of input, produces 7 bytes of output
void decode_7bit_to_8bit(const uint8_t *encoded, uint8_t *decoded, size_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        uint8_t msbs = encoded[i * 8];
        for (int j = 0; j < 7; j++) {
            uint8_t lsb = encoded[i * 8 + 1 + j];
            uint8_t msb = (msbs & (1 << j)) ? 0x80 : 0;
            decoded[i * 7 + j] = lsb | msb;
        }
    }
}

// Receive a chunk of data (7-bit encoded)
int sequence_upload_chunk(uint8_t slot, uint8_t chunk_num, const uint8_t *encoded_data, size_t encoded_len) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        printf("[SequenceUpload] ERROR: Invalid slot %d\n", slot);
        return -1;
    }

    UploadSession *session = &upload_sessions[slot];

    if (session->state != UPLOAD_STATE_RECEIVING) {
        printf("[SequenceUpload] ERROR: Slot %d not in receiving state\n", slot);
        return -1;
    }

    if (!session->buffer) {
        printf("[SequenceUpload] ERROR: Slot %d has no buffer allocated\n", slot);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    // Validate chunk number
    if (chunk_num != session->chunks_received) {
        printf("[SequenceUpload] ERROR: Expected chunk %d, got %d\n",
               session->chunks_received, chunk_num);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    // Calculate number of 8-byte blocks in encoded data
    size_t num_blocks = encoded_len / 8;
    size_t decoded_len = num_blocks * 7;

    // Truncate to remaining file size (removes 7-bit encoding padding in last chunk)
    size_t bytes_to_write = decoded_len;
    if (session->buffer_pos + decoded_len > session->file_size) {
        bytes_to_write = session->file_size - session->buffer_pos;
        printf("[SequenceUpload] Truncating last chunk: %zu → %zu bytes (padding removed)\n",
               decoded_len, bytes_to_write);
    }

    // Decode chunk (decode full block, then use only what we need)
    uint8_t temp_buffer[512];  // Temporary buffer for full decoded chunk
    decode_7bit_to_8bit(encoded_data, temp_buffer, num_blocks);

    // Copy only the bytes we need (truncates padding in last chunk)
    memcpy(session->buffer + session->buffer_pos, temp_buffer, bytes_to_write);
    session->buffer_pos += bytes_to_write;
    session->chunks_received++;
    session->last_activity = time(NULL);  // Update activity timestamp

    printf("[SequenceUpload] Slot %d: Received chunk %d/%d (%zu bytes decoded, %d total)\n",
           slot, chunk_num + 1, session->total_chunks, decoded_len, session->buffer_pos);

    return 0;
}

// Validate MIDI file header
static int validate_midi_header(const uint8_t *data, size_t len) {
    if (len < 14) {
        printf("[SequenceUpload] ERROR: File too small for MIDI header\n");
        return -1;
    }

    // Check for "MThd" header
    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        printf("[SequenceUpload] ERROR: Invalid MIDI header (expected MThd, got %c%c%c%c)\n",
               data[0], data[1], data[2], data[3]);
        return -1;
    }

    // Header length should be 6
    uint32_t header_len = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (header_len != 6) {
        printf("[SequenceUpload] ERROR: Invalid MIDI header length: %u (expected 6)\n", header_len);
        return -1;
    }

    printf("[SequenceUpload] MIDI file validated successfully\n");
    return 0;
}

// Complete upload and save to file
int sequence_upload_complete(uint8_t slot, const char* output_dir) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        printf("[SequenceUpload] ERROR: Invalid slot %d\n", slot);
        return -1;
    }

    UploadSession *session = &upload_sessions[slot];

    if (session->state != UPLOAD_STATE_RECEIVING) {
        printf("[SequenceUpload] ERROR: Slot %d not in receiving state\n", slot);
        return -1;
    }

    if (!session->buffer) {
        printf("[SequenceUpload] ERROR: Slot %d has no buffer\n", slot);
        return -1;
    }

    // Validate all chunks received
    if (session->chunks_received != session->total_chunks) {
        printf("[SequenceUpload] ERROR: Missing chunks (received %d of %d)\n",
               session->chunks_received, session->total_chunks);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    // Validate MIDI file
    if (validate_midi_header(session->buffer, session->buffer_pos) != 0) {
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    // Build directory path: <output_dir>/sequences/
    char dir_path[512];
    if (output_dir && output_dir[0] != '\0') {
        snprintf(dir_path, sizeof(dir_path), "%s/sequences", output_dir);
    } else {
        snprintf(dir_path, sizeof(dir_path), "sequences");
    }

    // Create sequences directory if it doesn't exist
    #ifdef _WIN32
        _mkdir(dir_path);
    #else
        mkdir(dir_path, 0755);
    #endif

    // Build filename: <dir_path>/seq_<slot>.mid
    char filename[768];
    snprintf(filename, sizeof(filename), "%s/seq_%d.mid", dir_path, slot);

    // Write to file
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("[SequenceUpload] ERROR: Failed to open %s for writing\n", filename);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    size_t written = fwrite(session->buffer, 1, session->buffer_pos, fp);
    fclose(fp);

    if (written != session->buffer_pos) {
        printf("[SequenceUpload] ERROR: Failed to write complete file (wrote %zu of %d bytes)\n",
               written, session->buffer_pos);
        session->state = UPLOAD_STATE_ERROR;
        return -1;
    }

    printf("[SequenceUpload] Successfully saved slot %d to %s (%d bytes)\n",
           slot, filename, session->buffer_pos);

    // Free buffer and reset session to IDLE (ready for next upload)
    free(session->buffer);
    session->buffer = nullptr;
    session->buffer_pos = 0;
    session->chunks_received = 0;
    session->total_chunks = 0;
    session->file_size = 0;
    session->state = UPLOAD_STATE_IDLE;  // Reset to IDLE, not COMPLETE

    return 0;
}

// Abort current upload session
void sequence_upload_abort(uint8_t slot) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        return;
    }

    UploadSession *session = &upload_sessions[slot];

    if (session->buffer) {
        free(session->buffer);
        session->buffer = nullptr;
    }

    session->state = UPLOAD_STATE_IDLE;
    session->program = 0;
    session->total_chunks = 0;
    session->file_size = 0;
    session->chunks_received = 0;
    session->buffer_pos = 0;
    session->last_activity = 0;

    printf("[SequenceUpload] Aborted upload for slot %d\n", slot);
}

// Check all sessions for timeouts and auto-abort stale uploads
void sequence_upload_check_timeouts(void) {
    time_t now = time(NULL);

    for (int i = 0; i < SEQUENCE_MAX_SLOTS; i++) {
        UploadSession *session = &upload_sessions[i];

        // Only check sessions that are actively receiving
        if (session->state == UPLOAD_STATE_RECEIVING) {
            time_t elapsed = now - session->last_activity;

            if (elapsed > SEQUENCE_UPLOAD_TIMEOUT_SECONDS) {
                printf("[SequenceUpload] TIMEOUT: Slot %d inactive for %ld seconds, aborting\n",
                       i, (long)elapsed);
                sequence_upload_abort(i);
            }
        }
    }
}

// Get upload session for a slot
UploadSession* sequence_upload_get_session(uint8_t slot) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        return nullptr;
    }
    return &upload_sessions[slot];
}
