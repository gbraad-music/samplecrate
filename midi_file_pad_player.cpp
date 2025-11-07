#include "midi_file_pad_player.h"
#include <iostream>

// Per-pad callback context
struct PadCallbackContext {
    MidiFilePadPlayer* pad_player;
    int pad_index;
};

struct MidiFilePadPlayer {
    MednessSequencer* sequencer;     // Reference to the sequencer (not owned)
    MednessTrack* tracks[MAX_PAD_PLAYERS];  // Tracks for each pad
    void* userdatas[MAX_PAD_PLAYERS];       // User context for each pad
    PadCallbackContext contexts[MAX_PAD_PLAYERS];  // Callback contexts

    MidiFileEventCallback midi_callback;    // Global MIDI callback
    void* midi_userdata;                    // Global MIDI callback userdata (unused now)

    MidiFileLoopCallback loop_callback;     // Global loop callback
    void* loop_userdata;                    // Global loop callback userdata
};

// Internal: MIDI callback wrapper for a specific pad
static void pad_midi_callback(int note, int velocity, int on, void* userdata) {
    PadCallbackContext* ctx = (PadCallbackContext*)userdata;
    if (ctx && ctx->pad_player && ctx->pad_player->midi_callback) {
        // Pass the PadCallbackContext directly so main.cpp can identify this as a pad event
        // (main.cpp will distinguish pads from sequences by checking if userdata is a PadCallbackContext*)
        ctx->pad_player->midi_callback(note, velocity, on, ctx);
    }
}

MidiFilePadPlayer* midi_file_pad_player_create(MednessSequencer* sequencer) {
    MidiFilePadPlayer* pad_player = new MidiFilePadPlayer();

    pad_player->sequencer = sequencer;
    pad_player->midi_callback = nullptr;
    pad_player->midi_userdata = nullptr;
    pad_player->loop_callback = nullptr;
    pad_player->loop_userdata = nullptr;

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        pad_player->tracks[i] = nullptr;
        pad_player->userdatas[i] = nullptr;
        // Initialize callback context for this pad
        pad_player->contexts[i].pad_player = pad_player;
        pad_player->contexts[i].pad_index = i;
    }

    return pad_player;
}

void midi_file_pad_player_destroy(MidiFilePadPlayer* pad_player) {
    if (!pad_player) return;

    // Destroy all tracks
    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        if (pad_player->tracks[i]) {
            medness_track_destroy(pad_player->tracks[i]);
        }
    }

    delete pad_player;
}

int midi_file_pad_player_load(MidiFilePadPlayer* pad_player, int pad_index, const char* filename, void* userdata) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS || !filename) {
        return -1;
    }

    // Unload existing track if any
    if (pad_player->tracks[pad_index]) {
        medness_track_destroy(pad_player->tracks[pad_index]);
        pad_player->tracks[pad_index] = nullptr;
    }

    // Create and load new track
    MednessTrack* track = medness_track_create();
    if (!track) return -1;

    if (medness_track_load_midi_file(track, filename) != 0) {
        medness_track_destroy(track);
        return -1;
    }

    pad_player->tracks[pad_index] = track;
    pad_player->userdatas[pad_index] = userdata;

    // Note: Caller (engine) prints the success message

    return 0;
}

void midi_file_pad_player_unload(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) return;

    // Stop playback if active
    midi_file_pad_player_stop(pad_player, pad_index);

    // Destroy track
    if (pad_player->tracks[pad_index]) {
        medness_track_destroy(pad_player->tracks[pad_index]);
        pad_player->tracks[pad_index] = nullptr;
    }

    pad_player->userdatas[pad_index] = nullptr;
}

void midi_file_pad_player_trigger(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || !pad_player->sequencer || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) {
        return;
    }

    MednessTrack* track = pad_player->tracks[pad_index];
    if (!track) return;

    std::cout << "Starting MIDI playback - will sync to current pattern position" << std::endl;

    // Add track to sequencer with per-pad context
    medness_sequencer_add_track(pad_player->sequencer, pad_index, track,
                                pad_midi_callback, &pad_player->contexts[pad_index]);
}

void midi_file_pad_player_stop(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || !pad_player->sequencer || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) {
        return;
    }

    // Remove track from sequencer
    medness_sequencer_remove_track(pad_player->sequencer, pad_index);
}

void midi_file_pad_player_stop_all(MidiFilePadPlayer* pad_player) {
    if (!pad_player) return;

    for (int i = 0; i < MAX_PAD_PLAYERS; i++) {
        midi_file_pad_player_stop(pad_player, i);
    }
}

void midi_file_pad_player_set_callback(MidiFilePadPlayer* pad_player, MidiFileEventCallback callback, void* userdata) {
    if (!pad_player) return;
    pad_player->midi_callback = callback;
    pad_player->midi_userdata = userdata;
}

void midi_file_pad_player_set_loop_callback(MidiFilePadPlayer* pad_player, MidiFileLoopCallback callback, void* userdata) {
    if (!pad_player) return;
    pad_player->loop_callback = callback;
    pad_player->loop_userdata = userdata;

    // Set sequencer loop callback
    if (pad_player->sequencer) {
        medness_sequencer_set_loop_callback(pad_player->sequencer,
            (SequencerLoopCallback)callback, userdata);
    }
}

void midi_file_pad_player_set_tempo(MidiFilePadPlayer* pad_player, float bpm) {
    if (!pad_player || !pad_player->sequencer) return;
    medness_sequencer_set_bpm(pad_player->sequencer, bpm);
}

void midi_file_pad_player_set_loop(MidiFilePadPlayer* pad_player, int loop) {
    // No-op: sequencer always loops the pattern
    (void)pad_player;
    (void)loop;
}

void midi_file_pad_player_update_all_samples(MidiFilePadPlayer* pad_player, int num_samples, int sample_rate, int current_beat) {
    // No-op: sequencer handles updates in main.cpp
    (void)pad_player;
    (void)num_samples;
    (void)sample_rate;
    (void)current_beat;
}

int midi_file_pad_player_is_playing(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || !pad_player->sequencer || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) {
        return 0;
    }

    return medness_sequencer_slot_is_active(pad_player->sequencer, pad_index);
}

MednessTrack* midi_file_pad_player_get_track(MidiFilePadPlayer* pad_player, int pad_index) {
    if (!pad_player || pad_index < 0 || pad_index >= MAX_PAD_PLAYERS) {
        return nullptr;
    }
    return pad_player->tracks[pad_index];
}

// Helper: Check if userdata is a pad context
// We use a simple heuristic: pad contexts are struct pointers with known memory layout
// Sequence events use int* which will have a small value when dereferenced
int midi_file_is_pad_context(void* userdata) {
    if (!userdata) return 0;

    // Try to dereference as PadCallbackContext
    PadCallbackContext* ctx = (PadCallbackContext*)userdata;

    // Simple sanity check: pad_index should be in valid range (0-31)
    // and pad_player should be non-null
    if (ctx->pad_player && ctx->pad_index >= 0 && ctx->pad_index < MAX_PAD_PLAYERS) {
        return 1;
    }
    return 0;
}

// Helper: Get pad index from pad context
int midi_file_get_pad_index(void* userdata) {
    if (!midi_file_is_pad_context(userdata)) return -1;

    PadCallbackContext* ctx = (PadCallbackContext*)userdata;
    return ctx->pad_index;
}
