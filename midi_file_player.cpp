#include "midi_file_player.h"
#include "MidiFile.h"
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>

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

    MidiFileLoopCallback loop_callback;
    void* loop_userdata;

    bool playing;
    bool loop;                 // Loop playback
    float tempo_bpm;           // Current tempo in BPM
    float position_seconds;    // Current playback position in seconds
    float duration_seconds;    // Total duration in seconds
    int ticks_per_quarter;     // MIDI ticks per quarter note (from file)
    int last_tick_processed;   // Last tick that was processed (to prevent duplicates)

    // Track which notes are currently on (for all-notes-off when stopping)
    std::vector<int> active_notes;
};

// Create a new MIDI file player
MidiFilePlayer* midi_file_player_create(void) {
    MidiFilePlayer* player = new MidiFilePlayer();
    player->callback = nullptr;
    player->userdata = nullptr;
    player->loop_callback = nullptr;
    player->loop_userdata = nullptr;
    player->playing = false;
    player->loop = false;  // Default: no looping
    player->tempo_bpm = 125.0f;  // Default BPM is 125
    player->position_seconds = 0.0f;
    player->duration_seconds = 0.0f;
    player->ticks_per_quarter = 480;  // Default TPQN
    player->last_tick_processed = -1;  // No ticks processed yet
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

    // Sort events by tick, with NOTE OFFs before NOTE ONs at the same tick
    // This prevents voice stealing issues when notes retrigger quickly
    std::sort(player->events.begin(), player->events.end(),
              [](const MidiEventState& a, const MidiEventState& b) {
                  if (a.tick == b.tick) {
                      // At same tick: OFF (0) before ON (1)
                      return a.on < b.on;
                  }
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

// Start playback immediately - syncs to global pattern position
void midi_file_player_play(MidiFilePlayer* player) {
    if (!player) return;

    std::cout << "Starting MIDI playback - will sync to current pattern position" << std::endl;
    player->playing = true;
    player->last_tick_processed = -1;  // Reset tick tracking
    player->active_notes.clear();
}

// Legacy function - now just calls play() immediately (no quantization)
int midi_file_player_play_quantized(MidiFilePlayer* player, int current_beat, int quantize_beats) {
    if (!player) return -1;

    (void)current_beat;     // Unused
    (void)quantize_beats;   // Unused

    midi_file_player_play(player);
    return current_beat;  // Return current position
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

// Set the loop restart callback
void midi_file_player_set_loop_callback(MidiFilePlayer* player, MidiFileLoopCallback callback, void* userdata) {
    if (!player) return;
    player->loop_callback = callback;
    player->loop_userdata = userdata;
}

// Set looping mode
void midi_file_player_set_loop(MidiFilePlayer* player, int loop) {
    if (!player) return;
    player->loop = (loop != 0);
}

// Get looping mode
int midi_file_player_get_loop(MidiFilePlayer* player) {
    if (!player) return 0;
    return player->loop ? 1 : 0;
}

// Update playback
void midi_file_player_update(MidiFilePlayer* player, float delta_ms, int current_beat) {
    if (!player || !player->callback) return;

    if (!player->playing) return;

    float old_position = player->position_seconds;
    bool did_wrap_in_fallback = false;  // Track if we already handled wrap in fallback mode
    bool spp_jumped = false;  // Track if SPP jumped (to skip firing missed events)

    // If MIDI clock is active (current_beat >= 0), use beat-based sync for perfect timing
    // Otherwise fall back to delta_ms
    // Note: current_beat is actually total_pulse_count (24 pulses per quarter note)

    // Debug: Log when we enter fallback mode on first frame
    // static bool logged_mode = false;
    // if (!logged_mode) {
    //     std::cout << "MIDI FILE PLAYER: current_beat=" << current_beat
    //               << " (using " << (current_beat >= 0 ? "MIDI CLOCK SYNC" : "FALLBACK MODE") << ")" << std::endl;
    //     logged_mode = true;
    // }

    if (current_beat >= 0) {
        // SIMPLE: current_beat IS the pattern position (0-383 pulses cycling)
        // Convert directly to seconds within the file
        const int PATTERN_LENGTH_PULSES = 384;
        int pattern_pulse = current_beat % PATTERN_LENGTH_PULSES;

        float beats_in_pattern = pattern_pulse / 24.0f;  // Convert pulses to beats
        float seconds_per_beat = 60.0f / player->tempo_bpm;
        player->position_seconds = beats_in_pattern * seconds_per_beat;

        // Debug: log position every 96 pulses (every bar)
        static int last_debug_pulse = -1;
        if (pattern_pulse / 96 != last_debug_pulse / 96) {
            std::cout << "[PLAYER] pulse=" << pattern_pulse << " pos=" << player->position_seconds
                      << "s duration=" << player->duration_seconds << "s" << std::endl;
            last_debug_pulse = pattern_pulse;
        }

        // Loop the position within the file duration
        if (player->loop && player->duration_seconds > 0.0f) {
            player->position_seconds = fmod(player->position_seconds, player->duration_seconds);
        }

        // Stop if reached end and not looping
        if (player->position_seconds >= player->duration_seconds && !player->loop) {
            midi_file_player_stop(player);
            return;
        }
    } else {
        // No MIDI clock - advance position by delta time
        // This is sample-accurate when called from audio callback
        float old_pos = player->position_seconds;
        player->position_seconds += (delta_ms / 1000.0f);


        // Handle reaching the end
        if (player->position_seconds >= player->duration_seconds) {
            if (player->loop) {
                // Loop back to beginning - send all note-offs first for clean loop
                if (player->callback) {
                    for (int note : player->active_notes) {
                        player->callback(note, 0, 0, player->userdata);
                    }
                }
                player->active_notes.clear();

                // Mark that we handled the wrap here (so we don't process events twice below)
                did_wrap_in_fallback = true;

                // Wrap position using modulo to prevent drift accumulation
                player->position_seconds = fmod(player->position_seconds, player->duration_seconds);

                // Fire loop restart callback
                if (player->loop_callback) {
                    player->loop_callback(player->loop_userdata);
                }
            } else {
                // Stop playback
                midi_file_player_stop(player);
                return;
            }
        }
    }

    // Convert current position to ticks
    int new_tick = (int)(player->position_seconds * player->tempo_bpm * player->ticks_per_quarter / 60.0f);

    // Use last_tick_processed to prevent duplicates across frame boundaries
    // On first frame after playback starts, last_tick_processed will be -1
    int old_tick = player->last_tick_processed;

    // Handle loop wrap: ONLY in fallback mode (no MIDI clock)
    // When synced to pattern position, we just play whatever events are at current position
    // NO "catching up" or firing missed events!
    bool did_wrap = did_wrap_in_fallback && (current_beat < 0);

    if (did_wrap) {
        // DON'T reset old_tick here - keep it from last_tick_processed so we only fire
        // events from where we left off to the end, not the entire file!
        // old_tick is already set to player->last_tick_processed above

        // Get the last tick in the file
        int last_tick = 0;
        if (!player->events.empty()) {
            last_tick = player->events.back().tick;
        }

        // Debug wrap events
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::cout << "[" << ms << "] WRAP EVENTS old_tick=" << old_tick
                  << " last_tick=" << last_tick << " new_tick=" << new_tick << std::endl;

        // Fire events from old_tick to end of file
        std::cout << "[WRAP PART 1] Firing events from " << old_tick << " to " << last_tick << std::endl;
        for (const MidiEventState& evt : player->events) {
            if (evt.tick > old_tick && evt.tick <= last_tick) {
                std::cout << "  [WRAP1] tick=" << evt.tick << " note=" << evt.note
                          << " vel=" << evt.velocity << " " << (evt.on ? "ON" : "OFF") << std::endl;
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

        // Fire events from beginning to new_tick (use >= to include tick 0)
        std::cout << "[WRAP PART 2] Firing events from 0 to " << new_tick << std::endl;
        for (const MidiEventState& evt : player->events) {
            if (evt.tick >= 0 && evt.tick <= new_tick) {
                std::cout << "  [WRAP2] tick=" << evt.tick << " note=" << evt.note
                          << " vel=" << evt.velocity << " " << (evt.on ? "ON" : "OFF") << std::endl;
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
        // Note: last_tick_processed will be updated to new_tick at the end of this function
    } else {
        // Normal case: fire events between old_tick and new_tick
        // When synced to sequencer, we trust its position completely
        // Use > (not >=) for old_tick to avoid firing the same event twice across frame boundaries

        // Check if position wrapped (tick went backward)
        bool position_wrapped = (new_tick < old_tick);

        // When synced to sequencer (current_beat >= 0), position wraps are handled by sequencer
        // Just play events in the current time slice, no "catch up"
        if (position_wrapped && current_beat >= 0) {
            // Sequencer wrapped - just reset old_tick to play from new position
            old_tick = new_tick - 1;  // Will fire events from (new_tick-1) to new_tick
        }

        for (const MidiEventState& evt : player->events) {
            bool should_fire = false;

            if (position_wrapped && current_beat < 0) {
                // Fallback mode wrap - fire events from old_tick to end, OR from 0 to new_tick
                should_fire = (evt.tick > old_tick) || (evt.tick <= new_tick);
            } else {
                // Normal forward progression (or sequencer-synced wrap)
                should_fire = (evt.tick > old_tick && evt.tick <= new_tick);
            }

            if (should_fire) {
                // Fire MIDI event
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

    // Update last_tick_processed to the current position
    // This ensures the next frame starts from where we left off (no duplicates!)
    player->last_tick_processed = new_tick;
}

// Update playback with sample-accurate timing (call from audio callback)
void midi_file_player_update_samples(MidiFilePlayer* player, int num_samples, int sample_rate, int current_beat) {
    if (!player || num_samples <= 0 || sample_rate <= 0) return;

    // Use EXACT sample count for perfect timing - no float conversion!
    // Position advances by exactly num_samples every call
    double delta_seconds = (double)num_samples / (double)sample_rate;
    float delta_ms = delta_seconds * 1000.0;

    // Call the existing update function
    midi_file_player_update(player, delta_ms, current_beat);
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

// Legacy function - no longer needed since position syncs to pattern automatically
void midi_file_player_sync_start_beat(MidiFilePlayer* player, int current_pulse) {
    (void)player;
    (void)current_pulse;
    // Position is now always calculated from current_beat, no manual sync needed
}
