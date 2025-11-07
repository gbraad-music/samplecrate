#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <sfizz.h>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <cstring>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <libgen.h>
#include <sys/stat.h>
#include <dirent.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <stdlib.h>
#define getcwd _getcwd
#endif

extern "C" {
#include "lcd.h"
#include "samplecrate_common.h"
#include "samplecrate_rsx.h"
#include "regroove_effects.h"
#include "midi.h"
#include "input_mappings.h"
#include "sfz_builder.h"
#include "medness_sequencer.h"
#include "midi_file_player.h"
#include "midi_sysex.h"
#include "medness_performance.h"
}

#include "samplecrate_engine.h"

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
static const char* appname = "SZ16W: Multi-timbral sample/drum sequencer";

// Cross-platform realpath wrapper
static char* cross_platform_realpath(const char* path, char* resolved_path) {
#ifdef _WIN32
    return _fullpath(resolved_path, path, 1024);
#else
    return realpath(path, resolved_path);
#endif
}

// UI Color Constants - Define once, reuse everywhere!
static const ImVec4 COLOR_BUTTON_ACTIVE = ImVec4(0.85f, 0.70f, 0.20f, 1.0f);   // Yellow/gold for active buttons
static const ImVec4 COLOR_BUTTON_INACTIVE = ImVec4(0.26f, 0.27f, 0.30f, 1.0f); // Dark gray for inactive
static const ImVec4 COLOR_BUTTON_AT_LIMIT = ImVec4(0.16f, 0.17f, 0.18f, 1.0f); // Very dark for nav buttons at limit (P-/P+ can't go further)
static const ImVec4 COLOR_LEARN_ACTIVE = ImVec4(0.90f, 0.15f, 0.18f, 1.0f);    // Red for LEARN mode

// =============================================================================
// CORE ENGINE - All audio/MIDI state lives here for headless operation
// =============================================================================
SamplecrateEngine* engine = nullptr;

// Convenience accessors (point to engine internals)
#define synth (engine->synth)
#define program_synths (engine->program_synths)
#define rsx (engine->rsx)
#define performance (engine->performance)
#define pad_program_numbers (engine->pad_program_numbers)
#define current_program (engine->current_program)
#define mixer (engine->mixer)
#define effects_master (engine->effects_master)
#define effects_program (engine->effects_program)
#define note_suppressed (engine->note_suppressed)

// =============================================================================
// GUI-ONLY STATE - Does not affect headless operation
// =============================================================================
std::atomic<bool> running(true);
std::mutex synth_mutex;
LCD* lcd_display = nullptr;
int current_note = -1;
int current_velocity = 0;

SamplecrateConfig config;

// Input mappings and MIDI
InputMappings* input_mappings = nullptr;
bool learn_mode_active = false;
InputAction learn_target_action = ACTION_NONE;
int learn_target_parameter = 0;

// MIDI device configuration
int midi_device_ports[MIDI_MAX_DEVICES] = {-1, -1, -1};  // -1 = not configured

// Audio device configuration
SDL_AudioDeviceID current_audio_device_id = 0;  // Current audio device ID
int num_audio_devices = 0;  // Number of available audio output devices

// RSX file path (GUI state - actual RSX lives in engine)
std::string rsx_file_path = "";

// File browser for .rsx and .sfz files
SamplecrateFileList* file_list = nullptr;
bool file_browser_mode = false;  // Set to true when browsing (shows filename in LCD)

// Pattern sequencer (single source of truth for pattern position)
MednessSequencer* sequencer = nullptr;

// MIDI sequence manager (for multi-phrase sequences)
MednessPerformance* sequence_manager = nullptr;

// MIDI sync settings

// Tempo control (for MIDI file playback)
float tempo_bpm = 125.0f;  // Manual tempo slider value
float active_bpm = 125.0f;  // Actual playback tempo (updated by MIDI clock or manual slider)

// Note pad visual feedback
float note_pad_fade[RSX_MAX_NOTE_PADS] = {0.0f};           // Normal note trigger fade (white/bright)
float note_pad_loop_fade[RSX_MAX_NOTE_PADS] = {0.0f};      // Loop restart fade (blue/cyan)

// Sequence visual feedback states
enum SequencePadState {
    SEQ_PAD_IDLE = 0,
    SEQ_PAD_QUEUED = 1,      // BLUE - waiting for row 0
    SEQ_PAD_PLAYING = 2,     // RED - currently playing
    SEQ_PAD_NEXT_PHRASE = 3  // YELLOW - next phrase coming
};
SequencePadState sequence_pad_states[RSX_MAX_NOTE_PADS] = {SEQ_PAD_IDLE};
float sequence_pad_blink[RSX_MAX_NOTE_PADS] = {0.0f};  // Blink timer for playing sequences

// Step sequencer visual feedback (16 steps represent 64 rows)
float step_fade[16] = {0.0f};  // Brightness for each step

// Note: note_suppressed and current_program are now in the engine (accessed via macros)
int midi_target_program[3] = {0, 0, 0};  // Per-device MIDI routing (set by program change messages)

// MIDI clock tracking
struct {
    bool active = false;           // Receiving MIDI clock
    bool running = false;          // Transport running (start/continue)
    float bpm = 0.0f;             // Calculated BPM
    float smoothed_bpm = 0.0f;    // Smoothed BPM for exponential moving average filter
    uint64_t last_clock_time = 0; // Last clock pulse timestamp (microseconds)
    int pulse_count = 0;          // Pulses since last beat (0-23)
    int beat_count = 0;           // Total quarter note beats since start
    int total_pulse_count = 0;    // Total pulses since start (for sub-beat precision: 24 ppqn)
    uint64_t last_bpm_calc_time = 0; // Last BPM calculation time
    int spp_position = 0;         // Song Position Pointer (in 16th notes / MIDI beats)
    bool spp_synced = false;      // True if we've received SPP and synced to it
} midi_clock;

// Error message for LCD display
std::string error_message = "";

// MIDI monitor (circular buffer for recent MIDI messages)
#define MIDI_MONITOR_SIZE 50
struct MidiMonitorEntry {
    char timestamp[16];
    int device_id;
    char type[16];      // "Note On", "Note Off", "CC", "Prog Change", etc.
    int number;         // Note number or CC number
    int value;          // Velocity or CC value
    int program;        // Which program it was routed to
};
static MidiMonitorEntry midi_monitor[MIDI_MONITOR_SIZE];
static int midi_monitor_head = 0;
static int midi_monitor_count = 0;

void add_to_midi_monitor(int device_id, const char* type, int number, int value, int program) {
    MidiMonitorEntry* entry = &midi_monitor[midi_monitor_head];

    // Get current time
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(entry->timestamp, sizeof(entry->timestamp), "%H:%M:%S", tm_info);

    entry->device_id = device_id;
    snprintf(entry->type, sizeof(entry->type), "%s", type);
    entry->number = number;
    entry->value = value;
    entry->program = program;

    midi_monitor_head = (midi_monitor_head + 1) % MIDI_MONITOR_SIZE;
    if (midi_monitor_count < MIDI_MONITOR_SIZE) {
        midi_monitor_count++;
    }
}

// Fullscreen pads mode (F12 toggle - hides left panel)
bool fullscreen_pads_mode = false;

// Expanded pads mode (config setting - 16 vs 32 pads)
bool expanded_pads = false;

// Track currently held pad for note_off on release
int held_pad_index = -1;
int held_pad_note = -1;
sfizz_synth_t* held_pad_synth = nullptr;

// UI mode
enum UIMode {
    UI_MODE_INSTRUMENT = 0,   // CRATE panel - high-level program list
    UI_MODE_PROGRAM = 1,      // PROG panel - detailed single program view
    UI_MODE_MIX = 2,
    UI_MODE_EFFECTS = 3,
    UI_MODE_PADS = 4,
    UI_MODE_TRACK = 5,
    UI_MODE_SEQUENCES = 6,
    UI_MODE_MIDI = 7,
    UI_MODE_SETTINGS = 8
};
UIMode ui_mode = UI_MODE_PADS;  // Default to PADS view

// FX mode (for EFFECTS panel)
enum FXMode {
    FX_MODE_MASTER = 0,   // FXM - Master effects
    FX_MODE_PROGRAM = 1   // FXP - Per-program effects
};
FXMode fx_mode = FX_MODE_MASTER;  // Default to master FX view

// Get current effects instance based on FX mode
RegrooveEffects* get_current_effects() {
    if (fx_mode == FX_MODE_MASTER) {
        return effects_master;
    } else {
        // FX_MODE_PROGRAM - return current program's effects
        if (current_program >= 0 && current_program < RSX_MAX_PROGRAMS) {
            return effects_program[current_program];
        }
        return nullptr;
    }
}

// Helper: apply RSX effects settings to RegrooveEffects instance
void apply_rsx_effects_to_instance(RegrooveEffects* fx, const RSXEffectsSettings* rsx_fx) {
    if (!fx || !rsx_fx) return;

    // Distortion
    regroove_effects_set_distortion_enabled(fx, rsx_fx->distortion_enabled);
    regroove_effects_set_distortion_drive(fx, rsx_fx->distortion_drive);
    regroove_effects_set_distortion_mix(fx, rsx_fx->distortion_mix);

    // Filter
    regroove_effects_set_filter_enabled(fx, rsx_fx->filter_enabled);
    regroove_effects_set_filter_cutoff(fx, rsx_fx->filter_cutoff);
    regroove_effects_set_filter_resonance(fx, rsx_fx->filter_resonance);

    // EQ
    regroove_effects_set_eq_enabled(fx, rsx_fx->eq_enabled);
    regroove_effects_set_eq_low(fx, rsx_fx->eq_low);
    regroove_effects_set_eq_mid(fx, rsx_fx->eq_mid);
    regroove_effects_set_eq_high(fx, rsx_fx->eq_high);

    // Compressor
    regroove_effects_set_compressor_enabled(fx, rsx_fx->compressor_enabled);
    regroove_effects_set_compressor_threshold(fx, rsx_fx->compressor_threshold);
    regroove_effects_set_compressor_ratio(fx, rsx_fx->compressor_ratio);
    regroove_effects_set_compressor_attack(fx, rsx_fx->compressor_attack);
    regroove_effects_set_compressor_release(fx, rsx_fx->compressor_release);
    regroove_effects_set_compressor_makeup(fx, rsx_fx->compressor_makeup);

    // Phaser
    regroove_effects_set_phaser_enabled(fx, rsx_fx->phaser_enabled);
    regroove_effects_set_phaser_rate(fx, rsx_fx->phaser_rate);
    regroove_effects_set_phaser_depth(fx, rsx_fx->phaser_depth);
    regroove_effects_set_phaser_feedback(fx, rsx_fx->phaser_feedback);

    // Reverb
    regroove_effects_set_reverb_enabled(fx, rsx_fx->reverb_enabled);
    regroove_effects_set_reverb_room_size(fx, rsx_fx->reverb_room_size);
    regroove_effects_set_reverb_damping(fx, rsx_fx->reverb_damping);
    regroove_effects_set_reverb_mix(fx, rsx_fx->reverb_mix);

    // Delay
    regroove_effects_set_delay_enabled(fx, rsx_fx->delay_enabled);
    regroove_effects_set_delay_time(fx, rsx_fx->delay_time);
    regroove_effects_set_delay_feedback(fx, rsx_fx->delay_feedback);
    regroove_effects_set_delay_mix(fx, rsx_fx->delay_mix);
}

// Helper: save current RegrooveEffects instance to RSX effects settings
void save_instance_to_rsx_effects(RegrooveEffects* fx, RSXEffectsSettings* rsx_fx) {
    if (!fx || !rsx_fx) return;

    // Distortion
    rsx_fx->distortion_enabled = regroove_effects_get_distortion_enabled(fx);
    rsx_fx->distortion_drive = regroove_effects_get_distortion_drive(fx);
    rsx_fx->distortion_mix = regroove_effects_get_distortion_mix(fx);

    // Filter
    rsx_fx->filter_enabled = regroove_effects_get_filter_enabled(fx);
    rsx_fx->filter_cutoff = regroove_effects_get_filter_cutoff(fx);
    rsx_fx->filter_resonance = regroove_effects_get_filter_resonance(fx);

    // EQ
    rsx_fx->eq_enabled = regroove_effects_get_eq_enabled(fx);
    rsx_fx->eq_low = regroove_effects_get_eq_low(fx);
    rsx_fx->eq_mid = regroove_effects_get_eq_mid(fx);
    rsx_fx->eq_high = regroove_effects_get_eq_high(fx);

    // Compressor
    rsx_fx->compressor_enabled = regroove_effects_get_compressor_enabled(fx);
    rsx_fx->compressor_threshold = regroove_effects_get_compressor_threshold(fx);
    rsx_fx->compressor_ratio = regroove_effects_get_compressor_ratio(fx);
    rsx_fx->compressor_attack = regroove_effects_get_compressor_attack(fx);
    rsx_fx->compressor_release = regroove_effects_get_compressor_release(fx);
    rsx_fx->compressor_makeup = regroove_effects_get_compressor_makeup(fx);

    // Phaser
    rsx_fx->phaser_enabled = regroove_effects_get_phaser_enabled(fx);
    rsx_fx->phaser_rate = regroove_effects_get_phaser_rate(fx);
    rsx_fx->phaser_depth = regroove_effects_get_phaser_depth(fx);
    rsx_fx->phaser_feedback = regroove_effects_get_phaser_feedback(fx);

    // Reverb
    rsx_fx->reverb_enabled = regroove_effects_get_reverb_enabled(fx);
    rsx_fx->reverb_room_size = regroove_effects_get_reverb_room_size(fx);
    rsx_fx->reverb_damping = regroove_effects_get_reverb_damping(fx);
    rsx_fx->reverb_mix = regroove_effects_get_reverb_mix(fx);

    // Delay
    rsx_fx->delay_enabled = regroove_effects_get_delay_enabled(fx);
    rsx_fx->delay_time = regroove_effects_get_delay_time(fx);
    rsx_fx->delay_feedback = regroove_effects_get_delay_feedback(fx);
    rsx_fx->delay_mix = regroove_effects_get_delay_mix(fx);
}

// Helper: load note suppression from RSX to runtime state
void load_note_suppression_from_rsx() {
    if (!rsx) return;

    // Copy global suppression
    for (int note = 0; note < 128; note++) {
        note_suppressed[note][0] = (rsx->note_suppressed_global[note] != 0);
    }

    // Copy per-program suppression
    for (int prog = 0; prog < RSX_MAX_PROGRAMS; prog++) {
        for (int note = 0; note < 128; note++) {
            note_suppressed[note][prog + 1] = (rsx->note_suppressed_program[prog][note] != 0);
        }
    }
}

// Helper: save note suppression from runtime state to RSX file
void save_note_suppression_to_rsx() {
    if (!rsx || rsx_file_path.empty()) return;

    // Copy global suppression
    for (int note = 0; note < 128; note++) {
        rsx->note_suppressed_global[note] = note_suppressed[note][0] ? 1 : 0;
    }

    // Copy per-program suppression
    for (int prog = 0; prog < RSX_MAX_PROGRAMS; prog++) {
        for (int note = 0; note < 128; note++) {
            rsx->note_suppressed_program[prog][note] = note_suppressed[note][prog + 1] ? 1 : 0;
        }
    }

    // Save to file
    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
}

// Helper: reload sequences from RSX structure (in memory)
void reload_sequences() {
    if (!sequence_manager || !rsx) return;

    std::cout << "[Sequences] Loading sequences from RSX structure..." << std::endl;

    // Load from RSX structure in memory (not from file)
    // This allows creating sequences in UI without saving to file first
    int num_sequences = medness_performance_load_from_rsx(sequence_manager, rsx_file_path.c_str(), rsx);

    if (num_sequences > 0) {
        std::cout << "[Sequences] Loaded " << num_sequences << " sequences" << std::endl;
    } else {
        std::cout << "[Sequences] No sequences found" << std::endl;
    }

    // Auto-save sequences to RSX file
    if (!rsx_file_path.empty()) {
        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
        std::cout << "[Sequences] Saved to " << rsx_file_path << std::endl;
    }
}

// Note: reload_program functionality is now in samplecrate_engine_reload_program()

// Helper: save current effects state to RSX file (auto-save)
void autosave_effects_to_rsx() {
    if (!rsx || rsx_file_path.empty()) return;

    // Save FX chain enable states from mixer
    rsx->master_fx_enable = mixer.master_fx_enable;
    for (int i = 0; i < 4; i++) {
        rsx->program_fx_enable[i] = mixer.program_fx_enable[i];
    }

    // Save master effects
    if (effects_master) {
        save_instance_to_rsx_effects(effects_master, &rsx->master_effects);
    }

    // Save per-program effects
    for (int i = 0; i < 4; i++) {
        if (effects_program[i]) {
            save_instance_to_rsx_effects(effects_program[i], &rsx->program_effects[i]);
        }
    }

    // Write to file
    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
}

// Convert MIDI note number to tracker format (e.g., 36 -> "C-1")
static void midi_note_to_string(int note, char* buf, size_t bufsize) {
    if (note < 0 || note > 127) {
        snprintf(buf, bufsize, "...");
        return;
    }

    const char* note_names[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    int octave = (note / 12) - 1;  // MIDI note 0 = C-(-1), 12 = C-0, 24 = C-1, etc.
    int semitone = note % 12;

    snprintf(buf, bufsize, "%s%d", note_names[semitone], octave);
}

// DrawLCD function from mock-ui.cpp
static void DrawLCD(const char* text, float width, float height)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 end(pos.x + width, pos.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, end, IM_COL32(25,50,18,255), 6.0f);
    dl->AddRect(pos, end, IM_COL32(95,140,65,255), 6.0f, 0, 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + 16));
    ImGui::TextColored(ImVec4(0.80f,1.0f,0.70f,1.0f), "%s", text);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y + 8));
}

// Find pad configured for a specific MIDI note
// Returns the first pad index that matches the note, or -1 if none found
int find_pad_for_note(int midi_note) {
    if (!rsx) return -1;

    for (int i = 0; i < RSX_MAX_NOTE_PADS && i < rsx->num_pads; i++) {
        NoteTriggerPad* pad = &rsx->pads[i];
        if (pad->enabled && pad->note == midi_note) {
            return i;  // Found a pad configured for this note
        }
    }
    return -1;  // No pad found for this note
}

// Switch to a different program (change active synth pointer)
void switch_program(int program_index) {
    if (!rsx || program_index < 0 || program_index >= rsx->num_programs) return;
    if (!program_synths[program_index]) return;  // Program not loaded

    current_program = program_index;

    // Update midi_target_program for all devices when program changes via UI
    // If a device has program change enabled, it can still be overridden by MIDI messages
    // But UI selection should update all devices by default
    midi_target_program[0] = program_index;
    midi_target_program[1] = program_index;
    midi_target_program[2] = program_index;

    std::cout << "Switching to program " << (program_index + 1) << ": " << rsx->program_files[program_index] << std::endl;

    // Switch synth pointer to the selected program
    std::lock_guard<std::mutex> lock(synth_mutex);
    synth = program_synths[program_index];
    error_message = "";  // Clear any previous errors
}

// Start MIDI learn mode for an action
void start_learn_for_action(InputAction action, int parameter = 0) {
    learn_mode_active = true;
    learn_target_action = action;
    learn_target_parameter = parameter;
    std::cout << "MIDI Learn active for: " << input_action_name(action) << std::endl;
}

// Handle input actions (from MIDI or keyboard)
void handle_input_event(InputEvent* event) {
    if (!event) return;

    float normalized_value = event->value / 127.0f;  // MIDI CC is 0-127

    switch (event->action) {
        // Effects parameters
        case ACTION_FX_DISTORTION_DRIVE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_distortion_drive(fx, normalized_value);
            break;
        }
        case ACTION_FX_DISTORTION_MIX: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_distortion_mix(fx, normalized_value);
            break;
        }
        case ACTION_FX_FILTER_CUTOFF: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_filter_cutoff(fx, normalized_value);
            break;
        }
        case ACTION_FX_FILTER_RESONANCE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_filter_resonance(fx, normalized_value);
            break;
        }
        case ACTION_FX_EQ_LOW: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_eq_low(fx, normalized_value);
            break;
        }
        case ACTION_FX_EQ_MID: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_eq_mid(fx, normalized_value);
            break;
        }
        case ACTION_FX_EQ_HIGH: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_eq_high(fx, normalized_value);
            break;
        }
        case ACTION_FX_COMPRESSOR_THRESHOLD: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_compressor_threshold(fx, normalized_value);
            break;
        }
        case ACTION_FX_COMPRESSOR_RATIO: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_compressor_ratio(fx, normalized_value);
            break;
        }
        case ACTION_FX_DELAY_TIME: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_delay_time(fx, normalized_value);
            break;
        }
        case ACTION_FX_DELAY_FEEDBACK: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_delay_feedback(fx, normalized_value);
            break;
        }
        case ACTION_FX_DELAY_MIX: {
            RegrooveEffects* fx = get_current_effects();
            if (fx) regroove_effects_set_delay_mix(fx, normalized_value);
            break;
        }

        // Effects toggles
        case ACTION_FX_DISTORTION_TOGGLE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx && event->value > 63) {
                int current = regroove_effects_get_distortion_enabled(fx);
                regroove_effects_set_distortion_enabled(fx, !current);
                autosave_effects_to_rsx();
            }
            break;
        }
        case ACTION_FX_FILTER_TOGGLE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx && event->value > 63) {
                int current = regroove_effects_get_filter_enabled(fx);
                regroove_effects_set_filter_enabled(fx, !current);
                autosave_effects_to_rsx();
            }
            break;
        }
        case ACTION_FX_EQ_TOGGLE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx && event->value > 63) {
                int current = regroove_effects_get_eq_enabled(fx);
                regroove_effects_set_eq_enabled(fx, !current);
                autosave_effects_to_rsx();
            }
            break;
        }
        case ACTION_FX_COMPRESSOR_TOGGLE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx && event->value > 63) {
                int current = regroove_effects_get_compressor_enabled(fx);
                regroove_effects_set_compressor_enabled(fx, !current);
                autosave_effects_to_rsx();
            }
            break;
        }
        case ACTION_FX_DELAY_TOGGLE: {
            RegrooveEffects* fx = get_current_effects();
            if (fx && event->value > 63) {
                int current = regroove_effects_get_delay_enabled(fx);
                regroove_effects_set_delay_enabled(fx, !current);
                autosave_effects_to_rsx();
            }
            break;
        }

        // Mixer parameters
        case ACTION_MASTER_VOLUME:
            mixer.master_volume = normalized_value;
            break;
        case ACTION_PLAYBACK_VOLUME:
            mixer.playback_volume = normalized_value;
            break;
        case ACTION_MASTER_PAN:
            mixer.master_pan = normalized_value;
            break;
        case ACTION_PLAYBACK_PAN:
            mixer.playback_pan = normalized_value;
            break;

        // Mixer toggles
        case ACTION_MASTER_MUTE:
            if (event->value > 63) mixer.master_mute = !mixer.master_mute;
            break;
        case ACTION_PLAYBACK_MUTE:
            if (event->value > 63) mixer.playback_mute = !mixer.playback_mute;
            break;

        // Note pad trigger
        case ACTION_TRIGGER_NOTE_PAD:
            if (event->value > 63 && rsx && event->parameter >= 0 && event->parameter < RSX_MAX_NOTE_PADS) {
                NoteTriggerPad* pad = &rsx->pads[event->parameter];
                if (pad->note >= 0 && pad->enabled) {
                    int velocity = pad->velocity > 0 ? pad->velocity : 100;

                    // Determine which synth to use based on pad's program setting
                    // For CC-triggered pads, use current_program as default (no device context)
                    int target_prog = current_program;
                    sfizz_synth_t* target_synth = nullptr;
                    int actual_program = target_prog;

                    if (pad->program >= 0 && pad->program < rsx->num_programs && program_synths[pad->program]) {
                        target_synth = program_synths[pad->program];
                        actual_program = pad->program;
                    } else {
                        target_synth = program_synths[target_prog];
                    }

                    std::lock_guard<std::mutex> lock(synth_mutex);
                    if (target_synth) {
                        // For CC triggers, just send note_on (no release event available)
                        // The SFZ file's envelope/release settings will control the sound
                        sfizz_send_note_on(target_synth, 0, pad->note, velocity);

                        current_note = pad->note;
                        current_velocity = velocity;

                        // Highlight ALL pads that would play this same note on this program
                        for (int i = 0; i < RSX_MAX_NOTE_PADS && i < rsx->num_pads; i++) {
                            NoteTriggerPad* check_pad = &rsx->pads[i];
                            if (check_pad->enabled && check_pad->note == pad->note) {
                                int check_pad_program = (check_pad->program >= 0) ? check_pad->program : target_prog;
                                if (check_pad_program == actual_program) {
                                    note_pad_fade[i] = 1.0f;
                                }
                            }
                        }
                    }
                }
            }
            break;

        // Program selection
        case ACTION_PROGRAM_PREV:
            if (event->value > 63 && rsx && rsx->num_programs > 1) {
                int prev_prog = current_program - 1;
                if (prev_prog < 0) prev_prog = rsx->num_programs - 1;
                switch_program(prev_prog);
            }
            break;

        case ACTION_PROGRAM_NEXT:
            if (event->value > 63 && rsx && rsx->num_programs > 1) {
                int next_prog = (current_program + 1) % rsx->num_programs;
                switch_program(next_prog);
            }
            break;

        case ACTION_FILE_LOAD_BYNAME: {
            // Parameter is the pad index - look up the filename from the pad config
            int pad_index = event->parameter;
            if (pad_index >= 0 && pad_index < MAX_TRIGGER_PADS && input_mappings) {
                TriggerPadConfig *pad = &input_mappings->trigger_pads[pad_index];
                if (pad->parameters[0] != '\0') {
                    const char* filename = pad->parameters;
                    printf("ACTION_FILE_LOAD_BYNAME: pad_index=%d, filename='%s'\n", pad_index, filename);

                    // Check if file exists using stat
                    struct stat buffer;
                    if (stat(filename, &buffer) == 0) {
                        // Unload current RSX if loaded
                        if (rsx) {
                            samplecrate_rsx_destroy(rsx);
                            rsx = nullptr;
                        }

                        // Create and load new RSX file
                        rsx = samplecrate_rsx_create();
                        if (samplecrate_rsx_load(rsx, filename) == 0) {
                            rsx_file_path = filename;
                            printf("Loaded RSX: %s\n", filename);
                            reload_sequences();  // Load sequences from RSX
                        } else {
                            fprintf(stderr, "Failed to load RSX: %s\n", filename);
                            samplecrate_rsx_destroy(rsx);
                            rsx = nullptr;
                        }
                    } else {
                        fprintf(stderr, "File not found: %s\n", filename);
                    }
                } else {
                    printf("ACTION_FILE_LOAD_BYNAME: pad_index=%d has empty parameters\n", pad_index);
                }
            }
            break;
        }

        // Note suppression toggle
        case ACTION_NOTE_SUPPRESS_TOGGLE: {
            if (event->value > 63) {
                // Extract note from event->parameter (lower byte)
                // Extract program from event->parameter (upper byte) or use a second parameter system
                // For now, we'll use parameter as note and assume global suppression
                // TODO: Need to extend InputEvent to support second parameter for program
                int note = event->parameter & 0x7F;  // Note 0-127
                int program = (event->parameter >> 8) & 0xFF;  // Program in upper byte (-1 or 0-3)

                if (note >= 0 && note < 128) {
                    if (program == -1 || program == 255) {
                        // Global suppression
                        note_suppressed[note][0] = !note_suppressed[note][0];
                        std::cout << "Note " << note << " global suppression: "
                                  << (note_suppressed[note][0] ? "ON" : "OFF") << std::endl;
                    } else if (program >= 0 && program < RSX_MAX_PROGRAMS) {
                        // Per-program suppression
                        note_suppressed[note][program + 1] = !note_suppressed[note][program + 1];
                        std::cout << "Note " << note << " suppression for program " << (program + 1) << ": "
                                  << (note_suppressed[note][program + 1] ? "ON" : "OFF") << std::endl;
                    }
                }
            }
            break;
        }

        // Program mute toggle
        case ACTION_PROGRAM_MUTE_TOGGLE: {
            if (event->value > 63) {
                int program = event->parameter;
                if (program >= 0 && program < RSX_MAX_PROGRAMS && rsx) {
                    mixer.program_mutes[program] = !mixer.program_mutes[program];
                    std::cout << "Program " << (program + 1) << " mute: "
                              << (mixer.program_mutes[program] ? "ON" : "OFF") << std::endl;
                }
            }
            break;
        }

        default:
            break;
    }
}

// Helper: get current time in microseconds
static uint64_t get_microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// SysEx callback for remote control
void sysex_callback(uint8_t device_id, SysExCommand command, const uint8_t *data, size_t data_len, void *userdata) {
    printf("[SysEx] Received command: %s for device %d\n", sysex_command_name(command), device_id);

    switch (command) {
        case SYSEX_CMD_FILE_LOAD: {
            if (data_len >= 2) {
                uint8_t filename_len = data[0];
                if (filename_len > 0 && data_len >= (size_t)(1 + filename_len)) {
                    char filename[256] = {0};
                    memcpy(filename, &data[1], filename_len);
                    filename[filename_len] = '\0';
                    printf("[SysEx] Loading RSX file: %s\n", filename);

                    // If filename is not an absolute path, prepend the directory of the current RSX file
                    char full_path[1024];
                    if (filename[0] == '/' || filename[0] == '\\' ||
                        (strlen(filename) > 2 && filename[1] == ':')) {
                        // Already absolute path
                        strncpy(full_path, filename, sizeof(full_path) - 1);
                    } else {
                        // Relative path - use the same directory as the current RSX file
                        if (!rsx_file_path.empty()) {
                            // Extract directory from current RSX file path
                            std::string dir = rsx_file_path;
                            size_t last_slash = dir.find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                dir = dir.substr(0, last_slash);
#ifdef _WIN32
                                snprintf(full_path, sizeof(full_path), "%s\\%s", dir.c_str(), filename);
#else
                                snprintf(full_path, sizeof(full_path), "%s/%s", dir.c_str(), filename);
#endif
                            } else {
                                // No directory in path, just use filename
                                strncpy(full_path, filename, sizeof(full_path) - 1);
                            }
                        } else {
                            // No RSX loaded yet, use current working directory
                            char cwd[512];
                            if (getcwd(cwd, sizeof(cwd)) != nullptr) {
#ifdef _WIN32
                                snprintf(full_path, sizeof(full_path), "%s\\%s", cwd, filename);
#else
                                snprintf(full_path, sizeof(full_path), "%s/%s", cwd, filename);
#endif
                            } else {
                                // Fallback to just the filename
                                strncpy(full_path, filename, sizeof(full_path) - 1);
                            }
                        }
                    }
                    full_path[sizeof(full_path) - 1] = '\0';

                    printf("[SysEx] Full path: %s\n", full_path);

                    // Load the RSX file
                    if (rsx && samplecrate_rsx_load(rsx, full_path) == 0) {
                        printf("[SysEx] Successfully loaded: %s\n", filename);
                        // Reload programs
                        for (int i = 0; i < rsx->num_programs; i++) {
                            samplecrate_engine_reload_program(engine, i);
                        }
                        load_note_suppression_from_rsx();
                        reload_sequences();  // Load sequences from RSX
                        current_program = 0;
                    } else {
                        printf("[SysEx] Failed to load: %s\n", filename);
                    }
                }
            }
            break;
        }

        case SYSEX_CMD_PING:
            printf("[SysEx] PING received\n");
            break;

        default:
            printf("[SysEx] Unhandled command: 0x%02X\n", command);
            break;
    }
}

// MIDI event callback from midi.c
void midi_event_callback(unsigned char status, unsigned char data1, unsigned char data2, int device_id, void* userdata) {
    int msg_type = status & 0xF0;
    int channel = status & 0x0F;

    // Debug: Log important real-time MIDI messages (start, stop, continue, SPP)
    // Clock (0xF8) is too spammy - it fires 24 times per beat!
    // Commented out - use MIDI Monitor in UI instead
    // if (status == 0xFA || status == 0xFC || status == 0xFB || status == 0xF2) {
    //     std::cout << "[MIDI RT] Received: 0x" << std::hex << (int)status << std::dec;
    //     if (status == 0xFA) std::cout << " (Start)";
    //     else if (status == 0xFC) std::cout << " (Stop)";
    //     else if (status == 0xFB) std::cout << " (Continue)";
    //     else if (status == 0xF2) std::cout << " (SPP) data1=" << (int)data1 << " data2=" << (int)data2;
    //     std::cout << " from device_id=" << device_id << std::endl;
    // }

    // Handle MIDI Clock messages (Real-Time messages - these don't have channels)
    if (status == 0xF8) {  // MIDI Clock (24 ppqn - pulses per quarter note)
        uint64_t now = get_microseconds();

        // Debug: Count clock pulses and show rate every second
        static int total_clock_pulses = 0;
        static int pulses_this_second = 0;
        static uint64_t last_report_time = 0;

        total_clock_pulses++;
        pulses_this_second++;

        if (last_report_time == 0) {
            last_report_time = now;
        }

        // Report every second
        if (now - last_report_time >= 1000000) {  // 1 second in microseconds
            std::cout << "[MIDI CLOCK] Received " << pulses_this_second
                      << " pulses in last second (total: " << total_clock_pulses
                      << ", device_id=" << device_id << ")" << std::endl;
            pulses_this_second = 0;
            last_report_time = now;
        }

        // Enable external clock mode on sequencer (single source of truth)
        if (sequencer && !midi_clock.active) {
            // std::cout << "[MIDI CLOCK] Switching to external clock mode" << std::endl;
            medness_sequencer_set_external_clock(sequencer, 1);

            // Reset BPM smoothing filter when first enabling external clock
            // This ensures we start fresh with the new clock source
            midi_clock.bpm = 0.0f;
            midi_clock.smoothed_bpm = 0.0f;
        }

        if (midi_clock.last_clock_time > 0) {
            uint64_t interval = now - midi_clock.last_clock_time;

            // Forward clock pulse to sequencer (it manages the pattern position)
            if (sequencer) {
                medness_sequencer_clock_pulse(sequencer);
            }

            // Increment total pulse count for sub-beat precision
            midi_clock.total_pulse_count++;

            // Calculate BPM every 24 pulses (one quarter note)
            midi_clock.pulse_count++;
            if (midi_clock.pulse_count >= 24) {
                uint64_t total_time = now - midi_clock.last_bpm_calc_time;
                // BPM = (60,000,000 microseconds/minute) / (time for one quarter note in microseconds)
                if (total_time > 0) {
                    float raw_bpm = 60000000.0f / total_time;

                    // Smooth BPM using exponential moving average to reduce jitter
                    // Alpha = 0.3 gives moderate smoothing (lower = more smoothing)
                    if (midi_clock.smoothed_bpm == 0.0f) {
                        midi_clock.smoothed_bpm = raw_bpm;  // Initialize on first reading
                    } else {
                        midi_clock.smoothed_bpm = midi_clock.smoothed_bpm * 0.7f + raw_bpm * 0.3f;  // Exponential moving average
                    }

                    float new_bpm = midi_clock.smoothed_bpm;

                    // Update BPM value
                    bool bpm_changed = fabs(new_bpm - midi_clock.bpm) > 0.1f;
                    midi_clock.bpm = new_bpm;

                    if (bpm_changed) {
                        std::cout << "MIDI CLOCK: BPM = " << new_bpm
                                  << " (raw: " << raw_bpm << ", smoothed, interval=" << total_time << "us)" << std::endl;
                    }

                    // Always update active playback tempo if sync is enabled
                    if (config.midi_clock_tempo_sync == 1) {
                        active_bpm = midi_clock.bpm;  // Update active playback tempo
                        tempo_bpm = midi_clock.bpm;   // Update UI slider to match

                        // Only update sequencer/player if BPM actually changed
                        if (bpm_changed && performance) {
                            if (sequencer) {
                                medness_sequencer_set_bpm(sequencer, active_bpm);
                            }
                            medness_performance_set_tempo(performance, active_bpm);
                            if (sequence_manager) {
                                medness_performance_set_tempo(sequence_manager, active_bpm);
                            }
                            std::cout << "  -> active_bpm/tempo_bpm updated to " << active_bpm << std::endl;
                        }
                    }
                }
                midi_clock.pulse_count = 0;
                midi_clock.beat_count++;  // Increment beat counter
                midi_clock.last_bpm_calc_time = now;
            }
        } else {
            midi_clock.last_bpm_calc_time = now;
        }

        midi_clock.last_clock_time = now;
        midi_clock.active = true;
        return;
    } else if (status == 0xFA) {  // MIDI Start
        midi_clock.running = true;
        // Don't set midi_clock.active = true yet - wait for first clock pulse!
        // This prevents switching to external mode when only SPP is sent (no 0xF8 pulses)
        midi_clock.pulse_count = 0;
        midi_clock.beat_count = 0;  // Reset beat counter on start
        midi_clock.total_pulse_count = 0;  // Reset total pulse count for precise sync
        midi_clock.last_bpm_calc_time = get_microseconds();
        midi_clock.last_clock_time = 0;  // Reset to get fresh timing on first pulse
        // Don't reset BPM here - keep displaying last known BPM until new one is calculated

        // Don't enable external clock mode yet - wait for first 0xF8 pulse
        // If source only sends SPP (no clock), we'll stay in internal mode

        // std::cout << "MIDI Start received (waiting for clock pulses to enable external mode)" << std::endl;
        return;
    } else if (status == 0xFC) {  // MIDI Stop
        midi_clock.running = false;
        midi_clock.active = false;

        // Reset BPM calculation state to prevent huge jumps on restart
        midi_clock.last_clock_time = 0;
        midi_clock.last_bpm_calc_time = 0;
        midi_clock.pulse_count = 0;
        midi_clock.bpm = 0.0f;  // Reset BPM to prevent displaying stale values
        midi_clock.smoothed_bpm = 0.0f;  // Reset smoothing filter

        // Switch sequencer back to internal clock
        if (sequencer) {
            // std::cout << "[MIDI CLOCK] Switching to internal clock mode" << std::endl;
            medness_sequencer_set_external_clock(sequencer, 0);
        }

        // std::cout << "MIDI Stop received" << std::endl;
        return;
    } else if (status == 0xFB) {  // MIDI Continue
        midi_clock.running = true;
        // std::cout << "MIDI Continue received" << std::endl;
        return;
    } else if (status == 0xF2) {  // MIDI Song Position Pointer
        // SPP format: 0xF2, LSB (7-bit), MSB (7-bit)
        // Position is in "MIDI beats" (1/16th notes from start of song)
        int spp_position = data1 | (data2 << 7);
        midi_clock.spp_position = spp_position;

        // std::cout << "DEBUG: SPP handler called, raw position=" << spp_position
        //           << ", config.midi_spp_receive=" << config.midi_spp_receive << std::endl;

        // Only sync position if SPP receive is enabled
        if (config.midi_spp_receive == 1) {
            // Only sync from SPP if we haven't started yet or clock isn't running
            // Once MIDI clock is active, ignore SPP (sender's song structure differs)
            if (midi_clock.active && midi_clock.running) {
                // Already synced and running - ignore SPP updates
                static int ignore_count = 0;
                if (ignore_count++ % 20 == 0) {
                    std::cout << "[SPP] Ignoring SPP during playback (sender: multi-pattern song, receiver: single pattern)" << std::endl;
                }
                return;
            }

            midi_clock.spp_synced = true;
            midi_clock.active = true;
            midi_clock.running = true;
            midi_clock.last_clock_time = get_microseconds();

            // SPP tells us the PATTERN POSITION (row in the pattern)
            // SPP is in 16th notes (0-63 for a 4-bar pattern)
            // We convert to pulses: each 16th note = 6 MIDI clock pulses
            // Pattern position just CYCLES 0-383 pulses
            const int PATTERN_LENGTH_SIXTEENTHS = 64;
            const int PATTERN_LENGTH_PULSES = 384;

            // SPP position within pattern (cycles 0-63)
            int spp_within_pattern = spp_position % PATTERN_LENGTH_SIXTEENTHS;
            int pulse_within_pattern = spp_within_pattern * 6;  // Convert to pulses (0-378)

            // Get current sequencer position
            int current_pulse = sequencer ? medness_sequencer_get_pulse(sequencer) : 0;

            // Calculate drift (accounting for pattern wrap)
            // Find the shortest distance between current and target, considering wrap
            int drift = pulse_within_pattern - current_pulse;

            // Normalize drift to the shortest path around the circular pattern
            // Example: current=380, target=10 → drift=10-380=-370
            //   After normalization: -370+384=14 (we're 14 pulses behind, wrap forward)
            // Example: current=10, target=380 → drift=380-10=370
            //   After normalization: 370-384=-14 (we're 14 pulses ahead, wrap backward)
            int original_drift = drift;
            if (drift > PATTERN_LENGTH_PULSES / 2) {
                drift -= PATTERN_LENGTH_PULSES;  // Target is behind us (wrap backward)
            } else if (drift <= -PATTERN_LENGTH_PULSES / 2) {
                drift += PATTERN_LENGTH_PULSES;  // Target is ahead of us (wrap forward)
            }

            // Debug: Log drift calculation (occasionally)
            static int spp_count = 0;
            if (spp_count++ % 5 == 0) {
                std::cout << "[SPP DEBUG] current=" << current_pulse << " target=" << pulse_within_pattern
                          << " raw_drift=" << original_drift << " normalized_drift=" << drift << std::endl;
            }

            // SPP sync strategy:
            // - Large drift (>6 pulses = 1 sixteenth): Hard sync (accept SPP position)
            // - Medium drift (3-6 pulses): Soft sync only if not using external clock
            // - Small drift (1-2 pulses): Ignore - normal jitter from MIDI timing

            const int HARD_SYNC_THRESHOLD = 6;   // More than 1 sixteenth note
            const int SOFT_SYNC_THRESHOLD = 3;   // Half a sixteenth note

            bool is_external_clock = (sequencer && midi_clock.active);

            if (abs(drift) >= HARD_SYNC_THRESHOLD) {
                // Significant drift - do hard position sync
                if (sequencer) {
                    medness_sequencer_set_spp(sequencer, spp_position);
                }
                midi_clock.total_pulse_count = pulse_within_pattern;

                std::cout << "*** SPP HARD SYNC *** spp=" << spp_position
                          << " pulse=" << pulse_within_pattern
                          << " (drift=" << drift << " pulses) ***" << std::endl;

            } else if (abs(drift) >= SOFT_SYNC_THRESHOLD && !is_external_clock) {
                // Medium drift and using internal clock - do soft sync
                // Don't sync when external clock is driving (trust the clock pulses)
                if (sequencer) {
                    medness_sequencer_set_spp(sequencer, spp_position);
                }
                midi_clock.total_pulse_count = pulse_within_pattern;

                std::cout << "*** SPP SOFT SYNC *** spp=" << spp_position
                          << " pulse=" << pulse_within_pattern
                          << " (drift=" << drift << " pulses) ***" << std::endl;
            }
            // else: drift is small or external clock is driving - ignore SPP, trust clock pulses
        } else {
            std::cout << "MIDI SPP received but IGNORED (disabled in settings): position="
                      << spp_position << std::endl;
        }
        return;
    }

    // Debug: print all incoming MIDI messages (DISABLED - too noisy)
    // std::cout << "MIDI device " << device_id << ": status=0x" << std::hex << (int)status
    //           << " data1=" << std::dec << (int)data1 << " data2=" << (int)data2
    //           << " channel=" << (channel + 1) << std::endl;

    // Global MIDI input channel filtering (for channel voice messages, not system messages)
    // System messages (0xF0-0xFF) don't have channels, so we don't filter them
    if (msg_type != 0xF0) {  // Not a system message
        // Apply global channel filter: 0 = Omni (all), 1-16 = specific channel
        if (config.midi_input_channel > 0) {
            int filter_channel = config.midi_input_channel - 1;  // Convert 1-16 to 0-15
            if (channel != filter_channel) {
                // Message is on a different channel, ignore it
                std::cout << "  Filtered out (channel " << (channel + 1)
                          << " != filter channel " << config.midi_input_channel << ")" << std::endl;
                return;
            }
        }
    }

    // Handle program change
    if (msg_type == 0xC0) {  // Program Change
        int program_number = data1;  // 0-127
        // std::cout << "*** PROGRAM CHANGE RECEIVED from device " << device_id << ": program=" << program_number << " ***" << std::endl;

        // Check if program change is enabled for this specific device
        // When disabled: UI program selection leads for all MIDI messages from this device
        // When enabled: Routes MIDI to target program but doesn't change UI selection
        if (!config.midi_program_change_enabled[device_id]) {
            std::cout << "    MIDI Program Change disabled for device " << device_id << " - UI program selection leads" << std::endl;
            return;
        }

        if (rsx && rsx->num_programs > 0) {
            // Map MIDI program number to our programs (0-3)
            int target_program = program_number % rsx->num_programs;
            midi_target_program[device_id] = target_program;

            // Log to MIDI monitor
            add_to_midi_monitor(device_id, "Prog Change", program_number, 0, target_program + 1);
        }
        return;
    }

    // Handle note on/off
    if (msg_type == 0x90 && data2 > 0) {  // Note on
        // Determine which program to target based on device-specific settings
        int target_prog = config.midi_program_change_enabled[device_id] ? midi_target_program[device_id] : current_program;

        // Check if note is suppressed (global or for target program)
        bool is_suppressed = note_suppressed[data1][0] ||  // Global suppression
                            (target_prog >= 0 && target_prog < RSX_MAX_PROGRAMS && note_suppressed[data1][target_prog + 1]);  // Per-program

        if (is_suppressed) {
            std::cout << "Note " << (int)data1 << " suppressed for program " << (target_prog + 1) << std::endl;
            return;  // Don't play suppressed notes
        }

        // Log to MIDI monitor
        add_to_midi_monitor(device_id, "Note On", data1, data2, target_prog + 1);

        // Send MIDI note directly to the appropriate synth (bypass pad mapping)
        std::lock_guard<std::mutex> lock(synth_mutex);
        sfizz_synth_t* target_synth = program_synths[target_prog];
        if (target_synth) {
            sfizz_send_note_on(target_synth, 0, data1, data2);

            // Highlight all pads configured for this note on the target program
            if (rsx) {
                for (int i = 0; i < RSX_MAX_NOTE_PADS && i < rsx->num_pads; i++) {
                    NoteTriggerPad* check_pad = &rsx->pads[i];
                    if (check_pad->enabled && check_pad->note == data1) {
                        int check_pad_program = (check_pad->program >= 0) ? check_pad->program : target_prog;
                        if (check_pad_program == target_prog) {
                            note_pad_fade[i] = 1.0f;
                        }
                    }
                }
            }
        }
    } else if (msg_type == 0x80 || (msg_type == 0x90 && data2 == 0)) {  // Note off
        // Determine which program to target (same logic as note on)
        int target_prog = config.midi_program_change_enabled[device_id] ? midi_target_program[device_id] : current_program;

        // Log to MIDI monitor
        add_to_midi_monitor(device_id, "Note Off", data1, data2, target_prog + 1);

        // Send MIDI note off directly to the appropriate synth (bypass pad mapping)
        std::lock_guard<std::mutex> lock(synth_mutex);
        sfizz_synth_t* target_synth = program_synths[target_prog];
        if (target_synth) sfizz_send_note_off(target_synth, 0, data1, 0);
    } else if (msg_type == 0xB0) {  // CC message
        // Check if in learn mode
        if (learn_mode_active) {
            std::cout << "Learning: MIDI device " << device_id << " CC " << (int)data1 << " -> "
                      << input_action_name(learn_target_action) << std::endl;

            // Add mapping
            if (input_mappings) {
                MidiMapping mapping;
                mapping.device_id = -1;  // -1 = any device (allow learned mappings to work across all MIDI devices)
                mapping.cc_number = data1;
                mapping.action = learn_target_action;
                mapping.parameter = learn_target_parameter;
                mapping.threshold = 64;

                // Set continuous mode for volume, pitch, pan, and effects parameter controls
                // All toggles (enable buttons, mutes, note pads) use button mode (threshold = 64)
                if (learn_target_action == ACTION_MASTER_VOLUME ||
                    learn_target_action == ACTION_MASTER_PAN ||
                    learn_target_action == ACTION_PLAYBACK_VOLUME ||
                    learn_target_action == ACTION_PLAYBACK_PAN ||
                    learn_target_action == ACTION_FX_DISTORTION_DRIVE ||
                    learn_target_action == ACTION_FX_DISTORTION_MIX ||
                    learn_target_action == ACTION_FX_FILTER_CUTOFF ||
                    learn_target_action == ACTION_FX_FILTER_RESONANCE ||
                    learn_target_action == ACTION_FX_EQ_LOW ||
                    learn_target_action == ACTION_FX_EQ_MID ||
                    learn_target_action == ACTION_FX_EQ_HIGH ||
                    learn_target_action == ACTION_FX_COMPRESSOR_THRESHOLD ||
                    learn_target_action == ACTION_FX_COMPRESSOR_RATIO ||
                    learn_target_action == ACTION_FX_DELAY_TIME ||
                    learn_target_action == ACTION_FX_DELAY_FEEDBACK ||
                    learn_target_action == ACTION_FX_DELAY_MIX) {
                    mapping.threshold = 0;
                    mapping.continuous = 1; // Continuous fader mode
                } else {
                    // Button mode for: toggles, mutes, note pads
                    // This includes: ACTION_FX_*_TOGGLE, ACTION_*_MUTE, ACTION_TRIGGER_NOTE_PAD
                    mapping.threshold = 64; // Button-style threshold
                    mapping.continuous = 0; // Button mode
                }

                // Remove any existing mappings for this CC number (from any device)
                // Since we're learning with device_id = -1, remove all CC mappings to avoid conflicts
                for (int i = 0; i < input_mappings->midi_count; i++) {
                    if (input_mappings->midi_mappings[i].cc_number == data1) {
                        // Shift remaining mappings down
                        for (int j = i; j < input_mappings->midi_count - 1; j++) {
                            input_mappings->midi_mappings[j] = input_mappings->midi_mappings[j + 1];
                        }
                        input_mappings->midi_count--;
                        i--;  // Check this index again since we shifted
                    }
                }

                // Add the new mapping
                if (input_mappings->midi_count < input_mappings->midi_capacity) {
                    input_mappings->midi_mappings[input_mappings->midi_count++] = mapping;
                }
            }

            learn_mode_active = false;
            learn_target_action = ACTION_NONE;
            return;
        }

        // Check for mapped actions
        if (input_mappings) {
            InputEvent event;
            if (input_mappings_get_midi_event(input_mappings, device_id, data1, data2, &event)) {
                handle_input_event(&event);
            }
        }
    }
}

// SDL audio callback
void audioCallback(void* userdata, Uint8* stream, int len) {
    float* out = reinterpret_cast<float*>(stream);
    int frames = len / (sizeof(float) * 2); // stereo

    // Update MIDI file playback BEFORE acquiring the lock
    // This runs in the audio thread for perfect timing (no UI blocking!)
    // The MIDI event callbacks will acquire the lock themselves
    if (performance) {
        // Get pattern position from sequencer (single source of truth)
        // Sequencer handles internal timing advancement
        int current_pulse = -1;
        if (sequencer && medness_sequencer_is_active(sequencer)) {
            // Update sequencer - it will advance position based on its clock mode
            current_pulse = medness_sequencer_update(sequencer, frames, 44100);

            // Debug: log sequencer position and BPM every 96 pulses (every bar)
            static int last_debug_pulse = -1;
            if (current_pulse >= 0 && current_pulse / 96 != last_debug_pulse / 96) {
                float sequencer_bpm = medness_sequencer_get_bpm(sequencer);
                std::cout << "[SEQUENCER] pulse=" << current_pulse
                          << " row=" << medness_sequencer_get_row(sequencer)
                          << " BPM=" << sequencer_bpm
                          << " (external_clock=" << (midi_clock.active ? "YES" : "NO") << ")"
                          << std::endl;
                last_debug_pulse = current_pulse;
            }
        }

        // Update unified performance manager (handles both pads and sequences)
        medness_performance_update_samples(performance, frames, 44100, current_pulse);

        // Update sequence manager (for multi-phrase sequences triggered via sequence_manager)
        if (sequence_manager) {
            medness_performance_update_samples(sequence_manager, frames, 44100, current_pulse);
        }
    }

    std::lock_guard<std::mutex> lock(synth_mutex);

    // Create temporary buffers for left and right channels
    std::vector<float> left(frames, 0.0f);
    std::vector<float> right(frames, 0.0f);

    // If we have multiple program synths loaded, mix them all together
    if (rsx && rsx->num_programs > 0) {
        std::vector<float> prog_left(frames);
        std::vector<float> prog_right(frames);

        for (int i = 0; i < rsx->num_programs; i++) {
            if (!program_synths[i]) continue;

            // Clear program buffers
            std::fill(prog_left.begin(), prog_left.end(), 0.0f);
            std::fill(prog_right.begin(), prog_right.end(), 0.0f);
            float* prog_channels[2] = { prog_left.data(), prog_right.data() };

            // Render this program's audio
            sfizz_render_block(program_synths[i], prog_channels, 2, frames);

            // Apply per-program FX if enabled (pre-fader)
            if (effects_program[i] && mixer.program_fx_enable[i]) {
                std::vector<int16_t> int16_buf(frames * 2);
                for (int j = 0; j < frames; j++) {
                    int16_buf[j * 2] = static_cast<int16_t>(prog_left[j] * 32767.0f);
                    int16_buf[j * 2 + 1] = static_cast<int16_t>(prog_right[j] * 32767.0f);
                }
                regroove_effects_process(effects_program[i], int16_buf.data(), frames, 44100);
                for (int j = 0; j < frames; j++) {
                    prog_left[j] = int16_buf[j * 2] / 32767.0f;
                    prog_right[j] = int16_buf[j * 2 + 1] / 32767.0f;
                }
            }

            // Apply per-program pan
            float prog_pan = mixer.program_pans[i];
            float prog_left_gain = 1.0f - prog_pan;
            float prog_right_gain = prog_pan;

            // Apply per-program volume and mute
            float prog_vol = mixer.program_mutes[i] ? 0.0f : mixer.program_volumes[i];
            for (int j = 0; j < frames; j++) {
                prog_left[j] *= prog_vol * prog_left_gain;
                prog_right[j] *= prog_vol * prog_right_gain;
            }

            // Mix into main buffers
            for (int j = 0; j < frames; j++) {
                left[j] += prog_left[j];
                right[j] += prog_right[j];
            }
        }
    } else if (synth) {
        // Single synth mode (no programs)
        float* channels[2] = { left.data(), right.data() };
        sfizz_render_block(synth, channels, 2, frames);
    }

    // Apply playback volume and pan
    // Pan: 0.0 = left, 0.5 = center (both at 100%), 1.0 = right
    float playback_vol = mixer.playback_mute ? 0.0f : mixer.playback_volume;
    float playback_pan = mixer.playback_pan;

    // Constant-power panning: center should be full volume on both channels
    float playback_left_gain = playback_vol * (playback_pan <= 0.5f ? 1.0f : (1.0f - (playback_pan - 0.5f) * 2.0f));
    float playback_right_gain = playback_vol * (playback_pan >= 0.5f ? 1.0f : (playback_pan * 2.0f));

    for (int i = 0; i < frames; i++) {
        left[i] *= playback_left_gain;
        right[i] *= playback_right_gain;
    }

    // Apply master volume and pan
    float master_vol = mixer.master_mute ? 0.0f : mixer.master_volume;
    float master_pan = mixer.master_pan;

    // Constant-power panning: center should be full volume on both channels
    float master_left_gain = master_vol * (master_pan <= 0.5f ? 1.0f : (1.0f - (master_pan - 0.5f) * 2.0f));
    float master_right_gain = master_vol * (master_pan >= 0.5f ? 1.0f : (master_pan * 2.0f));

    for (int i = 0; i < frames; i++) {
        left[i] *= master_left_gain;
        right[i] *= master_right_gain;
    }

    // Apply master effects if enabled
    if (effects_master && mixer.master_fx_enable) {
        // Convert float to int16 for effects
        std::vector<int16_t> int16_buf(frames * 2);
        for (int i = 0; i < frames; i++) {
            int16_buf[i * 2] = static_cast<int16_t>(left[i] * 32767.0f);
            int16_buf[i * 2 + 1] = static_cast<int16_t>(right[i] * 32767.0f);
        }

        regroove_effects_process(effects_master, int16_buf.data(), frames, 44100);

        // Convert back to float
        for (int i = 0; i < frames; i++) {
            left[i] = int16_buf[i * 2] / 32767.0f;
            right[i] = int16_buf[i * 2 + 1] / 32767.0f;
        }
    }

    // Interleave the channels into the output buffer
    for (int i = 0; i < frames; i++) {
        out[i * 2] = left[i];
        out[i * 2 + 1] = right[i];
    }
}

// MIDI file loop restart callback - triggers visual blink
void midi_file_loop_callback(void* userdata) {
    int pad_index = userdata ? *((int*)userdata) : -1;
    if (pad_index >= 0 && pad_index < RSX_MAX_NOTE_PADS) {
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::cout << "[" << ms << "] LOOP pad=" << (pad_index + 1) << std::endl;
        note_pad_loop_fade[pad_index] = 1.0f;  // Trigger blue/cyan blink on loop restart
    }
}

// Debug flag for MIDI event logging (disable for better performance)
static const bool DEBUG_MIDI_EVENTS = false;

// Visual feedback callback for pad MIDI events (UI RESPONSIBILITY)
// Called by engine when pad fires MIDI notes
void pad_visual_feedback(int pad_index, int note, int velocity, int on) {
    // Visual feedback: trigger white blink on THIS specific pad when note ON
    if (on && pad_index >= 0 && pad_index < RSX_MAX_NOTE_PADS) {
        note_pad_fade[pad_index] = 1.0f;  // Trigger white blink on note
    }

    // Optional: Log event timing for debugging sync issues (causes I/O blocking!)
    if (DEBUG_MIDI_EVENTS) {
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::cout << "[" << ms << "] PAD " << (pad_index + 1)
                  << " note=" << note << " vel=" << velocity << " " << (on ? "ON" : "OFF") << std::endl;
    }
}

// MIDI file player callback for sequences (not pads)
void midi_file_event_callback(int note, int velocity, int on, void* userdata) {
    std::lock_guard<std::mutex> lock(synth_mutex);

    // For sequences, userdata is nullptr - send to current program
    int target_program = current_program;

    // Send to target program synth
    sfizz_synth_t* target_synth = program_synths[target_program];
    if (target_synth) {
        if (on) {
            sfizz_send_note_on(target_synth, 0, note, velocity);
        } else {
            sfizz_send_note_off(target_synth, 0, note, 0);
        }
    }
}

int main(int argc, char* argv[]) {
    // Check for SFZ or RSX file argument
    const char* sfz_file = "assets/example.sfz";  // default
    std::string sfz_filename = "example.sfz";  // Just the filename for display
    bool is_rsx = false;

    if (argc > 1) {
        const char* input_file = argv[1];
        size_t len = strlen(input_file);

        // Check if input is a directory
        struct stat path_stat;
        bool is_directory = false;
        if (stat(input_file, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            is_directory = true;
        }

        if (is_directory) {
            // Directory-only mode: Initialize file browser without loading anything
            std::cout << "Directory mode: " << input_file << std::endl;
            file_list = samplecrate_filelist_create();
            if (file_list) {
                int file_count = samplecrate_filelist_load(file_list, input_file);
                std::cout << "File browser: Loaded " << file_count << " files from " << input_file << std::endl;
                if (file_count > 0) {
                    std::cout << "Use < and > buttons to browse, 'o' to load" << std::endl;
                }
            }
            // Don't load any SFZ/RSX file - user will select via file browser
        }
        // Check if file ends with .rsx
        else if (len > 4 && strcmp(input_file + len - 4, ".rsx") == 0) {
            is_rsx = true;

            // Store RSX path - will be loaded into engine later
            rsx_file_path = input_file;
            std::cout << "RSX file specified: " << input_file << " (will load into engine)" << std::endl;
            // Note: reload_sequences() will be called after sequence_manager is created

            // Initialize file browser with the directory of the loaded RSX file
            std::string dir = input_file;
            size_t last_slash = dir.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                dir = dir.substr(0, last_slash);
                file_list = samplecrate_filelist_create();
                if (file_list) {
                    int file_count = samplecrate_filelist_load(file_list, dir.c_str());
                    std::cout << "File browser: Loaded " << file_count << " files from " << dir << std::endl;

                    // Find and set current file as current index
                    const char* current_filename = strrchr(input_file, '/');
                    if (!current_filename) current_filename = strrchr(input_file, '\\');
                    if (current_filename) {
                        current_filename++;  // Skip the slash
                        for (int i = 0; i < file_list->count; i++) {
                            if (strcmp(file_list->filenames[i], current_filename) == 0) {
                                file_list->current_index = i;
                                std::cout << "  Current file: " << current_filename << " (index " << i << ")" << std::endl;
                                break;
                            }
                        }
                    }
                }
            }
        }
    } else {
        std::cout << "Usage: " << argv[0] << " <path_to_sfz_or_rsx_file>" << std::endl;
        std::cout << "Using default: " << sfz_file << std::endl;
        std::cout << std::endl;
        std::cout << "  .sfz files: Direct SFZ instrument files" << std::endl;
        std::cout << "  .rsx files: Samplecrate wrapper with note trigger pads" << std::endl;
        std::cout << std::endl;
        std::cout << "NOTE: Sample paths in SFZ files are resolved relative to the current" << std::endl;
        std::cout << "      working directory. Make sure to run from the correct location!" << std::endl;
        std::cout << "      Example: ./build/samplecrate ./assets/TR909.rsx" << std::endl;
    }

    // Print current working directory for debugging
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        std::cout << "Current working directory: " << cwd << std::endl;
    }

    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    SDL_Window* window = SDL_CreateWindow(
        appname, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 640, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    ImGui::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL2_Init();

    // Apply dark style (from mock-ui.cpp)
    ImGuiStyle& s = ImGui::GetStyle();
    s.FrameRounding    = 3.0f;
    s.GrabRounding     = 3.0f;
    s.ScrollbarRounding= 3.0f;
    s.WindowPadding    = ImVec2(6,6);
    s.FramePadding     = ImVec2(5,3);
    s.ItemSpacing      = ImVec2(8,6);
    s.ItemInnerSpacing = ImVec2(6,4);
    s.ChildBorderSize  = 1.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;

    ImVec4* c = s.Colors;
    ImVec4 black = ImVec4(0,0,0,1);
    ImVec4 dark2 = ImVec4(0.12f,0.12f,0.12f,1.0f);

    c[ImGuiCol_WindowBg]        = black;
    c[ImGuiCol_ChildBg]         = black;
    c[ImGuiCol_PopupBg]         = ImVec4(0.07f,0.07f,0.07f,1.0f);
    c[ImGuiCol_Border]          = ImVec4(0.15f,0.15f,0.15f,0.3f);
    c[ImGuiCol_BorderShadow]    = ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]         = dark2;
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f,0.18f,0.18f,1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f,0.24f,0.24f,1.0f);

    ImVec4 red       = ImVec4(0.90f,0.15f,0.18f,1.0f);
    ImVec4 redHover  = ImVec4(0.98f,0.26f,0.30f,1.0f);

    c[ImGuiCol_Button]          = dark2;
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.23f,0.23f,0.23f,1.0f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.16f,0.16f,0.16f,1.0f);

    c[ImGuiCol_SliderGrab]      = red;
    c[ImGuiCol_SliderGrabActive]= redHover;

    c[ImGuiCol_Text]            = ImVec4(0.88f,0.89f,0.90f,1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f,0.46f,0.48f,1.0f);

    // Initialize LCD display (20x4 character display)
    lcd_display = lcd_init(LCD_COLS, LCD_ROWS);

    // Load config (before engine creation so we can apply defaults)
    samplecrate_config_init(&config);
    samplecrate_config_load(&config, "samplecrate.ini");

    // Load expanded pads setting from config
    expanded_pads = (config.expanded_pads != 0);

    // Initialize pattern sequencer (single source of truth for pattern position)
    sequencer = medness_sequencer_create();
    if (sequencer) {
        medness_sequencer_set_bpm(sequencer, 125.0f);  // Default BPM
        medness_sequencer_set_active(sequencer, 1);    // Always active
    }

    // Create engine (contains all audio/MIDI state for headless operation)
    engine = samplecrate_engine_create(sequencer);
    if (!engine) {
        std::cerr << "Failed to create engine!" << std::endl;
        return -1;
    }

    // Initialize mixer (now that engine exists) and apply config defaults
    samplecrate_mixer_init(&mixer);
    mixer.master_volume = config.default_master_volume;
    mixer.master_pan = config.default_master_pan;
    mixer.playback_volume = config.default_playback_volume;
    mixer.playback_pan = config.default_playback_pan;

    // Apply config defaults to effects (created by engine)
    if (effects_master) {
        regroove_effects_set_distortion_drive(effects_master, config.fx_distortion_drive);
        regroove_effects_set_distortion_mix(effects_master, config.fx_distortion_mix);
        regroove_effects_set_filter_cutoff(effects_master, config.fx_filter_cutoff);
        regroove_effects_set_filter_resonance(effects_master, config.fx_filter_resonance);
        regroove_effects_set_eq_low(effects_master, config.fx_eq_low);
        regroove_effects_set_eq_mid(effects_master, config.fx_eq_mid);
        regroove_effects_set_eq_high(effects_master, config.fx_eq_high);
        regroove_effects_set_compressor_threshold(effects_master, config.fx_compressor_threshold);
        regroove_effects_set_compressor_ratio(effects_master, config.fx_compressor_ratio);
        regroove_effects_set_compressor_attack(effects_master, config.fx_compressor_attack);
        regroove_effects_set_compressor_release(effects_master, config.fx_compressor_release);
        regroove_effects_set_compressor_makeup(effects_master, config.fx_compressor_makeup);
        regroove_effects_set_phaser_rate(effects_master, config.fx_phaser_rate);
        regroove_effects_set_phaser_depth(effects_master, config.fx_phaser_depth);
        regroove_effects_set_phaser_feedback(effects_master, config.fx_phaser_feedback);
        regroove_effects_set_reverb_room_size(effects_master, config.fx_reverb_room_size);
        regroove_effects_set_reverb_damping(effects_master, config.fx_reverb_damping);
        regroove_effects_set_reverb_mix(effects_master, config.fx_reverb_mix);
        regroove_effects_set_delay_time(effects_master, config.fx_delay_time);
        regroove_effects_set_delay_feedback(effects_master, config.fx_delay_feedback);
        regroove_effects_set_delay_mix(effects_master, config.fx_delay_mix);
    }

    // Apply config defaults to per-program effects
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        if (effects_program[i]) {
            regroove_effects_set_distortion_drive(effects_program[i], config.fx_distortion_drive);
            regroove_effects_set_distortion_mix(effects_program[i], config.fx_distortion_mix);
            regroove_effects_set_filter_cutoff(effects_program[i], config.fx_filter_cutoff);
            regroove_effects_set_filter_resonance(effects_program[i], config.fx_filter_resonance);
            regroove_effects_set_eq_low(effects_program[i], config.fx_eq_low);
            regroove_effects_set_eq_mid(effects_program[i], config.fx_eq_mid);
            regroove_effects_set_eq_high(effects_program[i], config.fx_eq_high);
            regroove_effects_set_compressor_threshold(effects_program[i], config.fx_compressor_threshold);
            regroove_effects_set_compressor_ratio(effects_program[i], config.fx_compressor_ratio);
            regroove_effects_set_compressor_attack(effects_program[i], config.fx_compressor_attack);
            regroove_effects_set_compressor_release(effects_program[i], config.fx_compressor_release);
            regroove_effects_set_compressor_makeup(effects_program[i], config.fx_compressor_makeup);
            regroove_effects_set_phaser_rate(effects_program[i], config.fx_phaser_rate);
            regroove_effects_set_phaser_depth(effects_program[i], config.fx_phaser_depth);
            regroove_effects_set_phaser_feedback(effects_program[i], config.fx_phaser_feedback);
            regroove_effects_set_reverb_room_size(effects_program[i], config.fx_reverb_room_size);
            regroove_effects_set_reverb_damping(effects_program[i], config.fx_reverb_damping);
            regroove_effects_set_reverb_mix(effects_program[i], config.fx_reverb_mix);
            regroove_effects_set_delay_time(effects_program[i], config.fx_delay_time);
            regroove_effects_set_delay_feedback(effects_program[i], config.fx_delay_feedback);
            regroove_effects_set_delay_mix(effects_program[i], config.fx_delay_mix);
        }
    }

    // Note: performance is now created by the engine, accessed via macro
    // Callbacks for pads are set individually per-pad (each with its own context)
    // when pads are loaded in the pad loading code

    // Initialize MIDI sequence manager (for multi-phrase sequences triggered via GUI)
    sequence_manager = medness_performance_create();
    if (sequence_manager) {
        // Set the sequencer reference - sequences use the same sequencer as pads!
        medness_performance_set_sequencer(sequence_manager, sequencer);

        medness_performance_set_midi_callback(sequence_manager, midi_file_event_callback, nullptr);
        medness_performance_set_tempo(sequence_manager, 125.0f);  // Default BPM
        medness_performance_set_start_mode(sequence_manager, SEQUENCE_START_QUANTIZED);  // Wait for row 0

        // Load sequences if RSX file was already loaded
        if (rsx && !rsx_file_path.empty()) {
            reload_sequences();
        }
    }

    // If RSX was specified from command line, load it into the engine
    if (!rsx_file_path.empty()) {
        std::cout << "Loading RSX into engine: " << rsx_file_path << std::endl;
        if (samplecrate_engine_load_rsx(engine, rsx_file_path.c_str()) != 0) {
            std::cerr << "Failed to load RSX into engine!" << std::endl;
            error_message = "Failed to load RSX";
        } else {
            // Load MIDI files for pads (engine handles routing, provides visual feedback callback)
            samplecrate_engine_load_pads(engine, pad_visual_feedback);
        }
    }

    // Apply mixer and effects settings from RSX if loaded (via engine accessor)
    if (rsx && rsx->num_programs > 0) {
        // Apply program volume and pan settings from RSX
        for (int i = 0; i < rsx->num_programs; i++) {
            mixer.program_volumes[i] = rsx->program_volumes[i];
            mixer.program_pans[i] = rsx->program_pans[i];
            std::cout << "Applied program " << (i + 1) << " settings: volume=" << mixer.program_volumes[i]
                      << " pan=" << mixer.program_pans[i] << std::endl;
        }

        // Apply FX chain enable states from RSX
        mixer.master_fx_enable = rsx->master_fx_enable;
        for (int i = 0; i < rsx->num_programs; i++) {
            mixer.program_fx_enable[i] = rsx->program_fx_enable[i];
        }

        // Apply effects settings from RSX
        std::cout << "Loading effects settings from RSX..." << std::endl;
        if (effects_master) {
            apply_rsx_effects_to_instance(effects_master, &rsx->master_effects);
            std::cout << "  Master effects loaded from RSX (enabled=" << mixer.master_fx_enable << ")" << std::endl;
        }
        for (int i = 0; i < rsx->num_programs; i++) {
            if (effects_program[i]) {
                apply_rsx_effects_to_instance(effects_program[i], &rsx->program_effects[i]);
                std::cout << "  Program " << (i + 1) << " effects loaded from RSX (enabled=" << mixer.program_fx_enable[i] << ")" << std::endl;
            }
        }
        // Note: Note suppression and MIDI pad files are loaded by samplecrate_engine_load_rsx()
    }

    // Enumerate audio output devices
    num_audio_devices = SDL_GetNumAudioDevices(0);  // 0 = output devices
    std::cout << "Found " << num_audio_devices << " audio output device(s)" << std::endl;
    for (int i = 0; i < num_audio_devices && i < 16; i++) {
        const char* device_name = SDL_GetAudioDeviceName(i, 0);
        std::cout << "  Device " << i << ": " << (device_name ? device_name : "Unknown") << std::endl;
    }

    // Init SDL audio
    SDL_AudioSpec spec, obtained;
    spec.freq = 44100;
    spec.format = AUDIO_F32SYS;
    spec.channels = 2;
    spec.samples = 512;
    spec.callback = audioCallback;
    spec.userdata = nullptr;

    // Determine which audio device to use
    const char* device_to_open = nullptr;
    if (config.audio_device >= 0 && config.audio_device < num_audio_devices) {
        device_to_open = SDL_GetAudioDeviceName(config.audio_device, 0);
        std::cout << "Using configured audio device " << config.audio_device << ": " << device_to_open << std::endl;
    } else {
        std::cout << "Using default audio device" << std::endl;
    }

    // Open audio device (SDL_OpenAudioDevice with NULL uses default)
    current_audio_device_id = SDL_OpenAudioDevice(device_to_open, 0, &spec, &obtained, 0);
    if (current_audio_device_id == 0) {
        std::cerr << "Failed to open audio device: " << SDL_GetError() << std::endl;
    } else {
        std::cout << "Audio opened successfully" << std::endl;
        std::cout << "Sample rate: " << obtained.freq << " Hz" << std::endl;
        std::cout << "Channels: " << (int)obtained.channels << std::endl;
        std::cout << "Buffer size: " << obtained.samples << " samples" << std::endl;
        SDL_PauseAudioDevice(current_audio_device_id, 0);  // Start audio
    }

    // Initialize input mappings and load from config
    input_mappings = input_mappings_create();
    if (input_mappings) {
        input_mappings_load(input_mappings, "samplecrate.ini");
    }

    // Initialize MIDI input
    int num_midi_ports = midi_list_ports();
    std::cout << "Found " << num_midi_ports << " MIDI port(s)" << std::endl;

    if (num_midi_ports > 0) {
        // List available MIDI ports
        for (int i = 0; i < num_midi_ports && i < 4; i++) {
            char port_name[128];
            if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                std::cout << "  Port " << i << ": " << port_name << std::endl;
            }
        }

        // Load MIDI device configuration from config file
        // Respect user's configuration - don't auto-default to port 0
        midi_device_ports[0] = config.midi_device_0;
        midi_device_ports[1] = config.midi_device_1;
        midi_device_ports[2] = config.midi_device_2;

        // Initialize SysEx system with configured device ID
        sysex_init(config.sysex_device_id);
        sysex_register_callback(sysex_callback, nullptr);
        std::cout << "SysEx initialized (device ID: " << (int)sysex_get_device_id() << ")" << std::endl;

        if (midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES) == 0) {
            std::cout << "MIDI initialized successfully" << std::endl;
            if (midi_device_ports[0] >= 0) {
                std::cout << "  Device 0: port " << midi_device_ports[0] << std::endl;
            }
            if (midi_device_ports[1] >= 0) {
                std::cout << "  Device 1: port " << midi_device_ports[1] << std::endl;
            }
            if (midi_device_ports[2] >= 0) {
                std::cout << "  Device 2: port " << midi_device_ports[2] << std::endl;
            }
        } else {
            std::cout << "MIDI initialization failed, continuing without MIDI support" << std::endl;
        }
    } else {
        std::cout << "No MIDI ports found, continuing without MIDI support" << std::endl;
    }

    // UI loop
    int note = 60, velocity = 100;
    bool playing = true;
    SDL_Event event;
    while (playing) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) playing = false;

            // F11: Toggle fullscreen
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(window);
                if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                    SDL_SetWindowFullscreen(window, 0);
                    std::cout << "Fullscreen: OFF" << std::endl;
                } else {
                    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    std::cout << "Fullscreen: ON" << std::endl;
                }
            }

            // F12: Toggle fullscreen pads mode
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F12) {
                fullscreen_pads_mode = !fullscreen_pads_mode;
                if (fullscreen_pads_mode) {
                    ui_mode = UI_MODE_PADS;  // Auto-switch to PADS mode
                }
                std::cout << "Fullscreen pads mode: " << (fullscreen_pads_mode ? "ON" : "OFF") << std::endl;
            }
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // MIDI file players are now updated in the audio callback for sample-accurate timing!
        // This eliminates UI thread blocking and ensures perfect sync between pads.
        // (Previously called midi_file_pad_player_update_all here in UI thread)

        // Update note pad fade effects
        for (int i = 0; i < RSX_MAX_NOTE_PADS; i++) {
            // Fade the normal note trigger brightness (fast fade on each note)
            if (note_pad_fade[i] > 0.0f) {
                note_pad_fade[i] -= 0.04f;  // Fast fade on each note trigger
                if (note_pad_fade[i] < 0.0f) note_pad_fade[i] = 0.0f;
            }

            // Fade the loop restart brightness (slower for more visibility)
            if (note_pad_loop_fade[i] > 0.0f) {
                note_pad_loop_fade[i] -= 0.02f;  // Slower fade for loop events
                if (note_pad_loop_fade[i] < 0.0f) note_pad_loop_fade[i] = 0.0f;
            }

            // Update sequence pad states and blink timers
            bool pad_has_sequence = rsx && i < rsx->num_pads && rsx->pads[i].sequence_index >= 0;
            bool pad_has_midi_file = rsx && i < rsx->num_pads && rsx->pads[i].midi_file[0] != '\0';

            if (pad_has_sequence) {
                // Pad triggers a multi-phrase sequence via sequence_manager
                int seq_idx = rsx->pads[i].sequence_index;
                bool is_playing = sequence_manager && medness_performance_is_playing(sequence_manager, seq_idx);

                if (is_playing) {
                    sequence_pad_states[i] = SEQ_PAD_PLAYING;
                    // Blink timer for playing sequences (sine wave for smooth pulsing)
                    sequence_pad_blink[i] += 0.1f;
                    if (sequence_pad_blink[i] > 6.28f) sequence_pad_blink[i] = 0.0f;  // 2*PI
                } else {
                    sequence_pad_states[i] = SEQ_PAD_IDLE;
                    sequence_pad_blink[i] = 0.0f;
                }
            } else if (pad_has_midi_file) {
                // Pad plays a MIDI file via performance manager (unified system)
                bool is_playing = performance && medness_performance_is_playing(performance, i);

                if (is_playing) {
                    sequence_pad_states[i] = SEQ_PAD_PLAYING;
                    // Blink timer for playing pads (sine wave for smooth pulsing)
                    sequence_pad_blink[i] += 0.1f;
                    if (sequence_pad_blink[i] > 6.28f) sequence_pad_blink[i] = 0.0f;  // 2*PI
                } else {
                    sequence_pad_states[i] = SEQ_PAD_IDLE;
                    sequence_pad_blink[i] = 0.0f;
                }
            } else {
                sequence_pad_states[i] = SEQ_PAD_IDLE;
                sequence_pad_blink[i] = 0.0f;
            }
        }

        // Update step sequencer fade and highlight current position
        // 64 rows map to 16 steps (4 rows per step)
        if (sequencer) {
            int current_row = medness_sequencer_get_row(sequencer);  // 1-64
            int current_step = (current_row - 1) / 4;  // 0-15

            // Highlight current step
            step_fade[current_step] = 1.0f;

            // Fade all steps
            for (int i = 0; i < 16; i++) {
                if (step_fade[i] > 0.0f) {
                    step_fade[i] -= 0.02f;  // Slower fade for sequencer steps
                    if (step_fade[i] < 0.0f) step_fade[i] = 0.0f;
                }
            }
        }

        // NO timeout for MIDI clock - once sync is established, we keep the pulse count going
        // even if external pulses stop. Our internal clock continues, and we catch up when
        // external sync resumes. This prevents any playback interruptions.

        // Main window
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin(appname, nullptr, rootFlags);

        // Layout constants (matching mock-ui.cpp)
        const float SIDE_MARGIN = 10.0f;
        const float TOP_MARGIN = 8.0f;
        const float LEFT_PANEL_WIDTH = 190.0f;
        const float LCD_HEIGHT = 90.0f;
        const float BOTTOM_MARGIN = 8.0f;

        // Sequencer bar constants (matching Regroove)
        const float SEQUENCER_HEIGHT = 70.0f;
        const float GAP_ABOVE_SEQUENCER = 8.0f;

        float fullW = io.DisplaySize.x;
        float fullH = io.DisplaySize.y;

        // Calculate panel heights accounting for padding and borders (like mock-ui.cpp)
        ImGuiStyle& style = ImGui::GetStyle();
        float childPaddingY = style.WindowPadding.y * 2.0f;
        float childBorderY = style.ChildBorderSize * 2.0f;

        // Account for sequencer bar at bottom
        float channelAreaHeight = fullH - TOP_MARGIN - BOTTOM_MARGIN - GAP_ABOVE_SEQUENCER - SEQUENCER_HEIGHT - childPaddingY - childBorderY;
        float leftPanelHeight = channelAreaHeight;

        // LEFT PANEL (hidden in fullscreen pads mode)
        if (!fullscreen_pads_mode) {
            ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, TOP_MARGIN));
            ImGui::BeginChild("left_panel", ImVec2(LEFT_PANEL_WIDTH, leftPanelHeight),
                              false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
            // Update LCD display with current state
            if (lcd_display) {
                char lcd_text[256] = "";

                // Show error if present
                if (!error_message.empty()) {
                    snprintf(lcd_text, sizeof(lcd_text),
                             "ERROR:\n%s",
                             error_message.c_str());
                    lcd_write(lcd_display, lcd_text);
                }
                // Show file browser when in browse mode (takes priority)
                else if (file_browser_mode && file_list && file_list->count > 0) {
                    // Display current filename in browser (like Regroove)
                    char line1[64];
                    snprintf(line1, sizeof(line1), "File: %d/%d",
                             file_list->current_index + 1, file_list->count);

                    char line2[64];
                    snprintf(line2, sizeof(line2), "%s",
                             file_list->filenames[file_list->current_index]);

                    snprintf(lcd_text, sizeof(lcd_text), "%s\n%s\n\nPress 'o' to LOAD", line1, line2);
                    lcd_write(lcd_display, lcd_text);
                }
                // Show program information if RSX loaded
                else if (rsx && rsx->num_programs > 0) {
                    // Use program name if available, otherwise show program number
                    const char* prog_display = rsx->program_names[current_program][0] != '\0'
                        ? rsx->program_names[current_program]
                        : "";

                    // Build sync status and BPM display
                    char status_info[64] = "";
                    bool has_clock_sync = (midi_clock.active && midi_clock.bpm > 0.0f && config.midi_clock_tempo_sync == 1);
                    bool has_spp_sync = (midi_clock.spp_synced && config.midi_spp_receive == 1);

                    // Determine sync status indicators
                    if (has_clock_sync && has_spp_sync) {
                        snprintf(status_info, sizeof(status_info), "[SYNC+SPP]");
                    } else if (has_clock_sync) {
                        snprintf(status_info, sizeof(status_info), "[SYNC]");
                    } else if (has_spp_sync) {
                        snprintf(status_info, sizeof(status_info), "[SPP]");
                    }

                    // Format BPM - always show active playback tempo
                    // Use '>' only when actively receiving MIDI clock
                    char bpm_str[32];
                    if (has_clock_sync) {
                        snprintf(bpm_str, sizeof(bpm_str), "BPM:>%.0f", active_bpm);
                    } else {
                        snprintf(bpm_str, sizeof(bpm_str), "BPM:%.0f", active_bpm);
                    }

                    // Line 1: Program name + note info (if playing)
                    char line1[64];
                    if (current_note >= 0) {
                        if (prog_display[0] != '\0') {
                            snprintf(line1, sizeof(line1), "Prg %d/%d: %s N:%d V:%d",
                                     current_program + 1, rsx->num_programs,
                                     prog_display, current_note, current_velocity);
                        } else {
                            snprintf(line1, sizeof(line1), "Prg %d/%d N:%d V:%d",
                                     current_program + 1, rsx->num_programs,
                                     current_note, current_velocity);
                        }
                    } else {
                        if (prog_display[0] != '\0') {
                            snprintf(line1, sizeof(line1), "Prg %d/%d: %s",
                                     current_program + 1, rsx->num_programs,
                                     prog_display);
                        } else {
                            snprintf(line1, sizeof(line1), "Prg %d/%d",
                                     current_program + 1, rsx->num_programs);
                        }
                    }

                    // Get Row Position (1-64) from sequencer
                    int row_pos = sequencer ? medness_sequencer_get_row(sequencer) : 1;

                    // Line 2: BPM + Row Position + sync status
                    char line2[64];
                    if (status_info[0] != '\0') {
                        snprintf(line2, sizeof(line2), "%s R:%02d %s", bpm_str, row_pos, status_info);
                    } else {
                        snprintf(line2, sizeof(line2), "%s R:%02d", bpm_str, row_pos);
                    }

                    snprintf(lcd_text, sizeof(lcd_text), "%s\n%s", line1, line2);
                    lcd_write(lcd_display, lcd_text);
                }
                // Show file browser when file_list exists (no RSX loaded yet)
                else if (file_list && file_list->count > 0) {
                    // Display current filename in browser (like Regroove)
                    char line1[64];
                    snprintf(line1, sizeof(line1), "File: %d/%d",
                             file_list->current_index + 1, file_list->count);

                    char line2[64];
                    snprintf(line2, sizeof(line2), "%s",
                             file_list->filenames[file_list->current_index]);

                    snprintf(lcd_text, sizeof(lcd_text), "%s\n%s\n\nPress 'o' to LOAD", line1, line2);
                    lcd_write(lcd_display, lcd_text);
                }
                // No RSX and no file list - show simple display
                else {
                    // Build sync status and BPM display
                    char status_info[64] = "";
                    bool has_clock_sync = (midi_clock.active && midi_clock.bpm > 0.0f && config.midi_clock_tempo_sync == 1);
                    bool has_spp_sync = (midi_clock.spp_synced && config.midi_spp_receive == 1);

                    // Determine sync status indicators
                    if (has_clock_sync && has_spp_sync) {
                        snprintf(status_info, sizeof(status_info), "[SYNC+SPP]");
                    } else if (has_clock_sync) {
                        snprintf(status_info, sizeof(status_info), "[SYNC]");
                    } else if (has_spp_sync) {
                        snprintf(status_info, sizeof(status_info), "[SPP]");
                    }

                    // Format BPM - always show active playback tempo
                    // Use '>' only when actively receiving MIDI clock
                    char bpm_str[32];
                    if (has_clock_sync) {
                        snprintf(bpm_str, sizeof(bpm_str), "BPM:>%.0f", active_bpm);
                    } else {
                        snprintf(bpm_str, sizeof(bpm_str), "BPM:%.0f", active_bpm);
                    }

                    // Line 1: File name + note info (if playing)
                    char line1[64];
                    if (current_note >= 0) {
                        snprintf(line1, sizeof(line1), "File: %s N:%d V:%d",
                                 sfz_filename.c_str(), current_note, current_velocity);
                    } else {
                        snprintf(line1, sizeof(line1), "File: %s",
                                 sfz_filename.c_str());
                    }

                    // Get Row Position (1-64) from sequencer
                    int row_pos = sequencer ? medness_sequencer_get_row(sequencer) : 1;

                    // Line 2: BPM + Row Position + sync status
                    char line2[64];
                    if (status_info[0] != '\0') {
                        snprintf(line2, sizeof(line2), "%s R:%02d %s", bpm_str, row_pos, status_info);
                    } else {
                        snprintf(line2, sizeof(line2), "%s R:%02d", bpm_str, row_pos);
                    }

                    snprintf(lcd_text, sizeof(lcd_text), "%s\n%s", line1, line2);
                    lcd_write(lcd_display, lcd_text);
                }

                // Draw LCD
                DrawLCD(lcd_get_buffer(lcd_display), LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // File browser buttons (like Regroove) - always shown if file_list exists
            const float BUTTON_SIZE = 48.0f;
            if (file_list && file_list->count > 0) {
                ImGui::BeginGroup();
                if (ImGui::Button("<", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                    if (learn_mode_active) {
                        start_learn_for_action(ACTION_FILE_PREV);
                    } else {
                        samplecrate_filelist_prev(file_list);
                        file_browser_mode = true;  // Enter browse mode
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("o", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                    if (learn_mode_active) {
                        start_learn_for_action(ACTION_FILE_LOAD);
                    } else {
                        char path[COMMON_MAX_PATH * 2];
                        samplecrate_filelist_get_current_path(file_list, path, sizeof(path));
                        printf("[File Browser] Loading: %s\n", path);

                        // Check if it's an RSX file
                        size_t len = strlen(path);
                        if (len > 4 && (strcmp(path + len - 4, ".rsx") == 0 || strcmp(path + len - 4, ".RSX") == 0)) {
                            // Load RSX file using engine
                            printf("[File Browser] Loading RSX: %s\n", path);

                            // Stop all MIDI playback before reloading
                            if (performance) {
                                medness_performance_stop_all(performance);
                            }
                            if (sequence_manager) {
                                medness_performance_stop_all(sequence_manager);
                            }

                            // Pause audio device during reload to avoid accessing freed synths
                            if (current_audio_device_id != 0) {
                                SDL_PauseAudioDevice(current_audio_device_id, 1);
                            }

                            // Load the RSX (this can take a while, but audio is paused)
                            int load_result = samplecrate_engine_load_rsx(engine, path);

                            // Resume audio device
                            if (current_audio_device_id != 0) {
                                SDL_PauseAudioDevice(current_audio_device_id, 0);
                            }

                            if (load_result == 0) {
                                printf("[File Browser] Successfully loaded: %s\n", path);
                                rsx_file_path = path;  // Update current file path

                                reload_sequences();  // Load sequences from RSX

                                // Load MIDI files for pads (engine handles routing, provides visual feedback callback)
                                samplecrate_engine_load_pads(engine, pad_visual_feedback);

                                // Exit browse mode after successful load
                                file_browser_mode = false;

                                printf("[File Browser] Loaded program 1 from %s\n", file_list->filenames[file_list->current_index]);
                            } else {
                                printf("[File Browser] Failed to load: %s\n", path);
                            }
                        } else if (len > 4 && (strcmp(path + len - 4, ".sfz") == 0 || strcmp(path + len - 4, ".SFZ") == 0)) {
                            // Direct SFZ file loading (legacy mode)
                            printf("[File Browser] Direct SFZ loading not yet implemented\n");
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(">", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                    if (learn_mode_active) {
                        start_learn_for_action(ACTION_FILE_NEXT);
                    } else {
                        samplecrate_filelist_next(file_list);
                        file_browser_mode = true;  // Enter browse mode
                    }
                }
                ImGui::EndGroup();
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // Program selection buttons (if RSX with programs loaded AND file already loaded)
            if (rsx && rsx->num_programs > 1) {
                const float PROG_BUTTON_SIZE = 48.0f;
                ImGui::Spacing();

                // P- button (grayed out if at first program)
                bool at_first = (current_program == 0);
                if (at_first) {
                    ImGui::PushStyleColor(ImGuiCol_Button, COLOR_BUTTON_AT_LIMIT);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COLOR_BUTTON_AT_LIMIT);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, COLOR_BUTTON_AT_LIMIT);
                }
                if (ImGui::Button("P-", ImVec2(PROG_BUTTON_SIZE, PROG_BUTTON_SIZE)) && !at_first) {
                    if (learn_mode_active) {
                        start_learn_for_action(ACTION_PROGRAM_PREV);
                    } else {
                        int prev_prog = current_program - 1;
                        if (prev_prog >= 0) {
                            switch_program(prev_prog);
                        }
                    }
                }
                if (at_first) ImGui::PopStyleColor(3);
                ImGui::SameLine();

                // P1 button (force program 1)
                ImVec4 p1Col = (current_program == 0) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                ImGui::PushStyleColor(ImGuiCol_Button, p1Col);
                if (ImGui::Button("P1", ImVec2(PROG_BUTTON_SIZE, PROG_BUTTON_SIZE))) {
                    switch_program(0);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();

                // P+ button (grayed out if at last program)
                bool at_last = (current_program == rsx->num_programs - 1);
                if (at_last) {
                    ImGui::PushStyleColor(ImGuiCol_Button, COLOR_BUTTON_AT_LIMIT);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COLOR_BUTTON_AT_LIMIT);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, COLOR_BUTTON_AT_LIMIT);
                }
                if (ImGui::Button("P+", ImVec2(PROG_BUTTON_SIZE, PROG_BUTTON_SIZE)) && !at_last) {
                    if (learn_mode_active) {
                        start_learn_for_action(ACTION_PROGRAM_NEXT);
                    } else {
                        int next_prog = current_program + 1;
                        if (next_prog < rsx->num_programs) {
                            switch_program(next_prog);
                        }
                    }
                }
                if (at_last) ImGui::PopStyleColor(3);

                ImGui::Dummy(ImVec2(0, 8.0f));
            }

            // Mode selection buttons

            // Switch to master effects settings
            ImVec4 fxmCol = (ui_mode == UI_MODE_EFFECTS && fx_mode == FX_MODE_MASTER) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, fxmCol);
            if (ImGui::Button("FXM", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_EFFECTS;
                fx_mode = FX_MODE_MASTER;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Switch to Program effects settings
            ImVec4 fxpCol = (ui_mode == UI_MODE_EFFECTS && fx_mode == FX_MODE_PROGRAM) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, fxpCol);
            if (ImGui::Button("FXP", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_EFFECTS;
                fx_mode = FX_MODE_PROGRAM;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Switch to mixer panel
            ImVec4 mixCol = (ui_mode == UI_MODE_MIX) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, mixCol);
            if (ImGui::Button("MIX", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_MIX;
            }
            ImGui::PopStyleColor();

            // ROW 1: Performance/playback related
            // Sample crate (file) settings - high-level program list
            ImVec4 crateCol = (ui_mode == UI_MODE_INSTRUMENT) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, crateCol);
            if (ImGui::Button("CRATE", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_INSTRUMENT;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Program editor - detailed single program view
            ImVec4 progCol = (ui_mode == UI_MODE_PROGRAM) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, progCol);
            if (ImGui::Button("PROG", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_PROGRAM;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Note play buttons
            ImVec4 padsCol = (ui_mode == UI_MODE_PADS) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, padsCol);
            if (ImGui::Button("PADS", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_PADS;
            }
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 8.0f));

            // ROW 2: View modes
            // Track view button
            ImVec4 trackCol = (ui_mode == UI_MODE_TRACK) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, trackCol);
            if (ImGui::Button("TRACK", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_TRACK;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Sequences view button
            ImVec4 seqCol = (ui_mode == UI_MODE_SEQUENCES) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, seqCol);
            if (ImGui::Button("SEQ", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_SEQUENCES;
            }
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 8.0f));

            // ROW 3: Configuration/settings
            // LEARN button
            ImVec4 learnCol = learn_mode_active ? COLOR_LEARN_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, learnCol);
            if (ImGui::Button("LEARN", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                learn_mode_active = !learn_mode_active;
                if (!learn_mode_active) {
                    learn_target_action = ACTION_NONE;
                    learn_target_parameter = 0;
                }
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // MIDI setup
            ImVec4 midiCol = (ui_mode == UI_MODE_MIDI) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, midiCol);
            if (ImGui::Button("MIDI", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_MIDI;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Application  settings
            ImVec4 settingsCol = (ui_mode == UI_MODE_SETTINGS) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
            ImGui::PushStyleColor(ImGuiCol_Button, settingsCol);
            if (ImGui::Button("SETUP", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_SETTINGS;
            }
            ImGui::PopStyleColor();

            }
            ImGui::EndChild();
        }

        // RIGHT PANEL - Switch based on mode
        // In fullscreen pads mode, hide channel panel and use full screen for pads
        float rightX = fullscreen_pads_mode ? 0.0f : (SIDE_MARGIN + LEFT_PANEL_WIDTH + SIDE_MARGIN);
        float rightW = fullscreen_pads_mode ? fullW : (fullW - rightX - SIDE_MARGIN);

        // Calculate dynamic slider width and spacing based on available width (like Regroove)
        const float BASE_SLIDER_W = 44.0f;
        const float BASE_SPACING = 26.0f;
        const float MIN_SLIDER_HEIGHT = 140.0f;
        const float IMGUI_LAYOUT_COMPENSATION = SEQUENCER_HEIGHT / 2;
        float baseTotal = BASE_SLIDER_W * 9.0f + BASE_SPACING * 8.0f;  // Assumes 9 channels worth of space
        float widthScale = rightW / baseTotal;
        if (widthScale > 1.40f) widthScale = 1.40f;  // Cap maximum scale
        if (widthScale < 1.0f) widthScale = 1.0f;    // Don't scale smaller than base
        float sliderW = BASE_SLIDER_W * widthScale;
        float spacing = BASE_SPACING * widthScale;

        // In fullscreen pads mode, use entire window height for pads (account for sequencer at bottom)
        float actualChannelAreaHeight = fullscreen_pads_mode ?
            (fullH - TOP_MARGIN - GAP_ABOVE_SEQUENCER - SEQUENCER_HEIGHT - BOTTOM_MARGIN - childPaddingY - childBorderY) :
            channelAreaHeight;

        // Use actualChannelAreaHeight for right panel
        ImGui::SetCursorPos(ImVec2(rightX, TOP_MARGIN));
        bool show_border = !fullscreen_pads_mode;  // Remove border in fullscreen mode so bar is flush against edge
        ImGui::BeginChild("right_panel", ImVec2(rightW, actualChannelAreaHeight), show_border, ImGuiWindowFlags_NoScrollbar);
        {
            ImVec2 origin = ImGui::GetCursorPos();

            float labelH = ImGui::GetTextLineHeight();
            float contentHeight = actualChannelAreaHeight - childPaddingY;
            float panSliderH = 20.0f;  // Height for horizontal pan slider
            const float SOLO_SIZE = 34.0f;
            const float MUTE_SIZE = 34.0f;

            // Calculate slider height using Regroove's formula
            // Above: initial offset (8) + title (labelH) + spacing (4) + solo/fx button (SOLO_SIZE) + spacing (2) + pan slider (panSliderH) + spacing (2)
            // Note: The original Regroove formula has "+ labelH" after panSliderH for VOL mode, but we remove it for tighter layout
            float sliderTop = 8.0f + labelH + 4.0f + SOLO_SIZE + 2.0f + panSliderH + 2.0f;
            // Below: spacing (8) + mute button (MUTE_SIZE) + bottom margin (12)
            float bottomStack = 8.0f + MUTE_SIZE + 12.0f;
            // Available height for slider - use Regroove's exact formula
            float sliderH = contentHeight - sliderTop - bottomStack - IMGUI_LAYOUT_COMPENSATION;
            if (sliderH < MIN_SLIDER_HEIGHT) sliderH = MIN_SLIDER_HEIGHT;

            if (ui_mode == UI_MODE_INSTRUMENT) {
                ImGui::Text("CRATE CONFIGURATION");
                ImGui::Separator();
                ImGui::Spacing();

                if (!rsx) {
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "No .rsx file loaded");
                    ImGui::Text("Create or load a .rsx file to configure note pads");
                } else {
                    // Program management section
                    ImGui::Text("PROGRAMS:");
                    ImGui::Spacing();

                    // Show current programs
                    for (int i = 0; i < rsx->num_programs; i++) {
                        ImGui::PushID(i);

                        ImGui::Text("Program %d:", i + 1);

                        // Name input with label
                        ImGui::Text("Name:");
                        ImGui::SameLine(80);
                        char name_label[32];
                        snprintf(name_label, sizeof(name_label), "##prog%d_name", i);
                        ImGui::PushItemWidth(300);
                        if (ImGui::InputText(name_label, rsx->program_names[i], sizeof(rsx->program_names[i]))) {
                            // Autosave on change
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                        ImGui::PopItemWidth();

                        // Mode indicator
                        ImGui::Text("Mode:");
                        ImGui::SameLine(80);
                        if (rsx->program_modes[i] == PROGRAM_MODE_SAMPLES) {
                            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Samples");
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "SFZ File");
                        }

                        // SFZ file input (only show for SFZ mode)
                        if (rsx->program_modes[i] == PROGRAM_MODE_SFZ_FILE) {
                            ImGui::Text("SFZ File:");
                            ImGui::SameLine(80);
                            char file_label[32];
                            snprintf(file_label, sizeof(file_label), "##prog%d_file", i);
                            ImGui::PushItemWidth(300);

                            if (ImGui::InputText(file_label, rsx->program_files[i], sizeof(rsx->program_files[i]))) {
                                // Autosave on change
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }

                                // Auto-reload the program
                                samplecrate_engine_reload_program(engine, i);
                            }
                            ImGui::PopItemWidth();
                        }

                        // Sample list (only for Samples mode)
                        if (rsx->program_modes[i] == PROGRAM_MODE_SAMPLES) {
                            ImGui::Text("Samples: %d/%d", rsx->program_sample_counts[i], RSX_MAX_SAMPLES_PER_PROGRAM);

                            if (ImGui::Button("Add Sample##add_sample")) {
                                if (rsx->program_sample_counts[i] < RSX_MAX_SAMPLES_PER_PROGRAM) {
                                    int idx = rsx->program_sample_counts[i];
                                    RSXSampleMapping* sample = &rsx->program_samples[i][idx];

                                    // Initialize defaults
                                    sample->sample_path[0] = '\0';
                                    sample->key_low = 60;   // Middle C
                                    sample->key_high = 60;
                                    sample->root_key = 60;
                                    sample->vel_low = 0;
                                    sample->vel_high = 127;
                                    sample->amplitude = 1.0f;
                                    sample->pan = 0.0f;
                                    sample->enabled = 1;

                                    rsx->program_sample_counts[i]++;

                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }

                                    // Don't auto-reload - wait for user to set the sample path first
                                }
                            }

                            // Show sample list
                            for (int s = 0; s < rsx->program_sample_counts[i]; s++) {
                                RSXSampleMapping* sample = &rsx->program_samples[i][s];
                                ImGui::PushID(s);

                                ImGui::Separator();
                                ImGui::Text("Sample %d:", s + 1);

                                char path_label[32];
                                snprintf(path_label, sizeof(path_label), "Path##sample_%d", s);
                                ImGui::PushItemWidth(350);
                                ImGui::InputText(path_label, sample->sample_path, sizeof(sample->sample_path));
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after editing finished
                                }
                                ImGui::PopItemWidth();

                                int note_low = sample->key_low;
                                ImGui::SliderInt("Note Low", &note_low, 0, 127);
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    std::cout << "Note Low changed from " << sample->key_low << " to " << note_low << std::endl;
                                    sample->key_low = note_low;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after slider released
                                } else {
                                    sample->key_low = note_low;  // Update value while dragging
                                }

                                int note_high = sample->key_high;
                                ImGui::SliderInt("Note High", &note_high, 0, 127);
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    std::cout << "Note High changed from " << sample->key_high << " to " << note_high << std::endl;
                                    sample->key_high = note_high;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after slider released
                                } else {
                                    sample->key_high = note_high;  // Update value while dragging
                                }

                                int root_key = sample->root_key;
                                ImGui::SliderInt("Root Key", &root_key, 0, 127);
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    std::cout << "Root Key changed from " << sample->root_key << " to " << root_key << std::endl;
                                    sample->root_key = root_key;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after slider released
                                } else {
                                    sample->root_key = root_key;  // Update value while dragging
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("=Lo")) {
                                    sample->root_key = sample->key_low;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);
                                }
                                ImGui::SameLine();
                                ImGui::Text("(pitch center)");

                                ImGui::SliderFloat("Volume", &sample->amplitude, 0.0f, 1.0f);
                                if (ImGui::IsItemDeactivatedAfterEdit()) {
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after slider released
                                }

                                if (ImGui::Button("Remove")) {
                                    // Shift samples down
                                    for (int j = s; j < rsx->program_sample_counts[i] - 1; j++) {
                                        rsx->program_samples[i][j] = rsx->program_samples[i][j + 1];
                                    }
                                    rsx->program_sample_counts[i]--;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, i);  // Auto-reload after removing sample
                                }

                                ImGui::PopID();
                            }
                        }

                        // Volume slider with label
                        ImGui::Text("Volume:");
                        ImGui::SameLine(80);
                        char vol_label[32];
                        snprintf(vol_label, sizeof(vol_label), "##prog%d_vol", i);
                        ImGui::PushItemWidth(200);
                        if (ImGui::SliderFloat(vol_label, &rsx->program_volumes[i], 0.0f, 1.0f, "%.2f")) {
                            // Update mixer in real-time
                            mixer.program_volumes[i] = rsx->program_volumes[i];
                            // Autosave on change
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                        ImGui::PopItemWidth();

                        // Pan slider with label
                        ImGui::Text("Pan:");
                        ImGui::SameLine(80);
                        char pan_label[32];
                        snprintf(pan_label, sizeof(pan_label), "##prog%d_pan", i);
                        ImGui::PushItemWidth(200);
                        if (ImGui::SliderFloat(pan_label, &rsx->program_pans[i], 0.0f, 1.0f, "%.2f")) {
                            // Update mixer in real-time
                            mixer.program_pans[i] = rsx->program_pans[i];
                            // Autosave on change
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                        ImGui::PopItemWidth();

                        // Remove button
                        if (ImGui::Button("Remove Program")) {
                            // Shift programs down
                            for (int j = i; j < rsx->num_programs - 1; j++) {
                                strcpy(rsx->program_files[j], rsx->program_files[j + 1]);
                                strcpy(rsx->program_names[j], rsx->program_names[j + 1]);
                                rsx->program_volumes[j] = rsx->program_volumes[j + 1];
                                rsx->program_pans[j] = rsx->program_pans[j + 1];
                            }
                            // Clear last program
                            rsx->program_files[rsx->num_programs - 1][0] = '\0';
                            rsx->program_names[rsx->num_programs - 1][0] = '\0';
                            rsx->program_volumes[rsx->num_programs - 1] = 1.0f;
                            rsx->program_pans[rsx->num_programs - 1] = 0.5f;
                            rsx->num_programs--;

                            // Autosave
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }

                        ImGui::Spacing();
                        ImGui::PopID();
                    }

                    // Add program buttons - separate SFZ and Samples modes
                    if (rsx->num_programs < RSX_MAX_PROGRAMS) {
                        if (ImGui::Button("Add Program from SFZ", ImVec2(200, 0))) {
                            // Add a new SFZ program
                            rsx->program_modes[rsx->num_programs] = PROGRAM_MODE_SFZ_FILE;
                            rsx->program_files[rsx->num_programs][0] = '\0';
                            rsx->program_names[rsx->num_programs][0] = '\0';
                            rsx->program_volumes[rsx->num_programs] = 1.0f;  // Default volume (100%)
                            rsx->program_pans[rsx->num_programs] = 0.5f;     // Center pan
                            rsx->program_sample_counts[rsx->num_programs] = 0;   // No samples initially
                            rsx->num_programs++;

                            // Autosave
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Add Program from Samples", ImVec2(200, 0))) {
                            // Add a new Samples program
                            rsx->program_modes[rsx->num_programs] = PROGRAM_MODE_SAMPLES;
                            rsx->program_files[rsx->num_programs][0] = '\0';
                            rsx->program_names[rsx->num_programs][0] = '\0';
                            rsx->program_volumes[rsx->num_programs] = 1.0f;  // Default volume (100%)
                            rsx->program_pans[rsx->num_programs] = 0.5f;     // Center pan
                            rsx->program_sample_counts[rsx->num_programs] = 0;   // No samples initially
                            rsx->num_programs++;

                            // Autosave
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Maximum %d programs", RSX_MAX_PROGRAMS);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Pad configuration section
                    ImGui::Text("NOTE PADS:");
                    ImGui::Spacing();
                    // Pad selector
                    static int selected_pad = 0;
                    ImGui::Text("Select Pad:");
                    ImGui::Spacing();

                    // Always show all 32 pads in 8x4 layout for configuration
                    // Layout: N1-N16 on left, N17-N32 on right
                    for (int row = 0; row < 4; row++) {
                        for (int col = 0; col < 8; col++) {
                            int pad_idx;
                            // 8x4 layout: left 4 columns = N1-N16, right 4 columns = N17-N32
                            if (col < 4) {
                                pad_idx = row * 4 + col;
                            } else {
                                pad_idx = 16 + row * 4 + (col - 4);
                            }

                            if (col > 0) ImGui::SameLine();

                            bool is_selected = (selected_pad == pad_idx);
                            bool is_configured = (rsx->pads[pad_idx].enabled &&
                                                 (rsx->pads[pad_idx].note >= 0 || rsx->pads[pad_idx].midi_file[0] != '\0'));

                            ImVec4 btn_col = is_selected ? COLOR_BUTTON_ACTIVE :
                                            (is_configured ? COLOR_BUTTON_INACTIVE :
                                                            COLOR_BUTTON_AT_LIMIT);

                            ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                            char btn_label[8];
                            snprintf(btn_label, sizeof(btn_label), "N%d", pad_idx + 1);
                            if (ImGui::Button(btn_label, ImVec2(40, 40))) {
                                selected_pad = pad_idx;
                            }
                            ImGui::PopStyleColor();
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Configure selected pad
                    NoteTriggerPad* pad = &rsx->pads[selected_pad];

                    ImGui::Text("Configure Pad N%d:", selected_pad + 1);
                    ImGui::Spacing();

                    // Track if any changes were made for autosave
                    bool rsx_changed = false;

                    // Note number
                    int note_num = pad->note < 0 ? 60 : pad->note;
                    if (ImGui::SliderInt("MIDI Note", &note_num, 0, 127)) {
                        pad->note = note_num;
                        if (selected_pad >= rsx->num_pads) {
                            rsx->num_pads = selected_pad + 1;
                        }
                        rsx_changed = true;
                    }

                    // Description
                    if (ImGui::InputText("Description", pad->description, sizeof(pad->description))) {
                        rsx_changed = true;
                    }

                    // Velocity
                    int vel = pad->velocity;
                    if (ImGui::SliderInt("Velocity (0=default)", &vel, 0, 127)) {
                        pad->velocity = vel;
                        rsx_changed = true;
                    }

                    // Enabled
                    bool enabled = pad->enabled;
                    if (ImGui::Checkbox("Enabled", &enabled)) {
                        pad->enabled = enabled;
                        rsx_changed = true;
                    }

                    // Program selection (only show if RSX has multiple programs)
                    if (rsx->num_programs > 1) {
                        ImGui::Spacing();

                        // Build program items with names
                        char program_labels[5][128];
                        const char* program_items[5];

                        snprintf(program_labels[0], sizeof(program_labels[0]), "No program associated");
                        program_items[0] = program_labels[0];

                        for (int i = 0; i < rsx->num_programs; i++) {
                            if (rsx->program_names[i][0] != '\0') {
                                snprintf(program_labels[i + 1], sizeof(program_labels[i + 1]), "Program %d: %s", i + 1, rsx->program_names[i]);
                            } else {
                                snprintf(program_labels[i + 1], sizeof(program_labels[i + 1]), "Program %d", i + 1);
                            }
                            program_items[i + 1] = program_labels[i + 1];
                        }

                        int program_count = rsx->num_programs + 1;  // +1 for "No program associated"
                        int current_item = pad->program + 1;  // -1 becomes 0 (No program), 0 becomes 1 (Prog 1), etc.

                        if (ImGui::Combo("Program", &current_item, program_items, program_count)) {
                            pad->program = current_item - 1;  // 0 becomes -1 (No program), 1 becomes 0 (Prog 1), etc.
                            rsx_changed = true;
                        }
                    }

                    // MIDI File path (optional - for playing MIDI files instead of single notes)
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::Text("MIDI File Playback (optional):");
                    ImGui::TextWrapped("Leave empty to trigger a single note. Select a MIDI file to play when pad is triggered.");

                    // Build list of available MIDI files
                    static std::vector<std::string> pad_midi_files;
                    static bool pad_midi_files_loaded = false;
                    if (!pad_midi_files_loaded && !rsx_file_path.empty()) {
                        pad_midi_files.clear();
                        pad_midi_files.push_back("(none)");  // Option to clear

                        if (file_list) {
                            // Use file_list if available
                            for (int f = 0; f < file_list->count; f++) {
                                const char* name = file_list->filenames[f];
                                size_t len = strlen(name);
                                if (len > 4 && (strcmp(name + len - 4, ".mid") == 0 || strcmp(name + len - 4, ".MID") == 0)) {
                                    pad_midi_files.push_back(std::string(name));
                                }
                            }
                        } else {
                            // Scan RSX directory for MIDI files
                            char rsx_dir[512];
                            strncpy(rsx_dir, rsx_file_path.c_str(), sizeof(rsx_dir) - 1);
                            rsx_dir[sizeof(rsx_dir) - 1] = '\0';
                            char* dir_path = dirname(rsx_dir);

                            DIR* dir = opendir(dir_path);
                            if (dir) {
                                struct dirent* entry;
                                while ((entry = readdir(dir)) != nullptr) {
                                    size_t len = strlen(entry->d_name);
                                    if (len > 4 && (strcmp(entry->d_name + len - 4, ".mid") == 0 ||
                                                   strcmp(entry->d_name + len - 4, ".MID") == 0)) {
                                        pad_midi_files.push_back(std::string(entry->d_name));
                                    }
                                }
                                closedir(dir);
                            }
                        }
                        std::sort(pad_midi_files.begin() + 1, pad_midi_files.end());  // Sort all except "(none)"
                        pad_midi_files_loaded = true;
                    }

                    // MIDI file dropdown
                    const char* current_display = (pad->midi_file[0] == '\0') ? "(none)" : pad->midi_file;
                    if (ImGui::BeginCombo("MIDI File", current_display)) {
                        for (size_t idx = 0; idx < pad_midi_files.size(); idx++) {
                            bool is_selected = (strcmp(pad_midi_files[idx].c_str(), current_display) == 0);
                            if (ImGui::Selectable(pad_midi_files[idx].c_str(), is_selected)) {
                                // Selection changed
                                if (idx == 0) {
                                    // "(none)" selected - clear MIDI file
                                    pad->midi_file[0] = '\0';
                                } else {
                                    strncpy(pad->midi_file, pad_midi_files[idx].c_str(), sizeof(pad->midi_file) - 1);
                                    pad->midi_file[sizeof(pad->midi_file) - 1] = '\0';

                                    // Enable the pad when MIDI file is set
                                    if (!pad->enabled) {
                                        pad->enabled = 1;
                                        std::cout << "Auto-enabled pad " << (selected_pad + 1) << " for MIDI file playback" << std::endl;
                                    }

                                    // Reload pads via engine (handles routing and visual feedback)
                                    samplecrate_engine_load_pads(engine, pad_visual_feedback);
                                }
                                rsx_changed = true;
                            }
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // Refresh button to rescan MIDI files
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Refresh")) {
                        pad_midi_files_loaded = false;  // Force rescan on next frame
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) {
                        pad->midi_file[0] = '\0';
                        rsx_changed = true;

                        // Unload MIDI file
                        if (performance) {
                            medness_performance_unload_pad(performance, selected_pad);
                        }
                    }

                    // Show status if MIDI file is set
                    if (pad->midi_file[0] != '\0') {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "MIDI file configured: %s", pad->midi_file);
                    } else {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Single note mode (no MIDI file)");
                    }

                    // Autosave RSX if any changes were made
                    if (rsx_changed && !rsx_file_path.empty()) {
                        if (samplecrate_rsx_save(rsx, rsx_file_path.c_str()) == 0) {
                            // Saved successfully (silent)
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Test pad
                    if (ImGui::Button("Test Pad", ImVec2(200, 0))) {
                        if (pad->note >= 0) {
                            int velocity = pad->velocity > 0 ? pad->velocity : 100;

                            // Determine which synth to use based on pad's program setting
                            sfizz_synth_t* target_synth = synth;  // Default to current synth
                            if (pad->program >= 0 && pad->program < rsx->num_programs && program_synths[pad->program]) {
                                target_synth = program_synths[pad->program];
                            }

                            std::lock_guard<std::mutex> lock(synth_mutex);
                            if (target_synth) {
                                // For test button, just send note_on
                                // The SFZ file's envelope/release settings will control the sound
                                sfizz_send_note_on(target_synth, 0, pad->note, velocity);

                                current_note = pad->note;
                                current_velocity = velocity;
                                note_pad_fade[selected_pad] = 1.0f;
                            }
                        }
                    }

                    // Show autosave status
                    if (!rsx_file_path.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "[Autosave]");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Note Suppression section
                    ImGui::Text("NOTE SUPPRESSION:");
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Prevent specific MIDI notes from playing");
                    ImGui::Spacing();

                    // Track if any changes were made for autosave
                    bool suppression_changed = false;

                    // Global suppression section
                    ImGui::Text("Global Suppression (all programs):");
                    ImGui::Spacing();

                    // Display global suppression as a compact grid
                    const int NOTES_PER_ROW = 12;  // One octave per row
                    const int NUM_OCTAVES = 11;    // 128 notes = ~11 octaves (0-10 plus 8 notes)

                    for (int octave = 0; octave < NUM_OCTAVES; octave++) {
                        ImGui::Text("Oct %d:", octave);
                        ImGui::SameLine(60);

                        for (int note_in_octave = 0; note_in_octave < NOTES_PER_ROW; note_in_octave++) {
                            int note = octave * NOTES_PER_ROW + note_in_octave;
                            if (note >= 128) break;

                            if (note_in_octave > 0) ImGui::SameLine();

                            // Note names for reference
                            static const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                            const char* note_name = note_names[note_in_octave];

                            char btn_label[16];
                            snprintf(btn_label, sizeof(btn_label), "%s##glob_%d", note_name, note);

                            // Color: Red if suppressed, gray if not
                            ImVec4 btn_col = note_suppressed[note][0] ?
                                ImVec4(0.80f, 0.20f, 0.20f, 1.0f) :
                                COLOR_BUTTON_INACTIVE;

                            ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                            if (ImGui::Button(btn_label, ImVec2(32, 24))) {
                                note_suppressed[note][0] = !note_suppressed[note][0];
                                suppression_changed = true;
                            }
                            ImGui::PopStyleColor();

                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Note %d (%s%d): %s", note, note_name, octave,
                                    note_suppressed[note][0] ? "SUPPRESSED" : "Enabled");
                            }
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Per-program suppression (only show if multiple programs)
                    if (rsx->num_programs > 1) {
                        ImGui::Text("Per-Program Suppression:");
                        ImGui::Spacing();

                        // Program selector for suppression view
                        static int supp_program_view = 0;
                        if (supp_program_view >= rsx->num_programs) supp_program_view = 0;

                        ImGui::Text("View program:");
                        ImGui::SameLine();
                        for (int i = 0; i < rsx->num_programs; i++) {
                            if (i > 0) ImGui::SameLine();

                            char prog_btn[32];
                            if (rsx->program_names[i][0] != '\0') {
                                snprintf(prog_btn, sizeof(prog_btn), "%s##supp_prog_%d", rsx->program_names[i], i);
                            } else {
                                snprintf(prog_btn, sizeof(prog_btn), "Prog %d##supp_prog_%d", i + 1, i);
                            }

                            ImVec4 prog_col = (supp_program_view == i) ?
                                COLOR_BUTTON_ACTIVE :
                                COLOR_BUTTON_INACTIVE;

                            ImGui::PushStyleColor(ImGuiCol_Button, prog_col);
                            if (ImGui::Button(prog_btn, ImVec2(80, 30))) {
                                supp_program_view = i;
                            }
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();

                        // Display per-program suppression grid
                        for (int octave = 0; octave < NUM_OCTAVES; octave++) {
                            ImGui::Text("Oct %d:", octave);
                            ImGui::SameLine(60);

                            for (int note_in_octave = 0; note_in_octave < NOTES_PER_ROW; note_in_octave++) {
                                int note = octave * NOTES_PER_ROW + note_in_octave;
                                if (note >= 128) break;

                                if (note_in_octave > 0) ImGui::SameLine();

                                static const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                                const char* note_name = note_names[note_in_octave];

                                char btn_label[16];
                                snprintf(btn_label, sizeof(btn_label), "%s##p%d_%d", note_name, supp_program_view, note);

                                // Color: Red if suppressed, gray if not
                                ImVec4 btn_col = note_suppressed[note][supp_program_view + 1] ?
                                    ImVec4(0.80f, 0.20f, 0.20f, 1.0f) :
                                    COLOR_BUTTON_INACTIVE;

                                ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                                if (ImGui::Button(btn_label, ImVec2(32, 24))) {
                                    note_suppressed[note][supp_program_view + 1] = !note_suppressed[note][supp_program_view + 1];
                                    suppression_changed = true;
                                }
                                ImGui::PopStyleColor();

                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("Note %d (%s%d) for program %d: %s", note, note_name, octave,
                                        supp_program_view + 1,
                                        note_suppressed[note][supp_program_view + 1] ? "SUPPRESSED" : "Enabled");
                                }
                            }
                        }
                    }

                    // Auto-save if suppression changed
                    if (suppression_changed) {
                        save_note_suppression_to_rsx();
                    }
                }
            }
            else if (ui_mode == UI_MODE_PROGRAM) {
                // PROG MODE: Detailed single program editor
                ImGui::Text("PROGRAM CONFIGURATION");
                ImGui::Separator();
                ImGui::Spacing();

                if (!rsx) {
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "No .rsx file loaded");
                    ImGui::Text("Load a .rsx file to configure programs");
                } else {
                    // PROG panel follows current_program by default
                    // Use current_program directly instead of selected_program_for_edit
                    int prog_idx = current_program;

                    // Program selector dropdown - shows current program, allows quick switching
                    ImGui::Text("Program:");
                    ImGui::SameLine(80);
                    ImGui::PushItemWidth(300);

                    // Build list of programs for dropdown (only show loaded programs)
                    std::vector<std::string> program_names;
                    std::vector<int> program_indices;
                    for (int i = 0; i < rsx->num_programs; i++) {
                        char label[256];
                        if (strlen(rsx->program_names[i]) > 0) {
                            snprintf(label, sizeof(label), "%d: %s", i + 1, rsx->program_names[i]);
                        } else {
                            snprintf(label, sizeof(label), "%d: Program %d", i + 1, i + 1);
                        }
                        program_names.push_back(label);
                        program_indices.push_back(i);
                    }

                    // Find current program in the list
                    int current_selection = -1;
                    for (size_t i = 0; i < program_indices.size(); i++) {
                        if (program_indices[i] == current_program) {
                            current_selection = i;
                            break;
                        }
                    }
                    if (current_selection == -1 && !program_indices.empty()) {
                        // Current program not in list, shouldn't happen but default to first
                        current_selection = 0;
                    }

                    if (!program_indices.empty() && current_selection >= 0) {
                        if (ImGui::BeginCombo("##prog_selector", program_names[current_selection].c_str())) {
                            for (size_t i = 0; i < program_names.size(); i++) {
                                bool is_selected = (current_selection == (int)i);
                                if (ImGui::Selectable(program_names[i].c_str(), is_selected)) {
                                    // Switch to selected program (updates current_program)
                                    current_program = program_indices[i];
                                    // Also switch the active synth
                                    if (current_program >= 0 && current_program < RSX_MAX_PROGRAMS) {
                                        synth = program_synths[current_program];
                                    }
                                }
                                if (is_selected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::PopItemWidth();

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Show program details for current_program
                        if (prog_idx >= 0 && prog_idx < rsx->num_programs) {

                            // GENERAL section
                            ImGui::Text("GENERAL:");
                            ImGui::Spacing();

                            // Name
                            ImGui::Text("Name:");
                            ImGui::SameLine(120);
                            ImGui::PushItemWidth(300);
                            if (ImGui::InputText("##prog_name", rsx->program_names[prog_idx], sizeof(rsx->program_names[prog_idx]))) {
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                            ImGui::PopItemWidth();

                            // Mode indicator
                            ImGui::Text("Mode:");
                            ImGui::SameLine(120);
                            if (rsx->program_modes[prog_idx] == PROGRAM_MODE_SAMPLES) {
                                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Samples");
                            } else {
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "SFZ File");
                            }

                            // Volume
                            ImGui::Text("Volume:");
                            ImGui::SameLine(120);
                            ImGui::PushItemWidth(200);
                            if (ImGui::SliderFloat("##prog_volume", &rsx->program_volumes[prog_idx], 0.0f, 1.0f, "%.2f")) {
                                mixer.program_volumes[prog_idx] = rsx->program_volumes[prog_idx];
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                            ImGui::PopItemWidth();

                            // Pan
                            ImGui::Text("Pan:");
                            ImGui::SameLine(120);
                            ImGui::PushItemWidth(200);
                            if (ImGui::SliderFloat("##prog_pan", &rsx->program_pans[prog_idx], 0.0f, 1.0f, "%.2f")) {
                                mixer.program_pans[prog_idx] = rsx->program_pans[prog_idx];
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                            ImGui::PopItemWidth();

                            // Mute
                            ImGui::Text("Mute:");
                            ImGui::SameLine(120);
                            bool is_muted = (mixer.program_mutes[prog_idx] != 0);
                            if (ImGui::Checkbox("##prog_mute", &is_muted)) {
                                mixer.program_mutes[prog_idx] = is_muted ? 1 : 0;
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // SOURCE section
                            ImGui::Text("SOURCE:");
                            ImGui::Spacing();

                            if (rsx->program_modes[prog_idx] == PROGRAM_MODE_SFZ_FILE) {
                                // SFZ File mode
                                ImGui::Text("SFZ File:");
                                ImGui::SameLine(120);
                                ImGui::PushItemWidth(400);
                                if (ImGui::InputText("##prog_sfz_file", rsx->program_files[prog_idx], sizeof(rsx->program_files[prog_idx]))) {
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    samplecrate_engine_reload_program(engine, prog_idx);
                                }
                                ImGui::PopItemWidth();

                                // TODO: Add region count display (query from sfizz)
                                ImGui::Text("Regions:");
                                ImGui::SameLine(120);
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(query sfizz)");

                                if (ImGui::Button("Reload SFZ")) {
                                    samplecrate_engine_reload_program(engine, prog_idx);
                                }
                            } else {
                                // Samples mode
                                ImGui::Text("Samples:");
                                ImGui::SameLine(120);
                                ImGui::Text("%d / %d", rsx->program_sample_counts[prog_idx], RSX_MAX_SAMPLES_PER_PROGRAM);

                                ImGui::Text("(Sample editor - see CRATE panel)");
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // NOTE SUPPRESSION section (per-program)
                            ImGui::Text("NOTE SUPPRESSION (this program only):");
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click note to toggle suppression");
                            ImGui::Spacing();

                            bool prog_suppression_changed = false;

                            // Show notes in a compact grid (12 notes per row × 11 octaves = 132 notes)
                            const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

                            for (int octave = 0; octave < 11; octave++) {
                                ImGui::Text("Oct %d:", octave);
                                ImGui::SameLine(60);

                                for (int n = 0; n < 12; n++) {
                                    int note = octave * 12 + n;
                                    if (note >= 128) break;

                                    char btn_label[16];
                                    snprintf(btn_label, sizeof(btn_label), "%s##p%d_n%d", note_names[n], prog_idx, note);

                                    // Color: Red if suppressed, gray if not
                                    ImVec4 btn_col = note_suppressed[note][prog_idx + 1] ?
                                        ImVec4(0.80f, 0.20f, 0.20f, 1.0f) :
                                        COLOR_BUTTON_INACTIVE;

                                    ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                                    if (ImGui::Button(btn_label, ImVec2(28, 22))) {
                                        note_suppressed[note][prog_idx + 1] = !note_suppressed[note][prog_idx + 1];
                                        prog_suppression_changed = true;
                                    }
                                    ImGui::PopStyleColor();

                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("Note %d (%s%d): %s", note, note_names[n], octave,
                                            note_suppressed[note][prog_idx + 1] ? "SUPPRESSED" : "Enabled");
                                    }

                                    if (n < 11) ImGui::SameLine();
                                }
                            }

                            if (prog_suppression_changed) {
                                save_note_suppression_to_rsx();
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // EFFECTS section
                            ImGui::Text("EFFECTS:");
                            ImGui::Spacing();
                            ImGui::Text("FX Chain Enable:");
                            ImGui::SameLine(120);
                            bool fx_enabled = (mixer.program_fx_enable[prog_idx] != 0);
                            if (ImGui::Checkbox("##prog_fx_enable", &fx_enabled)) {
                                mixer.program_fx_enable[prog_idx] = fx_enabled ? 1 : 0;
                            }

                            if (ImGui::Button("Open Effects Editor (FXP)")) {
                                ui_mode = UI_MODE_EFFECTS;
                                fx_mode = FX_MODE_PROGRAM;
                                current_program = prog_idx;  // Switch to this program in FXP view
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // ACTIONS
                            if (ImGui::Button("Reload Program")) {
                                samplecrate_engine_reload_program(engine, prog_idx);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Delete Program")) {
                                // TODO: Implement program deletion
                                ImGui::OpenPopup("Delete Program?");
                            }

                            if (ImGui::BeginPopupModal("Delete Program?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("Are you sure you want to delete this program?");
                                ImGui::Spacing();
                                if (ImGui::Button("Yes, Delete", ImVec2(120, 0))) {
                                    // Delete program (shift down)
                                    for (int j = prog_idx; j < rsx->num_programs - 1; j++) {
                                        strcpy(rsx->program_files[j], rsx->program_files[j + 1]);
                                        strcpy(rsx->program_names[j], rsx->program_names[j + 1]);
                                        rsx->program_volumes[j] = rsx->program_volumes[j + 1];
                                        rsx->program_pans[j] = rsx->program_pans[j + 1];
                                    }
                                    rsx->num_programs--;
                                    if (!rsx_file_path.empty()) {
                                        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                    }
                                    // Adjust current_program if it was deleted or is now out of range
                                    if (current_program >= rsx->num_programs && rsx->num_programs > 0) {
                                        current_program = rsx->num_programs - 1;
                                        synth = program_synths[current_program];
                                    }
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "No programs loaded");
                        ImGui::Text("Add programs in CRATE panel");
                    }
                }
            }
            else if (ui_mode == UI_MODE_MIX) {
                // MIX MODE: Master and playback mixing
                // No title/separator in MIX mode (like Regroove) - sliderH calculation assumes we start from origin
                ImVec2 origin = ImGui::GetCursorScreenPos();

                int col_index = 0;

                // TEMPO slider (for MIDI file playback) - First column
                {
                    float colX = origin.x + col_index * (sliderW + spacing);
                    ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 8.0f));
                    ImGui::BeginGroup();
                    ImGui::Text("TEMPO");
                    ImGui::Dummy(ImVec2(0, 4.0f));

                    // Dummy FX button placeholder to match layout
                    ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                    ImGui::Dummy(ImVec2(0, 2.0f));

                    // Dummy pan slider placeholder to match layout
                    ImGui::Dummy(ImVec2(sliderW, panSliderH));
                    ImGui::Dummy(ImVec2(0, 2.0f));

                    // Tempo slider (50 BPM to 200 BPM)
                    // Up = faster (200), Down = slower (50)
                    // Disable slider when external MIDI clock is active
                    bool is_external_clock = midi_clock.active && midi_clock.running;
                    if (is_external_clock) {
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);  // Dim the slider
                    }

                    float prev_tempo = tempo_bpm;
                    if (ImGui::VSliderFloat("##tempo", ImVec2(sliderW, sliderH), &tempo_bpm, 50.0f, 200.0f, "")) {
                        // Only allow changes when external clock is NOT active
                        if (!is_external_clock && prev_tempo != tempo_bpm) {
                            // Update performance manager tempo and active BPM
                            active_bpm = tempo_bpm;
                            if (sequencer) {
                                medness_sequencer_set_bpm(sequencer, active_bpm);
                            }
                            if (performance) {
                                medness_performance_set_tempo(performance, active_bpm);
                            }
                            if (sequence_manager) {
                                medness_performance_set_tempo(sequence_manager, active_bpm);
                            }
                        } else if (is_external_clock) {
                            // Revert to MIDI clock BPM if user tries to drag during external sync
                            tempo_bpm = midi_clock.bpm;
                        }
                    }

                    if (is_external_clock) {
                        ImGui::PopStyleVar();
                    }
                    ImGui::Dummy(ImVec2(0, 8.0f));

                    // Reset button (reset to default 125 BPM)
                    if (ImGui::Button("R##tempo_reset", ImVec2(sliderW, MUTE_SIZE))) {
                        tempo_bpm = 125.0f;
                        active_bpm = tempo_bpm;
                        if (sequencer) {
                            medness_sequencer_set_bpm(sequencer, active_bpm);
                        }
                        if (performance) {
                            medness_performance_set_tempo(performance, active_bpm);
                        }
                        if (sequence_manager) {
                            medness_performance_set_tempo(sequence_manager, active_bpm);
                        }
                    }

                    ImGui::EndGroup();
                    col_index++;
                }

                // MASTER channel
                {
                    float colX = origin.x + col_index * (sliderW + spacing);
                    ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 8.0f));
                    ImGui::BeginGroup();
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "MASTER");
                    ImGui::Dummy(ImVec2(0, 4.0f));

                    // Master FX enable toggle
                    ImVec4 fxCol = mixer.master_fx_enable ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                    ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
                    if (ImGui::Button("FX##master_fx", ImVec2(sliderW, SOLO_SIZE))) {
                        mixer.master_fx_enable = !mixer.master_fx_enable;
                        autosave_effects_to_rsx();
                    }
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2.0f));

                    // Pan slider
                    ImGui::PushItemWidth(sliderW);
                    if (ImGui::SliderFloat("##master_pan", &mixer.master_pan, 0.0f, 1.0f, "")) {
                        if (learn_mode_active && ImGui::IsItemActive()) {
                            start_learn_for_action(ACTION_MASTER_PAN);
                        }
                    }
                    ImGui::PopItemWidth();
                    ImGui::Dummy(ImVec2(0, 2.0f));

                    // Volume fader
                    if (ImGui::VSliderFloat("##master_vol", ImVec2(sliderW, sliderH), &mixer.master_volume, 0.0f, 1.0f, "")) {
                        if (learn_mode_active && ImGui::IsItemActive()) {
                            start_learn_for_action(ACTION_MASTER_VOLUME);
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 8.0f));

                    // Mute button
                    ImVec4 muteCol = mixer.master_mute ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : COLOR_BUTTON_INACTIVE;
                    ImGui::PushStyleColor(ImGuiCol_Button, muteCol);
                    if (ImGui::Button("M##master_mute", ImVec2(sliderW, MUTE_SIZE))) {
                        mixer.master_mute = !mixer.master_mute;
                    }
                    ImGui::PopStyleColor();

                    ImGui::EndGroup();
                    col_index++;
                }

                // PROGRAM channels (if multi-program RSX loaded)
                if (rsx && rsx->num_programs > 1) {
                    for (int i = 0; i < rsx->num_programs; i++) {
                        if (!program_synths[i]) continue;

                        float colX = origin.x + col_index * (sliderW + spacing);
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 8.0f));
                        ImGui::BeginGroup();

                        // Program name or number
                        if (rsx->program_names[i][0] != '\0') {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", rsx->program_names[i]);
                        } else {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "PRG %d", i + 1);
                        }
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Per-program FX enable toggle
                        char fx_id[32];
                        snprintf(fx_id, sizeof(fx_id), "FX##prog%d_fx", i);
                        ImVec4 fxCol = mixer.program_fx_enable[i] ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
                        if (ImGui::Button(fx_id, ImVec2(sliderW, SOLO_SIZE))) {
                            mixer.program_fx_enable[i] = !mixer.program_fx_enable[i];
                            autosave_effects_to_rsx();
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 2.0f));

                        // Pan slider
                        char pan_id[32];
                        snprintf(pan_id, sizeof(pan_id), "##prog%d_pan", i);
                        ImGui::PushItemWidth(sliderW);
                        if (ImGui::SliderFloat(pan_id, &mixer.program_pans[i], 0.0f, 1.0f, "")) {
                            // Sync to RSX and autosave
                            if (rsx) {
                                rsx->program_pans[i] = mixer.program_pans[i];
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                        }
                        ImGui::PopItemWidth();
                        ImGui::Dummy(ImVec2(0, 2.0f));

                        // Volume fader
                        char slider_id[32];
                        snprintf(slider_id, sizeof(slider_id), "##prog%d_vol", i);
                        if (ImGui::VSliderFloat(slider_id, ImVec2(sliderW, sliderH), &mixer.program_volumes[i], 0.0f, 1.0f, "")) {
                            // Sync to RSX and autosave
                            if (rsx) {
                                rsx->program_volumes[i] = mixer.program_volumes[i];
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));

                        // Mute button
                        char mute_id[32];
                        snprintf(mute_id, sizeof(mute_id), "M##prog%d_mute", i);
                        ImVec4 muteCol = mixer.program_mutes[i] ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, muteCol);
                        if (ImGui::Button(mute_id, ImVec2(sliderW, MUTE_SIZE))) {
                            mixer.program_mutes[i] = !mixer.program_mutes[i];
                        }
                        ImGui::PopStyleColor();

                        ImGui::EndGroup();
                        col_index++;
                    }
                } else {
                    // PLAYBACK channel (single synth mode)
                    float colX = origin.x + col_index * (sliderW + spacing);
                    ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 8.0f));
                    ImGui::BeginGroup();
                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "PLAYBACK");
                    ImGui::Dummy(ImVec2(0, 4.0f));

                    // No FX button for playback (use master FX instead)
                    ImGui::Dummy(ImVec2(0, SOLO_SIZE + 2.0f));

                    // Pan slider
                    ImGui::PushItemWidth(sliderW);
                    if (ImGui::SliderFloat("##playback_pan", &mixer.playback_pan, 0.0f, 1.0f, "")) {
                        if (learn_mode_active && ImGui::IsItemActive()) {
                            start_learn_for_action(ACTION_PLAYBACK_PAN);
                        }
                    }
                    ImGui::PopItemWidth();
                    ImGui::Dummy(ImVec2(0, 2.0f));

                    // Volume fader
                    if (ImGui::VSliderFloat("##playback_vol", ImVec2(sliderW, sliderH), &mixer.playback_volume, 0.0f, 1.0f, "")) {
                        if (learn_mode_active && ImGui::IsItemActive()) {
                            start_learn_for_action(ACTION_PLAYBACK_VOLUME);
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 8.0f));

                    // Mute button
                    ImVec4 muteCol = mixer.playback_mute ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : COLOR_BUTTON_INACTIVE;
                    ImGui::PushStyleColor(ImGuiCol_Button, muteCol);
                    if (ImGui::Button("M##playback_mute", ImVec2(sliderW, MUTE_SIZE))) {
                        mixer.playback_mute = !mixer.playback_mute;
                    }
                    ImGui::PopStyleColor();

                    ImGui::EndGroup();
                    col_index++;
                }
            }
            else if (ui_mode == UI_MODE_EFFECTS) {
                // EFFECTS MODE: Effect parameters (matching mock-ui.cpp layout)
                RegrooveEffects* effects = get_current_effects();

                // Show FX mode header
                if (fx_mode == FX_MODE_MASTER) {
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "FXM - MASTER EFFECTS");
                } else {
                    if (rsx && rsx->program_names[current_program][0] != '\0') {
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "FXP - PROGRAM %d: %s",
                                          current_program + 1, rsx->program_names[current_program]);
                    } else {
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "FXP - PROGRAM %d", current_program + 1);
                    }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (!effects) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Effects system not initialized");
                } else {
                    const float fx_spacing = 16.0f;

                    // Calculate effects panel specific slider height
                    // Header section: labelH (mode header) + spacing + separator + spacing
                    // From origin: 24.0f (to column start, includes group header) + labelH (column label) + 4.0f (spacing) + SOLO_SIZE (enable button) + 6.0f (spacing)
                    float fx_header = labelH + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().ItemSpacing.y;
                    float fx_sliderTop = fx_header + 24.0f + labelH + 4.0f + SOLO_SIZE + 6.0f;
                    float fx_bottomStack = 8.0f + MUTE_SIZE + 12.0f;
                    float fx_sliderH = contentHeight - fx_sliderTop - fx_bottomStack - IMGUI_LAYOUT_COMPENSATION;
                    if (fx_sliderH < MIN_SLIDER_HEIGHT) fx_sliderH = MIN_SLIDER_HEIGHT;

                    ImVec2 origin = ImGui::GetCursorScreenPos();
                    int col_index = 0;
                    float group_gap_offset = 0.0f;

                    // --- DISTORTION GROUP ---
                    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 8.0f));
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "DISTORTION");

                    // Drive (with enable)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Drive");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        int dist_en = regroove_effects_get_distortion_enabled(effects);
                        ImVec4 enCol = dist_en ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##dist_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_DISTORTION_TOGGLE);
                            } else {
                                regroove_effects_set_distortion_enabled(effects, !dist_en);
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float drive = regroove_effects_get_distortion_drive(effects);
                        if (ImGui::VSliderFloat("##fx_drive", ImVec2(sliderW, fx_sliderH), &drive, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DISTORTION_DRIVE);
                            } else {
                                regroove_effects_set_distortion_drive(effects, drive);
                                config.fx_distortion_drive = drive;
                                autosave_effects_to_rsx();
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##dist_drive_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_distortion_drive(effects, 0.5f);
                            config.fx_distortion_drive = 0.5f;
                                autosave_effects_to_rsx();
                            autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Mix (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Mix");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float mix = regroove_effects_get_distortion_mix(effects);
                        if (ImGui::VSliderFloat("##fx_dist_mix", ImVec2(sliderW, fx_sliderH), &mix, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DISTORTION_MIX);
                            } else {
                                regroove_effects_set_distortion_mix(effects, mix);
                                config.fx_distortion_mix = mix;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##dist_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_distortion_mix(effects, 0.0f);
                            config.fx_distortion_mix = 0.0f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Add group spacing (wider gap between effect groups)
                    group_gap_offset += (spacing - fx_spacing);

                    // --- FILTER GROUP ---
                    float filter_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                    ImGui::SetCursorScreenPos(ImVec2(filter_start_x, origin.y + 8.0f));
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "FILTER");

                    // Cutoff (with enable)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Cutoff");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        int filt_en = regroove_effects_get_filter_enabled(effects);
                        ImVec4 enCol = filt_en ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##filt_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_FILTER_TOGGLE);
                            } else {
                                regroove_effects_set_filter_enabled(effects, !filt_en);
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float cutoff = regroove_effects_get_filter_cutoff(effects);
                        if (ImGui::VSliderFloat("##fx_cutoff", ImVec2(sliderW, fx_sliderH), &cutoff, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_FILTER_CUTOFF);
                            } else {
                                regroove_effects_set_filter_cutoff(effects, cutoff);
                                config.fx_filter_cutoff = cutoff;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##filt_cutoff_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_filter_cutoff(effects, 1.0f);
                            config.fx_filter_cutoff = 1.0f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Resonance (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Resonance");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float reso = regroove_effects_get_filter_resonance(effects);
                        if (ImGui::VSliderFloat("##fx_reso", ImVec2(sliderW, fx_sliderH), &reso, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_FILTER_RESONANCE);
                            } else {
                                regroove_effects_set_filter_resonance(effects, reso);
                                config.fx_filter_resonance = reso;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##filt_reso_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_filter_resonance(effects, 0.0f);
                            config.fx_filter_resonance = 0.0f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Add group spacing
                    group_gap_offset += (spacing - fx_spacing);

                    // --- EQ GROUP ---
                    float eq_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                    ImGui::SetCursorScreenPos(ImVec2(eq_start_x, origin.y + 8.0f));
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "EQ");

                    // EQ Low (with enable)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Low");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        int eq_en = regroove_effects_get_eq_enabled(effects);
                        ImVec4 enCol = eq_en ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##eq_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_EQ_TOGGLE);
                            } else {
                                regroove_effects_set_eq_enabled(effects, !eq_en);
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float eq_low = regroove_effects_get_eq_low(effects);
                        if (ImGui::VSliderFloat("##fx_eq_low", ImVec2(sliderW, fx_sliderH), &eq_low, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_LOW);
                            } else {
                                regroove_effects_set_eq_low(effects, eq_low);
                                config.fx_eq_low = eq_low;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_low_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_low(effects, 0.5f);
                            config.fx_eq_low = 0.5f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // EQ Mid (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Mid");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float eq_mid = regroove_effects_get_eq_mid(effects);
                        if (ImGui::VSliderFloat("##fx_eq_mid", ImVec2(sliderW, fx_sliderH), &eq_mid, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_MID);
                            } else {
                                regroove_effects_set_eq_mid(effects, eq_mid);
                                config.fx_eq_mid = eq_mid;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_mid_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_mid(effects, 0.5f);
                            config.fx_eq_mid = 0.5f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // EQ High (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("High");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float eq_high = regroove_effects_get_eq_high(effects);
                        if (ImGui::VSliderFloat("##fx_eq_high", ImVec2(sliderW, fx_sliderH), &eq_high, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_HIGH);
                            } else {
                                regroove_effects_set_eq_high(effects, eq_high);
                                config.fx_eq_high = eq_high;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_high_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_high(effects, 0.5f);
                            config.fx_eq_high = 0.5f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Add group spacing
                    group_gap_offset += (spacing - fx_spacing);

                    // --- COMPRESSOR GROUP ---
                    float comp_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                    ImGui::SetCursorScreenPos(ImVec2(comp_start_x, origin.y + 8.0f));
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "COMPRESSOR");

                    // Threshold (with enable)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Threshold");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        int comp_en = regroove_effects_get_compressor_enabled(effects);
                        ImVec4 enCol = comp_en ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##comp_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_TOGGLE);
                            } else {
                                regroove_effects_set_compressor_enabled(effects, !comp_en);
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float thresh = regroove_effects_get_compressor_threshold(effects);
                        if (ImGui::VSliderFloat("##fx_comp_thresh", ImVec2(sliderW, fx_sliderH), &thresh, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_THRESHOLD);
                            } else {
                                regroove_effects_set_compressor_threshold(effects, thresh);
                                config.fx_compressor_threshold = thresh;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##comp_thresh_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_compressor_threshold(effects, 0.8f);
                            config.fx_compressor_threshold = 0.8f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Ratio (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Ratio");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float ratio = regroove_effects_get_compressor_ratio(effects);
                        if (ImGui::VSliderFloat("##fx_comp_ratio", ImVec2(sliderW, fx_sliderH), &ratio, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_RATIO);
                            } else {
                                regroove_effects_set_compressor_ratio(effects, ratio);
                                config.fx_compressor_ratio = ratio;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##comp_ratio_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_compressor_ratio(effects, 0.2f);
                            config.fx_compressor_ratio = 0.2f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Add group spacing
                    group_gap_offset += (spacing - fx_spacing);

                    // --- DELAY GROUP ---
                    float delay_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                    ImGui::SetCursorScreenPos(ImVec2(delay_start_x, origin.y + 8.0f));
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "DELAY");

                    // Time (with enable)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Time");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        int delay_en = regroove_effects_get_delay_enabled(effects);
                        ImVec4 enCol = delay_en ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##delay_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_DELAY_TOGGLE);
                            } else {
                                regroove_effects_set_delay_enabled(effects, !delay_en);
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float delay_time = regroove_effects_get_delay_time(effects);
                        if (ImGui::VSliderFloat("##fx_delay_time", ImVec2(sliderW, fx_sliderH), &delay_time, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_TIME);
                            } else {
                                regroove_effects_set_delay_time(effects, delay_time);
                                config.fx_delay_time = delay_time;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_time_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_time(effects, 0.3f);
                            config.fx_delay_time = 0.3f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Feedback (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Feedback");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float feedback = regroove_effects_get_delay_feedback(effects);
                        if (ImGui::VSliderFloat("##fx_delay_fb", ImVec2(sliderW, fx_sliderH), &feedback, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_FEEDBACK);
                            } else {
                                regroove_effects_set_delay_feedback(effects, feedback);
                                config.fx_delay_feedback = feedback;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_fb_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_feedback(effects, 0.3f);
                            config.fx_delay_feedback = 0.3f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }

                    // Mix (with reset button)
                    {
                        float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 24.0f));
                        ImGui::BeginGroup();
                        ImGui::Text("Mix");
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // Spacer to align with faders that have enable buttons
                        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float delay_mix = regroove_effects_get_delay_mix(effects);
                        if (ImGui::VSliderFloat("##fx_delay_mix", ImVec2(sliderW, fx_sliderH), &delay_mix, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_MIX);
                            } else {
                                regroove_effects_set_delay_mix(effects, delay_mix);
                                config.fx_delay_mix = delay_mix;
                                autosave_effects_to_rsx();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_mix(effects, 0.0f);
                            config.fx_delay_mix = 0.0f;
                                autosave_effects_to_rsx();
                        }
                        ImGui::EndGroup();
                        col_index++;
                    }
                }
            }
            else if (ui_mode == UI_MODE_PADS) {
                // PADS MODE: Show note trigger pads
                // Normal mode: 16 pads (N1-N16) in 4x4 grid
                // Expanded mode: 32 pads (N1-N32) in 4x8 grid

                // Calculate pad layout
                int PADS_PER_ROW, NUM_ROWS, total_pads;
                if (fullscreen_pads_mode) {
                    // Fullscreen: 32 pads in 8x4 grid (16 on left, 16 on right)
                    PADS_PER_ROW = 8;
                    NUM_ROWS = 4;
                    total_pads = 32;
                } else if (expanded_pads) {
                    // Expanded: 32 pads in 8x4 grid (16 on left, 16 on right)
                    PADS_PER_ROW = 8;
                    NUM_ROWS = 4;
                    total_pads = 32;
                } else {
                    // Normal: 16 pads in 4x4 grid
                    PADS_PER_ROW = 4;
                    NUM_ROWS = 4;
                    total_pads = 16;
                }

                float padSpacing = 12.0f;
                float availWidth = rightW - 2 * padSpacing;
                float availHeight = contentHeight - 16.0f;

                // Calculate pad size (square buttons)
                float padW = (availWidth - padSpacing * (PADS_PER_ROW - 1)) / PADS_PER_ROW;
                float padH = (availHeight - padSpacing * (NUM_ROWS - 1)) / NUM_ROWS;
                float padSize = fminf(padW, padH);

                // Allow pads to dynamically fill available space in ALL modes
                if (padSize < 40.0f) padSize = 40.0f;   // Min pad size for usability

                // Center the grid
                float gridW = PADS_PER_ROW * padSize + (PADS_PER_ROW - 1) * padSpacing;
                float gridH = NUM_ROWS * padSize + (NUM_ROWS - 1) * padSpacing;
                float startX = origin.x + (rightW - gridW) * 0.5f;
                float startY = origin.y + (contentHeight - gridH) * 0.5f;

                // Draw trigger pads
                for (int row = 0; row < NUM_ROWS; row++) {
                    for (int col = 0; col < PADS_PER_ROW; col++) {
                        int idx = row * PADS_PER_ROW + col;
                        if (idx >= total_pads) break;

                        // Calculate position relative to child window
                        float posX = startX + col * (padSize + padSpacing);
                        float posY = startY + row * (padSize + padSpacing);

                        // Determine pad index based on layout:
                        // Normal mode (4x4):
                        // Row 0: N1  N2  N3  N4
                        // Row 1: N5  N6  N7  N8
                        // Row 2: N9  N10 N11 N12
                        // Row 3: N13 N14 N15 N16
                        //
                        // Fullscreen/Expanded mode (8x4):
                        // Row 0: N1  N2  N3  N4   N17 N18 N19 N20
                        // Row 1: N5  N6  N7  N8   N21 N22 N23 N24
                        // Row 2: N9  N10 N11 N12  N25 N26 N27 N28
                        // Row 3: N13 N14 N15 N16  N29 N30 N31 N32
                        int pad_idx;
                        if (fullscreen_pads_mode || expanded_pads) {
                            // 8x4 layout: left 4 columns = N1-N16, right 4 columns = N17-N32
                            if (col < 4) {
                                // Left half: N1-N16
                                pad_idx = row * 4 + col;
                            } else {
                                // Right half: N17-N32
                                pad_idx = 16 + row * 4 + (col - 4);
                            }
                        } else {
                            // 4x4 layout: simple sequential
                            pad_idx = row * 4 + col;
                        }

                        ImGui::SetCursorPos(ImVec2(posX, posY));

                        // Get pad configuration (may be null if no RSX loaded)
                        NoteTriggerPad* pad = (rsx && pad_idx < rsx->num_pads) ? &rsx->pads[pad_idx] : nullptr;
                        bool pad_configured = (pad && pad->enabled && (pad->note >= 0 || pad->midi_file[0] != '\0'));

                        // Pad button with fade effect (inspired by regroove visual feedback)
                        float note_brightness = note_pad_fade[pad_idx];        // White/bright on note triggers
                        float loop_brightness = note_pad_loop_fade[pad_idx];    // Blue/cyan on loop restarts
                        ImVec4 pad_col;

                        // Check if this pad has a sequence assigned
                        bool has_sequence = (pad && pad->sequence_index >= 0);
                        SequencePadState seq_state = sequence_pad_states[pad_idx];

                        if (has_sequence && seq_state == SEQ_PAD_PLAYING) {
                            // PLAYING SEQUENCE - RED with blinking
                            float blink = sinf(sequence_pad_blink[pad_idx]) * 0.5f + 0.5f;  // 0.0-1.0 sine wave
                            pad_col = ImVec4(
                                0.5f + blink * 0.4f,  // Red pulses 0.5-0.9
                                0.1f + blink * 0.1f,  // Slight green for warmth
                                0.1f + blink * 0.1f,  // Slight blue for warmth
                                1.0f
                            );
                        } else if (has_sequence && seq_state == SEQ_PAD_QUEUED) {
                            // QUEUED SEQUENCE - BLUE with blinking
                            float blink = sinf(sequence_pad_blink[pad_idx]) * 0.5f + 0.5f;
                            pad_col = ImVec4(
                                0.1f + blink * 0.1f,
                                0.1f + blink * 0.2f,
                                0.5f + blink * 0.4f,  // Blue pulses 0.5-0.9
                                1.0f
                            );
                        } else if (has_sequence && seq_state == SEQ_PAD_NEXT_PHRASE) {
                            // NEXT PHRASE - YELLOW blinking
                            float blink = sinf(sequence_pad_blink[pad_idx]) * 0.5f + 0.5f;
                            pad_col = ImVec4(
                                0.6f + blink * 0.3f,  // Yellow pulses
                                0.6f + blink * 0.3f,
                                0.1f + blink * 0.1f,
                                1.0f
                            );
                        } else if (loop_brightness > 0.0f) {
                            // Loop restart - blue/cyan blink (like regroove transition fade)
                            pad_col = ImVec4(
                                0.26f + loop_brightness * 0.15f,
                                0.27f + loop_brightness * 0.45f,  // More green for cyan
                                0.30f + loop_brightness * 0.60f,  // Strong blue
                                1.0f
                            );
                        } else if (note_brightness > 0.0f) {
                            // Note trigger - white/bright blink (like regroove trigger fade)
                            pad_col = ImVec4(
                                0.26f + note_brightness * 0.64f,  // Add white
                                0.27f + note_brightness * 0.63f,
                                0.30f + note_brightness * 0.60f,
                                1.0f
                            );
                        } else if (!pad_configured) {
                            // Not configured (darker gray)
                            pad_col = COLOR_BUTTON_AT_LIMIT;
                        } else {
                            // Configured but inactive
                            pad_col = COLOR_BUTTON_INACTIVE;
                        }

                        ImGui::PushStyleColor(ImGuiCol_Button, pad_col);

                        // Pad label
                        char pad_label[128];
                        if (pad_configured) {
                            // Check if this is a MIDI file pad or single note pad
                            if (pad->midi_file[0] != '\0') {
                                // MIDI file mode - show filename and program
                                // Extract just the filename from the path
                                const char* filename = strrchr(pad->midi_file, '/');
                                if (!filename) filename = strrchr(pad->midi_file, '\\');
                                if (!filename) filename = pad->midi_file;
                                else filename++;  // Skip the slash

                                if (pad->program >= 0 && pad->program < rsx->num_programs) {
                                    const char* assigned_prog_name = (rsx->program_names[pad->program][0] != '\0')
                                        ? rsx->program_names[pad->program]
                                        : "";
                                    if (assigned_prog_name[0] != '\0') {
                                        snprintf(pad_label, sizeof(pad_label), "%s\n%s\n%s",
                                                pad->description[0] ? pad->description : "",
                                                filename,
                                                assigned_prog_name);
                                    } else {
                                        snprintf(pad_label, sizeof(pad_label), "%s\n%s\nPrg %d",
                                                pad->description[0] ? pad->description : "",
                                                filename,
                                                pad->program + 1);
                                    }
                                } else {
                                    const char* prog_name = (rsx && current_program < rsx->num_programs && rsx->program_names[current_program][0] != '\0')
                                        ? rsx->program_names[current_program]
                                        : "";
                                    if (prog_name[0] != '\0') {
                                        snprintf(pad_label, sizeof(pad_label), "%s\n%s\n[%s]",
                                                pad->description[0] ? pad->description : "",
                                                filename,
                                                prog_name);
                                    } else {
                                        snprintf(pad_label, sizeof(pad_label), "%s\n%s\n[Prg %d]",
                                                pad->description[0] ? pad->description : "",
                                                filename,
                                                current_program + 1);
                                    }
                                }
                            } else {
                                // Single note mode - show note and program
                                if (pad->program >= 0 && pad->program < rsx->num_programs) {
                                    const char* assigned_prog_name = (rsx->program_names[pad->program][0] != '\0')
                                        ? rsx->program_names[pad->program]
                                        : "";
                                    if (assigned_prog_name[0] != '\0') {
                                        snprintf(pad_label, sizeof(pad_label), "%s\nNote %d\n%s",
                                                pad->description[0] ? pad->description : "",
                                                pad->note,
                                                assigned_prog_name);
                                    } else {
                                        snprintf(pad_label, sizeof(pad_label), "%s\nNote %d\nPrg %d",
                                                pad->description[0] ? pad->description : "",
                                                pad->note,
                                                pad->program + 1);
                                    }
                                } else {
                                    // No specific program: show current UI program in brackets
                                    const char* prog_name = (rsx && current_program < rsx->num_programs && rsx->program_names[current_program][0] != '\0')
                                        ? rsx->program_names[current_program]
                                        : "";
                                    if (prog_name[0] != '\0') {
                                        snprintf(pad_label, sizeof(pad_label), "%s\nNote %d\n[%s]",
                                                pad->description[0] ? pad->description : "",
                                                pad->note,
                                                prog_name);
                                    } else {
                                        snprintf(pad_label, sizeof(pad_label), "%s\nNote %d\n[Prg %d]",
                                                pad->description[0] ? pad->description : "",
                                                pad->note,
                                                current_program + 1);
                                    }
                                }
                            }
                        } else {
                            snprintf(pad_label, sizeof(pad_label), "N%d\n\n[Not Set]", pad_idx + 1);
                        }

                        // Add unique ID suffix so ImGui can distinguish buttons with same label
                        char button_id[256];
                        snprintf(button_id, sizeof(button_id), "%s##pad%d", pad_label, idx);
                        ImGui::Button(button_id, ImVec2(padSize, padSize));

                        // Overlay red rectangle when button is actively pressed (exactly like Regroove)
                        bool is_active = ImGui::IsItemActive();
                        if (is_active && pad_configured) {
                            ImVec2 p_min = ImGui::GetItemRectMin();
                            ImVec2 p_max = ImGui::GetItemRectMax();
                            const ImGuiStyle& style = ImGui::GetStyle();

                            // Draw rounded rectangle overlay matching the button's FrameRounding
                            ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, IM_COL32(220, 40, 40, 180), style.FrameRounding);

                            // Redraw label using the EXACT same internal function ImGui uses for buttons
                            ImVec2 label_size = ImGui::CalcTextSize(pad_label, NULL, true);
                            ImVec2 text_min = ImVec2(p_min.x + style.FramePadding.x, p_min.y + style.FramePadding.y);
                            ImVec2 text_max = ImVec2(p_max.x - style.FramePadding.x, p_max.y - style.FramePadding.y);
                            ImRect clip_rect(p_min, p_max);
                            ImGui::RenderTextClipped(text_min, text_max, pad_label, NULL, &label_size,
                                                     style.ButtonTextAlign, &clip_rect);
                        }

                        bool was_held = (held_pad_index == pad_idx);
                        bool just_clicked = ImGui::IsItemClicked();

                        // For MIDI file pads, use click detection; for regular pads, use active state
                        bool should_trigger = pad_configured && !learn_mode_active &&
                                            ((pad->midi_file[0] != '\0' && just_clicked) ||
                                             (pad->midi_file[0] == '\0' && is_active && !was_held));

                        if (should_trigger) {
                            // Button just pressed
                            int velocity = pad->velocity > 0 ? pad->velocity : 100;

                            // Check if pad has sequence configured
                            if (sequence_manager && pad->sequence_index >= 0) {
                                // Sequence trigger
                                int seq_idx = pad->sequence_index;
                                int current_pulse = sequencer ? medness_sequencer_get_pulse(sequencer) : 0;

                                if (medness_performance_is_playing(sequence_manager, seq_idx)) {
                                    // Already playing - stop it
                                    std::cout << "=== STOPPING SEQUENCE " << (seq_idx + 1) << " from pad " << (pad_idx + 1) << " ===" << std::endl;
                                    medness_performance_stop(sequence_manager, seq_idx);
                                    note_pad_fade[pad_idx] = 0.0f;
                                } else {
                                    // Start sequence
                                    std::cout << "=== TRIGGERING SEQUENCE " << (seq_idx + 1) << " from pad " << (pad_idx + 1) << " ===" << std::endl;
                                    medness_performance_play(sequence_manager, seq_idx, current_pulse);
                                    note_pad_fade[pad_idx] = 1.5f;  // Bright blink for sequence start
                                }
                            }
                            // Check if pad has MIDI file configured
                            else if (performance && pad->midi_file[0] != '\0') {
                                // Check if already playing
                                if (medness_performance_is_playing(performance, pad_idx)) {
                                    // Already playing - stop it
                                    medness_performance_stop(performance, pad_idx);
                                    note_pad_fade[pad_idx] = 0.0f;
                                } else {
                                    // Not playing - start/retrigger MIDI file playback
                                    std::cout << "=== TRIGGERING PAD " << (pad_idx + 1) << ": " << pad->midi_file << " ===" << std::endl;
                                    std::cout << "  midi_clock.active=" << midi_clock.active << std::endl;
                                    std::cout << "  midi_clock.running=" << midi_clock.running << std::endl;
                                    std::cout << "  midi_clock.total_pulse_count=" << midi_clock.total_pulse_count << std::endl;

                                    // Get current pulse from sequencer
                                    int current_pulse = (sequencer && medness_sequencer_is_active(sequencer))
                                        ? medness_sequencer_get_pulse(sequencer) : -1;

                                    // Groovebox model: patterns are always running, trigger just "unmutes" them
                                    // Start immediately at current pattern position (like unmuting a track)
                                    if (midi_clock.active && midi_clock.running) {
                                        // MIDI clock/SPP active - start at current pattern position
                                        std::cout << "  MIDI Clock/SPP active: starting at current pattern position" << std::endl;
                                        std::cout << "  Current pulse: " << midi_clock.total_pulse_count << std::endl;

                                        // Start immediately at current pulse - pattern is already running
                                        medness_performance_play(performance, pad_idx, current_pulse);
                                        note_pad_fade[pad_idx] = 1.5f;  // Bright blink for immediate start
                                    } else {
                                        // No MIDI clock - trigger from beginning
                                        std::cout << "  No MIDI clock: triggering from beginning" << std::endl;
                                        medness_performance_play(performance, pad_idx, current_pulse);
                                        note_pad_fade[pad_idx] = 1.5f;  // Extra bright for blink effect
                                    }
                                }

                                // Mark as held to prevent retriggering while mouse is down (MIDI file pads only)
                                // DON'T set held_pad_index here - that's for regular note pads with note-off!
                                // held_pad_index = pad_idx;  // REMOVED - causes issues with note-off for regular pads
                            } else {
                                // Regular single note trigger
                                // Determine which synth to use based on pad's program setting
                                int target_prog = current_program;
                                sfizz_synth_t* target_synth = nullptr;
                                int actual_program = target_prog;

                                if (pad->program >= 0 && pad->program < rsx->num_programs && program_synths[pad->program]) {
                                    target_synth = program_synths[pad->program];
                                    actual_program = pad->program;
                                } else {
                                    target_synth = program_synths[target_prog];
                                }

                                std::lock_guard<std::mutex> lock(synth_mutex);
                                if (target_synth) {
                                    sfizz_send_note_on(target_synth, 0, pad->note, velocity);

                                    // Track which pad/note is held for note_off on release
                                    held_pad_index = pad_idx;
                                    held_pad_note = pad->note;
                                    held_pad_synth = target_synth;

                                    current_note = pad->note;
                                    current_velocity = velocity;

                                    // Highlight ALL pads that would play this same note on this program
                                    for (int i = 0; i < RSX_MAX_NOTE_PADS && i < rsx->num_pads; i++) {
                                        NoteTriggerPad* check_pad = &rsx->pads[i];
                                        if (check_pad->enabled && check_pad->note == pad->note) {
                                            int check_pad_program = (check_pad->program >= 0) ? check_pad->program : target_prog;
                                            if (check_pad_program == actual_program) {
                                                note_pad_fade[i] = 1.0f;
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (!is_active && was_held) {
                            // Button just released - send note_off
                            std::lock_guard<std::mutex> lock(synth_mutex);
                            if (held_pad_synth && held_pad_note >= 0) {
                                sfizz_send_note_off(held_pad_synth, 0, held_pad_note, 0);
                            }
                            held_pad_index = -1;
                            held_pad_note = -1;
                            held_pad_synth = nullptr;
                        } else if (is_active && !pad_configured && !learn_mode_active) {
                            // Clicked on unconfigured pad - do nothing
                        } else if (is_active && learn_mode_active) {
                            // Learn mode - start learning for this pad
                            if (!was_held) {  // Only trigger once on initial press
                                start_learn_for_action(ACTION_TRIGGER_NOTE_PAD, pad_idx);
                            }
                        }

                        ImGui::PopStyleColor();
                    }
                }

                // Add vertical bar on left side to toggle fullscreen mode (drawn after pads)
                // Save current cursor position to restore after drawing bar
                float barWidth = 12.0f;
                ImVec2 cursorBeforeBar = ImGui::GetCursorPos();

                // Bar should span the full content height (like Regroove)
                float barHeight = contentHeight;

                if (!fullscreen_pads_mode) {
                    // Enter fullscreen bar
                    ImGui::SetCursorPos(ImVec2(origin.x, origin.y));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.23f, 0.23f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
                    if (ImGui::Button("##fullscreen_bar_left", ImVec2(barWidth, barHeight))) {
                        fullscreen_pads_mode = true;
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Fullscreen Pads (F12)");
                    }
                }

                // Add vertical exit bar when in fullscreen pads mode
                if (fullscreen_pads_mode) {
                    // Exit fullscreen bar
                    ImGui::SetCursorPos(ImVec2(origin.x, origin.y));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.23f, 0.23f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
                    if (ImGui::Button("##exit_fullscreen_bar_left", ImVec2(barWidth, barHeight))) {
                        fullscreen_pads_mode = false;
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Exit Fullscreen Pads (F12)");
                    }
                }

                // Restore cursor position so bar doesn't affect content size
                ImGui::SetCursorPos(cursorBeforeBar);
            }
            else if (ui_mode == UI_MODE_TRACK) {
                // TRACK MODE: Display MIDI track data for currently playing pads (side-by-side like ModPlug Tracker)
                ImGui::Text("TRACK VIEW - MIDI Note Data");
                ImGui::Separator();
                ImGui::Spacing();

                if (!performance || !rsx) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No pads loaded");
                } else {
                    // Get current row from sequencer
                    int current_row = sequencer ? medness_sequencer_get_row(sequencer) : 0;

                    // Collect all playing pads with their track data
                    struct PlayingPad {
                        int pad_idx;
                        MednessTrack* track;
                        const MednessTrackEvent* events;
                        int event_count;
                        int tpqn;
                        int program_num;
                    };

                    std::vector<PlayingPad> playing_pads;
                    for (int pad_idx = 0; pad_idx < rsx->num_pads && pad_idx < RSX_MAX_NOTE_PADS; pad_idx++) {
                        if (!medness_performance_is_playing(performance, pad_idx)) continue;

                        // Get the sequence player for this pad
                        MednessSequence* seq = medness_performance_get_player(performance, pad_idx);
                        if (!seq) continue;

                        // Get the track from the sequence
                        MednessTrack* track = medness_sequence_get_current_track(seq);
                        if (!track) continue;

                        int event_count = 0;
                        const MednessTrackEvent* events = medness_track_get_events(track, &event_count);
                        int tpqn = medness_track_get_tpqn(track);

                        if (event_count == 0) continue;

                        PlayingPad pp;
                        pp.pad_idx = pad_idx;
                        pp.track = track;
                        pp.events = events;
                        pp.event_count = event_count;
                        pp.tpqn = tpqn;
                        // Get actual program number from pad
                        pp.program_num = pad_program_numbers[pad_idx] + 1;
                        playing_pads.push_back(pp);
                    }

                    if (playing_pads.empty()) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No pads playing - trigger a pad to view track data");
                    } else {
                        // Display pads side-by-side as columns
                        ImGui::BeginChild("##track_scroll", ImVec2(rightW - 32.0f, contentHeight - 64.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);

                        // Set up columns: Row + one column per playing pad
                        int num_cols = 1 + playing_pads.size();  // Row number + pads
                        ImGui::Columns(num_cols, "track_columns", true);

                        // Column widths
                        ImGui::SetColumnWidth(0, 50.0f);  // Row column
                        for (size_t i = 0; i < playing_pads.size(); i++) {
                            ImGui::SetColumnWidth(i + 1, 140.0f);  // Each pad column
                        }

                        // Header row
                        ImGui::Text("Row"); ImGui::NextColumn();
                        for (const auto& pp : playing_pads) {
                            ImGui::Text("%s", rsx->pads[pp.pad_idx].description);
                            ImGui::NextColumn();
                        }
                        ImGui::Separator();

                        // Build event maps for each pad (tick -> list of events)
                        // Pattern is 64 rows = 384 pulses
                        // Sequencer: tick = (pulse * TPQN) / 24
                        // Therefore: row = (tick * 24) / (TPQN * 6) = (tick * 4) / TPQN
                        std::vector<std::map<int, std::vector<int>>> event_maps;
                        for (const auto& pp : playing_pads) {
                            std::map<int, std::vector<int>> emap;
                            for (int i = 0; i < pp.event_count; i++) {
                                // Convert MIDI tick to pattern row (0-63)
                                int row = (pp.events[i].tick * 4) / pp.tpqn;
                                if (row >= 0 && row < 64) {
                                    emap[row].push_back(i);
                                }
                            }
                            event_maps.push_back(emap);
                        }

                        // Display 64 rows
                        const int num_rows = 64;
                        for (int row = 0; row < num_rows; row++) {
                            bool is_current = (row == current_row);

                            // Get full row dimensions for background highlight
                            ImVec2 row_start_pos = ImGui::GetCursorScreenPos();
                            float row_height = ImGui::GetTextLineHeightWithSpacing();

                            // Draw background highlight for current row
                            if (is_current) {
                                float total_width = 0.0f;
                                for (int c = 0; c < num_cols; c++) {
                                    total_width += ImGui::GetColumnWidth(c);
                                }
                                ImGui::GetWindowDrawList()->AddRectFilled(
                                    row_start_pos,
                                    ImVec2(row_start_pos.x + total_width, row_start_pos.y + row_height),
                                    IM_COL32(60, 60, 40, 255)
                                );
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                            }

                            // Row number column
                            ImGui::Text("%02d", row);
                            ImGui::NextColumn();

                            // Display each pad's data for this row
                            for (size_t pad_col = 0; pad_col < playing_pads.size(); pad_col++) {
                                const auto& pp = playing_pads[pad_col];
                                const auto& emap = event_maps[pad_col];

                                auto it = emap.find(row);
                                if (it != emap.end() && !it->second.empty()) {
                                    // Found event(s) at this row
                                    // Prioritize note-ON events, only show note-OFF if no note-ON exists
                                    const MednessTrackEvent* evt = nullptr;

                                    // First, look for a note-ON event in this row
                                    for (int idx : it->second) {
                                        if (pp.events[idx].on) {
                                            evt = &pp.events[idx];
                                            break;
                                        }
                                    }

                                    // If no note-ON found, check for note-OFF (for sustained notes)
                                    if (!evt && !it->second.empty()) {
                                        evt = &pp.events[it->second[0]];
                                    }

                                    if (evt) {
                                        char note_str[8];
                                        midi_note_to_string(evt->note, note_str, sizeof(note_str));

                                        if (evt->on) {
                                            // Note ON format: "NOTE PROG VOL EFFECT"
                                            // Volume is only shown if velocity < 0x7F (100%)
                                            if (evt->velocity < 0x7F) {
                                                ImGui::Text("%s %02d %02X ...", note_str, pp.program_num, evt->velocity);
                                            } else {
                                                ImGui::Text("%s %02d .. ...", note_str, pp.program_num);
                                            }
                                        } else {
                                            // Note OFF: "NOTE PROG .. FFF"
                                            // Only shown if at different row than note-ON (sustained sounds)
                                            ImGui::Text("%s %02d .. FFF", note_str, pp.program_num);
                                        }
                                    } else {
                                        // Empty row: "... .. .. ..."
                                        ImGui::Text("... .. .. ...");
                                    }
                                } else {
                                    // Empty row: "... .. .. ..."
                                    ImGui::Text("... .. .. ...");
                                }
                                ImGui::NextColumn();
                            }

                            if (is_current) {
                                ImGui::PopStyleColor();
                            }
                        }

                        ImGui::Columns(1);

                        // Auto-scroll to keep current row centered
                        float header_height = ImGui::GetTextLineHeightWithSpacing() * 2;  // Header + separator
                        float current_row_y = header_height + (current_row * ImGui::GetTextLineHeightWithSpacing());
                        float window_height = ImGui::GetWindowHeight();
                        float target_scroll = current_row_y - (window_height * 0.5f);
                        target_scroll = fmaxf(0.0f, target_scroll);

                        ImGui::SetScrollY(target_scroll);

                        ImGui::EndChild();
                    }
                }
            }
            else if (ui_mode == UI_MODE_SEQUENCES) {
                // SEQUENCES MODE: Control MIDI sequence playback
                ImGui::Text("SEQUENCE PLAYER");
                ImGui::Separator();
                ImGui::Spacing();

                if (!sequence_manager || !rsx) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Sequence manager not initialized");
                } else {
                    // Use RSX sequence count (includes empty sequences), not manager count
                    int num_sequences = rsx->num_sequences;

                    // Button to create new sequence
                    if (rsx->num_sequences < RSX_MAX_SEQUENCES) {
                        if (ImGui::Button("+ NEW SEQUENCE", ImVec2(150.0f, 30.0f))) {
                            // Create a new empty sequence
                            int new_idx = rsx->num_sequences;
                            snprintf(rsx->sequences[new_idx].name, sizeof(rsx->sequences[new_idx].name), "Sequence %d", new_idx + 1);
                            rsx->sequences[new_idx].num_phrases = 0;
                            rsx->sequences[new_idx].enabled = 1;
                            rsx->sequences[new_idx].loop = 1;
                            rsx->num_sequences++;
                            std::cout << "[SEQ UI] Created new sequence " << (new_idx + 1) << std::endl;
                        }
                    }

                    ImGui::Spacing();

                    if (num_sequences == 0) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No sequences. Click '+ NEW SEQUENCE' to create one.");
                    } else {
                        // Start mode selector
                        ImGui::Text("Start Mode:");
                        ImGui::SameLine();
                        SequenceStartMode current_mode = medness_performance_get_start_mode(sequence_manager);
                        const char* mode_names[] = { "Immediate", "Quantized (Row 0)" };
                        int mode_idx = (int)current_mode;

                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::Combo("##start_mode", &mode_idx, mode_names, 2)) {
                            medness_performance_set_start_mode(sequence_manager, (SequenceStartMode)mode_idx);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Get current pulse from sequencer
                        int current_pulse = sequencer ? medness_sequencer_get_pulse(sequencer) : 0;

                        // Display sequences
                        ImGui::BeginChild("##sequences_scroll", ImVec2(rightW - 32.0f, contentHeight - 120.0f), true);

                        for (int i = 0; i < num_sequences; i++) {
                            ImGui::PushID(i);

                            RSXSequence* seq_def = &rsx->sequences[i];
                            bool has_phrases = (seq_def->num_phrases > 0);
                            bool is_playing = has_phrases && medness_performance_is_playing(sequence_manager, i);

                            // Sequence number and name
                            ImGui::Text("Sequence %d: %s", i + 1, seq_def->name);
                            ImGui::SameLine(250.0f);

                            // Program assignment
                            ImGui::SameLine(250.0f);
                            ImGui::SetNextItemWidth(80.0f);
                            int prog_display = seq_def->program_number + 1;  // Display as 1-4
                            if (ImGui::InputInt("##prog", &prog_display)) {
                                if (prog_display < 1) prog_display = 1;
                                if (prog_display > 4) prog_display = 4;
                                seq_def->program_number = prog_display - 1;
                                // Auto-save when program assignment changes
                                if (!rsx_file_path.empty()) {
                                    samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Text("Prog");

                            // Status indicator
                            ImGui::SameLine(380.0f);
                            if (!has_phrases) {
                                ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.0f, 1.0f), "[EMPTY]");
                            } else if (is_playing) {
                                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[PLAYING]");
                            } else {
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[READY]");
                            }

                            // Play/Stop button
                            ImGui::SameLine(490.0f);
                            if (is_playing) {
                                if (ImGui::Button("STOP", ImVec2(100.0f, 0.0f))) {
                                    medness_performance_stop(sequence_manager, i);
                                }
                            } else if (has_phrases) {
                                if (ImGui::Button("PLAY", ImVec2(100.0f, 0.0f))) {
                                    // UI test button - start immediately, don't queue
                                    SequenceStartMode prev_mode = medness_performance_get_start_mode(sequence_manager);
                                    medness_performance_set_start_mode(sequence_manager, SEQUENCE_START_IMMEDIATE);
                                    medness_performance_play(sequence_manager, i, current_pulse);
                                    medness_performance_set_start_mode(sequence_manager, prev_mode);
                                }
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                                ImGui::Button("PLAY", ImVec2(100.0f, 0.0f));
                                ImGui::PopStyleColor(2);
                            }

                            // Show current phrase info if playing
                            if (is_playing) {
                                MednessSequence* player = medness_performance_get_player(sequence_manager, i);
                                if (player) {
                                    int phrase_idx = medness_sequence_get_current_phrase(player);
                                    int phrase_loop = medness_sequence_get_current_phrase_loop(player);
                                    int phrase_count = medness_sequence_get_phrase_count(player);

                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f),
                                                      "Phrase %d/%d (loop %d)",
                                                      phrase_idx + 1, phrase_count, phrase_loop);
                                }
                            }

                            // Show which pads are assigned to this sequence
                            ImGui::Text("  Assigned pads: ");
                            ImGui::SameLine();
                            bool found_any = false;
                            for (int p = 0; p < rsx->num_pads; p++) {
                                if (rsx->pads[p].sequence_index == i) {
                                    if (found_any) ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "N%d", p + 1);
                                    found_any = true;
                                }
                            }
                            if (!found_any) {
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "None");
                            }

                            // Phrase list and management
                            ImGui::Spacing();
                            ImGui::Text("  Phrases (%d):", seq_def->num_phrases);

                            // Show existing phrases with move up/down/delete
                            for (int p = 0; p < seq_def->num_phrases; p++) {
                                ImGui::PushID(1000 + p);
                                RSXPhrase* phrase = &seq_def->phrases[p];

                                ImGui::Text("    %d.", p + 1);
                                ImGui::SameLine();
                                ImGui::Text("%s (loops: %d)", phrase->midi_file, phrase->loop_count);

                                // Move up button
                                if (p > 0) {
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("^")) {
                                        // Swap with previous
                                        RSXPhrase temp = seq_def->phrases[p];
                                        seq_def->phrases[p] = seq_def->phrases[p-1];
                                        seq_def->phrases[p-1] = temp;
                                        reload_sequences();
                                    }
                                }

                                // Move down button
                                if (p < seq_def->num_phrases - 1) {
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("v")) {
                                        // Swap with next
                                        RSXPhrase temp = seq_def->phrases[p];
                                        seq_def->phrases[p] = seq_def->phrases[p+1];
                                        seq_def->phrases[p+1] = temp;
                                        reload_sequences();
                                    }
                                }

                                // Delete button
                                ImGui::SameLine();
                                if (ImGui::SmallButton("X")) {
                                    // Remove phrase
                                    for (int j = p; j < seq_def->num_phrases - 1; j++) {
                                        seq_def->phrases[j] = seq_def->phrases[j+1];
                                    }
                                    seq_def->num_phrases--;
                                    reload_sequences();
                                }

                                // Loop count adjuster
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(60.0f);
                                if (ImGui::InputInt("##loops", &phrase->loop_count)) {
                                    if (phrase->loop_count < 0) phrase->loop_count = 0;
                                    reload_sequences();
                                }

                                ImGui::PopID();
                            }

                            // Add phrase from available MIDI files
                            if (seq_def->num_phrases < RSX_MAX_PHRASES_PER_SEQUENCE && !rsx_file_path.empty()) {
                                ImGui::Spacing();
                                static int selected_file_idx = 0;

                                // Build list of .mid files using the filtered file list function
                                std::vector<std::string> midi_files;

                                // Get directory from file_list (which knows the RSX directory)
                                const char* scan_dir = file_list ? file_list->directory : nullptr;

                                if (!scan_dir && !rsx_file_path.empty()) {
                                    // Fallback: extract directory from RSX path
                                    static char rsx_dir_buf[512];
                                    strncpy(rsx_dir_buf, rsx_file_path.c_str(), sizeof(rsx_dir_buf) - 1);
                                    rsx_dir_buf[sizeof(rsx_dir_buf) - 1] = '\0';
                                    char* dir = dirname(rsx_dir_buf);
                                    scan_dir = dir;
                                }

                                if (scan_dir) {
                                    // Create temporary file list for MIDI files
                                    SamplecrateFileList* midi_list = samplecrate_filelist_create();
                                    if (midi_list) {
                                        // Load only .mid files
                                        if (samplecrate_filelist_load_filtered(midi_list, scan_dir, "mid,MID") > 0) {
                                            for (int f = 0; f < midi_list->count; f++) {
                                                midi_files.push_back(std::string(midi_list->filenames[f]));
                                            }
                                        }
                                        samplecrate_filelist_destroy(midi_list);
                                    }
                                }

                                // Sort alphabetically
                                std::sort(midi_files.begin(), midi_files.end());

                                if (selected_file_idx >= (int)midi_files.size()) {
                                    selected_file_idx = 0;
                                }

                                if (!midi_files.empty()) {
                                    ImGui::Text("    Add MIDI file:");
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(200.0f);
                                    if (ImGui::BeginCombo("##addmidi", midi_files[selected_file_idx].c_str())) {
                                        for (size_t idx = 0; idx < midi_files.size(); idx++) {
                                            bool is_selected = (idx == selected_file_idx);
                                            if (ImGui::Selectable(midi_files[idx].c_str(), is_selected)) {
                                                selected_file_idx = idx;
                                            }
                                            if (is_selected) {
                                                ImGui::SetItemDefaultFocus();
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }

                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("+ ADD")) {
                                        RSXPhrase* new_phrase = &seq_def->phrases[seq_def->num_phrases];
                                        strncpy(new_phrase->midi_file, midi_files[selected_file_idx].c_str(), sizeof(new_phrase->midi_file) - 1);
                                        new_phrase->midi_file[sizeof(new_phrase->midi_file) - 1] = '\0';
                                        strncpy(new_phrase->name, midi_files[selected_file_idx].c_str(), sizeof(new_phrase->name) - 1);
                                        new_phrase->name[sizeof(new_phrase->name) - 1] = '\0';
                                        new_phrase->loop_count = 1;
                                        seq_def->num_phrases++;
                                        reload_sequences();
                                        std::cout << "[SEQ UI] Added phrase: " << new_phrase->midi_file << std::endl;
                                    }
                                } else {
                                    // No MIDI files found in directory
                                    ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.0f, 1.0f), "    No .mid files found in RSX directory");
                                }
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            ImGui::PopID();
                        }

                        // Stop All button (inside scroll area)
                        ImGui::Spacing();
                        ImGui::Spacing();
                        if (ImGui::Button("STOP ALL", ImVec2(150.0f, 40.0f))) {
                            medness_performance_stop_all(sequence_manager);
                        }

                        ImGui::EndChild();
                    }
                }
            }
            else if (ui_mode == UI_MODE_MIDI) {
                // MIDI MODE: MIDI device configuration
                ImGui::Text("MIDI DEVICE CONFIGURATION");
                ImGui::Separator();
                ImGui::Spacing();

                // Global MIDI Input Channel Filter
                ImGui::Text("MIDI Input Channel:");
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);

                char channel_preview[32];
                if (config.midi_input_channel == 0) {
                    snprintf(channel_preview, sizeof(channel_preview), "Omni (All)");
                } else {
                    snprintf(channel_preview, sizeof(channel_preview), "Channel %d", config.midi_input_channel);
                }

                if (ImGui::BeginCombo("##global_midi_channel", channel_preview)) {
                    if (ImGui::Selectable("Omni (All Channels)", config.midi_input_channel == 0)) {
                        config.midi_input_channel = 0;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    for (int ch = 1; ch <= 16; ch++) {
                        char label[16];
                        snprintf(label, sizeof(label), "Channel %d", ch);
                        if (ImGui::Selectable(label, config.midi_input_channel == ch)) {
                            config.midi_input_channel = ch;
                            samplecrate_config_save(&config, "samplecrate.ini");
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Filter incoming MIDI by channel.\nOmni: Receive from all channels\n1-16: Only receive from specified channel");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                int num_ports = midi_list_ports();
                ImGui::Text("Available MIDI Ports: %d", num_ports);
                ImGui::Spacing();

                // Device 1 selection
                ImGui::Text("MIDI Device 1:");
                ImGui::PushItemWidth(300.0f);

                // Get current selection from global state
                int selected_port_0 = midi_device_ports[0];

                // Build preview label showing current selection
                char preview_label_0[256];
                if (selected_port_0 == -1) {
                    snprintf(preview_label_0, sizeof(preview_label_0), "Not configured");
                } else {
                    char port_name[128];
                    if (midi_get_port_name(selected_port_0, port_name, sizeof(port_name)) == 0) {
                        snprintf(preview_label_0, sizeof(preview_label_0), "Port %d: %s", selected_port_0, port_name);
                    } else {
                        snprintf(preview_label_0, sizeof(preview_label_0), "Port %d", selected_port_0);
                    }
                }

                if (ImGui::BeginCombo("##midi_device_1", preview_label_0)) {
                    if (ImGui::Selectable("Not configured", selected_port_0 == -1)) {
                        midi_device_ports[0] = -1;
                        config.midi_device_0 = -1;
                        // Save config immediately
                        samplecrate_config_save(&config, "samplecrate.ini");
                        // Reinitialize MIDI with new configuration
                        midi_deinit();
                        if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0) {
                            midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                        }
                    }
                    for (int i = 0; i < num_ports && i < 16; i++) {
                        char port_name[128];
                        if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                            char label[256];
                            snprintf(label, sizeof(label), "Port %d: %s", i, port_name);
                            if (ImGui::Selectable(label, selected_port_0 == i)) {
                                midi_device_ports[0] = i;
                                config.midi_device_0 = i;
                                // Save config immediately
                                samplecrate_config_save(&config, "samplecrate.ini");
                                // Reinitialize MIDI with new configuration
                                midi_deinit();
                                if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0) {
                                    midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                // Program routing for Device 1
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);

                char prog_preview_0[32];
                if (config.midi_program_change_enabled[0]) {
                    snprintf(prog_preview_0, sizeof(prog_preview_0), "Program Change");
                } else {
                    snprintf(prog_preview_0, sizeof(prog_preview_0), "Follow UI");
                }

                if (ImGui::BeginCombo("##prog_route_0", prog_preview_0)) {
                    if (ImGui::Selectable("Follow UI", config.midi_program_change_enabled[0] == 0)) {
                        config.midi_program_change_enabled[0] = 0;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    if (ImGui::Selectable("Program Change", config.midi_program_change_enabled[0] == 1)) {
                        config.midi_program_change_enabled[0] = 1;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::Spacing();

                // Device 2 selection
                ImGui::Text("MIDI Device 2:");
                ImGui::PushItemWidth(300.0f);

                // Get current selection from global state
                int selected_port_1 = midi_device_ports[1];

                // Build preview label showing current selection
                char preview_label_1[256];
                if (selected_port_1 == -1) {
                    snprintf(preview_label_1, sizeof(preview_label_1), "Not configured");
                } else {
                    char port_name[128];
                    if (midi_get_port_name(selected_port_1, port_name, sizeof(port_name)) == 0) {
                        snprintf(preview_label_1, sizeof(preview_label_1), "Port %d: %s", selected_port_1, port_name);
                    } else {
                        snprintf(preview_label_1, sizeof(preview_label_1), "Port %d", selected_port_1);
                    }
                }

                if (ImGui::BeginCombo("##midi_device_2", preview_label_1)) {
                    if (ImGui::Selectable("Not configured", selected_port_1 == -1)) {
                        midi_device_ports[1] = -1;
                        config.midi_device_1 = -1;
                        // Save config immediately
                        samplecrate_config_save(&config, "samplecrate.ini");
                        // Reinitialize MIDI with new configuration
                        midi_deinit();
                        if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0) {
                            midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                        }
                    }
                    for (int i = 0; i < num_ports && i < 16; i++) {
                        char port_name[128];
                        if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                            char label[256];
                            snprintf(label, sizeof(label), "Port %d: %s", i, port_name);
                            if (ImGui::Selectable(label, selected_port_1 == i)) {
                                midi_device_ports[1] = i;
                                config.midi_device_1 = i;
                                // Save config immediately
                                samplecrate_config_save(&config, "samplecrate.ini");
                                // Reinitialize MIDI with new configuration
                                midi_deinit();
                                if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0) {
                                    midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                // Program routing for Device 2
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);

                char prog_preview_1[32];
                if (config.midi_program_change_enabled[1]) {
                    snprintf(prog_preview_1, sizeof(prog_preview_1), "Program Change");
                } else {
                    snprintf(prog_preview_1, sizeof(prog_preview_1), "Follow UI");
                }

                if (ImGui::BeginCombo("##prog_route_1", prog_preview_1)) {
                    if (ImGui::Selectable("Follow UI", config.midi_program_change_enabled[1] == 0)) {
                        config.midi_program_change_enabled[1] = 0;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    if (ImGui::Selectable("Program Change", config.midi_program_change_enabled[1] == 1)) {
                        config.midi_program_change_enabled[1] = 1;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::Spacing();

                // Device 3 selection
                ImGui::Text("MIDI Device 3:");
                ImGui::PushItemWidth(300.0f);

                // Get current selection from global state
                int selected_port_2 = midi_device_ports[2];

                // Build preview label showing current selection
                char preview_label_2[256];
                if (selected_port_2 == -1) {
                    snprintf(preview_label_2, sizeof(preview_label_2), "Not configured");
                } else {
                    char port_name[128];
                    if (midi_get_port_name(selected_port_2, port_name, sizeof(port_name)) == 0) {
                        snprintf(preview_label_2, sizeof(preview_label_2), "Port %d: %s", selected_port_2, port_name);
                    } else {
                        snprintf(preview_label_2, sizeof(preview_label_2), "Port %d", selected_port_2);
                    }
                }

                if (ImGui::BeginCombo("##midi_device_3", preview_label_2)) {
                    if (ImGui::Selectable("Not configured", selected_port_2 == -1)) {
                        midi_device_ports[2] = -1;
                        config.midi_device_2 = -1;
                        // Save config immediately
                        samplecrate_config_save(&config, "samplecrate.ini");
                        // Reinitialize MIDI with new configuration
                        midi_deinit();
                        if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0 || midi_device_ports[2] >= 0) {
                            midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                        }
                    }
                    for (int i = 0; i < num_ports && i < 16; i++) {
                        char port_name[128];
                        if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                            char label[256];
                            snprintf(label, sizeof(label), "Port %d: %s", i, port_name);
                            if (ImGui::Selectable(label, selected_port_2 == i)) {
                                midi_device_ports[2] = i;
                                config.midi_device_2 = i;
                                // Save config immediately
                                samplecrate_config_save(&config, "samplecrate.ini");
                                // Reinitialize MIDI with new configuration
                                midi_deinit();
                                if (midi_device_ports[0] >= 0 || midi_device_ports[1] >= 0 || midi_device_ports[2] >= 0) {
                                    midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES);
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                // Program routing for Device 3
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);

                char prog_preview_2[32];
                if (config.midi_program_change_enabled[2]) {
                    snprintf(prog_preview_2, sizeof(prog_preview_2), "Program Change");
                } else {
                    snprintf(prog_preview_2, sizeof(prog_preview_2), "Follow UI");
                }

                if (ImGui::BeginCombo("##prog_route_2", prog_preview_2)) {
                    if (ImGui::Selectable("Follow UI", config.midi_program_change_enabled[2] == 0)) {
                        config.midi_program_change_enabled[2] = 0;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    if (ImGui::Selectable("Program Change", config.midi_program_change_enabled[2] == 1)) {
                        config.midi_program_change_enabled[2] = 1;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // MIDI SYNC SETTINGS
                ImGui::Text("MIDI CLOCK SYNC:");
                ImGui::Spacing();

                // MIDI Clock Tempo Sync
                bool midi_clock_tempo_sync = (config.midi_clock_tempo_sync == 1);
                if (ImGui::Checkbox("Sync Playback Tempo to MIDI Clock", &midi_clock_tempo_sync)) {
                    config.midi_clock_tempo_sync = midi_clock_tempo_sync ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "When enabled, MIDI file playback tempo follows external MIDI clock");

                ImGui::Spacing();

                // MIDI SPP Receive
                bool midi_spp_receive = (config.midi_spp_receive == 1);
                if (ImGui::Checkbox("Receive Song Position Pointer (SPP)", &midi_spp_receive)) {
                    config.midi_spp_receive = midi_spp_receive ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "When enabled, sync playback position to external MIDI clock");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // SysEx Device ID
                ImGui::Text("SysEx Device ID (Remote Control):");
                ImGui::PushItemWidth(100.0f);
                int sysex_id = config.sysex_device_id;
                if (ImGui::SliderInt("##sysex_device_id", &sysex_id, 0, 127)) {
                    config.sysex_device_id = sysex_id;
                    sysex_set_device_id((uint8_t)sysex_id);
                    samplecrate_config_save(&config, "samplecrate.ini");
                }
                ImGui::PopItemWidth();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Device ID for receiving SysEx remote control commands (0-127)");
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Use different IDs for multiple instances. ID 127 = broadcast to all.");

                ImGui::Spacing();

                // MIDI Clock Quantization
                ImGui::Text("MIDI Clock Quantization:");
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "When MIDI Clock is active, pads trigger on beat boundaries");
                ImGui::Spacing();

                ImGui::Separator();
                ImGui::Spacing();

                // MIDI mappings display
                if (input_mappings) {
                    ImGui::Text("MIDI Mappings: %d", input_mappings->midi_count);
                    ImGui::Spacing();

                    if (ImGui::Button("Clear All Mappings", ImVec2(200, 0))) {
                        input_mappings->midi_count = 0;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset to Defaults", ImVec2(200, 0))) {
                        input_mappings_reset_defaults(input_mappings);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Display individual mappings
                    ImGui::Text("Current MIDI Mappings:");
                    ImGui::Spacing();

                    if (input_mappings->midi_count == 0) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No MIDI mappings configured");
                    } else {
                        // Table header
                        ImGui::Columns(5, "midi_mappings_table", true);
                        ImGui::Text("Device");
                        ImGui::NextColumn();
                        ImGui::Text("CC");
                        ImGui::NextColumn();
                        ImGui::Text("Action");
                        ImGui::NextColumn();
                        ImGui::Text("Type");
                        ImGui::NextColumn();
                        ImGui::Text("");  // Remove button column
                        ImGui::NextColumn();
                        ImGui::Separator();

                        // Display each mapping
                        for (int i = 0; i < input_mappings->midi_count; i++) {
                            MidiMapping* mapping = &input_mappings->midi_mappings[i];

                            ImGui::PushID(i);

                            // Device column
                            if (mapping->device_id == -1) {
                                ImGui::Text("Any");
                            } else {
                                ImGui::Text("%d", mapping->device_id);
                            }
                            ImGui::NextColumn();

                            // CC column
                            ImGui::Text("%d", mapping->cc_number);
                            ImGui::NextColumn();

                            // Action column (with parameter if applicable)
                            const char* action_name = input_action_name(mapping->action);
                            if (mapping->parameter > 0) {
                                ImGui::Text("%s (%d)", action_name, mapping->parameter);
                            } else {
                                ImGui::Text("%s", action_name);
                            }
                            ImGui::NextColumn();

                            // Type column
                            if (mapping->continuous) {
                                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Continuous");
                            } else {
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "Button");
                            }
                            ImGui::NextColumn();

                            // Remove button
                            if (ImGui::Button("Remove", ImVec2(80, 0))) {
                                // Shift remaining mappings down
                                for (int j = i; j < input_mappings->midi_count - 1; j++) {
                                    input_mappings->midi_mappings[j] = input_mappings->midi_mappings[j + 1];
                                }
                                input_mappings->midi_count--;
                                i--;  // Check this index again since we shifted
                            }
                            ImGui::NextColumn();

                            ImGui::PopID();
                        }

                        ImGui::Columns(1);
                    }

                    ImGui::Spacing();
                    ImGui::Text("Use LEARN mode to create new MIDI mappings");
                }

                // MIDI Monitor
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("MIDI MONITOR");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextWrapped("Recent MIDI messages (shows device, type, and which program received it):");
                ImGui::Dummy(ImVec2(0, 8.0f));

                // MIDI monitor table
                ImGui::BeginChild("##midi_monitor", ImVec2(0, 250.0f), true);

                ImGui::Columns(6, "midi_monitor_columns");
                ImGui::SetColumnWidth(0, 70.0f);   // Time
                ImGui::SetColumnWidth(1, 60.0f);   // Device
                ImGui::SetColumnWidth(2, 100.0f);  // Type
                ImGui::SetColumnWidth(3, 70.0f);   // Number
                ImGui::SetColumnWidth(4, 70.0f);   // Value
                ImGui::SetColumnWidth(5, 90.0f);   // Program

                ImGui::Text("Time"); ImGui::NextColumn();
                ImGui::Text("Device"); ImGui::NextColumn();
                ImGui::Text("Type"); ImGui::NextColumn();
                ImGui::Text("Number"); ImGui::NextColumn();
                ImGui::Text("Value"); ImGui::NextColumn();
                ImGui::Text("Program"); ImGui::NextColumn();
                ImGui::Separator();

                // Display MIDI monitor entries (newest first)
                for (int i = 0; i < midi_monitor_count; i++) {
                    int idx = (midi_monitor_head - 1 - i + MIDI_MONITOR_SIZE) % MIDI_MONITOR_SIZE;
                    MidiMonitorEntry* entry = &midi_monitor[idx];

                    ImGui::Text("%s", entry->timestamp); ImGui::NextColumn();

                    ImGui::Text("IN%d", entry->device_id); ImGui::NextColumn();

                    // Color-code message type
                    if (strstr(entry->type, "Note On")) {
                        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", entry->type);
                    } else if (strstr(entry->type, "Note Off")) {
                        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "%s", entry->type);
                    } else if (strstr(entry->type, "Prog")) {
                        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.5f, 1.0f), "%s", entry->type);
                    } else {
                        ImGui::Text("%s", entry->type);
                    }
                    ImGui::NextColumn();

                    ImGui::Text("%d", entry->number); ImGui::NextColumn();
                    ImGui::Text("%d", entry->value); ImGui::NextColumn();
                    ImGui::Text("P%d", entry->program); ImGui::NextColumn();
                }

                ImGui::Columns(1);
                ImGui::EndChild();

                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("Clear Monitor", ImVec2(120.0f, 0.0f))) {
                    midi_monitor_count = 0;
                    midi_monitor_head = 0;
                }

                // MIDI Sync Settings
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("MIDI SYNC SETTINGS");
                ImGui::Separator();
                ImGui::Spacing();

                // Tempo sync toggle
                bool tempo_sync = (config.midi_clock_tempo_sync == 1);
                if (ImGui::Checkbox("Sync tempo to MIDI Clock", &tempo_sync)) {
                    config.midi_clock_tempo_sync = tempo_sync ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When ENABLED: MIDI file playback tempo adjusts to match incoming MIDI Clock.\n"
                                      "When DISABLED: Incoming tempo is shown in LCD but doesn't affect playback (visual only).");
                }

                ImGui::Spacing();

                // SPP receive toggle
                bool spp_receive = (config.midi_spp_receive == 1);
                if (ImGui::Checkbox("Sync position to MIDI SPP", &spp_receive)) {
                    config.midi_spp_receive = spp_receive ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When ENABLED: Incoming MIDI Song Position Pointer messages sync playback position.\n"
                                      "When DISABLED: SPP messages are ignored (only tempo sync).");
                }
            }
            else if (ui_mode == UI_MODE_SETTINGS) {
                // SETTINGS MODE: Audio configuration
                ImGui::Text("AUDIO SETTINGS");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Current Audio Device:");
                ImGui::Spacing();

                // Show current audio device info
                ImGui::Text("Sample Rate: %d Hz", obtained.freq);
                ImGui::Text("Channels: %d", (int)obtained.channels);
                ImGui::Text("Buffer Size: %d samples", obtained.samples);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Audio device selection
                ImGui::Text("Select Audio Output Device:");
                ImGui::Spacing();

                ImGui::PushItemWidth(400.0f);

                // Build current device name for preview
                char current_device_preview[256];
                if (config.audio_device >= 0 && config.audio_device < num_audio_devices) {
                    const char* device_name = SDL_GetAudioDeviceName(config.audio_device, 0);
                    snprintf(current_device_preview, sizeof(current_device_preview),
                             "Device %d: %s", config.audio_device, device_name ? device_name : "Unknown");
                } else {
                    snprintf(current_device_preview, sizeof(current_device_preview), "Default (System)");
                }

                if (ImGui::BeginCombo("##audio_device", current_device_preview)) {
                    // Default option
                    if (ImGui::Selectable("Default (System)", config.audio_device == -1)) {
                        config.audio_device = -1;
                        samplecrate_config_save(&config, "samplecrate.ini");
                    }

                    // List all available audio devices
                    for (int i = 0; i < num_audio_devices && i < 16; i++) {
                        const char* device_name = SDL_GetAudioDeviceName(i, 0);
                        char label[256];
                        snprintf(label, sizeof(label), "Device %d: %s", i, device_name ? device_name : "Unknown");

                        if (ImGui::Selectable(label, config.audio_device == i)) {
                            config.audio_device = i;
                            samplecrate_config_save(&config, "samplecrate.ini");
                        }
                    }

                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                    "Audio device changes require application restart to take effect");
            }
        }
        ImGui::EndChild();

        // SEQUENCER BAR (step indicators)
        // Position below the main panels
        float sequencerTop = TOP_MARGIN + channelAreaHeight + GAP_ABOVE_SEQUENCER;

        // Debug: Print sequencer position once
        static bool debug_printed = false;
        if (!debug_printed) {
            std::cout << "[SEQUENCER BAR] fullH=" << fullH
                      << " channelAreaHeight=" << channelAreaHeight
                      << " sequencerTop=" << sequencerTop
                      << " SEQUENCER_HEIGHT=" << SEQUENCER_HEIGHT << std::endl;
            debug_printed = true;
        }

        ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, sequencerTop));
        ImGui::BeginChild("sequencer_bar", ImVec2(fullW - 2*SIDE_MARGIN, SEQUENCER_HEIGHT),
                          true, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);

        const int numSteps = 16;
        const float STEP_GAP = 6.0f;
        const float STEP_MIN = 28.0f;
        const float STEP_MAX = 60.0f;

        float gap = STEP_GAP;
        float availWidth = ImGui::GetContentRegionAvail().x;
        float stepWidth = (availWidth - gap * (numSteps - 1)) / numSteps;

        // Clamp step width
        if (stepWidth < STEP_MIN) stepWidth = STEP_MIN;
        if (stepWidth > STEP_MAX) stepWidth = STEP_MAX;

        float rowWidth = numSteps * stepWidth + (numSteps - 1) * gap;
        float centerOffset = (availWidth - rowWidth) * 0.5f;
        if (centerOffset < 0) centerOffset = 0;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerOffset);

        // Draw 16 step buttons (representing 64 rows, 4 rows per step)
        for (int i = 0; i < numSteps; ++i) {
            float brightness = step_fade[i];
            ImVec4 btnCol = ImVec4(0.18f + brightness * 0.24f,
                                0.27f + brightness * 0.38f,
                                0.18f + brightness * 0.24f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f,0.48f,0.32f,1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f,0.65f,0.42f,1.0f));

            char label[16];
            snprintf(label, sizeof(label), "##step%d", i);
            if (ImGui::Button(label, ImVec2(stepWidth, stepWidth))) {
                // Click on step - could be used for seeking in the future
            }

            ImGui::PopStyleColor(3);
            if (i != numSteps - 1) ImGui::SameLine(0.0f, gap);
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    // Save configuration and input mappings
    samplecrate_config_save(&config, "samplecrate.ini");
    if (input_mappings) {
        input_mappings_save(input_mappings, "samplecrate.ini");
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    // Close audio before destroying synth to avoid race conditions
    if (current_audio_device_id != 0) {
        SDL_CloseAudioDevice(current_audio_device_id);
    }

    // Safely destroy synths
    // Cleanup engine (frees all synths, RSX, performance, effects)
    if (engine) {
        samplecrate_engine_destroy(engine);
        engine = nullptr;
        // Note: synth, program_synths, rsx, performance, effects are all owned by engine
    }

    // Cleanup MIDI and input mappings
    midi_deinit();
    if (input_mappings) {
        input_mappings_destroy(input_mappings);
    }
    if (lcd_display) {
        lcd_destroy(lcd_display);
    }
    // Note: effects_master, effects_program, and performance are freed by engine
    if (sequence_manager) {
        medness_performance_destroy(sequence_manager);
    }
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
