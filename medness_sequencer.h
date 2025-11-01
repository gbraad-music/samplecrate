#ifndef MEDNESS_SEQUENCER_H
#define MEDNESS_SEQUENCER_H

#include "medness_track.h"

#ifdef __cplusplus
extern "C" {
#endif

// The sequencer is the single source of truth for pattern position
// It plays tracks, manages the global pattern position (ROW) and handles loop wrapping
// Note: The arranger (higher level) will eventually determine which pattern plays when

typedef struct MednessSequencer MednessSequencer;
typedef struct MednessSequencerTrackSlot MednessSequencerTrackSlot;

// Callback fired when the pattern wraps from row 64 back to row 1
typedef void (*SequencerLoopCallback)(void* userdata);

// Callback fired when a MIDI event needs to be sent
// note: MIDI note number, velocity: 0-127, on: 1=note_on 0=note_off, userdata: user context
typedef void (*SequencerMidiCallback)(int note, int velocity, int on, void* userdata);

// Create a new sequencer
MednessSequencer* medness_sequencer_create(void);

// Destroy the sequencer
void medness_sequencer_destroy(MednessSequencer* sequencer);

// Set the BPM (beats per minute)
void medness_sequencer_set_bpm(MednessSequencer* sequencer, float bpm);

// Get the current BPM
float medness_sequencer_get_bpm(MednessSequencer* sequencer);

// Set the pattern position from external SPP (Song Position Pointer)
// spp_position: MIDI SPP value (in 16th notes)
void medness_sequencer_set_spp(MednessSequencer* sequencer, int spp_position);

// Update the sequencer with time delta (called from audio callback)
// num_samples: number of audio samples processed
// sample_rate: audio sample rate (e.g., 44100)
// Returns: current pulse count (0-383), or -1 if not active
int medness_sequencer_update(MednessSequencer* sequencer, int num_samples, int sample_rate);

// Advance position by one MIDI clock pulse (called when receiving 0xF8)
void medness_sequencer_clock_pulse(MednessSequencer* sequencer);

// Get current row position (1-64)
int medness_sequencer_get_row(MednessSequencer* sequencer);

// Get current pulse within pattern (0-383)
int medness_sequencer_get_pulse(MednessSequencer* sequencer);

// Set loop callback (called when pattern wraps)
void medness_sequencer_set_loop_callback(MednessSequencer* sequencer, SequencerLoopCallback callback, void* userdata);

// Enable/disable the sequencer
void medness_sequencer_set_active(MednessSequencer* sequencer, int active);

// Check if sequencer is active
int medness_sequencer_is_active(MednessSequencer* sequencer);

// Set external clock mode (true = external MIDI clock, false = internal sample-accurate)
// When external clock mode is enabled, medness_sequencer_update() will NOT advance position
// Position advancement happens only via medness_sequencer_clock_pulse()
void medness_sequencer_set_external_clock(MednessSequencer* sequencer, int external);

// --- Track Management ---

// Add a track to a specific slot (0-15, matching pad indices)
// The sequencer will play this track when active
// track: the track to play (sequencer doesn't own it, just references it)
// slot: which slot to assign (0-15)
// midi_callback: callback for MIDI events from this track
// userdata: user context for the callback
void medness_sequencer_add_track(MednessSequencer* sequencer, int slot, MednessTrack* track,
                                  SequencerMidiCallback midi_callback, void* userdata);

// Remove a track from a slot
void medness_sequencer_remove_track(MednessSequencer* sequencer, int slot);

// Check if a slot has an active track
int medness_sequencer_slot_is_active(MednessSequencer* sequencer, int slot);

#ifdef __cplusplus
}
#endif

#endif // MEDNESS_SEQUENCER_H
