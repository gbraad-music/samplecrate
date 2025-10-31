#include "midi_file_pad_player.h"
#include <cstring>

struct MidiFilePadPlayer {
    MidiFilePlayer* players[MAX_PAD_PLAYERS];
    MidiFileEventCallback callback;
    void* userdata;
    MidiFileLoopCallback loop_callback;
    void* loop_userdata;
    float tempo_bpm;
};

// Create a new pad player manager
MidiFilePadPlayer* midi_file_pad_player_create(void) {
    MidiFilePadPlayer* pad_player = new MidiFilePadPlayer();

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        pad_player->players[i] = nullptr;
    }

    pad_player->callback = nullptr;
    pad_player->userdata = nullptr;
    pad_player->loop_callback = nullptr;
    pad_player->loop_userdata = nullptr;
    pad_player->tempo_bpm = 125.0f;  // Default BPM is 125

    return pad_player;
}

// Destroy a pad player manager
void midi_file_pad_player_destroy(MidiFilePadPlayer* pad_player) {
    if (!pad_player) return;

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_destroy(pad_player->players[i]);
        }
    }

    delete pad_player;
}

// Load a MIDI file for a specific pad
int midi_file_pad_player_load(MidiFilePadPlayer* pad_player, int pad_index, const char* filename, void* userdata) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS || !filename) {
        return -1;
    }

    // Unload existing player if any
    if (pad_player->players[pad_index]) {
        midi_file_player_destroy(pad_player->players[pad_index]);
        pad_player->players[pad_index] = nullptr;
    }

    // Create new player
    MidiFilePlayer* player = midi_file_player_create();
    if (!player) return -1;

    // Load MIDI file
    if (midi_file_player_load(player, filename) != 0) {
        midi_file_player_destroy(player);
        return -1;
    }

    // Set callback with per-pad userdata, tempo, loop callback, and loop mode
    midi_file_player_set_callback(player, pad_player->callback, userdata);
    midi_file_player_set_loop_callback(player, pad_player->loop_callback, userdata);  // Use same userdata for loop callback
    midi_file_player_set_tempo(player, pad_player->tempo_bpm);
    midi_file_player_set_loop(player, 1);  // Enable looping by default

    pad_player->players[pad_index] = player;
    return 0;
}

// Unload MIDI file for a specific pad
void midi_file_pad_player_unload(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return;

    if (pad_player->players[pad_index]) {
        midi_file_player_destroy(pad_player->players[pad_index]);
        pad_player->players[pad_index] = nullptr;
    }
}

// Trigger playback for a specific pad (immediately, no quantization)
void midi_file_pad_player_trigger(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return;

    MidiFilePlayer* player = pad_player->players[pad_index];
    if (player) {
        midi_file_player_play(player);
    }
}

// Trigger playback for a specific pad with quantization
void midi_file_pad_player_trigger_quantized(MidiFilePadPlayer* pad_player, int pad_index, int current_beat, int quantize_beats) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return;

    MidiFilePlayer* player = pad_player->players[pad_index];
    if (player) {
        midi_file_player_play_quantized(player, current_beat, quantize_beats);
    }
}

// Stop playback for a specific pad
void midi_file_pad_player_stop(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return;

    MidiFilePlayer* player = pad_player->players[pad_index];
    if (player) {
        midi_file_player_stop(player);
    }
}

// Stop all playback
void midi_file_pad_player_stop_all(MidiFilePadPlayer* pad_player) {
    if (!pad_player) return;

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_stop(pad_player->players[i]);
        }
    }
}

// Set the MIDI event callback for all pad players
void midi_file_pad_player_set_callback(MidiFilePadPlayer* pad_player, MidiFileEventCallback callback, void* userdata) {
    if (!pad_player) return;

    pad_player->callback = callback;
    pad_player->userdata = userdata;

    // Update all existing players
    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_set_callback(pad_player->players[i], callback, userdata);
        }
    }
}

// Set the loop restart callback for all pad players
void midi_file_pad_player_set_loop_callback(MidiFilePadPlayer* pad_player, MidiFileLoopCallback callback, void* userdata) {
    if (!pad_player) return;

    pad_player->loop_callback = callback;
    pad_player->loop_userdata = userdata;

    // No need to update existing players - they get it when loaded
}

// Set tempo for all pad players
void midi_file_pad_player_set_tempo(MidiFilePadPlayer* pad_player, float bpm) {
    if (!pad_player || bpm <= 0.0f) return;

    pad_player->tempo_bpm = bpm;

    // Update all existing players
    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_set_tempo(pad_player->players[i], bpm);
        }
    }
}

// Set looping for all pad players
void midi_file_pad_player_set_loop(MidiFilePadPlayer* pad_player, int loop) {
    if (!pad_player) return;

    // Update all existing players
    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_set_loop(pad_player->players[i], loop);
        }
    }
}

// Update all active pad players
void midi_file_pad_player_update_all(MidiFilePadPlayer* pad_player, float delta_ms, int current_beat) {
    if (!pad_player) return;

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->players[i]) {
            midi_file_player_update(pad_player->players[i], delta_ms, current_beat);
        }
    }
}

// Check if a specific pad is playing
int midi_file_pad_player_is_playing(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return 0;

    MidiFilePlayer* player = pad_player->players[pad_index];
    if (!player) return 0;

    return midi_file_player_is_playing(player);
}
