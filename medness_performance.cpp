#include "medness_performance.h"
#include "medness_sequencer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <iostream>

// Queued sequence info
typedef struct {
    int active;           // Is this queue entry active?
    int seq_index;        // Which sequence to start
    int start_pulse;      // At which pulse to start
} QueuedSequence;

struct MednessPerformance {
    MednessSequencer* sequencer;  // Reference to shared sequencer

    // Sequences (SysEx uploaded, use slots 0-15)
    MednessSequence* players[RSX_MAX_SEQUENCES];
    int num_sequences;

    // Pads (dynamically allocated to slots 16-31 when playing)
    MednessSequence* pad_players[RSX_MAX_NOTE_PADS];

    SequenceStartMode start_mode;

    // Queued sequences (for quantized start)
    QueuedSequence queue[RSX_MAX_SEQUENCES];
    int num_queued;

    // Callbacks (shared by all sequences)
    MednessSequenceEventCallback midi_callback;
    void* midi_userdata;
    MednessSequencePhraseChangeCallback phrase_callback;
    void* phrase_userdata;

    // Per-sequence program numbers (for routing MIDI to correct synth)
    int sequence_programs[RSX_MAX_SEQUENCES];

    // Dynamic slot allocation for pads (slots 16-31)
    // pad_slot_map[i] = pad_index using slot (16+i), or -1 if free
    int pad_slot_map[16];  // 16 dynamic pad slots

    // Requested slots for pads (from RSX file)
    // pad_requested_slot[pad_idx] = 0-15 for explicit slot, -1 for dynamic
    int pad_requested_slot[RSX_MAX_NOTE_PADS];

    float tempo_bpm;
};

// Helper: Resolve MIDI file path relative to RSX file
static void resolve_midi_path(const char* rsx_path, const char* midi_file, char* out_path, size_t out_size) {
    if (!rsx_path || !midi_file || !out_path) return;

    // If MIDI path is absolute, use as-is
    if (midi_file[0] == '/' || (strlen(midi_file) > 1 && midi_file[1] == ':')) {
        strncpy(out_path, midi_file, out_size - 1);
        out_path[out_size - 1] = '\0';
        return;
    }

    // All relative paths (including sequences/) are resolved relative to RSX directory
    // Make a copy for dirname (it modifies the string)
    char rsx_copy[1024];
    strncpy(rsx_copy, rsx_path, sizeof(rsx_copy) - 1);
    rsx_copy[sizeof(rsx_copy) - 1] = '\0';

    char* dir = dirname(rsx_copy);

    // Combine directory with MIDI filename
    snprintf(out_path, out_size, "%s/%s", dir, midi_file);
}

// Helper: Calculate next quantization point
static int calculate_next_start_pulse(SequenceStartMode mode, int current_pulse) {
    switch (mode) {
        case SEQUENCE_START_IMMEDIATE:
            return current_pulse;  // Start now

        case SEQUENCE_START_QUANTIZED:
            // Next pattern start (pulse 0 = row 0)
            return 0;

        default:
            return current_pulse;
    }
}

MednessPerformance* medness_performance_create(void) {
    MednessPerformance* mgr = new MednessPerformance();
    memset(mgr, 0, sizeof(MednessPerformance));

    mgr->start_mode = SEQUENCE_START_QUANTIZED;  // Default: queued=1 (bar-quantized)
    mgr->tempo_bpm = 120.0f;

    // Initialize pad slot map (all slots free)
    for (int i = 0; i < 16; i++) {
        mgr->pad_slot_map[i] = -1;  // -1 = slot is free
    }

    // Initialize pad requested slots (all dynamic by default)
    for (int i = 0; i < RSX_MAX_NOTE_PADS; i++) {
        mgr->pad_requested_slot[i] = -1;  // -1 = dynamic allocation
    }

    return mgr;
}

void medness_performance_set_sequencer(MednessPerformance* manager, MednessSequencer* sequencer) {
    if (!manager) return;
    manager->sequencer = sequencer;
}

void medness_performance_destroy(MednessPerformance* manager) {
    if (!manager) return;

    // Destroy all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_destroy(manager->players[i]);
        }
    }

    delete manager;
}

int medness_performance_load_from_rsx(MednessPerformance* manager,
                                        const char* rsx_path,
                                        SamplecrateRSX* rsx) {
    if (!manager || !rsx_path || !rsx) return -1;

    // Clear existing sequences
    medness_performance_clear(manager);

    std::cout << "[SEQUENCE MANAGER] Loading " << rsx->num_sequences << " sequences from RSX" << std::endl;

    for (int i = 0; i < rsx->num_sequences; i++) {
        RSXSequence* seq_def = &rsx->sequences[i];

        if (!seq_def->enabled) {
            std::cout << "[SEQUENCE MANAGER] Skipping disabled sequence " << i << std::endl;
            continue;
        }

        if (seq_def->num_phrases == 0) {
            std::cout << "[SEQUENCE MANAGER] Skipping empty sequence " << i << std::endl;
            continue;
        }

        std::cout << "[SEQUENCE MANAGER] Loading sequence " << i << ": " << seq_def->name
                  << " (" << seq_def->num_phrases << " phrases)" << std::endl;

        // Create sequence player
        MednessSequence* seq = medness_sequence_create();
        if (!seq) {
            std::cerr << "[SEQUENCE MANAGER] Failed to create player for sequence " << i << std::endl;
            continue;
        }

        // Set sequencer reference and slot
        // Uploaded sequences use slots 0-15 (matching their upload slot number)
        medness_sequence_set_sequencer(seq, manager->sequencer, i);

        // Add phrases
        for (int p = 0; p < seq_def->num_phrases; p++) {
            RSXPhrase* phrase = &seq_def->phrases[p];

            // Resolve MIDI file path
            char full_path[1024];
            resolve_midi_path(rsx_path, phrase->midi_file, full_path, sizeof(full_path));

            std::cout << "  Adding phrase " << p << ": " << phrase->name
                      << " (loops: " << phrase->loop_count << ")" << std::endl;

            if (medness_sequence_add_phrase(seq, full_path,
                                               phrase->loop_count,
                                               phrase->name) < 0) {
                std::cerr << "  Failed to load phrase: " << phrase->name << " (" << full_path << ")" << std::endl;
            }
        }

        // Configure sequence
        medness_sequence_set_tempo(seq, manager->tempo_bpm);
        medness_sequence_set_loop(seq, seq_def->loop);

        // Store sequence's program number
        manager->sequence_programs[i] = seq_def->program_number;

        // Set callbacks (if already set)
        // Pass pointer to this sequence's program number as userdata
        if (manager->midi_callback) {
            medness_sequence_set_callback(seq, manager->midi_callback, &manager->sequence_programs[i]);
        }
        if (manager->phrase_callback) {
            medness_sequence_set_phrase_change_callback(seq, manager->phrase_callback, manager->phrase_userdata);
        }

        manager->players[i] = seq;
        manager->num_sequences++;

        std::cout << "[SEQUENCE MANAGER] Loaded sequence " << i << ": " << seq_def->name << std::endl;
    }

    std::cout << "[SEQUENCE MANAGER] Loaded " << manager->num_sequences << " sequences total" << std::endl;
    return manager->num_sequences;
}

// Reload a single sequence without stopping other playing sequences
int medness_performance_reload_sequence(MednessPerformance* manager,
                                         int seq_index,
                                         const char* rsx_path,
                                         SamplecrateRSX* rsx) {
    if (!manager || !rsx_path || !rsx) return -1;
    if (seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return -1;
    if (seq_index >= rsx->num_sequences) return -1;

    RSXSequence* seq_def = &rsx->sequences[seq_index];

    // Check if this sequence is currently playing
    bool was_playing = false;
    if (manager->players[seq_index]) {
        was_playing = (medness_sequence_is_playing(manager->players[seq_index]) != 0);

        // If currently playing, DON'T reload (keep playing old version)
        // New version will be loaded next time user manually triggers it
        if (was_playing) {
            std::cout << "[SEQUENCE MANAGER] Sequence " << seq_index << " (" << seq_def->name
                      << ") is currently playing - keeping old version, new version queued for next manual trigger"
                      << std::endl;
            return 0;  // Success - RSX updated, but player not reloaded
        }

        // Not playing - safe to destroy and reload
        std::cout << "[SEQUENCE MANAGER] Reloading sequence " << seq_index << ": " << seq_def->name << std::endl;
        medness_sequence_destroy(manager->players[seq_index]);
        manager->players[seq_index] = nullptr;
    }

    // If sequence is disabled or empty, just leave it unloaded
    if (!seq_def->enabled || seq_def->num_phrases == 0) {
        std::cout << "[SEQUENCE MANAGER] Sequence " << seq_index << " is disabled or empty, leaving unloaded" << std::endl;
        return 0;
    }

    // Create new sequence player
    MednessSequence* seq = medness_sequence_create();
    if (!seq) {
        std::cerr << "[SEQUENCE MANAGER] Failed to create player for sequence " << seq_index << std::endl;
        return -1;
    }

    // Set sequencer reference and slot
    medness_sequence_set_sequencer(seq, manager->sequencer, seq_index);

    // Add phrases
    for (int p = 0; p < seq_def->num_phrases; p++) {
        RSXPhrase* phrase = &seq_def->phrases[p];

        // Resolve MIDI file path
        char full_path[1024];
        resolve_midi_path(rsx_path, phrase->midi_file, full_path, sizeof(full_path));

        std::cout << "  Adding phrase " << p << ": " << phrase->name
                  << " (loops: " << phrase->loop_count << ")" << std::endl;

        if (medness_sequence_add_phrase(seq, full_path,
                                           phrase->loop_count,
                                           phrase->name) < 0) {
            std::cerr << "  Failed to load phrase: " << phrase->name << " (" << full_path << ")" << std::endl;
            medness_sequence_destroy(seq);
            return -1;
        }
    }

    // Configure sequence
    medness_sequence_set_tempo(seq, manager->tempo_bpm);
    medness_sequence_set_loop(seq, seq_def->loop);

    // Store sequence's program number
    manager->sequence_programs[seq_index] = seq_def->program_number;

    // Set callbacks
    if (manager->midi_callback) {
        medness_sequence_set_callback(seq, manager->midi_callback, &manager->sequence_programs[seq_index]);
    }
    if (manager->phrase_callback) {
        medness_sequence_set_phrase_change_callback(seq, manager->phrase_callback, manager->phrase_userdata);
    }

    manager->players[seq_index] = seq;

    std::cout << "[SEQUENCE MANAGER] Reloaded sequence " << seq_index << ": " << seq_def->name << std::endl;

    return 0;
}

void medness_performance_clear(MednessPerformance* manager) {
    if (!manager) return;

    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_destroy(manager->players[i]);
            manager->players[i] = nullptr;
        }
    }

    manager->num_sequences = 0;
    manager->num_queued = 0;
}

int medness_performance_get_count(MednessPerformance* manager) {
    if (!manager) return 0;
    return manager->num_sequences;
}

void medness_performance_play(MednessPerformance* manager, int seq_index, int current_pulse) {
    if (!manager) return;

    MednessSequence* seq = nullptr;
    bool is_pad = false;

    // Check if this is a sequence (0-15) or pad (0-31)
    if (seq_index >= 0 && seq_index < RSX_MAX_SEQUENCES && manager->players[seq_index]) {
        seq = manager->players[seq_index];
        is_pad = false;
    } else if (seq_index >= 0 && seq_index < RSX_MAX_NOTE_PADS && manager->pad_players[seq_index]) {
        seq = manager->pad_players[seq_index];
        is_pad = true;
    } else {
        return;  // Invalid index or not loaded
    }

    // For pads: allocate a dynamic slot from pool 16-31
    if (is_pad) {
        // Find first free slot in range 16-31
        int allocated_slot = -1;
        for (int i = 0; i < 16; i++) {
            if (manager->pad_slot_map[i] == -1) {
                allocated_slot = 16 + i;
                manager->pad_slot_map[i] = seq_index;  // Mark slot as used by this pad
                break;
            }
        }

        if (allocated_slot == -1) {
            std::cerr << "[PAD] No free slots available (all 16 pad slots in use)" << std::endl;
            return;
        }

        // Update the sequence's slot assignment
        medness_sequence_set_sequencer(seq, manager->sequencer, allocated_slot);
        std::cout << "[PAD] Assigned pad " << seq_index << " to dynamic slot " << allocated_slot
                  << " (P" << (allocated_slot - 15) << ")" << std::endl;

        // Start immediately
        medness_sequence_play(seq);
        return;
    }

    // For sequences: use fixed slot assignment (already set at load time)
    if (manager->start_mode == SEQUENCE_START_IMMEDIATE) {
        // Start immediately
        std::cout << "[SEQUENCE MANAGER] Starting sequence " << seq_index << " immediately" << std::endl;
        medness_sequence_play(seq);
    } else {
        // Queue for quantized start
        int start_pulse = calculate_next_start_pulse(manager->start_mode, current_pulse);

        std::cout << "[SEQUENCE MANAGER] Queueing sequence " << seq_index
                  << " to start at pulse " << start_pulse
                  << " (current: " << current_pulse << ")" << std::endl;

        // Find or create queue entry
        int queue_idx = -1;
        for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
            if (!manager->queue[i].active) {
                queue_idx = i;
                break;
            }
        }

        if (queue_idx >= 0) {
            manager->queue[queue_idx].active = 1;
            manager->queue[queue_idx].seq_index = seq_index;
            manager->queue[queue_idx].start_pulse = start_pulse;
            manager->num_queued++;
        }
    }
}

void medness_performance_stop(MednessPerformance* manager, int seq_index) {
    if (!manager) return;

    MednessSequence* seq = nullptr;
    bool is_pad = false;

    // Check if this is a sequence or pad
    if (seq_index >= 0 && seq_index < RSX_MAX_SEQUENCES && manager->players[seq_index]) {
        seq = manager->players[seq_index];
        is_pad = false;
    } else if (seq_index >= 0 && seq_index < RSX_MAX_NOTE_PADS && manager->pad_players[seq_index]) {
        seq = manager->pad_players[seq_index];
        is_pad = true;
    } else {
        return;  // Not loaded
    }

    // For pads: free the allocated slot
    if (is_pad) {
        // Find which slot this pad is using
        for (int i = 0; i < 16; i++) {
            if (manager->pad_slot_map[i] == seq_index) {
                int freed_slot = 16 + i;
                manager->pad_slot_map[i] = -1;  // Mark slot as free
                std::cout << "[PAD] Freed slot " << freed_slot << " (P" << (freed_slot - 15) << ") from pad " << seq_index << std::endl;
                break;
            }
        }
    } else {
        // For sequences: cancel any queued start
        for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
            if (manager->queue[i].active && manager->queue[i].seq_index == seq_index) {
                manager->queue[i].active = 0;
                manager->num_queued--;
            }
        }
    }

    if (seq) {
        medness_sequence_stop(seq);
    }
}

void medness_performance_stop_all(MednessPerformance* manager) {
    if (!manager) return;

    // Clear all queued starts
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        manager->queue[i].active = 0;
    }
    manager->num_queued = 0;

    // Stop all playing sequences
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_stop(manager->players[i]);
        }
    }
}

int medness_performance_is_playing(MednessPerformance* manager, int seq_index) {
    if (!manager) return 0;

    MednessSequence* seq = nullptr;

    // Check both sequence and pad arrays
    if (seq_index >= 0 && seq_index < RSX_MAX_SEQUENCES && manager->players[seq_index]) {
        seq = manager->players[seq_index];
    } else if (seq_index >= 0 && seq_index < RSX_MAX_NOTE_PADS && manager->pad_players[seq_index]) {
        seq = manager->pad_players[seq_index];
    }

    if (!seq) return 0;

    return medness_sequence_is_playing(seq);
}

void medness_performance_set_start_mode(MednessPerformance* manager, SequenceStartMode mode) {
    if (!manager) return;
    manager->start_mode = mode;
}

SequenceStartMode medness_performance_get_start_mode(MednessPerformance* manager) {
    if (!manager) return SEQUENCE_START_IMMEDIATE;
    return manager->start_mode;
}

void medness_performance_set_tempo(MednessPerformance* manager, float bpm) {
    if (!manager || bpm <= 0.0f) return;

    manager->tempo_bpm = bpm;

    // Update all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_set_tempo(manager->players[i], bpm);
        }
    }
}

void medness_performance_set_midi_callback(MednessPerformance* manager,
                                             MednessSequenceEventCallback callback,
                                             void* userdata) {
    if (!manager) return;

    manager->midi_callback = callback;
    manager->midi_userdata = userdata;

    // Update all sequence players with the callback
    // BUT keep their individual program-specific userdata (stored in sequence_programs[])
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            // Use per-sequence program number as userdata, not the manager's global userdata
            medness_sequence_set_callback(manager->players[i], callback, &manager->sequence_programs[i]);
        }
    }
}

void medness_performance_set_phrase_change_callback(MednessPerformance* manager,
                                                      MednessSequencePhraseChangeCallback callback,
                                                      void* userdata) {
    if (!manager) return;

    manager->phrase_callback = callback;
    manager->phrase_userdata = userdata;

    // Update all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_set_phrase_change_callback(manager->players[i], callback, userdata);
        }
    }
}

void medness_performance_update_samples(MednessPerformance* manager,
                                         int num_samples,
                                         int sample_rate,
                                         int current_pulse) {
    if (!manager) return;

    // Check for queued sequences that should start now
    if (manager->num_queued > 0) {
        for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
            if (!manager->queue[i].active) continue;

            QueuedSequence* q = &manager->queue[i];

            // Check if we've reached the start pulse
            bool should_start = false;

            if (manager->start_mode == SEQUENCE_START_QUANTIZED) {
                // Wait for pulse 0 (row 0)
                should_start = (current_pulse == 0);
            } else {
                // IMMEDIATE mode - start now (target pulse already = current pulse)
                should_start = (current_pulse == q->start_pulse);
            }

            if (should_start) {
                std::cout << "[SEQUENCE MANAGER] Starting queued sequence " << q->seq_index
                          << " at pulse " << current_pulse << std::endl;

                MednessSequence* seq = manager->players[q->seq_index];
                if (seq) {
                    medness_sequence_play(seq);
                }

                // Clear queue entry
                q->active = 0;
                manager->num_queued--;
            }
        }
    }

    // Update all playing sequences
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            medness_sequence_update_samples(manager->players[i],
                                               num_samples,
                                               sample_rate,
                                               current_pulse);
        }
    }
}

void medness_performance_jump_to_phrase(MednessPerformance* manager,
                                         int seq_index,
                                         int phrase_index) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return;

    MednessSequence* seq = manager->players[seq_index];
    if (seq) {
        medness_sequence_jump_to_phrase(seq, phrase_index);
    }
}

MednessSequence* medness_performance_get_player(MednessPerformance* manager, int seq_index) {
    if (!manager) return nullptr;

    // Check both sequence and pad arrays
    if (seq_index >= 0 && seq_index < RSX_MAX_SEQUENCES && manager->players[seq_index]) {
        return manager->players[seq_index];
    } else if (seq_index >= 0 && seq_index < RSX_MAX_NOTE_PADS && manager->pad_players[seq_index]) {
        return manager->pad_players[seq_index];
    }

    return nullptr;
}

int medness_performance_load_pad(MednessPerformance* manager,
                                   int pad_index,
                                   const char* midi_file,
                                   int requested_slot,
                                   void* callback_userdata) {
    if (!manager || !manager->sequencer || !midi_file) return -1;
    if (pad_index < 0 || pad_index >= RSX_MAX_NOTE_PADS) return -1;

    // Validate requested_slot if provided
    if (requested_slot >= 0 && (requested_slot < 0 || requested_slot > 15)) {
        std::cerr << "[PAD] Invalid requested_slot " << requested_slot << " (must be 0-15 or -1)" << std::endl;
        return -1;
    }

    // Store requested slot
    manager->pad_requested_slot[pad_index] = requested_slot;

    // Unload existing pad if present
    medness_performance_unload_pad(manager, pad_index);

    if (requested_slot >= 0) {
        std::cout << "[PAD " << (pad_index + 1) << "] Loading MIDI file: " << midi_file
                  << " (requested slot: P" << (requested_slot + 1) << ")" << std::endl;
    } else {
        std::cout << "[PAD " << (pad_index + 1) << "] Loading MIDI file: " << midi_file
                  << " (dynamic slot allocation)" << std::endl;
    }

    // Create a sequence player for this pad
    MednessSequence* seq = medness_sequence_create();
    if (!seq) {
        std::cerr << "[PAD " << (pad_index + 1) << "] Failed to create sequence player" << std::endl;
        return -1;
    }

    // Set sequencer reference with slot = -1 (unassigned, will be allocated dynamically on play)
    medness_sequence_set_sequencer(seq, manager->sequencer, -1);

    // Add single phrase with infinite loop (loop_count = 0)
    if (medness_sequence_add_phrase(seq, midi_file, 0, "Pad MIDI") < 0) {
        std::cerr << "[PAD " << (pad_index + 1) << "] Failed to load MIDI file: " << midi_file << std::endl;
        medness_sequence_destroy(seq);
        return -1;
    }

    // Debug: Check if track has events
    MednessTrack* track = medness_sequence_get_current_track(seq);
    if (track) {
        int event_count = 0;
        medness_track_get_events(track, &event_count);
        std::cout << "[PAD " << (pad_index + 1) << "] Track has " << event_count << " MIDI events" << std::endl;
    }

    // Configure sequence
    medness_sequence_set_tempo(seq, manager->tempo_bpm);
    medness_sequence_set_loop(seq, 1);  // Loop the sequence

    // Set callbacks (if already set) with provided userdata
    // GUI code establishes the pad-to-program relationship via callback_userdata
    if (manager->midi_callback) {
        medness_sequence_set_callback(seq, manager->midi_callback, callback_userdata);
    }
    if (manager->phrase_callback) {
        medness_sequence_set_phrase_change_callback(seq, manager->phrase_callback, manager->phrase_userdata);
    }

    manager->pad_players[pad_index] = seq;

    std::cout << "[PAD " << (pad_index + 1) << "] Loaded successfully (slot will be assigned on play)" << std::endl;
    return 0;
}

void medness_performance_unload_pad(MednessPerformance* manager, int pad_index) {
    if (!manager || pad_index < 0 || pad_index >= RSX_MAX_NOTE_PADS) return;

    // Stop playback if active
    medness_performance_stop(manager, pad_index);

    // Destroy sequence player
    if (manager->pad_players[pad_index]) {
        medness_sequence_destroy(manager->pad_players[pad_index]);
        manager->pad_players[pad_index] = nullptr;
    }
}
