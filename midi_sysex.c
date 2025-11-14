#include "midi_sysex.h"
#include <stdio.h>
#include <string.h>

// Current device ID
static uint8_t local_device_id = 0;

// Callback for incoming messages
static SysExCallback message_callback = NULL;
static void *callback_userdata = NULL;

void sysex_init(uint8_t device_id) {
    local_device_id = device_id & 0x7F;  // Ensure 7-bit
    printf("[SysEx] Initialized with device ID: %d\n", local_device_id);
}

void sysex_set_device_id(uint8_t device_id) {
    local_device_id = device_id & 0x7F;
    printf("[SysEx] Device ID set to: %d\n", local_device_id);
}

uint8_t sysex_get_device_id(void) {
    return local_device_id;
}

void sysex_register_callback(SysExCallback callback, void *userdata) {
    message_callback = callback;
    callback_userdata = userdata;
}

int sysex_parse_message(const uint8_t *msg, size_t msg_len) {
    // Minimum valid message: F0 7D <dev> <cmd> F7 = 5 bytes
    if (!msg || msg_len < 5) return 0;

    // Check for SysEx start
    if (msg[0] != SYSEX_START) return 0;

    // Check for our manufacturer ID
    if (msg[1] != SYSEX_MANUFACTURER_ID) return 0;

    // Check for SysEx end
    if (msg[msg_len - 1] != SYSEX_END) return 0;

    // Extract device ID and command
    uint8_t device_id = msg[2];
    uint8_t command = msg[3];

    // Check if message is for us (or broadcast)
    if (device_id != local_device_id && device_id != SYSEX_DEVICE_BROADCAST) {
        return 0;  // Not for us - silently ignore
    }

    // Extract data (everything between command and end byte)
    const uint8_t *data = (msg_len > 5) ? &msg[4] : NULL;
    size_t data_len = (msg_len > 5) ? (msg_len - 5) : 0;

    // Silently parse SysEx messages (logging is done in callback if needed)

    // Call registered callback
    if (message_callback) {
        message_callback(device_id, (SysExCommand)command, data, data_len, callback_userdata);
    }

    return 1;  // Message was handled
}

// --- Message Building Functions ---

size_t sysex_build_ping(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 5) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_PING;
    buffer[4] = SYSEX_END;

    return 5;
}

size_t sysex_build_file_load(uint8_t target_device_id, const char *filename,
                              uint8_t *buffer, size_t buffer_size) {
    if (!buffer || !filename) return 0;

    size_t filename_len = strlen(filename);
    if (filename_len == 0 || filename_len > 255) return 0;

    // Calculate required size: F0 7D <dev> <cmd> <len> <name...> F7
    size_t required = 6 + filename_len;
    if (buffer_size < required) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_FILE_LOAD;
    buffer[4] = (uint8_t)filename_len;
    memcpy(&buffer[5], filename, filename_len);
    buffer[5 + filename_len] = SYSEX_END;

    return required;
}

size_t sysex_build_play(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 5) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_PLAY;
    buffer[4] = SYSEX_END;

    return 5;
}

size_t sysex_build_stop(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 5) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_STOP;
    buffer[4] = SYSEX_END;

    return 5;
}

size_t sysex_build_pause(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 5) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_PAUSE;
    buffer[4] = SYSEX_END;

    return 5;
}

size_t sysex_build_channel_mute(uint8_t target_device_id, uint8_t channel, uint8_t mute,
                                 uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_CHANNEL_MUTE;
    buffer[4] = channel & 0x7F;
    buffer[5] = mute ? 1 : 0;
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_channel_solo(uint8_t target_device_id, uint8_t channel, uint8_t solo,
                                 uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_CHANNEL_SOLO;
    buffer[4] = channel & 0x7F;
    buffer[5] = solo ? 1 : 0;
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_channel_volume(uint8_t target_device_id, uint8_t channel, uint8_t volume,
                                   uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_CHANNEL_VOLUME;
    buffer[4] = channel & 0x7F;
    buffer[5] = volume & 0x7F;
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_set_position(uint8_t target_device_id, uint16_t position,
                                 uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_SET_POSITION;
    buffer[4] = position & 0x7F;        // LSB
    buffer[5] = (position >> 7) & 0x7F; // MSB
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_set_bpm(uint8_t target_device_id, uint16_t bpm,
                           uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_SET_BPM;
    buffer[4] = bpm & 0x7F;        // LSB
    buffer[5] = (bpm >> 7) & 0x7F; // MSB
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_trigger_pad(uint8_t target_device_id, uint8_t pad_index,
                                uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 6) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_TRIGGER_PAD;
    buffer[4] = pad_index & 0x7F;
    buffer[5] = SYSEX_END;

    return 6;
}

// --- Effects Control Functions ---

size_t sysex_build_fx_effect_get(uint8_t target_device_id,
                                  uint8_t program_id,
                                  uint8_t effect_id,
                                  uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 7) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_FX_EFFECT_GET;
    buffer[4] = program_id & 0x7F;
    buffer[5] = effect_id & 0x7F;
    buffer[6] = SYSEX_END;

    return 7;
}

size_t sysex_build_fx_effect_set(uint8_t target_device_id,
                                  uint8_t program_id,
                                  uint8_t effect_id,
                                  uint8_t enabled,
                                  const uint8_t *params,
                                  size_t param_count,
                                  uint8_t *buffer, size_t buffer_size) {
    if (!buffer || !params) return 0;

    // Calculate required size: F0 7D <dev> <cmd> <prog> <effect> <enabled> <params...> F7
    size_t required = 8 + param_count;
    if (buffer_size < required) return 0;

    size_t pos = 0;
    buffer[pos++] = SYSEX_START;
    buffer[pos++] = SYSEX_MANUFACTURER_ID;
    buffer[pos++] = target_device_id & 0x7F;
    buffer[pos++] = SYSEX_CMD_FX_EFFECT_SET;
    buffer[pos++] = program_id & 0x7F;
    buffer[pos++] = effect_id & 0x7F;
    buffer[pos++] = enabled ? 1 : 0;

    // Copy parameters
    for (size_t i = 0; i < param_count; i++) {
        buffer[pos++] = params[i] & 0x7F;
    }

    buffer[pos++] = SYSEX_END;

    return pos;
}

size_t sysex_build_fx_get_all_state(uint8_t target_device_id,
                                     uint8_t program_id,
                                     uint8_t *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 6) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_FX_GET_ALL_STATE;
    buffer[4] = program_id & 0x7F;
    buffer[5] = SYSEX_END;

    return 6;
}

size_t sysex_build_fx_state_response(uint8_t target_device_id,
                                      uint8_t program_id,
                                      uint8_t version,
                                      uint8_t fx_route,
                                      uint8_t enable_flags,
                                      const uint8_t *distortion_params,  // drive, mix
                                      const uint8_t *filter_params,      // cutoff, resonance
                                      const uint8_t *eq_params,          // low, mid, high
                                      const uint8_t *compressor_params,  // threshold, ratio, attack, release, makeup
                                      const uint8_t *delay_params,       // time, feedback, mix
                                      uint8_t *buffer, size_t buffer_size) {
    if (!buffer) return 0;
    if (!distortion_params || !filter_params || !eq_params || !compressor_params || !delay_params) return 0;

    // Fixed size: F0 7D <dev> <cmd> <32 data bytes> F7 = 37 bytes
    const size_t required = 37;
    if (buffer_size < required) return 0;

    size_t pos = 0;
    buffer[pos++] = SYSEX_START;
    buffer[pos++] = SYSEX_MANUFACTURER_ID;
    buffer[pos++] = target_device_id & 0x7F;
    buffer[pos++] = SYSEX_CMD_FX_STATE_RESPONSE;

    // 32-byte data section
    buffer[pos++] = program_id & 0x7F;      // Byte 0
    buffer[pos++] = version & 0x7F;         // Byte 1
    buffer[pos++] = fx_route & 0x7F;        // Byte 2
    buffer[pos++] = enable_flags & 0x7F;    // Byte 3

    // Distortion (bytes 4-5)
    buffer[pos++] = distortion_params[0] & 0x7F;  // drive
    buffer[pos++] = distortion_params[1] & 0x7F;  // mix

    // Filter (bytes 6-7)
    buffer[pos++] = filter_params[0] & 0x7F;  // cutoff
    buffer[pos++] = filter_params[1] & 0x7F;  // resonance

    // EQ (bytes 8-10)
    buffer[pos++] = eq_params[0] & 0x7F;  // low
    buffer[pos++] = eq_params[1] & 0x7F;  // mid
    buffer[pos++] = eq_params[2] & 0x7F;  // high

    // Compressor (bytes 11-15)
    buffer[pos++] = compressor_params[0] & 0x7F;  // threshold
    buffer[pos++] = compressor_params[1] & 0x7F;  // ratio
    buffer[pos++] = compressor_params[2] & 0x7F;  // attack
    buffer[pos++] = compressor_params[3] & 0x7F;  // release
    buffer[pos++] = compressor_params[4] & 0x7F;  // makeup

    // Delay (bytes 16-18)
    buffer[pos++] = delay_params[0] & 0x7F;  // time
    buffer[pos++] = delay_params[1] & 0x7F;  // feedback
    buffer[pos++] = delay_params[2] & 0x7F;  // mix

    // Reserved (bytes 19-31): 13 bytes
    for (int i = 0; i < 13; i++) {
        buffer[pos++] = 0x00;
    }

    buffer[pos++] = SYSEX_END;

    return pos;
}

int sysex_parse_fx_state_response(const uint8_t *data, size_t data_len,
                                   uint8_t *out_program_id,
                                   uint8_t *out_version,
                                   uint8_t *out_fx_route,
                                   uint8_t *out_enable_flags,
                                   uint8_t *out_distortion_params,  // 2 bytes
                                   uint8_t *out_filter_params,      // 2 bytes
                                   uint8_t *out_eq_params,          // 3 bytes
                                   uint8_t *out_compressor_params,  // 5 bytes
                                   uint8_t *out_delay_params) {     // 3 bytes
    // Minimum: 32 bytes
    if (!data || data_len < 32) return 0;

    // Extract header
    if (out_program_id) *out_program_id = data[0];
    if (out_version) *out_version = data[1];
    if (out_fx_route) *out_fx_route = data[2];
    if (out_enable_flags) *out_enable_flags = data[3];

    // Extract distortion (bytes 4-5)
    if (out_distortion_params) {
        out_distortion_params[0] = data[4];  // drive
        out_distortion_params[1] = data[5];  // mix
    }

    // Extract filter (bytes 6-7)
    if (out_filter_params) {
        out_filter_params[0] = data[6];  // cutoff
        out_filter_params[1] = data[7];  // resonance
    }

    // Extract EQ (bytes 8-10)
    if (out_eq_params) {
        out_eq_params[0] = data[8];   // low
        out_eq_params[1] = data[9];   // mid
        out_eq_params[2] = data[10];  // high
    }

    // Extract compressor (bytes 11-15)
    if (out_compressor_params) {
        out_compressor_params[0] = data[11];  // threshold
        out_compressor_params[1] = data[12];  // ratio
        out_compressor_params[2] = data[13];  // attack
        out_compressor_params[3] = data[14];  // release
        out_compressor_params[4] = data[15];  // makeup
    }

    // Extract delay (bytes 16-18)
    if (out_delay_params) {
        out_delay_params[0] = data[16];  // time
        out_delay_params[1] = data[17];  // feedback
        out_delay_params[2] = data[18];  // mix
    }

    // Note: Reserved bytes (19-31) are ignored

    return 1;
}

// --- Sequence Track Upload Response Functions ---

size_t sysex_build_sequence_track_upload_response(uint8_t target_device_id,
                                                   uint8_t subcommand,
                                                   uint8_t slot,
                                                   uint8_t status,
                                                   uint8_t *buffer, size_t buffer_size) {
    // Message format: F0 7D <dev> 81 <subcommand> <slot> <status> F7
    if (!buffer || buffer_size < 8) return 0;

    buffer[0] = SYSEX_START;
    buffer[1] = SYSEX_MANUFACTURER_ID;
    buffer[2] = target_device_id & 0x7F;
    buffer[3] = SYSEX_CMD_SEQUENCE_TRACK_UPLOAD_RESPONSE;
    buffer[4] = subcommand & 0x7F;    // Upload subcommand (0=START, 1=CHUNK, 2=COMPLETE)
    buffer[5] = slot & 0x0F;          // Slot number (0-15)
    buffer[6] = status & 0x7F;        // Status: 0=success, 1=error, 2=chunk received
    buffer[7] = SYSEX_END;

    return 8;
}

// --- Helper Functions ---

const char* sysex_command_name(SysExCommand cmd) {
    switch (cmd) {
        case SYSEX_CMD_PING:           return "PING";
        case SYSEX_CMD_FILE_LOAD:      return "FILE_LOAD";
        case SYSEX_CMD_PLAY:           return "PLAY";
        case SYSEX_CMD_STOP:           return "STOP";
        case SYSEX_CMD_PAUSE:          return "PAUSE";
        case SYSEX_CMD_CHANNEL_MUTE:   return "CHANNEL_MUTE";
        case SYSEX_CMD_CHANNEL_SOLO:   return "CHANNEL_SOLO";
        case SYSEX_CMD_CHANNEL_VOLUME: return "CHANNEL_VOLUME";
        case SYSEX_CMD_MASTER_VOLUME:  return "MASTER_VOLUME";
        case SYSEX_CMD_MASTER_MUTE:    return "MASTER_MUTE";
        case SYSEX_CMD_CHANNEL_FX_ENABLE: return "CHANNEL_FX_ENABLE";
        case SYSEX_CMD_SET_POSITION:   return "SET_POSITION";
        case SYSEX_CMD_SET_BPM:        return "SET_BPM";
        case SYSEX_CMD_TRIGGER_PAD:    return "TRIGGER_PAD";
        case SYSEX_CMD_CHANNEL_PANNING: return "CHANNEL_PANNING";
        case SYSEX_CMD_MASTER_PANNING: return "MASTER_PANNING";
        case SYSEX_CMD_SEQUENCE_TRACK_UPLOAD: return "SEQUENCE_TRACK_UPLOAD";
        case SYSEX_CMD_SEQUENCE_TRACK_UPLOAD_RESPONSE: return "SEQUENCE_TRACK_UPLOAD_RESPONSE";
        case SYSEX_CMD_SEQUENCE_TRACK_PLAY:  return "SEQUENCE_TRACK_PLAY";
        case SYSEX_CMD_SEQUENCE_TRACK_STOP:  return "SEQUENCE_TRACK_STOP";
        case SYSEX_CMD_SEQUENCE_TRACK_MUTE:  return "SEQUENCE_TRACK_MUTE";
        case SYSEX_CMD_SEQUENCE_TRACK_SOLO:  return "SEQUENCE_TRACK_SOLO";
        case SYSEX_CMD_SEQUENCE_TRACK_GET_STATE: return "SEQUENCE_TRACK_GET_STATE";
        case SYSEX_CMD_SEQUENCE_TRACK_STATE_RESPONSE: return "SEQUENCE_TRACK_STATE_RESPONSE";
        case SYSEX_CMD_SEQUENCE_TRACK_CLEAR: return "SEQUENCE_TRACK_CLEAR";
        case SYSEX_CMD_SEQUENCE_TRACK_LIST:  return "SEQUENCE_TRACK_LIST";
        case SYSEX_CMD_SEQUENCE_TRACK_DOWNLOAD: return "SEQUENCE_TRACK_DOWNLOAD";
        case SYSEX_CMD_SEQUENCE_TRACK_DOWNLOAD_RESPONSE: return "SEQUENCE_TRACK_DOWNLOAD_RESPONSE";
        case SYSEX_CMD_FX_EFFECT_GET:  return "FX_EFFECT_GET";
        case SYSEX_CMD_FX_EFFECT_SET:  return "FX_EFFECT_SET";
        case SYSEX_CMD_FX_GET_ALL_STATE: return "FX_GET_ALL_STATE";
        case SYSEX_CMD_FX_STATE_RESPONSE: return "FX_STATE_RESPONSE";
        default:                       return "UNKNOWN";
    }
}

int sysex_is_valid_device_id(uint8_t device_id) {
    return device_id <= 0x7F;
}
