#ifndef MIDI_SYSEX_H
#define MIDI_SYSEX_H

#include <stddef.h>
#include <stdint.h>

// SysEx Message Format for Regroove Inter-Instance Communication
// F0 7D <device_id> <command> [<data>...] F7
//
// F0 = SysEx Start
// 7D = Manufacturer ID (Educational/Research use)
// <device_id> = 0-127, identifies target Regroove instance
// <command> = Command byte (see below)
// [<data>...] = Variable-length command data
// F7 = SysEx End

// Manufacturer ID for educational/research/non-commercial use
#define SYSEX_MANUFACTURER_ID 0x7D

// SysEx Start/End bytes
#define SYSEX_START 0xF0
#define SYSEX_END   0xF7

// Special device IDs
#define SYSEX_DEVICE_BROADCAST 0x7F  // Broadcast to all devices
#define SYSEX_DEVICE_ANY       0x7E  // Accept from any device (for receiving)

// SysEx Command Codes
typedef enum {
    SYSEX_CMD_PING              = 0x01,  // Device discovery/heartbeat
    SYSEX_CMD_FILE_LOAD         = 0x10,  // Load file by name
    SYSEX_CMD_PLAY              = 0x20,  // Start playback
    SYSEX_CMD_STOP              = 0x21,  // Stop playback
    SYSEX_CMD_PAUSE             = 0x22,  // Pause/Continue
    SYSEX_CMD_CHANNEL_MUTE      = 0x30,  // Mute/unmute channel/program
    SYSEX_CMD_CHANNEL_SOLO      = 0x31,  // Solo/unsolo channel/program
    SYSEX_CMD_CHANNEL_VOLUME    = 0x32,  // Set channel/program volume
    SYSEX_CMD_MASTER_VOLUME     = 0x33,  // Set master output volume
    SYSEX_CMD_MASTER_MUTE       = 0x34,  // Set master mute
    SYSEX_CMD_CHANNEL_FX_ENABLE = 0x38,  // Enable/disable FX chain per channel/program
    SYSEX_CMD_SET_POSITION      = 0x40,  // Jump to position
    SYSEX_CMD_SET_BPM           = 0x41,  // Set tempo
    SYSEX_CMD_TRIGGER_PAD       = 0x50,  // Trigger a pad action
    SYSEX_CMD_CHANNEL_PANNING   = 0x58,  // Set channel/program panning
    SYSEX_CMD_MASTER_PANNING    = 0x59,  // Set master panning
    // Effects control (per-program)
    SYSEX_CMD_FX_EFFECT_GET     = 0x70,  // Get effect parameters by effect ID
    SYSEX_CMD_FX_EFFECT_SET     = 0x71,  // Set effect parameters by effect ID
    SYSEX_CMD_FX_GET_ALL_STATE  = 0x7E,  // Request complete effects state
    SYSEX_CMD_FX_STATE_RESPONSE = 0x7F,  // Complete effects state response
} SysExCommand;

// Effect IDs for FX_EFFECT_GET/SET commands
typedef enum {
    SYSEX_FX_DISTORTION = 0x00,  // Distortion (drive, mix)
    SYSEX_FX_FILTER     = 0x01,  // Filter (cutoff, resonance)
    SYSEX_FX_EQ         = 0x02,  // EQ (low, mid, high)
    SYSEX_FX_COMPRESSOR = 0x03,  // Compressor (threshold, ratio, attack, release, makeup)
    SYSEX_FX_DELAY      = 0x04,  // Delay (time, feedback, mix)
    SYSEX_FX_RESERVED_1 = 0x05,  // Reserved for future effect
    SYSEX_FX_RESERVED_2 = 0x06,  // Reserved for future effect
} SysExEffectID;

// SysEx message callback
// Called when a valid Regroove SysEx message is received
// device_id: sender's device ID
// command: command code
// data: command data bytes
// data_len: length of data
typedef void (*SysExCallback)(uint8_t device_id, SysExCommand command,
                              const uint8_t *data, size_t data_len, void *userdata);

// Initialize SysEx system with this device's ID
void sysex_init(uint8_t device_id);

// Set device ID (0-127)
void sysex_set_device_id(uint8_t device_id);

// Get current device ID
uint8_t sysex_get_device_id(void);

// Register callback for incoming SysEx commands
void sysex_register_callback(SysExCallback callback, void *userdata);

// Parse incoming MIDI message - returns 1 if it was a valid Regroove SysEx message
int sysex_parse_message(const uint8_t *msg, size_t msg_len);

// --- SysEx Message Building Functions ---

// Build PING message
// Returns message length, fills buffer
size_t sysex_build_ping(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size);

// Build FILE_LOAD message
// filename: null-terminated filename string
size_t sysex_build_file_load(uint8_t target_device_id, const char *filename,
                              uint8_t *buffer, size_t buffer_size);

// Build PLAY message
size_t sysex_build_play(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size);

// Build STOP message
size_t sysex_build_stop(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size);

// Build PAUSE message
size_t sysex_build_pause(uint8_t target_device_id, uint8_t *buffer, size_t buffer_size);

// Build CHANNEL_MUTE message
// channel: channel index (0-63)
// mute: 1 = mute, 0 = unmute
size_t sysex_build_channel_mute(uint8_t target_device_id, uint8_t channel, uint8_t mute,
                                 uint8_t *buffer, size_t buffer_size);

// Build CHANNEL_SOLO message
// channel: channel index (0-63)
// solo: 1 = solo, 0 = unsolo
size_t sysex_build_channel_solo(uint8_t target_device_id, uint8_t channel, uint8_t solo,
                                 uint8_t *buffer, size_t buffer_size);

// Build CHANNEL_VOLUME message
// channel: channel index (0-63)
// volume: volume level (0-127)
size_t sysex_build_channel_volume(uint8_t target_device_id, uint8_t channel, uint8_t volume,
                                   uint8_t *buffer, size_t buffer_size);

// Build SET_POSITION message
// position: position in rows (16-bit value, sent as two 7-bit bytes)
size_t sysex_build_set_position(uint8_t target_device_id, uint16_t position,
                                 uint8_t *buffer, size_t buffer_size);

// Build SET_BPM message
// bpm: tempo in BPM (16-bit value, sent as two 7-bit bytes)
size_t sysex_build_set_bpm(uint8_t target_device_id, uint16_t bpm,
                           uint8_t *buffer, size_t buffer_size);

// Build TRIGGER_PAD message
// pad_index: pad number (0-31)
size_t sysex_build_trigger_pad(uint8_t target_device_id, uint8_t pad_index,
                                uint8_t *buffer, size_t buffer_size);

// --- Effects Control Functions ---

// Build FX_EFFECT_GET message
// Requests effect parameters for a specific effect ID on a program
// program_id: Program/pad ID (0-31 for samplecrate pads)
// effect_id: SYSEX_FX_DISTORTION (0x00) through SYSEX_FX_DELAY (0x04)
// Response: Device sends FX_EFFECT_SET message with current parameters
size_t sysex_build_fx_effect_get(uint8_t target_device_id,
                                  uint8_t program_id,
                                  uint8_t effect_id,
                                  uint8_t *buffer, size_t buffer_size);

// Build FX_EFFECT_SET message
// Sets effect parameters for a specific effect ID on a program
// program_id: Program/pad ID (0-31 for samplecrate pads)
// effect_id: Effect to control (0x00-0x06)
// enabled: 0 = disabled, 1 = enabled
// params: Effect-specific parameters (see REGROOVE_EFFECTS_SYSEX.md)
// param_count: Number of parameters (2-5 depending on effect)
// Variable-length: parameter count depends on effect_id
// - DISTORTION (0x00): 2 params (drive, mix)
// - FILTER (0x01): 2 params (cutoff, resonance)
// - EQ (0x02): 3 params (low, mid, high)
// - COMPRESSOR (0x03): 5 params (threshold, ratio, attack, release, makeup)
// - DELAY (0x04): 3 params (time, feedback, mix)
size_t sysex_build_fx_effect_set(uint8_t target_device_id,
                                  uint8_t program_id,
                                  uint8_t effect_id,
                                  uint8_t enabled,
                                  const uint8_t *params,
                                  size_t param_count,
                                  uint8_t *buffer, size_t buffer_size);

// Build FX_GET_ALL_STATE message
// Requests complete effects state for a program
// program_id: Program/pad ID (0-31 for samplecrate pads)
size_t sysex_build_fx_get_all_state(uint8_t target_device_id,
                                     uint8_t program_id,
                                     uint8_t *buffer, size_t buffer_size);

// Build FX_STATE_RESPONSE message
// Sends complete effects state (32 bytes fixed size, version 1)
// program_id: Program/pad ID (0-31)
// version: Format version (0x01)
// fx_route: FX routing (not used in samplecrate, always per-program)
// enable_flags: Bit-packed enable flags (bit 0=distortion, 1=filter, 2=EQ, 3=compressor, 4=delay)
// distortion_params: 2 bytes (drive, mix)
// filter_params: 2 bytes (cutoff, resonance)
// eq_params: 3 bytes (low, mid, high)
// compressor_params: 5 bytes (threshold, ratio, attack, release, makeup)
// delay_params: 3 bytes (time, feedback, mix)
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
                                      uint8_t *buffer, size_t buffer_size);

// Parse FX_STATE_RESPONSE message
// Extracts complete effects state from received message
// Returns 1 on success, 0 on failure
// All param buffers must be allocated before calling (see sizes above)
int sysex_parse_fx_state_response(const uint8_t *data, size_t data_len,
                                   uint8_t *out_program_id,
                                   uint8_t *out_version,
                                   uint8_t *out_fx_route,
                                   uint8_t *out_enable_flags,
                                   uint8_t *out_distortion_params,  // 2 bytes
                                   uint8_t *out_filter_params,      // 2 bytes
                                   uint8_t *out_eq_params,          // 3 bytes
                                   uint8_t *out_compressor_params,  // 5 bytes
                                   uint8_t *out_delay_params);      // 3 bytes

// --- Helper Functions ---

// Get command name for debugging
const char* sysex_command_name(SysExCommand cmd);

// Validate device ID
int sysex_is_valid_device_id(uint8_t device_id);

#endif // MIDI_SYSEX_H
