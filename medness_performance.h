#ifndef MEDNESS_PERFORMANCE_H
#define MEDNESS_PERFORMANCE_H

#include "medness_sequence.h"
#include "samplecrate_rsx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct MednessSequencer MednessSequencer;

// Start mode for sequences
// SIMPLE MODEL:
//   queued=0 → Start immediately from current position (jump in)
//   queued=1 → Wait for next bar boundary (96-pulse boundary)
typedef enum {
    SEQUENCE_START_IMMEDIATE = 0,  // queued=0: Start now, jump into current position
    SEQUENCE_START_QUANTIZED = 1   // queued=1: Wait for next bar (96-pulse boundary)
} SequenceStartMode;

// Opaque handle for sequence manager
typedef struct MednessPerformance MednessPerformance;

// Create a sequence manager
MednessPerformance* medness_performance_create(void);

// Destroy the sequence manager
void medness_performance_destroy(MednessPerformance* manager);

// Load sequences from an RSX file
// rsx_path: path to the RSX file (for resolving relative MIDI paths)
// rsx: loaded RSX structure
// Returns: number of sequences loaded, or -1 on error
int medness_performance_load_from_rsx(MednessPerformance* manager,
                                        const char* rsx_path,
                                        SamplecrateRSX* rsx);

// Clear all sequences
void medness_performance_clear(MednessPerformance* manager);

// Set the sequencer reference (must be called before loading sequences)
void medness_performance_set_sequencer(MednessPerformance* manager, MednessSequencer* sequencer);

// Get number of sequences loaded
int medness_performance_get_count(MednessPerformance* manager);

// Play a specific sequence
void medness_performance_play(MednessPerformance* manager, int seq_index, int current_pulse);

// Stop a specific sequence
void medness_performance_stop(MednessPerformance* manager, int seq_index);

// Stop all sequences
void medness_performance_stop_all(MednessPerformance* manager);

// Check if a sequence is playing
int medness_performance_is_playing(MednessPerformance* manager, int seq_index);

// Set the start mode (immediate, quantized, etc.)
void medness_performance_set_start_mode(MednessPerformance* manager, SequenceStartMode mode);

// Get the current start mode
SequenceStartMode medness_performance_get_start_mode(MednessPerformance* manager);

// Set tempo for all sequences
void medness_performance_set_tempo(MednessPerformance* manager, float bpm);

// Set MIDI event callback for all sequences
void medness_performance_set_midi_callback(MednessPerformance* manager,
                                             MednessSequenceEventCallback callback,
                                             void* userdata);

// Set phrase change callback for all sequences
void medness_performance_set_phrase_change_callback(MednessPerformance* manager,
                                                      MednessSequencePhraseChangeCallback callback,
                                                      void* userdata);

// Update all sequences (call from audio callback)
void medness_performance_update_samples(MednessPerformance* manager,
                                         int num_samples,
                                         int sample_rate,
                                         int current_pulse);

// Jump to a specific phrase in a sequence
void medness_performance_jump_to_phrase(MednessPerformance* manager,
                                         int seq_index,
                                         int phrase_index);

// Get the underlying sequence player for direct access
MednessSequence* medness_performance_get_player(MednessPerformance* manager, int seq_index);

// Load a pad with a MIDI file as a single-phrase sequence
// pad_index: 0-31 (pads use sequence slots 0-31)
// midi_file: absolute path to MIDI file
// callback_userdata: userdata to pass to MIDI callback (should contain pad_index and program info)
// Returns: 0 on success, -1 on error
int medness_performance_load_pad(MednessPerformance* manager,
                                   int pad_index,
                                   const char* midi_file,
                                   void* callback_userdata);

// Unload a pad sequence
void medness_performance_unload_pad(MednessPerformance* manager, int pad_index);

#ifdef __cplusplus
}
#endif

#endif // MEDNESS_PERFORMANCE_H
