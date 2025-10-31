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
    bool scheduled;            // Waiting for quantized start
    int scheduled_start_beat;  // Beat number to start on (-1 = not scheduled)
    int start_beat;            // Beat when playback started (for master clock sync)
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
    player->scheduled = false;  // Not scheduled
    player->scheduled_start_beat = -1;  // No scheduled beat
    player->start_beat = -1;  // No start beat yet
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

// Start playback
void midi_file_player_play(MidiFilePlayer* player) {
    if (!player) return;

    player->playing = true;
    player->scheduled = false;
    player->scheduled_start_beat = -1;
    player->start_beat = -1;  // Will be set on first update
    player->position_seconds = 0.0f;
    player->last_tick_processed = -1;  // Reset tick tracking
    player->active_notes.clear();
}

// Schedule playback to start on a specific beat (for quantization)
int midi_file_player_play_quantized(MidiFilePlayer* player, int current_beat, int quantize_beats) {
    if (!player) return -1;

    // NOTE: current_beat is actually in PULSES (24 ppqn), not beats
    // Convert quantize_beats to pulses: 1 beat = 24 pulses
    int quantize_pulses = quantize_beats * 24;

    // Calculate the next quantized pulse boundary
    int next_pulse = ((current_beat / quantize_pulses) + 1) * quantize_pulses;

    std::cout << "QUANTIZE: Scheduling playback for pulse " << next_pulse
              << " (current=" << current_beat << ", quantize=" << quantize_beats
              << " beats = " << quantize_pulses << " pulses)" << std::endl;

    player->playing = false;  // Not playing yet
    player->scheduled = true;
    player->scheduled_start_beat = next_pulse;
    player->position_seconds = 0.0f;
    player->last_tick_processed = -1;  // Reset tick tracking
    player->active_notes.clear();

    return next_pulse;
}

// Stop playback
void midi_file_player_stop(MidiFilePlayer* player) {
    if (!player) return;

    player->playing = false;
    player->scheduled = false;
    player->scheduled_start_beat = -1;

    // Send note-off for all active notes
    if (player->callback) {
        for (int note : player->active_notes) {
            player->callback(note, 0, 0, player->userdata);
        }
    }
    player->active_notes.clear();
}

// Check if currently playing (includes scheduled playback)
int midi_file_player_is_playing(MidiFilePlayer* player) {
    if (!player) return 0;
    return (player->playing || player->scheduled) ? 1 : 0;
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

    // Check if we're scheduled to start and the beat has arrived
    if (player->scheduled && current_beat >= 0) {
        if (current_beat >= player->scheduled_start_beat) {
            std::cout << "QUANTIZE: Starting scheduled playback NOW on pulse " << current_beat
                      << " (scheduled=" << player->scheduled_start_beat << ")" << std::endl;
            player->scheduled = false;
            player->scheduled_start_beat = -1;
            player->playing = true;
            player->start_beat = current_beat;
            player->position_seconds = 0.0f;

            std::cout << "  Initial state: start_beat=" << player->start_beat
                      << " position=" << player->position_seconds << "s" << std::endl;
        } else {
            // Still waiting for the scheduled beat
            static int wait_count = 0;
            if (wait_count++ % 1000 == 0) {  // Print every 1000 frames to avoid spam
                std::cout << "QUANTIZE: WAITING... current=" << current_beat
                          << " scheduled=" << player->scheduled_start_beat << std::endl;
            }
            return;
        }
    }

    if (!player->playing) return;

    float old_position = player->position_seconds;
    bool did_wrap_in_fallback = false;  // Track if we already handled wrap in fallback mode

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
        // First update: record start pulse count
        if (player->start_beat < 0) {
            player->start_beat = current_beat;
        }

        // Calculate position from pulses elapsed since start
        // MIDI clock sends 24 pulses per quarter note (ppqn)
        int pulses_elapsed = current_beat - player->start_beat;
        float beats_elapsed = pulses_elapsed / 24.0f;  // Convert pulses to beats
        float seconds_per_beat = 60.0f / player->tempo_bpm;
        float new_position = beats_elapsed * seconds_per_beat;

        // Handle looping with beat precision
        if (player->loop && player->duration_seconds > 0.0f) {
            // Detect loop restart BEFORE wrapping position
            bool will_wrap = (new_position >= player->duration_seconds && old_position < player->duration_seconds);

            // Use modulo for seamless looping
            player->position_seconds = fmod(new_position, player->duration_seconds);

            if (will_wrap) {
                // Calculate how many loop cycles have passed
                int loop_cycles = (int)(new_position / player->duration_seconds);

                // Adjust start_beat to maintain sync across loops
                // Each loop cycle represents duration_seconds worth of beats
                // start_beat is in PULSES (not beats), so convert: beats * 24 = pulses
                float beats_per_loop = player->duration_seconds / (60.0f / player->tempo_bpm);
                int pulses_per_loop = (int)(beats_per_loop * 24.0f);
                player->start_beat += loop_cycles * pulses_per_loop;

                // Note: We don't send note-offs here anymore - they'll be handled
                // by the MIDI events themselves when we process the wrap range below

                // Fire loop restart callback
                if (player->loop_callback) {
                    player->loop_callback(player->loop_userdata);
                }
            }
        } else {
            player->position_seconds = new_position;
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

    // Handle loop wrap: if position wrapped around, we need to fire events in two ranges:
    // 1. From old_tick to end of file
    // 2. From beginning (tick 0) to new_tick
    // Note: This can be triggered either by the MIDI clock path OR the fallback path above
    bool did_wrap = did_wrap_in_fallback || ((old_position > player->position_seconds) && player->loop);

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
        // auto now = std::chrono::high_resolution_clock::now();
        // auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        // std::cout << "[" << ms << "] WRAP EVENTS old_tick=" << old_tick
        //           << " last_tick=" << last_tick << " new_tick=" << new_tick << std::endl;

        // Fire events from old_tick to end of file
        // std::cout << "[WRAP PART 1] Firing events from " << old_tick << " to " << last_tick << std::endl;
        for (const MidiEventState& evt : player->events) {
            if (evt.tick > old_tick && evt.tick <= last_tick) {
                // std::cout << "  [WRAP1] tick=" << evt.tick << " note=" << evt.note
                //           << " vel=" << evt.velocity << " " << (evt.on ? "ON" : "OFF") << std::endl;
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
        // std::cout << "[WRAP PART 2] Firing events from 0 to " << new_tick << std::endl;
        for (const MidiEventState& evt : player->events) {
            if (evt.tick >= 0 && evt.tick <= new_tick) {
                // std::cout << "  [WRAP2] tick=" << evt.tick << " note=" << evt.note
                //           << " vel=" << evt.velocity << " " << (evt.on ? "ON" : "OFF") << std::endl;
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
        // Use > (not >=) for old_tick to avoid firing the same event twice across frame boundaries
        for (const MidiEventState& evt : player->events) {
            if (evt.tick > old_tick && evt.tick <= new_tick) {
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

// Sync the player's start_beat reference to current MIDI clock pulse
void midi_file_player_sync_start_beat(MidiFilePlayer* player, int current_pulse) {
    if (!player) return;

    // Only sync if the player is actually playing and using MIDI clock
    if (!player->playing || player->start_beat < 0) return;

    // Update start_beat to maintain current playback position
    // Formula: new_start = current_pulse - (current_position_in_pulses)
    // current_position_in_pulses = position_seconds * bpm * 24 / 60
    int current_position_pulses = (int)(player->position_seconds * player->tempo_bpm * 24.0f / 60.0f);
    player->start_beat = current_pulse - current_position_pulses;

    // Reset last_tick_processed to avoid duplicate events after sync
    int current_tick = (int)(player->position_seconds * player->tempo_bpm * player->ticks_per_quarter / 60.0f);
    player->last_tick_processed = current_tick;
}
