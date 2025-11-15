#include "medness_sequencer.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <iostream>

// Pattern is 64 rows = 64 sixteenths = 4 bars at 4/4
// At 24 PPQN: 64 sixteenths * 6 pulses/sixteenth = 384 pulses
#define PATTERN_LENGTH_PULSES 384
#define PATTERN_LENGTH_ROWS 64
#define MAX_TRACK_SLOTS 16

// Forward declaration
static void medness_sequencer_play_tracks(MednessSequencer* sequencer, int old_pulse, int new_pulse);

// Track slot - holds reference to track and playback state
struct MednessSequencerTrackSlot {
    MednessTrack* track;                // Reference to track (not owned)
    SequencerMidiCallback midi_callback; // MIDI event callback
    void* userdata;                     // User context
    int last_tick_processed;            // Last tick fired (to prevent duplicates)
    int active;                         // Is this slot active?
};

struct MednessSequencer {
    float bpm;                      // Current tempo in BPM
    int pulse_count;                // Current pulse within pattern (0-383)
    int active;                     // Is sequencer active?
    int external_clock;             // Is external MIDI clock driving? (1=yes, 0=no)

    SequencerLoopCallback loop_callback;
    void* loop_userdata;

    // Track slots (one per pad)
    MednessSequencerTrackSlot slots[MAX_TRACK_SLOTS];

    // For internal timing advancement
    float accumulated_pulses;
};

MednessSequencer* medness_sequencer_create(void) {
    MednessSequencer* sequencer = new MednessSequencer();

    sequencer->bpm = 125.0f;
    sequencer->pulse_count = 0;
    sequencer->active = 0;
    sequencer->external_clock = 0;  // Default to internal clock
    sequencer->loop_callback = NULL;
    sequencer->loop_userdata = NULL;
    sequencer->accumulated_pulses = 0.0f;

    // Initialize all slots as inactive
    for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
        sequencer->slots[i].track = NULL;
        sequencer->slots[i].midi_callback = NULL;
        sequencer->slots[i].userdata = NULL;
        sequencer->slots[i].last_tick_processed = -1;
        sequencer->slots[i].active = 0;
    }

    return sequencer;
}

void medness_sequencer_destroy(MednessSequencer* sequencer) {
    if (sequencer) {
        delete sequencer;
    }
}

void medness_sequencer_set_bpm(MednessSequencer* sequencer, float bpm) {
    if (!sequencer || bpm <= 0.0f) return;
    sequencer->bpm = bpm;
}

float medness_sequencer_get_bpm(MednessSequencer* sequencer) {
    if (!sequencer) return 125.0f;
    return sequencer->bpm;
}

void medness_sequencer_set_spp(MednessSequencer* sequencer, int spp_position) {
    if (!sequencer) return;

    // SPP is in 16th notes - convert to pulse within pattern
    int spp_within_pattern = spp_position % PATTERN_LENGTH_ROWS;
    sequencer->pulse_count = spp_within_pattern * 6;  // 6 pulses per 16th note

    // Update all active tracks' last_tick_processed to prevent retriggering
    // When SPP jumps position, we don't want to fire events that may have already played
    const int TPQN = 480;
    int new_tick = (sequencer->pulse_count * TPQN) / 24;
    for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
        if (sequencer->slots[i].active) {
            sequencer->slots[i].last_tick_processed = new_tick - 1;
        }
    }
}

int medness_sequencer_update(MednessSequencer* sequencer, int num_samples, int sample_rate) {
    if (!sequencer || !sequencer->active) return -1;
    if (num_samples <= 0 || sample_rate <= 0) return sequencer->pulse_count;

    // Check if any tracks are playing - only advance if we have active tracks
    bool has_active_tracks = false;
    for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
        if (sequencer->slots[i].active) {
            has_active_tracks = true;
            break;
        }
    }

    // If no tracks playing: reset position to 0 (unless externally synced to SPP)
    // This ensures next trigger starts from the beginning
    if (!has_active_tracks) {
        // TODO: Check if we're synced to external SPP - if so, keep position
        // For now: always reset to 0 when nothing is playing
        if (sequencer->pulse_count != 0) {
            printf("[SEQUENCER] No active tracks - resetting position to 0\n");
            sequencer->pulse_count = 0;
            sequencer->accumulated_pulses = 0.0f;
        }
        return -1;  // Return -1 to indicate sequencer is not running
    }

    int old_pulse = sequencer->pulse_count;

    // Always advance position using internal clock
    // External MIDI clock only adjusts the BPM, doesn't control position directly
    // (external_clock flag is deprecated - sequencer always runs on internal timebase)
    {
        // Calculate how many pulses elapsed based on sample count
        double seconds_elapsed = (double)num_samples / (double)sample_rate;
        double pulses_per_second = (sequencer->bpm * 24.0) / 60.0;
        double exact_pulses = seconds_elapsed * pulses_per_second;

        sequencer->accumulated_pulses += (float)exact_pulses;

        int pulse_increment = (int)sequencer->accumulated_pulses;
        if (pulse_increment > 0) {
            sequencer->pulse_count += pulse_increment;
            sequencer->accumulated_pulses -= pulse_increment;

            // Check for pattern wrap
            if (sequencer->pulse_count >= PATTERN_LENGTH_PULSES) {
                sequencer->pulse_count = sequencer->pulse_count % PATTERN_LENGTH_PULSES;

                // On wrap, update track positions to match new pulse position
                // This prevents double-firing when position jumps
                const int TPQN = 480;
                int new_tick = (sequencer->pulse_count * TPQN) / 24;
                for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
                    if (sequencer->slots[i].active) {
                        sequencer->slots[i].last_tick_processed = new_tick - 1;
                    }
                }

                // Fire loop callback
                if (sequencer->loop_callback) {
                    sequencer->loop_callback(sequencer->loop_userdata);
                }
            }
        }

        // Play all active tracks at current position
        medness_sequencer_play_tracks(sequencer, old_pulse, sequencer->pulse_count);
    }
    // If external clock mode: position is already updated by medness_sequencer_clock_pulse()
    // We don't need to fire events here - they're fired in clock_pulse()

    return sequencer->pulse_count;
}

void medness_sequencer_clock_pulse(MednessSequencer* sequencer) {
    if (!sequencer || !sequencer->active) return;

    int old_pulse = sequencer->pulse_count;
    sequencer->pulse_count++;

    // Check for pattern wrap
    if (sequencer->pulse_count >= PATTERN_LENGTH_PULSES) {
        sequencer->pulse_count = 0;

        // On wrap, update track positions to match new pulse position
        // This prevents double-firing when position jumps
        const int TPQN = 480;
        int new_tick = (sequencer->pulse_count * TPQN) / 24;
        for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
            if (sequencer->slots[i].active) {
                sequencer->slots[i].last_tick_processed = new_tick - 1;
            }
        }

        // Fire loop callback
        if (sequencer->loop_callback) {
            sequencer->loop_callback(sequencer->loop_userdata);
        }
    }

    // Play all active tracks at current position
    medness_sequencer_play_tracks(sequencer, old_pulse, sequencer->pulse_count);
}

int medness_sequencer_get_row(MednessSequencer* sequencer) {
    if (!sequencer) return 1;
    return (sequencer->pulse_count / 6) + 1;
}

int medness_sequencer_get_pulse(MednessSequencer* sequencer) {
    if (!sequencer) return 0;
    return sequencer->pulse_count;
}

void medness_sequencer_set_loop_callback(MednessSequencer* sequencer, SequencerLoopCallback callback, void* userdata) {
    if (!sequencer) return;
    sequencer->loop_callback = callback;
    sequencer->loop_userdata = userdata;
}

void medness_sequencer_set_active(MednessSequencer* sequencer, int active) {
    if (!sequencer) return;
    sequencer->active = active ? 1 : 0;
}

int medness_sequencer_is_active(MednessSequencer* sequencer) {
    if (!sequencer) return 0;
    return sequencer->active;
}

void medness_sequencer_set_external_clock(MednessSequencer* sequencer, int external) {
    if (!sequencer) return;
    sequencer->external_clock = external ? 1 : 0;
}

// Track management

void medness_sequencer_add_track(MednessSequencer* sequencer, int slot, MednessTrack* track,
                                  SequencerMidiCallback midi_callback, void* userdata) {
    if (!sequencer || slot < 0 || slot >= MAX_TRACK_SLOTS) return;

    sequencer->slots[slot].track = track;
    sequencer->slots[slot].midi_callback = midi_callback;
    sequencer->slots[slot].userdata = userdata;

    // Initialize last_tick_processed to current position to prevent double-firing
    // Convert current pulse to tick
    const int TPQN = 480;
    int current_tick = (sequencer->pulse_count * TPQN) / 24;
    sequencer->slots[slot].last_tick_processed = current_tick - 1;

    sequencer->slots[slot].active = (track != NULL) ? 1 : 0;
}

void medness_sequencer_remove_track(MednessSequencer* sequencer, int slot) {
    if (!sequencer || slot < 0 || slot >= MAX_TRACK_SLOTS) return;

    sequencer->slots[slot].track = NULL;
    sequencer->slots[slot].midi_callback = NULL;
    sequencer->slots[slot].userdata = NULL;
    sequencer->slots[slot].last_tick_processed = -1;
    sequencer->slots[slot].active = 0;
}

int medness_sequencer_slot_is_active(MednessSequencer* sequencer, int slot) {
    if (!sequencer || slot < 0 || slot >= MAX_TRACK_SLOTS) return 0;
    return sequencer->slots[slot].active;
}

// Internal: Play all tracks from old_pulse to new_pulse
static void medness_sequencer_play_tracks(MednessSequencer* sequencer, int old_pulse, int new_pulse) {
    if (!sequencer) return;

    // Convert pulses to ticks (assumes all tracks use same TPQN)
    // For now, use a fixed TPQN of 480 (will get from track later)
    const int TPQN = 480;

    // Calculate ticks from pulses: tick = (pulse / 24) * TPQN
    int old_tick = (old_pulse * TPQN) / 24;
    int new_tick = (new_pulse * TPQN) / 24;

    // Iterate through all active slots
    for (int i = 0; i < MAX_TRACK_SLOTS; i++) {
        if (!sequencer->slots[i].active) continue;

        MednessSequencerTrackSlot* slot = &sequencer->slots[i];

        // Get events from track
        int event_count = 0;
        const MednessTrackEvent* events = medness_track_get_events(slot->track, &event_count);
        if (!events) continue;

        // Fire events between old_tick and new_tick
        for (int e = 0; e < event_count; e++) {
            const MednessTrackEvent* evt = &events[e];

            if (evt->tick > slot->last_tick_processed && evt->tick <= new_tick) {
                // Fire MIDI event
                if (slot->midi_callback) {
                    slot->midi_callback(evt->note, evt->velocity, evt->on, slot->userdata);
                }
            }
        }

        slot->last_tick_processed = new_tick;
    }
}
