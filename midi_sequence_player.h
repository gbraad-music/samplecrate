#ifndef MIDI_SEQUENCE_PLAYER_H
#define MIDI_SEQUENCE_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for MIDI sequence player
typedef struct MidiSequencePlayer MidiSequencePlayer;

// Callback type for MIDI sequence events
// Parameters: note, velocity, on (1=note_on, 0=note_off), userdata
typedef void (*MidiSequenceEventCallback)(int note, int velocity, int on, void* userdata);

// Callback type for phrase change events
// Parameters: phrase_index, phrase_name, userdata
typedef void (*MidiSequencePhraseChangeCallback)(int phrase_index, const char* phrase_name, void* userdata);

// Create a new MIDI sequence player
MidiSequencePlayer* midi_sequence_player_create(void);

// Destroy a MIDI sequence player
void midi_sequence_player_destroy(MidiSequencePlayer* player);

// Add a phrase to the sequence
// filename: path to MIDI file
// loop_count: how many times to play this phrase before moving to next
//             -1 or 0 = infinite loop (default - never auto-advance)
//             N > 0 = loop N times then move to next phrase
// name: optional name for this phrase (can be NULL)
// Returns phrase index on success, -1 on error
int midi_sequence_player_add_phrase(MidiSequencePlayer* player, const char* filename, int loop_count, const char* name);

// Clear all phrases from the sequence
void midi_sequence_player_clear_phrases(MidiSequencePlayer* player);

// Get number of phrases in the sequence
int midi_sequence_player_get_phrase_count(MidiSequencePlayer* player);

// Get current phrase index
int midi_sequence_player_get_current_phrase(MidiSequencePlayer* player);

// Get current phrase loop count (how many times current phrase has looped)
int midi_sequence_player_get_current_phrase_loop(MidiSequencePlayer* player);

// Start playback (resets to beginning of sequence)
void midi_sequence_player_play(MidiSequencePlayer* player);

// Stop playback
void midi_sequence_player_stop(MidiSequencePlayer* player);

// Check if currently playing
int midi_sequence_player_is_playing(MidiSequencePlayer* player);

// Set tempo in BPM (default: 125)
void midi_sequence_player_set_tempo(MidiSequencePlayer* player, float bpm);

// Get current tempo
float midi_sequence_player_get_tempo(MidiSequencePlayer* player);

// Set the MIDI event callback
void midi_sequence_player_set_callback(MidiSequencePlayer* player, MidiSequenceEventCallback callback, void* userdata);

// Set the phrase change callback (called when advancing to next phrase)
void midi_sequence_player_set_phrase_change_callback(MidiSequencePlayer* player, MidiSequencePhraseChangeCallback callback, void* userdata);

// Set sequence looping mode (default: on)
// When on, sequence restarts from first phrase after completing
// When off, playback stops after last phrase
void midi_sequence_player_set_loop(MidiSequencePlayer* player, int loop);

// Get sequence looping mode
int midi_sequence_player_get_loop(MidiSequencePlayer* player);

// Update playback (call regularly from main loop)
// delta_ms: time elapsed since last update in milliseconds
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_sequence_player_update(MidiSequencePlayer* player, float delta_ms, int current_beat);

// Update playback with sample-accurate timing (call from audio callback)
// num_samples: number of audio samples to advance
// sample_rate: audio sample rate in Hz (e.g., 44100)
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void midi_sequence_player_update_samples(MidiSequencePlayer* player, int num_samples, int sample_rate, int current_beat);

// Jump to a specific phrase in the sequence
// phrase_index: index of phrase to jump to (0-based)
// Returns 0 on success, -1 on error
int midi_sequence_player_jump_to_phrase(MidiSequencePlayer* player, int phrase_index);

// Get total duration of current phrase in seconds
float midi_sequence_player_get_current_phrase_duration(MidiSequencePlayer* player);

// Get current position within current phrase in seconds
float midi_sequence_player_get_current_phrase_position(MidiSequencePlayer* player);

#ifdef __cplusplus
}
#endif

#endif // MIDI_SEQUENCE_PLAYER_H
