#ifndef MIDI_SEQUENCE_MANAGER_H
#define MIDI_SEQUENCE_MANAGER_H

#include "midi_sequence_player.h"
#include "samplecrate_rsx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start mode for sequences
// SIMPLE MODEL:
//   queued=0 → Start immediately from current position (jump in)
//   queued=1 → Wait for next bar boundary (96-pulse boundary)
typedef enum {
    SEQUENCE_START_IMMEDIATE = 0,  // queued=0: Start now, jump into current position
    SEQUENCE_START_QUANTIZED = 1   // queued=1: Wait for next bar (96-pulse boundary)
} SequenceStartMode;

// Opaque handle for sequence manager
typedef struct MidiSequenceManager MidiSequenceManager;

// Create a sequence manager
MidiSequenceManager* midi_sequence_manager_create(void);

// Destroy the sequence manager
void midi_sequence_manager_destroy(MidiSequenceManager* manager);

// Load sequences from an RSX file
// rsx_path: path to the RSX file (for resolving relative MIDI paths)
// rsx: loaded RSX structure
// Returns: number of sequences loaded, or -1 on error
int midi_sequence_manager_load_from_rsx(MidiSequenceManager* manager,
                                        const char* rsx_path,
                                        SamplecrateRSX* rsx);

// Clear all sequences
void midi_sequence_manager_clear(MidiSequenceManager* manager);

// Get number of sequences loaded
int midi_sequence_manager_get_count(MidiSequenceManager* manager);

// Play a specific sequence
void midi_sequence_manager_play(MidiSequenceManager* manager, int seq_index, int current_pulse);

// Stop a specific sequence
void midi_sequence_manager_stop(MidiSequenceManager* manager, int seq_index);

// Stop all sequences
void midi_sequence_manager_stop_all(MidiSequenceManager* manager);

// Check if a sequence is playing
int midi_sequence_manager_is_playing(MidiSequenceManager* manager, int seq_index);

// Set the start mode (immediate, quantized, etc.)
void midi_sequence_manager_set_start_mode(MidiSequenceManager* manager, SequenceStartMode mode);

// Get the current start mode
SequenceStartMode midi_sequence_manager_get_start_mode(MidiSequenceManager* manager);

// Set tempo for all sequences
void midi_sequence_manager_set_tempo(MidiSequenceManager* manager, float bpm);

// Set MIDI event callback for all sequences
void midi_sequence_manager_set_midi_callback(MidiSequenceManager* manager,
                                             MidiSequenceEventCallback callback,
                                             void* userdata);

// Set phrase change callback for all sequences
void midi_sequence_manager_set_phrase_change_callback(MidiSequenceManager* manager,
                                                      MidiSequencePhraseChangeCallback callback,
                                                      void* userdata);

// Update all sequences (call from audio callback)
void midi_sequence_manager_update_samples(MidiSequenceManager* manager,
                                         int num_samples,
                                         int sample_rate,
                                         int current_pulse);

// Jump to a specific phrase in a sequence
void midi_sequence_manager_jump_to_phrase(MidiSequenceManager* manager,
                                         int seq_index,
                                         int phrase_index);

// Get the underlying sequence player for direct access
MidiSequencePlayer* midi_sequence_manager_get_player(MidiSequenceManager* manager, int seq_index);

#ifdef __cplusplus
}
#endif

#endif // MIDI_SEQUENCE_MANAGER_H
