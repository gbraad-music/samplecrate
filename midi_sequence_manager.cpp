#include "midi_sequence_manager.h"
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

struct MidiSequenceManager {
    MidiSequencePlayer* players[RSX_MAX_SEQUENCES];
    int num_sequences;
    SequenceStartMode start_mode;

    // Queued sequences (for quantized start)
    QueuedSequence queue[RSX_MAX_SEQUENCES];
    int num_queued;

    // Callbacks (shared by all sequences)
    MidiSequenceEventCallback midi_callback;
    void* midi_userdata;
    MidiSequencePhraseChangeCallback phrase_callback;
    void* phrase_userdata;

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

MidiSequenceManager* midi_sequence_manager_create(void) {
    MidiSequenceManager* mgr = new MidiSequenceManager();
    memset(mgr, 0, sizeof(MidiSequenceManager));

    mgr->start_mode = SEQUENCE_START_QUANTIZED;  // Default: queued=1 (bar-quantized)
    mgr->tempo_bpm = 120.0f;

    return mgr;
}

void midi_sequence_manager_destroy(MidiSequenceManager* manager) {
    if (!manager) return;

    // Destroy all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_destroy(manager->players[i]);
        }
    }

    delete manager;
}

int midi_sequence_manager_load_from_rsx(MidiSequenceManager* manager,
                                        const char* rsx_path,
                                        SamplecrateRSX* rsx) {
    if (!manager || !rsx_path || !rsx) return -1;

    // Clear existing sequences
    midi_sequence_manager_clear(manager);

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
        MidiSequencePlayer* seq = midi_sequence_player_create();
        if (!seq) {
            std::cerr << "[SEQUENCE MANAGER] Failed to create player for sequence " << i << std::endl;
            continue;
        }

        // Add phrases
        for (int p = 0; p < seq_def->num_phrases; p++) {
            RSXPhrase* phrase = &seq_def->phrases[p];

            // Resolve MIDI file path
            char full_path[1024];
            resolve_midi_path(rsx_path, phrase->midi_file, full_path, sizeof(full_path));

            std::cout << "  Adding phrase " << p << ": " << phrase->name
                      << " (loops: " << phrase->loop_count << ")" << std::endl;

            if (midi_sequence_player_add_phrase(seq, full_path,
                                               phrase->loop_count,
                                               phrase->name) < 0) {
                std::cerr << "  Failed to load phrase: " << phrase->name << " (" << full_path << ")" << std::endl;
            }
        }

        // Configure sequence
        midi_sequence_player_set_tempo(seq, manager->tempo_bpm);
        midi_sequence_player_set_loop(seq, seq_def->loop);

        // Set callbacks (if already set)
        if (manager->midi_callback) {
            midi_sequence_player_set_callback(seq, manager->midi_callback, manager->midi_userdata);
        }
        if (manager->phrase_callback) {
            midi_sequence_player_set_phrase_change_callback(seq, manager->phrase_callback, manager->phrase_userdata);
        }

        manager->players[i] = seq;
        manager->num_sequences++;

        std::cout << "[SEQUENCE MANAGER] Loaded sequence " << i << ": " << seq_def->name << std::endl;
    }

    std::cout << "[SEQUENCE MANAGER] Loaded " << manager->num_sequences << " sequences total" << std::endl;
    return manager->num_sequences;
}

void midi_sequence_manager_clear(MidiSequenceManager* manager) {
    if (!manager) return;

    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_destroy(manager->players[i]);
            manager->players[i] = nullptr;
        }
    }

    manager->num_sequences = 0;
    manager->num_queued = 0;
}

int midi_sequence_manager_get_count(MidiSequenceManager* manager) {
    if (!manager) return 0;
    return manager->num_sequences;
}

void midi_sequence_manager_play(MidiSequenceManager* manager, int seq_index, int current_pulse) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return;

    MidiSequencePlayer* seq = manager->players[seq_index];
    if (!seq) return;

    if (manager->start_mode == SEQUENCE_START_IMMEDIATE) {
        // Start immediately
        std::cout << "[SEQUENCE MANAGER] Starting sequence " << seq_index << " immediately" << std::endl;
        midi_sequence_player_play(seq);
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

void midi_sequence_manager_stop(MidiSequenceManager* manager, int seq_index) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return;

    // Cancel any queued start
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->queue[i].active && manager->queue[i].seq_index == seq_index) {
            manager->queue[i].active = 0;
            manager->num_queued--;
        }
    }

    MidiSequencePlayer* seq = manager->players[seq_index];
    if (seq) {
        midi_sequence_player_stop(seq);
    }
}

void midi_sequence_manager_stop_all(MidiSequenceManager* manager) {
    if (!manager) return;

    // Clear all queued starts
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        manager->queue[i].active = 0;
    }
    manager->num_queued = 0;

    // Stop all playing sequences
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_stop(manager->players[i]);
        }
    }
}

int midi_sequence_manager_is_playing(MidiSequenceManager* manager, int seq_index) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return 0;

    MidiSequencePlayer* seq = manager->players[seq_index];
    if (!seq) return 0;

    return midi_sequence_player_is_playing(seq);
}

void midi_sequence_manager_set_start_mode(MidiSequenceManager* manager, SequenceStartMode mode) {
    if (!manager) return;
    manager->start_mode = mode;
}

SequenceStartMode midi_sequence_manager_get_start_mode(MidiSequenceManager* manager) {
    if (!manager) return SEQUENCE_START_IMMEDIATE;
    return manager->start_mode;
}

void midi_sequence_manager_set_tempo(MidiSequenceManager* manager, float bpm) {
    if (!manager || bpm <= 0.0f) return;

    manager->tempo_bpm = bpm;

    // Update all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_set_tempo(manager->players[i], bpm);
        }
    }
}

void midi_sequence_manager_set_midi_callback(MidiSequenceManager* manager,
                                             MidiSequenceEventCallback callback,
                                             void* userdata) {
    if (!manager) return;

    manager->midi_callback = callback;
    manager->midi_userdata = userdata;

    // Update all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_set_callback(manager->players[i], callback, userdata);
        }
    }
}

void midi_sequence_manager_set_phrase_change_callback(MidiSequenceManager* manager,
                                                      MidiSequencePhraseChangeCallback callback,
                                                      void* userdata) {
    if (!manager) return;

    manager->phrase_callback = callback;
    manager->phrase_userdata = userdata;

    // Update all sequence players
    for (int i = 0; i < RSX_MAX_SEQUENCES; i++) {
        if (manager->players[i]) {
            midi_sequence_player_set_phrase_change_callback(manager->players[i], callback, userdata);
        }
    }
}

void midi_sequence_manager_update_samples(MidiSequenceManager* manager,
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

                MidiSequencePlayer* seq = manager->players[q->seq_index];
                if (seq) {
                    midi_sequence_player_play(seq);
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
            midi_sequence_player_update_samples(manager->players[i],
                                               num_samples,
                                               sample_rate,
                                               current_pulse);
        }
    }
}

void midi_sequence_manager_jump_to_phrase(MidiSequenceManager* manager,
                                         int seq_index,
                                         int phrase_index) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return;

    MidiSequencePlayer* seq = manager->players[seq_index];
    if (seq) {
        midi_sequence_player_jump_to_phrase(seq, phrase_index);
    }
}

MidiSequencePlayer* midi_sequence_manager_get_player(MidiSequenceManager* manager, int seq_index) {
    if (!manager || seq_index < 0 || seq_index >= RSX_MAX_SEQUENCES) return nullptr;
    return manager->players[seq_index];
}
