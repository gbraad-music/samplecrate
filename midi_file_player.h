#ifndef MIDI_FILE_PLAYER_H
#define MIDI_FILE_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for MIDI file player
typedef struct MidiFilePlayer MidiFilePlayer;

// Callback type for MIDI file playback events
// Parameters: note, velocity, on (1=note_on, 0=note_off), userdata
typedef void (*MidiFileEventCallback)(int note, int velocity, int on, void* userdata);

// Create a new MIDI file player
MidiFilePlayer* midi_file_player_create(void);

// Destroy a MIDI file player
void midi_file_player_destroy(MidiFilePlayer* player);

// Load a MIDI file from disk
// Returns 0 on success, -1 on error
int midi_file_player_load(MidiFilePlayer* player, const char* filename);

// Start playback (resets to beginning)
void midi_file_player_play(MidiFilePlayer* player);

// Stop playback
void midi_file_player_stop(MidiFilePlayer* player);

// Check if currently playing
int midi_file_player_is_playing(MidiFilePlayer* player);

// Set tempo in BPM (default: 125)
void midi_file_player_set_tempo(MidiFilePlayer* player, float bpm);

// Get current tempo
float midi_file_player_get_tempo(MidiFilePlayer* player);

// Set the MIDI event callback
void midi_file_player_set_callback(MidiFilePlayer* player, MidiFileEventCallback callback, void* userdata);

// Set looping mode (default: off)
void midi_file_player_set_loop(MidiFilePlayer* player, int loop);

// Get looping mode
int midi_file_player_get_loop(MidiFilePlayer* player);

// Update playback (call regularly from main loop)
// delta_ms: time elapsed since last update in milliseconds
void midi_file_player_update(MidiFilePlayer* player, float delta_ms);

// Get current playback position in seconds
float midi_file_player_get_position(MidiFilePlayer* player);

// Seek to a position in seconds
void midi_file_player_seek(MidiFilePlayer* player, float seconds);

// Get total duration in seconds
float midi_file_player_get_duration(MidiFilePlayer* player);

#ifdef __cplusplus
}
#endif

#endif // MIDI_FILE_PLAYER_H
