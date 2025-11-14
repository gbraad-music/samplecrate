#include "sequence_download.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Download sessions for each slot
static DownloadSession download_sessions[SEQUENCE_MAX_SLOTS];

// Initialize sequence download system
void sequence_download_init(void) {
    for (int i = 0; i < SEQUENCE_MAX_SLOTS; i++) {
        download_sessions[i].state = DOWNLOAD_STATE_IDLE;
        download_sessions[i].slot = i;
        download_sessions[i].program = 0;
        download_sessions[i].total_chunks = 0;
        download_sessions[i].file_size = 0;
        download_sessions[i].buffer = nullptr;
        download_sessions[i].last_activity = 0;
    }
}

// Encode 8-bit data to 7-bit for MIDI SysEx
// For every 7 bytes of input, produces 8 bytes of output
void encode_8bit_to_7bit(const uint8_t *data, uint8_t *encoded, size_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        uint8_t msbs = 0;

        // Collect MSBs from 7 input bytes
        for (int j = 0; j < 7; j++) {
            if (data[i * 7 + j] & 0x80) {
                msbs |= (1 << j);
            }
        }

        // Write MSBs byte
        encoded[i * 8] = msbs;

        // Write lower 7 bits of each byte
        for (int j = 0; j < 7; j++) {
            encoded[i * 8 + 1 + j] = data[i * 7 + j] & 0x7F;
        }
    }
}

// Get file size
static long get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return -1;
}

// Start a new download session
int sequence_download_start(uint8_t slot, uint8_t *out_program, uint16_t *out_total_chunks, uint16_t *out_file_size) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        printf("[SequenceDownload] ERROR: Invalid slot %d\n", slot);
        return -1;
    }

    DownloadSession *session = &download_sessions[slot];

    // Abort any existing session
    if (session->state != DOWNLOAD_STATE_IDLE) {
        printf("[SequenceDownload] WARNING: Aborting previous session on slot %d\n", slot);
        sequence_download_abort(slot);
    }

    // Build filename: sequences/seq_<slot>.mid
    char filename[256];
    snprintf(filename, sizeof(filename), "sequences/seq_%d.mid", slot);

    // Check if file exists and get size
    long file_size = get_file_size(filename);
    if (file_size < 0) {
        printf("[SequenceDownload] ERROR: File not found: %s\n", filename);
        return -1;
    }

    if (file_size > SEQUENCE_MAX_FILE_SIZE) {
        printf("[SequenceDownload] ERROR: File too large: %ld bytes (max %d)\n",
               file_size, SEQUENCE_MAX_FILE_SIZE);
        return -1;
    }

    // Allocate buffer
    session->buffer = (uint8_t*)malloc(file_size);
    if (!session->buffer) {
        printf("[SequenceDownload] ERROR: Failed to allocate %ld bytes\n", file_size);
        return -1;
    }

    // Read file into buffer
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("[SequenceDownload] ERROR: Failed to open %s\n", filename);
        free(session->buffer);
        session->buffer = nullptr;
        return -1;
    }

    size_t bytes_read = fread(session->buffer, 1, file_size, fp);
    fclose(fp);

    if (bytes_read != (size_t)file_size) {
        printf("[SequenceDownload] ERROR: Failed to read complete file (read %zu of %ld bytes)\n",
               bytes_read, file_size);
        free(session->buffer);
        session->buffer = nullptr;
        return -1;
    }

    // Calculate number of chunks
    size_t chunk_count = (file_size + SEQUENCE_DOWNLOAD_CHUNK_SIZE - 1) / SEQUENCE_DOWNLOAD_CHUNK_SIZE;

    // Initialize session
    session->state = DOWNLOAD_STATE_ACTIVE;
    session->program = 0;  // TODO: Load program assignment from metadata file
    session->total_chunks = chunk_count;
    session->file_size = file_size;
    session->last_activity = time(NULL);

    // Return metadata
    if (out_program) *out_program = session->program;
    if (out_total_chunks) *out_total_chunks = session->total_chunks;
    if (out_file_size) *out_file_size = session->file_size;

    printf("[SequenceDownload] Started download from slot %d: %d chunks, %ld bytes, program %d\n",
           slot, session->total_chunks, file_size, session->program);

    return 0;
}

// Get a chunk of data (7-bit encoded)
size_t sequence_download_get_chunk(uint8_t slot, uint8_t chunk_num, uint8_t *encoded_buffer, size_t buffer_size) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        printf("[SequenceDownload] ERROR: Invalid slot %d\n", slot);
        return 0;
    }

    DownloadSession *session = &download_sessions[slot];

    if (session->state != DOWNLOAD_STATE_ACTIVE) {
        printf("[SequenceDownload] ERROR: Slot %d not in active state\n", slot);
        return 0;
    }

    if (!session->buffer) {
        printf("[SequenceDownload] ERROR: Slot %d has no buffer\n", slot);
        return 0;
    }

    if (chunk_num >= session->total_chunks) {
        printf("[SequenceDownload] ERROR: Chunk %d out of range (max %d)\n",
               chunk_num, session->total_chunks - 1);
        return 0;
    }

    // Calculate chunk boundaries
    size_t chunk_start = chunk_num * SEQUENCE_DOWNLOAD_CHUNK_SIZE;
    size_t chunk_end = chunk_start + SEQUENCE_DOWNLOAD_CHUNK_SIZE;
    if (chunk_end > session->file_size) {
        chunk_end = session->file_size;
    }
    size_t chunk_size = chunk_end - chunk_start;

    // Calculate number of 7-byte blocks (round up)
    size_t num_blocks = (chunk_size + 6) / 7;
    size_t encoded_size = num_blocks * 8;

    // Check buffer size
    if (buffer_size < encoded_size) {
        printf("[SequenceDownload] ERROR: Buffer too small (need %zu, have %zu)\n",
               encoded_size, buffer_size);
        return 0;
    }

    // Pad last block if necessary
    uint8_t padded_chunk[SEQUENCE_DOWNLOAD_CHUNK_SIZE];
    memcpy(padded_chunk, session->buffer + chunk_start, chunk_size);

    // Zero-pad remaining bytes in last block
    size_t padded_size = num_blocks * 7;
    for (size_t i = chunk_size; i < padded_size; i++) {
        padded_chunk[i] = 0;
    }

    // Encode to 7-bit
    encode_8bit_to_7bit(padded_chunk, encoded_buffer, num_blocks);

    // Update activity timestamp
    session->last_activity = time(NULL);

    printf("[SequenceDownload] Slot %d: Sent chunk %d/%d (%zu bytes raw, %zu bytes encoded)\n",
           slot, chunk_num + 1, session->total_chunks, chunk_size, encoded_size);

    return encoded_size;
}

// Complete download session and cleanup
void sequence_download_complete(uint8_t slot) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        return;
    }

    DownloadSession *session = &download_sessions[slot];

    if (session->buffer) {
        free(session->buffer);
        session->buffer = nullptr;
    }

    session->state = DOWNLOAD_STATE_IDLE;
    session->total_chunks = 0;
    session->file_size = 0;
    session->program = 0;
    session->last_activity = 0;

    printf("[SequenceDownload] Completed download for slot %d\n", slot);
}

// Abort current download session
void sequence_download_abort(uint8_t slot) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        return;
    }

    DownloadSession *session = &download_sessions[slot];

    if (session->buffer) {
        free(session->buffer);
        session->buffer = nullptr;
    }

    session->state = DOWNLOAD_STATE_IDLE;
    session->total_chunks = 0;
    session->file_size = 0;
    session->program = 0;
    session->last_activity = 0;

    printf("[SequenceDownload] Aborted download for slot %d\n", slot);
}

// Check all sessions for timeouts and auto-abort stale downloads
void sequence_download_check_timeouts(void) {
    time_t now = time(NULL);

    for (int i = 0; i < SEQUENCE_MAX_SLOTS; i++) {
        DownloadSession *session = &download_sessions[i];

        // Only check sessions that are actively downloading
        if (session->state == DOWNLOAD_STATE_ACTIVE) {
            time_t elapsed = now - session->last_activity;

            if (elapsed > SEQUENCE_DOWNLOAD_TIMEOUT_SECONDS) {
                printf("[SequenceDownload] TIMEOUT: Slot %d inactive for %ld seconds, aborting\n",
                       i, (long)elapsed);
                sequence_download_abort(i);
            }
        }
    }
}

// Get download session for a slot
DownloadSession* sequence_download_get_session(uint8_t slot) {
    if (slot >= SEQUENCE_MAX_SLOTS) {
        return nullptr;
    }
    return &download_sessions[slot];
}
