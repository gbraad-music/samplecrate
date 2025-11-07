#ifndef MEDNESS_SEQUENCE_H
#define MEDNESS_SEQUENCE_H

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for sequence player
typedef struct MednessSequence MednessSequence;

// Callback type for MIDI sequence events
// Parameters: note, velocity, on (1=note_on, 0=note_off), userdata
typedef void (*MednessSequenceEventCallback)(int note, int velocity, int on, void* userdata);

// Callback type for phrase change events
// Parameters: phrase_index, phrase_name, userdata
typedef void (*MednessSequencePhraseChangeCallback)(int phrase_index, const char* phrase_name, void* userdata);

// Forward declaration for MednessSequencer
typedef struct MednessSequencer MednessSequencer;

// Create a new sequence player
MednessSequence* medness_sequence_create(void);

// Destroy a sequence player
void medness_sequence_destroy(MednessSequence* player);

// Set the sequencer reference and slot number (must be called before use)
void medness_sequence_set_sequencer(MednessSequence* player, MednessSequencer* sequencer, int slot);

// Add a phrase to the sequence
// filename: path to MIDI file
// loop_count: how many times to play this phrase before moving to next
//             -1 or 0 = infinite loop (default - never auto-advance)
//             N > 0 = loop N times then move to next phrase
// name: optional name for this phrase (can be NULL)
// Returns phrase index on success, -1 on error
int medness_sequence_add_phrase(MednessSequence* player, const char* filename, int loop_count, const char* name);

// Clear all phrases from the sequence
void medness_sequence_clear_phrases(MednessSequence* player);

// Get number of phrases in the sequence
int medness_sequence_get_phrase_count(MednessSequence* player);

// Get current phrase index
int medness_sequence_get_current_phrase(MednessSequence* player);

// Get current phrase loop count (how many times current phrase has looped)
int medness_sequence_get_current_phrase_loop(MednessSequence* player);

// Start playback (resets to beginning of sequence)
void medness_sequence_play(MednessSequence* player);

// Stop playback
void medness_sequence_stop(MednessSequence* player);

// Check if currently playing
int medness_sequence_is_playing(MednessSequence* player);

// Set tempo in BPM (default: 125)
void medness_sequence_set_tempo(MednessSequence* player, float bpm);

// Get current tempo
float medness_sequence_get_tempo(MednessSequence* player);

// Set the MIDI event callback
void medness_sequence_set_callback(MednessSequence* player, MednessSequenceEventCallback callback, void* userdata);

// Set the phrase change callback (called when advancing to next phrase)
void medness_sequence_set_phrase_change_callback(MednessSequence* player, MednessSequencePhraseChangeCallback callback, void* userdata);

// Set sequence looping mode (default: on)
// When on, sequence restarts from first phrase after completing
// When off, playback stops after last phrase
void medness_sequence_set_loop(MednessSequence* player, int loop);

// Get sequence looping mode
int medness_sequence_get_loop(MednessSequence* player);

// Update playback (call regularly from main loop)
// delta_ms: time elapsed since last update in milliseconds
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void medness_sequence_update(MednessSequence* player, float delta_ms, int current_beat);

// Update playback with sample-accurate timing (call from audio callback)
// num_samples: number of audio samples to advance
// sample_rate: audio sample rate in Hz (e.g., 44100)
// current_beat: current MIDI clock beat number (for quantization, -1 if no MIDI clock)
void medness_sequence_update_samples(MednessSequence* player, int num_samples, int sample_rate, int current_beat);

// Jump to a specific phrase in the sequence
// phrase_index: index of phrase to jump to (0-based)
// Returns 0 on success, -1 on error
int medness_sequence_jump_to_phrase(MednessSequence* player, int phrase_index);

// Get total duration of current phrase in seconds
float medness_sequence_get_current_phrase_duration(MednessSequence* player);

// Get current position within current phrase in seconds
float medness_sequence_get_current_phrase_position(MednessSequence* player);

#ifdef __cplusplus
}
#endif

#endif // MEDNESS_SEQUENCE_H
