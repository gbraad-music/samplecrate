#include "midi_file_player.h"
#include "MidiFile.h"
#include <chrono>
#include <vector>
#include <algorithm>

using namespace smf;

// Internal structure for MIDI event tracking
struct MidiEventState {
    int tick;           // MIDI tick when event occurs
    int note;           // Note number
    int velocity;       // Note velocity
    int on;             // 1=note_on, 0=note_off
};

struct MidiFilePlayer {
    MidiFile midifile;
    std::vector<MidiEventState> events;

    MidiFileEventCallback callback;
    void* userdata;

    bool playing;
    float tempo_bpm;           // Current tempo in BPM
    float position_seconds;    // Current playback position in seconds
    float duration_seconds;    // Total duration in seconds
    int ticks_per_quarter;     // MIDI ticks per quarter note (from file)

    // Track which notes are currently on (for all-notes-off when stopping)
    std::vector<int> active_notes;
};

// Create a new MIDI file player
MidiFilePlayer* midi_file_player_create(void) {
    MidiFilePlayer* player = new MidiFilePlayer();
    player->callback = nullptr;
    player->userdata = nullptr;
    player->playing = false;
    player->tempo_bpm = 125.0f;  // Default BPM is 125
    player->position_seconds = 0.0f;
    player->duration_seconds = 0.0f;
    player->ticks_per_quarter = 480;  // Default TPQN
    return player;
}

// Destroy a MIDI file player
void midi_file_player_destroy(MidiFilePlayer* player) {
    if (!player) return;
    delete player;
}

// Load a MIDI file from disk
int midi_file_player_load(MidiFilePlayer* player, const char* filename) {
    if (!player || !filename) return -1;

    // Load the MIDI file
    if (!player->midifile.read(filename)) {
        return -1;
    }

    // Make absolute ticks
    player->midifile.doTimeAnalysis();
    player->midifile.linkNotePairs();

    player->ticks_per_quarter = player->midifile.getTicksPerQuarterNote();

    // Extract note events from all tracks
    player->events.clear();

    for (int track = 0; track < player->midifile.getTrackCount(); track++) {
        for (int event = 0; event < player->midifile[track].size(); event++) {
            MidiEvent& me = player->midifile[track][event];

            if (me.isNoteOn()) {
                MidiEventState evt;
                evt.tick = me.tick;
                evt.note = me.getKeyNumber();
                evt.velocity = me.getVelocity();
                evt.on = 1;
                player->events.push_back(evt);
            } else if (me.isNoteOff()) {
                MidiEventState evt;
                evt.tick = me.tick;
                evt.note = me.getKeyNumber();
                evt.velocity = 0;
                evt.on = 0;
                player->events.push_back(evt);
            }
        }
    }

    // Sort events by tick
    std::sort(player->events.begin(), player->events.end(),
              [](const MidiEventState& a, const MidiEventState& b) {
                  return a.tick < b.tick;
              });

    // Calculate duration
    if (!player->events.empty()) {
        int last_tick = player->events.back().tick;
        // Duration = (ticks / ticks_per_quarter) * (60 / bpm) * quarters
        // = ticks * 60 / (ticks_per_quarter * bpm)
        player->duration_seconds = (float)last_tick * 60.0f / (player->ticks_per_quarter * player->tempo_bpm);
    } else {
        player->duration_seconds = 0.0f;
    }

    return 0;
}

// Start playback
void midi_file_player_play(MidiFilePlayer* player) {
    if (!player) return;

    std::cout << "midi_file_player_play: Starting playback, event_count=" << player->events.size()
              << " duration=" << player->duration_seconds << "s" << std::endl;

    player->playing = true;
    player->position_seconds = 0.0f;
    player->active_notes.clear();
}

// Stop playback
void midi_file_player_stop(MidiFilePlayer* player) {
    if (!player) return;

    player->playing = false;

    // Send note-off for all active notes
    if (player->callback) {
        for (int note : player->active_notes) {
            player->callback(note, 0, 0, player->userdata);
        }
    }
    player->active_notes.clear();
}

// Check if currently playing
int midi_file_player_is_playing(MidiFilePlayer* player) {
    if (!player) return 0;
    return player->playing ? 1 : 0;
}

// Set tempo in BPM
void midi_file_player_set_tempo(MidiFilePlayer* player, float bpm) {
    if (!player || bpm <= 0.0f) return;

    player->tempo_bpm = bpm;

    // Recalculate duration with new tempo
    if (!player->events.empty()) {
        int last_tick = player->events.back().tick;
        player->duration_seconds = (float)last_tick * 60.0f / (player->ticks_per_quarter * player->tempo_bpm);
    }
}

// Get current tempo
float midi_file_player_get_tempo(MidiFilePlayer* player) {
    if (!player) return 125.0f;
    return player->tempo_bpm;
}

// Set the MIDI event callback
void midi_file_player_set_callback(MidiFilePlayer* player, MidiFileEventCallback callback, void* userdata) {
    if (!player) return;
    player->callback = callback;
    player->userdata = userdata;
}

// Update playback
void midi_file_player_update(MidiFilePlayer* player, float delta_ms) {
    if (!player || !player->playing || !player->callback) return;

    float old_position = player->position_seconds;
    player->position_seconds += delta_ms / 1000.0f;

    // Stop if we reached the end
    if (player->position_seconds >= player->duration_seconds) {
        midi_file_player_stop(player);
        return;
    }

    // Convert positions to ticks
    int old_tick = (int)(old_position * player->tempo_bpm * player->ticks_per_quarter / 60.0f);
    int new_tick = (int)(player->position_seconds * player->tempo_bpm * player->ticks_per_quarter / 60.0f);

    // Fire events between old_tick and new_tick
    for (const MidiEventState& evt : player->events) {
        if (evt.tick > old_tick && evt.tick <= new_tick) {
            player->callback(evt.note, evt.velocity, evt.on, player->userdata);

            // Track active notes
            if (evt.on) {
                player->active_notes.push_back(evt.note);
            } else {
                auto it = std::find(player->active_notes.begin(), player->active_notes.end(), evt.note);
                if (it != player->active_notes.end()) {
                    player->active_notes.erase(it);
                }
            }
        }
    }
}

// Get current playback position in seconds
float midi_file_player_get_position(MidiFilePlayer* player) {
    if (!player) return 0.0f;
    return player->position_seconds;
}

// Seek to a position in seconds
void midi_file_player_seek(MidiFilePlayer* player, float seconds) {
    if (!player) return;

    // Send note-off for all active notes
    if (player->callback) {
        for (int note : player->active_notes) {
            player->callback(note, 0, 0, player->userdata);
        }
    }
    player->active_notes.clear();

    player->position_seconds = seconds;
    if (player->position_seconds < 0.0f) player->position_seconds = 0.0f;
    if (player->position_seconds > player->duration_seconds) player->position_seconds = player->duration_seconds;
}

// Get total duration in seconds
float midi_file_player_get_duration(MidiFilePlayer* player) {
    if (!player) return 0.0f;
    return player->duration_seconds;
}
