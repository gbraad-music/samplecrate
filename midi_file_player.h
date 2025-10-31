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

// Callback type for loop restart events
// Parameters: userdata
typedef void (*MidiFileLoopCallback)(void* userdata);

// Create a new MIDI file player
MidiFilePlayer* midi_file_player_create(void);

// Destroy a MIDI file player
void midi_file_player_destroy(MidiFilePlayer* player);

// Load a MIDI file from disk
// Returns 0 on success, -1 on error
int midi_file_player_load(MidiFilePlayer* player, const char* filename);

// Start playback (resets to beginning)
void midi_file_player_play(MidiFilePlayer* player);

// Schedule playback to start on a specific beat (for quantization)
// current_beat: the current beat number
// Returns the beat number when playback will actually start
int midi_file_player_play_quantized(MidiFilePlayer* player, int current_beat, int quantize_beats);

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

// Set the loop restart callback (called when loop restarts)
void midi_file_player_set_loop_callback(MidiFilePlayer* player, MidiFileLoopCallback callback, void* userdata);

// Set looping mode (default: off)
void midi_file_player_set_loop(MidiFilePlayer* player, int loop);

// Get looping mode
int midi_file_player_get_loop(MidiFilePlayer* player);

// Update playback (call regularly from main loop)
// delta_ms: time elapsed since last update in milliseconds
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_file_player_update(MidiFilePlayer* player, float delta_ms, int current_beat);

// Update playback with sample-accurate timing (call from audio callback)
// num_samples: number of audio samples to advance
// sample_rate: audio sample rate in Hz (e.g., 44100)
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_file_player_update_samples(MidiFilePlayer* player, int num_samples, int sample_rate, int current_beat);

// Get current playback position in seconds
float midi_file_player_get_position(MidiFilePlayer* player);

// Seek to a position in seconds
void midi_file_player_seek(MidiFilePlayer* player, float seconds);

// Get total duration in seconds
float midi_file_player_get_duration(MidiFilePlayer* player);

// Sync the player's start_beat reference to current MIDI clock pulse
// Call this when SPP (Song Position Pointer) changes the absolute pulse count
// to prevent the player from calculating incorrect elapsed time
void midi_file_player_sync_start_beat(MidiFilePlayer* player, int current_pulse);

#ifdef __cplusplus
}
#endif

#endif // MIDI_FILE_PLAYER_H
