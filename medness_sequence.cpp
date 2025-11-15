#include "medness_sequence.h"
#include "medness_sequencer.h"
#include "medness_track.h"
#include <vector>
#include <string>
#include <iostream>

// Internal structure for a phrase in the sequence
struct Phrase {
    std::string filename;
    std::string name;
    int loop_count;        // How many times to play (0 = infinite)
    MednessTrack* track;   // Using MednessTrack like pads do!
};

struct MednessSequence {
    MednessSequencer* sequencer;  // Reference to shared sequencer (not owned)
    std::vector<Phrase> phrases;
    int current_phrase_index;
    int current_phrase_loop;     // How many times current phrase has completed
    int sequencer_slot;          // Which slot in sequencer we're using
    bool playing;
    bool sequence_loop;          // Loop entire sequence
    float tempo_bpm;

    MednessSequenceEventCallback callback;
    void* userdata;

    MednessSequencePhraseChangeCallback phrase_change_callback;
    void* phrase_change_userdata;
};

// Sequencer slot allocation for sequences
// Pads use slots 0-31, sequences use slots 32-47 (16 sequences max)
#define SEQUENCE_SLOT_BASE 32
#define MAX_SEQUENCES 16

// Internal callback from MednessSequencer for MIDI events
static void sequence_midi_callback(int note, int velocity, int on, void* userdata) {
    MednessSequence* seq = (MednessSequence*)userdata;
    if (!seq) {
        std::cout << "[SEQUENCE CALLBACK] ERROR: seq is NULL!" << std::endl;
        return;
    }

    std::cout << "[SEQUENCE CALLBACK] slot=" << seq->sequencer_slot
              << " note=" << note << " vel=" << velocity << " on=" << on << std::endl;

    if (!seq->callback) {
        std::cout << "[SEQUENCE CALLBACK] WARNING: No user callback set!" << std::endl;
        return;
    }

    // Pass through to user callback
    seq->callback(note, velocity, on, seq->userdata);
}

// Internal callback from MednessSequencer when track loops
static void sequence_loop_callback(void* userdata) {
    MednessSequence* seq = (MednessSequence*)userdata;
    if (!seq) return;

    seq->current_phrase_loop++;

    // Check if we should advance to next phrase
    if (seq->current_phrase_index >= 0 &&
        seq->current_phrase_index < (int)seq->phrases.size()) {

        Phrase& phrase = seq->phrases[seq->current_phrase_index];

        // If loop_count is 0, stay on this phrase forever
        if (phrase.loop_count == 0) {
            std::cout << "[SEQUENCE] Phrase " << seq->current_phrase_index
                      << " (" << phrase.name << ") looping infinitely (loop #"
                      << seq->current_phrase_loop << ")" << std::endl;
            return;
        }

        // Check if we've completed the required loops for this phrase
        if (seq->current_phrase_loop >= phrase.loop_count) {
            std::cout << "[SEQUENCE] Phrase " << seq->current_phrase_index
                      << " (" << phrase.name << ") completed "
                      << seq->current_phrase_loop << " loops" << std::endl;

            // Move to next phrase
            int next_index = seq->current_phrase_index + 1;

            // Check if we've reached the end of the sequence
            if (next_index >= (int)seq->phrases.size()) {
                if (seq->sequence_loop) {
                    // Loop back to beginning
                    std::cout << "[SEQUENCE] End of sequence, looping back to start" << std::endl;
                    next_index = 0;
                } else {
                    // Stop playback
                    std::cout << "[SEQUENCE] End of sequence, stopping" << std::endl;
                    seq->playing = false;
                    medness_sequencer_remove_track(seq->sequencer, seq->sequencer_slot);
                    return;
                }
            }

            // Remove current phrase from sequencer
            medness_sequencer_remove_track(seq->sequencer, seq->sequencer_slot);

            // Switch to next phrase
            seq->current_phrase_index = next_index;
            seq->current_phrase_loop = 0;

            if (seq->current_phrase_index < (int)seq->phrases.size()) {
                Phrase& next_phrase = seq->phrases[seq->current_phrase_index];

                std::cout << "[SEQUENCE] Starting phrase " << seq->current_phrase_index
                          << " (" << next_phrase.name << ")" << std::endl;

                // Add the new phrase track to sequencer
                if (next_phrase.track) {
                    medness_sequencer_add_track(seq->sequencer, seq->sequencer_slot,
                                               next_phrase.track,
                                               sequence_midi_callback, seq);
                }

                // Fire phrase change callback
                if (seq->phrase_change_callback) {
                    seq->phrase_change_callback(seq->current_phrase_index,
                                               next_phrase.name.c_str(),
                                               seq->phrase_change_userdata);
                }
            }
        } else {
            std::cout << "[SEQUENCE] Phrase " << seq->current_phrase_index
                      << " (" << phrase.name << ") loop #"
                      << seq->current_phrase_loop << "/" << phrase.loop_count << std::endl;
        }
    }
}

// Create a new MIDI sequence player
MednessSequence* medness_sequence_create(void) {
    MednessSequence* seq = new MednessSequence();
    seq->sequencer = nullptr;  // Will be set externally
    seq->current_phrase_index = -1;
    seq->current_phrase_loop = 0;
    seq->sequencer_slot = -1;  // Will be assigned
    seq->playing = false;
    seq->sequence_loop = true;  // Default: loop sequence
    seq->tempo_bpm = 125.0f;
    seq->callback = nullptr;
    seq->userdata = nullptr;
    seq->phrase_change_callback = nullptr;
    seq->phrase_change_userdata = nullptr;
    return seq;
}

// Destroy a MIDI sequence player
void medness_sequence_destroy(MednessSequence* player) {
    if (!player) return;

    // Stop playback first
    medness_sequence_stop(player);

    // Clean up all phrase tracks
    for (Phrase& phrase : player->phrases) {
        if (phrase.track) {
            medness_track_destroy(phrase.track);
        }
    }

    delete player;
}

// Set the sequencer reference and slot number
void medness_sequence_set_sequencer(MednessSequence* player, MednessSequencer* sequencer, int slot) {
    if (!player) return;
    player->sequencer = sequencer;
    player->sequencer_slot = slot;
}

// Add a phrase to the sequence
int medness_sequence_add_phrase(MednessSequence* player, const char* filename, int loop_count, const char* name) {
    if (!player || !filename) return -1;

    // Create a new track for this phrase (like pads do!)
    MednessTrack* track = medness_track_create();
    if (!track) return -1;

    // Load the MIDI file into the track
    if (medness_track_load_midi_file(track, filename) != 0) {
        medness_track_destroy(track);
        std::cerr << "[SEQUENCE] Failed to load MIDI file: " << filename << std::endl;
        return -1;
    }

    // Create phrase entry
    Phrase phrase;
    phrase.filename = filename;
    phrase.name = name ? name : filename;
    phrase.loop_count = loop_count;
    phrase.track = track;

    player->phrases.push_back(phrase);

    int phrase_index = (int)player->phrases.size() - 1;
    std::cout << "[SEQUENCE] Added phrase " << phrase_index << ": " << phrase.name
              << " (loops: " << (loop_count == 0 ? "infinite" : std::to_string(loop_count)) << ")" << std::endl;

    return phrase_index;
}

// Clear all phrases from the sequence
void medness_sequence_clear_phrases(MednessSequence* player) {
    if (!player) return;

    // Stop if playing
    medness_sequence_stop(player);

    // Clean up all tracks
    for (Phrase& phrase : player->phrases) {
        if (phrase.track) {
            medness_track_destroy(phrase.track);
        }
    }

    player->phrases.clear();
    player->current_phrase_index = -1;
    player->current_phrase_loop = 0;
}

// Get number of phrases in the sequence
int medness_sequence_get_phrase_count(MednessSequence* player) {
    if (!player) return 0;
    return (int)player->phrases.size();
}

// Get current phrase index
int medness_sequence_get_current_phrase(MednessSequence* player) {
    if (!player) return -1;
    return player->current_phrase_index;
}

// Get current phrase loop count
int medness_sequence_get_current_phrase_loop(MednessSequence* player) {
    if (!player) return 0;
    return player->current_phrase_loop;
}

// Start playback
void medness_sequence_play(MednessSequence* player) {
    if (!player || !player->sequencer) return;

    if (player->phrases.empty()) {
        std::cerr << "[SEQUENCE] Cannot play: no phrases loaded" << std::endl;
        return;
    }

    std::cout << "[SEQUENCE] Starting playback with " << player->phrases.size() << " phrases" << std::endl;

    player->playing = true;
    player->current_phrase_index = 0;
    player->current_phrase_loop = 0;

    // Start the first phrase by adding its track to the sequencer
    Phrase& first_phrase = player->phrases[0];
    if (first_phrase.track) {
        std::cout << "[SEQUENCE] Starting phrase 0: " << first_phrase.name << std::endl;

        // Debug: check track event count
        int event_count = 0;
        medness_track_get_events(first_phrase.track, &event_count);
        std::cout << "[SEQUENCE] Track has " << event_count << " events" << std::endl;
        std::cout << "[SEQUENCE] Adding track to sequencer slot " << player->sequencer_slot << std::endl;

        // Add track to sequencer with loop callback
        medness_sequencer_add_track(player->sequencer, player->sequencer_slot,
                                   first_phrase.track,
                                   sequence_midi_callback, player);

        // Set the loop callback to handle phrase transitions
        medness_sequencer_set_loop_callback(player->sequencer, sequence_loop_callback, player);

        // Fire phrase change callback
        if (player->phrase_change_callback) {
            player->phrase_change_callback(0, first_phrase.name.c_str(),
                                          player->phrase_change_userdata);
        }
    }
}

// Stop playback
void medness_sequence_stop(MednessSequence* player) {
    if (!player || !player->sequencer) return;

    std::cout << "[SEQUENCE] Stopping playback (slot=" << player->sequencer_slot << ")" << std::endl;

    player->playing = false;

    // Remove track from sequencer
    medness_sequencer_remove_track(player->sequencer, player->sequencer_slot);

    player->current_phrase_index = -1;
    player->current_phrase_loop = 0;
}

// Check if currently playing
int medness_sequence_is_playing(MednessSequence* player) {
    if (!player) return 0;
    return player->playing ? 1 : 0;
}

// Set tempo in BPM
void medness_sequence_set_tempo(MednessSequence* player, float bpm) {
    if (!player || !player->sequencer) return;
    player->tempo_bpm = bpm;
    // Tempo is controlled by the sequencer globally
    medness_sequencer_set_bpm(player->sequencer, bpm);
}

// Get current tempo
float medness_sequence_get_tempo(MednessSequence* player) {
    if (!player) return 125.0f;
    return player->tempo_bpm;
}

// Set the MIDI event callback
void medness_sequence_set_callback(MednessSequence* player, MednessSequenceEventCallback callback, void* userdata) {
    if (!player) return;
    player->callback = callback;
    player->userdata = userdata;
}

// Set the phrase change callback
void medness_sequence_set_phrase_change_callback(MednessSequence* player, MednessSequencePhraseChangeCallback callback, void* userdata) {
    if (!player) return;
    player->phrase_change_callback = callback;
    player->phrase_change_userdata = userdata;
}

// Set sequence looping mode
void medness_sequence_set_loop(MednessSequence* player, int loop) {
    if (!player) return;
    player->sequence_loop = (loop != 0);
}

// Get sequence looping mode
int medness_sequence_get_loop(MednessSequence* player) {
    if (!player) return 1;
    return player->sequence_loop ? 1 : 0;
}

// These update functions are no longer needed - sequencer handles timing!
void medness_sequence_update(MednessSequence* player, float delta_ms, int current_beat) {
    // No-op: MednessSequencer handles all timing
}

void medness_sequence_update_samples(MednessSequence* player, int num_samples, int sample_rate, int current_beat) {
    // No-op: MednessSequencer handles all timing
}

// Jump to a specific phrase
int medness_sequence_jump_to_phrase(MednessSequence* player, int phrase_index) {
    if (!player || !player->sequencer) return -1;
    if (phrase_index < 0 || phrase_index >= (int)player->phrases.size()) return -1;

    // Remove current track
    if (player->current_phrase_index >= 0) {
        medness_sequencer_remove_track(player->sequencer, player->sequencer_slot);
    }

    // Switch to new phrase
    player->current_phrase_index = phrase_index;
    player->current_phrase_loop = 0;

    std::cout << "[SEQUENCE] Jumping to phrase " << phrase_index << std::endl;

    // Start new phrase if playing
    if (player->playing) {
        Phrase& new_phrase = player->phrases[phrase_index];
        if (new_phrase.track) {
            medness_sequencer_add_track(player->sequencer, player->sequencer_slot,
                                       new_phrase.track,
                                       sequence_midi_callback, player);
        }
    }

    return 0;
}

// Duration/position functions - would need track introspection
float medness_sequence_get_current_phrase_duration(MednessSequence* player) {
    // Would need to query track length from MednessTrack
    return 0.0f;
}

float medness_sequence_get_current_phrase_position(MednessSequence* player) {
    // Would need to query current position from sequencer
    return 0.0f;
}

MednessTrack* medness_sequence_get_current_track(MednessSequence* player) {
    if (!player || player->current_phrase_index < 0) return nullptr;
    if (player->current_phrase_index >= (int)player->phrases.size()) return nullptr;

    return player->phrases[player->current_phrase_index].track;
}
