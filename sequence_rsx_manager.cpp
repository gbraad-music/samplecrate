#include "sequence_rsx_manager.h"
#include <stdio.h>
#include <string.h>

/**
 * Find sequence index for a given slot
 */
int sequence_rsx_find_slot(SamplecrateRSX* rsx, uint8_t slot) {
    if (!rsx) return -1;

    // Search for sequence that references seq_<slot>.mid
    for (int i = 0; i < rsx->num_sequences; i++) {
        if (rsx->sequences[i].num_phrases > 0) {
            const char* midi_file = rsx->sequences[i].phrases[0].midi_file;

            // Check if this phrase references the slot's MIDI file
            int existing_slot = -1;
            if (sscanf(midi_file, "seq_%d.mid", &existing_slot) == 1) {
                if (existing_slot == slot) {
                    return i;
                }
            }
        }
    }

    return -1;
}

/**
 * Add or update a sequence entry in RSX structure for an uploaded MIDI file
 */
int sequence_rsx_add_uploaded(SamplecrateRSX* rsx, uint8_t slot, uint8_t program, const char* rsx_path) {
    if (!rsx) {
        printf("[SeqRSXManager] ERROR: NULL RSX pointer\n");
        return -1;
    }

    if (slot >= 16) {
        printf("[SeqRSXManager] ERROR: Invalid slot %d (max 15)\n", slot);
        return -1;
    }

    // Find existing sequence for this slot
    int seq_idx = sequence_rsx_find_slot(rsx, slot);

    // If not found, create new sequence entry
    if (seq_idx == -1) {
        if (rsx->num_sequences >= RSX_MAX_SEQUENCES) {
            printf("[SeqRSXManager] ERROR: Cannot add sequence - maximum (%d) reached\n", RSX_MAX_SEQUENCES);
            return -1;
        }

        seq_idx = rsx->num_sequences;
        rsx->num_sequences++;
        printf("[SeqRSXManager] Creating new sequence entry %d for slot %d\n", seq_idx, slot);
    } else {
        printf("[SeqRSXManager] Updating existing sequence entry %d for slot %d\n", seq_idx, slot);
    }

    RSXSequence* seq = &rsx->sequences[seq_idx];

    // Set sequence properties
    snprintf(seq->name, sizeof(seq->name), "Upload Slot %d", slot);
    seq->enabled = 1;
    seq->loop = 1;  // Loop by default
    seq->program_number = program;  // Target program

    // Add phrase pointing to uploaded MIDI file
    seq->num_phrases = 1;
    snprintf(seq->phrases[0].midi_file, sizeof(seq->phrases[0].midi_file), "seq_%d.mid", slot);
    snprintf(seq->phrases[0].name, sizeof(seq->phrases[0].name), "Slot %d", slot);
    seq->phrases[0].loop_count = 0;  // 0 = infinite loop

    printf("[SeqRSXManager] Added sequence %d: %s -> %s (program %d)\n",
           seq_idx, seq->name, seq->phrases[0].midi_file, program);

    // Save RSX file to persist changes
    if (rsx_path && rsx_path[0] != '\0') {
        if (samplecrate_rsx_save(rsx, rsx_path) == 0) {
            printf("[SeqRSXManager] Saved to %s\n", rsx_path);
        } else {
            printf("[SeqRSXManager] WARNING: Failed to save RSX file\n");
            return -1;
        }
    }

    return 0;
}

/**
 * Remove a sequence entry from RSX structure
 */
int sequence_rsx_remove(SamplecrateRSX* rsx, uint8_t slot, const char* rsx_path) {
    if (!rsx) {
        printf("[SeqRSXManager] ERROR: NULL RSX pointer\n");
        return -1;
    }

    // Find sequence for this slot
    int seq_idx = sequence_rsx_find_slot(rsx, slot);
    if (seq_idx == -1) {
        printf("[SeqRSXManager] No sequence found for slot %d\n", slot);
        return 0;  // Not an error - already removed
    }

    printf("[SeqRSXManager] Removing sequence %d for slot %d\n", seq_idx, slot);

    // Shift remaining sequences down
    for (int i = seq_idx; i < rsx->num_sequences - 1; i++) {
        rsx->sequences[i] = rsx->sequences[i + 1];
    }
    rsx->num_sequences--;

    // Clear the last entry
    RSXSequence* last_seq = &rsx->sequences[rsx->num_sequences];
    last_seq->name[0] = '\0';
    last_seq->num_phrases = 0;
    last_seq->enabled = 1;
    last_seq->loop = 1;
    last_seq->program_number = 0;

    // Save RSX file to persist changes
    if (rsx_path && rsx_path[0] != '\0') {
        if (samplecrate_rsx_save(rsx, rsx_path) == 0) {
            printf("[SeqRSXManager] Saved to %s\n", rsx_path);
        } else {
            printf("[SeqRSXManager] WARNING: Failed to save RSX file\n");
            return -1;
        }
    }

    return 0;
}
