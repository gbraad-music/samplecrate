#include "midi_sequence_player.h"
#include "midi_file_player.h"
#include <vector>
#include <string>
#include <iostream>

// Internal structure for a phrase in the sequence
struct Phrase {
    std::string filename;
    std::string name;
    int loop_count;        // How many times to play (0 = infinite)
    MidiFilePlayer* player;
};

struct MidiSequencePlayer {
    std::vector<Phrase> phrases;
    int current_phrase_index;
    int current_phrase_loop;     // How many times current phrase has completed
    bool playing;
    bool sequence_loop;          // Loop entire sequence
    float tempo_bpm;

    MidiSequenceEventCallback callback;
    void* userdata;

    MidiSequencePhraseChangeCallback phrase_change_callback;
    void* phrase_change_userdata;
};

// Internal callback to handle when current phrase loops
static void phrase_loop_callback(void* userdata) {
    MidiSequencePlayer* seq = (MidiSequencePlayer*)userdata;
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
                    std::cout << "[SEQUENCE] End of sequence, stopping playback" << std::endl;
                    midi_sequence_player_stop(seq);
                    return;
                }
            }

            // Stop current phrase
            if (phrase.player) {
                midi_file_player_stop(phrase.player);
            }

            // Switch to next phrase
            seq->current_phrase_index = next_index;
            seq->current_phrase_loop = 0;

            if (seq->current_phrase_index < (int)seq->phrases.size()) {
                Phrase& next_phrase = seq->phrases[seq->current_phrase_index];

                std::cout << "[SEQUENCE] Starting phrase " << seq->current_phrase_index
                          << " (" << next_phrase.name << ")" << std::endl;

                // Start the new phrase
                if (next_phrase.player) {
                    midi_file_player_play(next_phrase.player);
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

// Internal callback to forward MIDI events to sequence callback
static void phrase_event_callback(int note, int velocity, int on, void* userdata) {
    MidiSequencePlayer* seq = (MidiSequencePlayer*)userdata;
    if (!seq || !seq->callback) return;

    seq->callback(note, velocity, on, seq->userdata);
}

// Create a new MIDI sequence player
MidiSequencePlayer* midi_sequence_player_create(void) {
    MidiSequencePlayer* seq = new MidiSequencePlayer();
    seq->current_phrase_index = -1;
    seq->current_phrase_loop = 0;
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
void midi_sequence_player_destroy(MidiSequencePlayer* player) {
    if (!player) return;

    // Destroy all phrase players
    for (Phrase& phrase : player->phrases) {
        if (phrase.player) {
            midi_file_player_destroy(phrase.player);
        }
    }

    delete player;
}

// Add a phrase to the sequence
int midi_sequence_player_add_phrase(MidiSequencePlayer* player, const char* filename, int loop_count, const char* name) {
    if (!player || !filename) return -1;

    // Create a new MIDI file player for this phrase
    MidiFilePlayer* file_player = midi_file_player_create();
    if (!file_player) return -1;

    // Load the MIDI file
    if (midi_file_player_load(file_player, filename) != 0) {
        std::cerr << "[SEQUENCE] Failed to load MIDI file: " << filename << std::endl;
        midi_file_player_destroy(file_player);
        return -1;
    }

    // Set tempo
    midi_file_player_set_tempo(file_player, player->tempo_bpm);

    // Enable looping on the phrase player
    midi_file_player_set_loop(file_player, 1);

    // Set callbacks
    midi_file_player_set_callback(file_player, phrase_event_callback, player);
    midi_file_player_set_loop_callback(file_player, phrase_loop_callback, player);

    // Create phrase entry
    Phrase phrase;
    phrase.filename = filename;
    phrase.name = name ? name : filename;
    phrase.loop_count = loop_count;
    phrase.player = file_player;

    player->phrases.push_back(phrase);

    int phrase_index = (int)player->phrases.size() - 1;
    std::cout << "[SEQUENCE] Added phrase " << phrase_index << ": " << phrase.name
              << " (loops: " << (loop_count == 0 ? "infinite" : std::to_string(loop_count)) << ")" << std::endl;

    return phrase_index;
}

// Clear all phrases from the sequence
void midi_sequence_player_clear_phrases(MidiSequencePlayer* player) {
    if (!player) return;

    // Stop playback first
    midi_sequence_player_stop(player);

    // Destroy all phrase players
    for (Phrase& phrase : player->phrases) {
        if (phrase.player) {
            midi_file_player_destroy(phrase.player);
        }
    }

    player->phrases.clear();
    player->current_phrase_index = -1;
    player->current_phrase_loop = 0;
}

// Get number of phrases in the sequence
int midi_sequence_player_get_phrase_count(MidiSequencePlayer* player) {
    if (!player) return 0;
    return (int)player->phrases.size();
}

// Get current phrase index
int midi_sequence_player_get_current_phrase(MidiSequencePlayer* player) {
    if (!player) return -1;
    return player->current_phrase_index;
}

// Get current phrase loop count
int midi_sequence_player_get_current_phrase_loop(MidiSequencePlayer* player) {
    if (!player) return 0;
    return player->current_phrase_loop;
}

// Start playback
void midi_sequence_player_play(MidiSequencePlayer* player) {
    if (!player) return;

    if (player->phrases.empty()) {
        std::cerr << "[SEQUENCE] Cannot play: no phrases loaded" << std::endl;
        return;
    }

    std::cout << "[SEQUENCE] Starting playback with " << player->phrases.size() << " phrases" << std::endl;

    player->playing = true;
    player->current_phrase_index = 0;
    player->current_phrase_loop = 0;

    // Start the first phrase
    Phrase& first_phrase = player->phrases[0];
    if (first_phrase.player) {
        std::cout << "[SEQUENCE] Starting phrase 0: " << first_phrase.name << std::endl;
        midi_file_player_play(first_phrase.player);

        // Fire phrase change callback
        if (player->phrase_change_callback) {
            player->phrase_change_callback(0, first_phrase.name.c_str(), player->phrase_change_userdata);
        }
    }
}

// Stop playback
void midi_sequence_player_stop(MidiSequencePlayer* player) {
    if (!player) return;

    std::cout << "[SEQUENCE] Stopping playback" << std::endl;

    player->playing = false;

    // Stop all phrase players
    for (Phrase& phrase : player->phrases) {
        if (phrase.player) {
            midi_file_player_stop(phrase.player);
        }
    }

    player->current_phrase_index = -1;
    player->current_phrase_loop = 0;
}

// Check if currently playing
int midi_sequence_player_is_playing(MidiSequencePlayer* player) {
    if (!player) return 0;
    return player->playing ? 1 : 0;
}

// Set tempo in BPM
void midi_sequence_player_set_tempo(MidiSequencePlayer* player, float bpm) {
    if (!player || bpm <= 0.0f) return;

    player->tempo_bpm = bpm;

    // Update tempo for all phrase players
    for (Phrase& phrase : player->phrases) {
        if (phrase.player) {
            midi_file_player_set_tempo(phrase.player, bpm);
        }
    }
}

// Get current tempo
float midi_sequence_player_get_tempo(MidiSequencePlayer* player) {
    if (!player) return 125.0f;
    return player->tempo_bpm;
}

// Set the MIDI event callback
void midi_sequence_player_set_callback(MidiSequencePlayer* player, MidiSequenceEventCallback callback, void* userdata) {
    if (!player) return;
    player->callback = callback;
    player->userdata = userdata;
}

// Set the phrase change callback
void midi_sequence_player_set_phrase_change_callback(MidiSequencePlayer* player, MidiSequencePhraseChangeCallback callback, void* userdata) {
    if (!player) return;
    player->phrase_change_callback = callback;
    player->phrase_change_userdata = userdata;
}

// Set sequence looping mode
void midi_sequence_player_set_loop(MidiSequencePlayer* player, int loop) {
    if (!player) return;
    player->sequence_loop = (loop != 0);
}

// Get sequence looping mode
int midi_sequence_player_get_loop(MidiSequencePlayer* player) {
    if (!player) return 0;
    return player->sequence_loop ? 1 : 0;
}

// Update playback
void midi_sequence_player_update(MidiSequencePlayer* player, float delta_ms, int current_beat) {
    if (!player || !player->playing) return;

    if (player->current_phrase_index < 0 ||
        player->current_phrase_index >= (int)player->phrases.size()) {
        return;
    }

    // Update the current phrase player
    Phrase& phrase = player->phrases[player->current_phrase_index];
    if (phrase.player) {
        midi_file_player_update(phrase.player, delta_ms, current_beat);
    }
}

// Update playback with sample-accurate timing
void midi_sequence_player_update_samples(MidiSequencePlayer* player, int num_samples, int sample_rate, int current_beat) {
    if (!player || !player->playing) return;

    if (player->current_phrase_index < 0 ||
        player->current_phrase_index >= (int)player->phrases.size()) {
        return;
    }

    // Update the current phrase player
    Phrase& phrase = player->phrases[player->current_phrase_index];
    if (phrase.player) {
        midi_file_player_update_samples(phrase.player, num_samples, sample_rate, current_beat);
    }
}

// Jump to a specific phrase
int midi_sequence_player_jump_to_phrase(MidiSequencePlayer* player, int phrase_index) {
    if (!player) return -1;

    if (phrase_index < 0 || phrase_index >= (int)player->phrases.size()) {
        std::cerr << "[SEQUENCE] Invalid phrase index: " << phrase_index << std::endl;
        return -1;
    }

    // Stop current phrase
    if (player->current_phrase_index >= 0 &&
        player->current_phrase_index < (int)player->phrases.size()) {
        Phrase& old_phrase = player->phrases[player->current_phrase_index];
        if (old_phrase.player) {
            midi_file_player_stop(old_phrase.player);
        }
    }

    // Switch to new phrase
    player->current_phrase_index = phrase_index;
    player->current_phrase_loop = 0;

    std::cout << "[SEQUENCE] Jumping to phrase " << phrase_index << std::endl;

    // Start new phrase if playing
    if (player->playing) {
        Phrase& new_phrase = player->phrases[phrase_index];
        if (new_phrase.player) {
            midi_file_player_play(new_phrase.player);
        }

        // Fire phrase change callback
        if (player->phrase_change_callback) {
            player->phrase_change_callback(phrase_index, new_phrase.name.c_str(), player->phrase_change_userdata);
        }
    }

    return 0;
}

// Get total duration of current phrase in seconds
float midi_sequence_player_get_current_phrase_duration(MidiSequencePlayer* player) {
    if (!player) return 0.0f;

    if (player->current_phrase_index < 0 ||
        player->current_phrase_index >= (int)player->phrases.size()) {
        return 0.0f;
    }

    Phrase& phrase = player->phrases[player->current_phrase_index];
    if (phrase.player) {
        return midi_file_player_get_duration(phrase.player);
    }

    return 0.0f;
}

// Get current position within current phrase in seconds
float midi_sequence_player_get_current_phrase_position(MidiSequencePlayer* player) {
    if (!player) return 0.0f;

    if (player->current_phrase_index < 0 ||
        player->current_phrase_index >= (int)player->phrases.size()) {
        return 0.0f;
    }

    Phrase& phrase = player->phrases[player->current_phrase_index];
    if (phrase.player) {
        return midi_file_player_get_position(phrase.player);
    }

    return 0.0f;
}
