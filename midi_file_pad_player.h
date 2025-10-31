#ifndef MIDI_FILE_PAD_PLAYER_H
#define MIDI_FILE_PAD_PLAYER_H

#include "midi_file_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PAD_PLAYERS 32

// Opaque handle for pad player manager
typedef struct MidiFilePadPlayer MidiFilePadPlayer;

// Create a new pad player manager
MidiFilePadPlayer* midi_file_pad_player_create(void);

// Destroy a pad player manager
void midi_file_pad_player_destroy(MidiFilePadPlayer* pad_player);

// Load a MIDI file for a specific pad (0-31)
// Returns 0 on success, -1 on error
// The userdata parameter will be passed to the callback for this specific pad
int midi_file_pad_player_load(MidiFilePadPlayer* pad_player, int pad_index, const char* filename, void* userdata);

// Unload MIDI file for a specific pad
void midi_file_pad_player_unload(MidiFilePadPlayer* pad_player, int pad_index);

// Trigger playback for a specific pad (immediately, no quantization)
void midi_file_pad_player_trigger(MidiFilePadPlayer* pad_player, int pad_index);

// Trigger playback for a specific pad with quantization
// current_beat: current MIDI clock beat number
// quantize_beats: quantize to this many beats (e.g., 1=quarter note, 4=one bar)
void midi_file_pad_player_trigger_quantized(MidiFilePadPlayer* pad_player, int pad_index, int current_beat, int quantize_beats);

// Stop playback for a specific pad
void midi_file_pad_player_stop(MidiFilePadPlayer* pad_player, int pad_index);

// Stop all playback
void midi_file_pad_player_stop_all(MidiFilePadPlayer* pad_player);

// Set the MIDI event callback for all pad players
void midi_file_pad_player_set_callback(MidiFilePadPlayer* pad_player, MidiFileEventCallback callback, void* userdata);

// Set the loop restart callback for all pad players
void midi_file_pad_player_set_loop_callback(MidiFilePadPlayer* pad_player, MidiFileLoopCallback callback, void* userdata);

// Set tempo for all pad players
void midi_file_pad_player_set_tempo(MidiFilePadPlayer* pad_player, float bpm);

// Set looping for all pad players
void midi_file_pad_player_set_loop(MidiFilePadPlayer* pad_player, int loop);

// Update all active pad players (call from main loop)
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_file_pad_player_update_all(MidiFilePadPlayer* pad_player, float delta_ms, int current_beat);

// Update all active pad players with sample-accurate timing (call from audio callback)
// num_samples: number of audio samples to advance
// sample_rate: audio sample rate in Hz (e.g., 44100)
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_file_pad_player_update_all_samples(MidiFilePadPlayer* pad_player, int num_samples, int sample_rate, int current_beat);

// Check if a specific pad is playing
int midi_file_pad_player_is_playing(MidiFilePadPlayer* pad_player, int pad_index);

// Sync a specific pad's timing reference to current MIDI clock pulse
// Call this when SPP arrives to adjust timing without resetting playback position
// This prevents drift while maintaining continuous playback
void midi_file_pad_player_sync_pad(MidiFilePadPlayer* pad_player, int pad_index, int current_pulse);

// Sync all playing pads to current MIDI clock pulse
// Convenience function to sync all active pads at once (e.g., when SPP arrives)
void midi_file_pad_player_sync_all(MidiFilePadPlayer* pad_player, int current_pulse);

#ifdef __cplusplus
}
#endif

#endif // MIDI_FILE_PAD_PLAYER_H
