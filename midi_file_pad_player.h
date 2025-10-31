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

// Trigger playback for a specific pad
void midi_file_pad_player_trigger(MidiFilePadPlayer* pad_player, int pad_index);

// Stop playback for a specific pad
void midi_file_pad_player_stop(MidiFilePadPlayer* pad_player, int pad_index);

// Stop all playback
void midi_file_pad_player_stop_all(MidiFilePadPlayer* pad_player);

// Set the MIDI event callback for all pad players
void midi_file_pad_player_set_callback(MidiFilePadPlayer* pad_player, MidiFileEventCallback callback, void* userdata);

// Set tempo for all pad players
void midi_file_pad_player_set_tempo(MidiFilePadPlayer* pad_player, float bpm);

// Set looping for all pad players
void midi_file_pad_player_set_loop(MidiFilePadPlayer* pad_player, int loop);

// Update all active pad players (call from main loop)
void midi_file_pad_player_update_all(MidiFilePadPlayer* pad_player, float delta_ms);

// Check if a specific pad is playing
int midi_file_pad_player_is_playing(MidiFilePadPlayer* pad_player, int pad_index);

#ifdef __cplusplus
}
#endif

#endif // MIDI_FILE_PAD_PLAYER_H
