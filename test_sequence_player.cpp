// Simple test program for MIDI Sequence Player
// Compile: g++ -o test_sequence test_sequence_player.cpp midi_sequence_player.cpp midi_file_player.cpp external/midifile/src/*.cpp -Iexternal/midifile/include -std=c++11

#include "midi_sequence_player.h"
#include <iostream>
#include <unistd.h>

// Callback for MIDI events
void midi_callback(int note, int velocity, int on, void* userdata) {
    std::cout << "  MIDI: Note " << note << " " << (on ? "ON" : "OFF")
              << " (vel=" << velocity << ")" << std::endl;
}

// Callback for phrase changes
void phrase_change_callback(int phrase_index, const char* phrase_name, void* userdata) {
    std::cout << "\n*** PHRASE CHANGE: [" << phrase_index << "] "
              << phrase_name << " ***\n" << std::endl;
}

int main(int argc, char** argv) {
    std::cout << "=== MIDI Sequence Player Test ===" << std::endl;

    // Create sequence player
    MidiSequencePlayer* seq = midi_sequence_player_create();
    if (!seq) {
        std::cerr << "Failed to create sequence player" << std::endl;
        return 1;
    }

    // Add test phrases (you'll need to create these MIDI files or use existing ones)
    std::cout << "\nAdding phrases to sequence..." << std::endl;

    // For testing, we'll try to add some MIDI files
    // Adjust these paths to match your actual MIDI files
    int phrase1 = midi_sequence_player_add_phrase(seq, "test1.mid", 2, "Pattern A");
    int phrase2 = midi_sequence_player_add_phrase(seq, "test2.mid", 1, "Pattern B");
    int phrase3 = midi_sequence_player_add_phrase(seq, "test3.mid", 3, "Pattern C");

    if (phrase1 < 0 || phrase2 < 0 || phrase3 < 0) {
        std::cerr << "\nWarning: Failed to load some MIDI files." << std::endl;
        std::cerr << "Please create test1.mid, test2.mid, test3.mid or modify the paths." << std::endl;

        // Clean up
        midi_sequence_player_destroy(seq);
        return 1;
    }

    std::cout << "Added " << midi_sequence_player_get_phrase_count(seq) << " phrases" << std::endl;

    // Set tempo
    midi_sequence_player_set_tempo(seq, 120.0f);
    std::cout << "Tempo: " << midi_sequence_player_get_tempo(seq) << " BPM" << std::endl;

    // Enable looping
    midi_sequence_player_set_loop(seq, 1);
    std::cout << "Loop: " << (midi_sequence_player_get_loop(seq) ? "ON" : "OFF") << std::endl;

    // Set callbacks
    midi_sequence_player_set_callback(seq, midi_callback, nullptr);
    midi_sequence_player_set_phrase_change_callback(seq, phrase_change_callback, nullptr);

    // Start playback
    std::cout << "\n=== Starting Playback ===" << std::endl;
    midi_sequence_player_play(seq);

    // Simulate playback for 30 seconds
    // In a real application, you'd call update_samples() from your audio callback
    std::cout << "Running for 30 seconds (press Ctrl+C to stop)..." << std::endl;

    float delta_ms = 10.0f;  // 10ms updates
    int updates = 0;
    int max_updates = 3000;  // 30 seconds at 10ms intervals

    while (updates < max_updates && midi_sequence_player_is_playing(seq)) {
        // Update the player (pass -1 for no MIDI clock)
        midi_sequence_player_update(seq, delta_ms, -1);

        // Print status every second
        if (updates % 100 == 0) {
            int current_phrase = midi_sequence_player_get_current_phrase(seq);
            int phrase_loop = midi_sequence_player_get_current_phrase_loop(seq);
            float position = midi_sequence_player_get_current_phrase_position(seq);
            float duration = midi_sequence_player_get_current_phrase_duration(seq);

            std::cout << "[" << (updates / 100) << "s] Phrase " << current_phrase
                      << " Loop " << phrase_loop
                      << " Position: " << position << "/" << duration << "s"
                      << std::endl;
        }

        usleep(10000);  // Sleep for 10ms
        updates++;
    }

    // Stop playback
    std::cout << "\n=== Stopping Playback ===" << std::endl;
    midi_sequence_player_stop(seq);

    // Clean up
    midi_sequence_player_destroy(seq);

    std::cout << "Test completed successfully!" << std::endl;
    return 0;
}
