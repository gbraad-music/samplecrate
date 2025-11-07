#ifndef MIDI_FILE_PAD_PLAYER_H
#define MIDI_FILE_PAD_PLAYER_H

#include "medness_track.h"
#include "medness_sequencer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PAD_PLAYERS 32

// Opaque handle for pad player manager
// Now just manages tracks - the sequencer does the actual playing
typedef struct MidiFilePadPlayer MidiFilePadPlayer;

// Per-pad callback context (opaque in C, but accessible via helper)
typedef struct PadCallbackContext PadCallbackContext;

// MIDI event callback (same as before for compatibility)
// NOTE: For pad events, userdata will be a PadCallbackContext*
// For sequence events, userdata will be an int* pointing to program number
typedef void (*MidiFileEventCallback)(int note, int velocity, int on, void* userdata);

// Loop callback (same as before for compatibility)
typedef void (*MidiFileLoopCallback)(void* userdata);

// Helper: Check if userdata is a pad context (vs sequence program number)
// Returns 1 if pad context, 0 otherwise
int midi_file_is_pad_context(void* userdata);

// Helper: Get pad index from pad context
// Returns pad index (0-31) or -1 if invalid
int midi_file_get_pad_index(void* userdata);

// Create a new pad player manager
// sequencer: the sequencer that will play the tracks
MidiFilePadPlayer* midi_file_pad_player_create(MednessSequencer* sequencer);

// Destroy a pad player manager
void midi_file_pad_player_destroy(MidiFilePadPlayer* pad_player);

// Load a MIDI file for a specific pad (0-31)
// Returns 0 on success, -1 on error
int midi_file_pad_player_load(MidiFilePadPlayer* pad_player, int pad_index, const char* filename, void* userdata);

// Unload MIDI file for a specific pad
void midi_file_pad_player_unload(MidiFilePadPlayer* pad_player, int pad_index);

// Trigger playback for a specific pad (adds track to sequencer)
void midi_file_pad_player_trigger(MidiFilePadPlayer* pad_player, int pad_index);

// Stop playback for a specific pad (removes track from sequencer)
void midi_file_pad_player_stop(MidiFilePadPlayer* pad_player, int pad_index);

// Stop all playback
void midi_file_pad_player_stop_all(MidiFilePadPlayer* pad_player);

// Set the MIDI event callback for all pads
void midi_file_pad_player_set_callback(MidiFilePadPlayer* pad_player, MidiFileEventCallback callback, void* userdata);

// Set the loop restart callback for all pads
void midi_file_pad_player_set_loop_callback(MidiFilePadPlayer* pad_player, MidiFileLoopCallback callback, void* userdata);

// Set tempo (delegates to sequencer)
void midi_file_pad_player_set_tempo(MidiFilePadPlayer* pad_player, float bpm);

// Set looping (no longer needed - sequencer always loops)
void midi_file_pad_player_set_loop(MidiFilePadPlayer* pad_player, int loop);

// Update function - no longer needed (sequencer handles updates)
// Kept for compatibility but does nothing
void midi_file_pad_player_update_all_samples(MidiFilePadPlayer* pad_player, int num_samples, int sample_rate, int current_beat);

// Check if a specific pad is playing
int midi_file_pad_player_is_playing(MidiFilePadPlayer* pad_player, int pad_index);

// Get the track for a specific pad (for display/visualization)
// Returns NULL if pad_index is invalid or no track loaded
MednessTrack* midi_file_pad_player_get_track(MidiFilePadPlayer* pad_player, int pad_index);

#ifdef __cplusplus
}
#endif

#endif // MIDI_FILE_PAD_PLAYER_H
