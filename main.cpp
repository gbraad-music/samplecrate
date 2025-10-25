#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <sfizz.h>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <mutex>

extern "C" {
#include "lcd.h"
#include "samplecrate_common.h"
#include "samplecrate_rsx.h"
#include "regroove_effects.h"
#include "midi.h"
#include "input_mappings.h"
}

sfizz_synth_t* synth = nullptr;  // Current/legacy synth
sfizz_synth_t* program_synths[4] = {nullptr, nullptr, nullptr, nullptr};  // One synth per program
std::atomic<bool> running(true);
std::mutex synth_mutex;
LCD* lcd_display = nullptr;
int current_note = -1;
int current_velocity = 0;

// Mixer and effects
SamplecrateMixer mixer;
SamplecrateConfig config;
RegrooveEffects* effects = nullptr;

// Input mappings and MIDI
InputMappings* input_mappings = nullptr;
bool learn_mode_active = false;
InputAction learn_target_action = ACTION_NONE;
int learn_target_parameter = 0;

// MIDI device configuration
int midi_device_ports[MIDI_MAX_DEVICES] = {-1, -1};  // -1 = not configured

// RSX file (note pads and SFZ wrapper)
SamplecrateRSX* rsx = nullptr;
std::string rsx_file_path = "";

// Note pad visual feedback
float note_pad_fade[RSX_MAX_NOTE_PADS] = {0.0f};

// Current program selection (which SFZ is loaded)
int current_program = 0;  // 0-3 for programs 1-4 (UI selection)
int midi_target_program[2] = {0, 0};  // Per-device MIDI routing (set by program change messages)
int current_scene = 0;    // 0-3 for scene detection (note range mapping)

// Error message for LCD display
std::string error_message = "";

// Fullscreen pads mode (F12 toggle - hides left panel)
bool fullscreen_pads_mode = false;

// Expanded pads mode (config setting - 16 vs 32 pads)
bool expanded_pads = false;

// UI mode
enum UIMode {
    UI_MODE_INSTRUMENT = 0,
    UI_MODE_MIX = 1,
    UI_MODE_EFFECTS = 2,
    UI_MODE_PADS = 3,
    UI_MODE_MIDI = 4,
    UI_MODE_SETTINGS = 5
};
UIMode ui_mode = UI_MODE_PADS;  // Default to PADS view

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

// Map incoming MIDI note to pad index based on scene
// NanoPAD2 scenes:
// Scene 1: notes 37-50 (14 notes)
// Scene 2: notes 53-66 (14 notes)
// Scene 3: notes 69-82 (14 notes)
// Scene 4: notes 85-98 (14 notes)
int map_note_to_pad(int midi_note) {
    // Detect which scene based on note range and calculate pad index
    if (midi_note >= 37 && midi_note <= 50) {
        // Scene 1
        current_scene = 0;
        return midi_note - 37;  // Pads 0-13
    } else if (midi_note >= 53 && midi_note <= 66) {
        // Scene 2
        current_scene = 1;
        return midi_note - 53;  // Pads 0-13
    } else if (midi_note >= 69 && midi_note <= 82) {
        // Scene 3
        current_scene = 2;
        return midi_note - 69;  // Pads 0-13
    } else if (midi_note >= 85 && midi_note <= 98) {
        // Scene 4
        current_scene = 3;
        return midi_note - 85;  // Pads 0-13
    }
    return -1;  // Note not in any scene range
}

// Switch to a different program (change active synth pointer)
void switch_program(int program_index) {
    if (!rsx || program_index < 0 || program_index >= rsx->num_programs) return;
    if (!program_synths[program_index]) return;  // Program not loaded

    current_program = program_index;

    // Only update midi_target_program for devices that have program change DISABLED
    // Devices with program change enabled should be controlled only by MIDI messages
    if (!config.midi_program_change_enabled[0]) {
        midi_target_program[0] = program_index;
    }
    if (!config.midi_program_change_enabled[1]) {
        midi_target_program[1] = program_index;
    }

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
        case ACTION_FX_DISTORTION_DRIVE:
            if (effects) regroove_effects_set_distortion_drive(effects, normalized_value);
            break;
        case ACTION_FX_DISTORTION_MIX:
            if (effects) regroove_effects_set_distortion_mix(effects, normalized_value);
            break;
        case ACTION_FX_FILTER_CUTOFF:
            if (effects) regroove_effects_set_filter_cutoff(effects, normalized_value);
            break;
        case ACTION_FX_FILTER_RESONANCE:
            if (effects) regroove_effects_set_filter_resonance(effects, normalized_value);
            break;
        case ACTION_FX_EQ_LOW:
            if (effects) regroove_effects_set_eq_low(effects, normalized_value);
            break;
        case ACTION_FX_EQ_MID:
            if (effects) regroove_effects_set_eq_mid(effects, normalized_value);
            break;
        case ACTION_FX_EQ_HIGH:
            if (effects) regroove_effects_set_eq_high(effects, normalized_value);
            break;
        case ACTION_FX_COMPRESSOR_THRESHOLD:
            if (effects) regroove_effects_set_compressor_threshold(effects, normalized_value);
            break;
        case ACTION_FX_COMPRESSOR_RATIO:
            if (effects) regroove_effects_set_compressor_ratio(effects, normalized_value);
            break;
        case ACTION_FX_DELAY_TIME:
            if (effects) regroove_effects_set_delay_time(effects, normalized_value);
            break;
        case ACTION_FX_DELAY_FEEDBACK:
            if (effects) regroove_effects_set_delay_feedback(effects, normalized_value);
            break;
        case ACTION_FX_DELAY_MIX:
            if (effects) regroove_effects_set_delay_mix(effects, normalized_value);
            break;

        // Effects toggles
        case ACTION_FX_DISTORTION_TOGGLE:
            if (effects && event->value > 63) {
                int current = regroove_effects_get_distortion_enabled(effects);
                regroove_effects_set_distortion_enabled(effects, !current);
            }
            break;
        case ACTION_FX_FILTER_TOGGLE:
            if (effects && event->value > 63) {
                int current = regroove_effects_get_filter_enabled(effects);
                regroove_effects_set_filter_enabled(effects, !current);
            }
            break;
        case ACTION_FX_EQ_TOGGLE:
            if (effects && event->value > 63) {
                int current = regroove_effects_get_eq_enabled(effects);
                regroove_effects_set_eq_enabled(effects, !current);
            }
            break;
        case ACTION_FX_COMPRESSOR_TOGGLE:
            if (effects && event->value > 63) {
                int current = regroove_effects_get_compressor_enabled(effects);
                regroove_effects_set_compressor_enabled(effects, !current);
            }
            break;
        case ACTION_FX_DELAY_TOGGLE:
            if (effects && event->value > 63) {
                int current = regroove_effects_get_delay_enabled(effects);
                regroove_effects_set_delay_enabled(effects, !current);
            }
            break;

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

        default:
            break;
    }
}

// MIDI event callback from midi.c
void midi_event_callback(unsigned char status, unsigned char data1, unsigned char data2, int device_id, void* userdata) {
    int msg_type = status & 0xF0;
    int channel = status & 0x0F;

    // Debug: print all incoming MIDI messages
    std::cout << "MIDI device " << device_id << ": status=0x" << std::hex << (int)status
              << " data1=" << std::dec << (int)data1 << " data2=" << (int)data2 << std::endl;

    // Handle program change
    if (msg_type == 0xC0) {  // Program Change
        int program_number = data1;  // 0-127
        std::cout << "*** PROGRAM CHANGE RECEIVED from device " << device_id << ": program=" << program_number << " ***" << std::endl;

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
            std::cout << "    Device " << device_id << " MIDI routed to program " << (target_program + 1)
                      << " (MIDI program " << program_number << " mod " << rsx->num_programs << ")"
                      << " - UI remains at program " << (current_program + 1) << std::endl;
        } else {
            std::cout << "    No RSX programs loaded, ignoring Program Change" << std::endl;
        }
        return;
    }

    // Handle note on/off
    if (msg_type == 0x90 && data2 > 0) {  // Note on
        // Determine which program to target based on device-specific settings
        int target_prog = config.midi_program_change_enabled[device_id] ? midi_target_program[device_id] : current_program;

        // Try to map to RSX pad first via scene detection
        int pad_idx = map_note_to_pad(data1);
        bool handled_by_pad = false;

        if (pad_idx >= 0 && rsx && pad_idx < RSX_MAX_NOTE_PADS) {
            NoteTriggerPad* pad = &rsx->pads[pad_idx];
            if (pad->note >= 0 && pad->enabled) {
                // This pad will be triggered - determine which synth to use
                sfizz_synth_t* target_synth = nullptr;
                int actual_program = target_prog;  // Default to current/MIDI target

                if (pad->program >= 0 && pad->program < rsx->num_programs && program_synths[pad->program]) {
                    target_synth = program_synths[pad->program];
                    actual_program = pad->program;
                } else {
                    target_synth = program_synths[target_prog];
                }

                // Trigger the note
                int velocity = pad->velocity > 0 ? pad->velocity : data2;
                int note_to_play = pad->note;

                std::lock_guard<std::mutex> lock(synth_mutex);
                if (target_synth) {
                    sfizz_send_note_on(target_synth, 0, note_to_play, velocity);
                    current_note = note_to_play;
                    current_velocity = velocity;
                    handled_by_pad = true;

                    // Highlight ALL pads that would play this same note on this program
                    for (int i = 0; i < RSX_MAX_NOTE_PADS && i < rsx->num_pads; i++) {
                        NoteTriggerPad* check_pad = &rsx->pads[i];
                        if (check_pad->enabled && check_pad->note == note_to_play) {
                            // Check if this pad plays on the same program
                            int check_pad_program = (check_pad->program >= 0) ? check_pad->program : target_prog;
                            if (check_pad_program == actual_program) {
                                note_pad_fade[i] = 1.0f;
                            }
                        }
                    }
                }
            }
        }

        // If not handled by any pad, send note directly to the appropriate synth
        if (!handled_by_pad) {
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
        }
    } else if (msg_type == 0x80 || (msg_type == 0x90 && data2 == 0)) {  // Note off
        // Try to map to RSX pad first
        int pad_idx = map_note_to_pad(data1);
        if (pad_idx >= 0 && rsx && pad_idx < RSX_MAX_NOTE_PADS) {
            NoteTriggerPad* pad = &rsx->pads[pad_idx];
            if (pad->note >= 0 && pad->enabled) {
                // Determine which synth to use based on pad's program setting
                // If pad has no program set, use device-specific midi_target_program when enabled, else current_program
                sfizz_synth_t* target_synth = nullptr;
                if (pad->program >= 0 && pad->program < rsx->num_programs && program_synths[pad->program]) {
                    target_synth = program_synths[pad->program];
                } else {
                    int target_prog = config.midi_program_change_enabled[device_id] ? midi_target_program[device_id] : current_program;
                    target_synth = program_synths[target_prog];
                }

                // Send note off for the pad's configured note
                std::lock_guard<std::mutex> lock(synth_mutex);
                if (target_synth) sfizz_send_note_off(target_synth, 0, pad->note, 0);
                return;  // Handled by pad
            }
        }

        // If not mapped to a pad, send note off to the appropriate synth
        // Use device-specific midi_target_program if program change is enabled, otherwise use current UI selection
        std::lock_guard<std::mutex> lock(synth_mutex);
        int target_prog = config.midi_program_change_enabled[device_id] ? midi_target_program[device_id] : current_program;
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
            if (effects && mixer.program_fx_routes[i] == 1) {
                std::vector<int16_t> int16_buf(frames * 2);
                for (int j = 0; j < frames; j++) {
                    int16_buf[j * 2] = static_cast<int16_t>(prog_left[j] * 32767.0f);
                    int16_buf[j * 2 + 1] = static_cast<int16_t>(prog_right[j] * 32767.0f);
                }
                regroove_effects_process(effects, int16_buf.data(), frames, 44100);
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
    float playback_vol = mixer.playback_mute ? 0.0f : mixer.playback_volume;
    float playback_pan = mixer.playback_pan;
    float playback_left_gain = playback_vol * (1.0f - playback_pan);
    float playback_right_gain = playback_vol * playback_pan;

    for (int i = 0; i < frames; i++) {
        left[i] *= playback_left_gain;
        right[i] *= playback_right_gain;
    }

    // Apply effects if routed to playback
    if (effects && mixer.fx_route == FX_ROUTE_PLAYBACK) {
        // Convert float to int16 for effects
        std::vector<int16_t> int16_buf(frames * 2);
        for (int i = 0; i < frames; i++) {
            int16_buf[i * 2] = static_cast<int16_t>(left[i] * 32767.0f);
            int16_buf[i * 2 + 1] = static_cast<int16_t>(right[i] * 32767.0f);
        }

        regroove_effects_process(effects, int16_buf.data(), frames, 44100);

        // Convert back to float
        for (int i = 0; i < frames; i++) {
            left[i] = int16_buf[i * 2] / 32767.0f;
            right[i] = int16_buf[i * 2 + 1] / 32767.0f;
        }
    }

    // Apply master volume and pan
    float master_vol = mixer.master_mute ? 0.0f : mixer.master_volume;
    float master_pan = mixer.master_pan;
    float master_left_gain = master_vol * (1.0f - master_pan);
    float master_right_gain = master_vol * master_pan;

    for (int i = 0; i < frames; i++) {
        left[i] *= master_left_gain;
        right[i] *= master_right_gain;
    }

    // Apply effects if routed to master
    if (effects && mixer.fx_route == FX_ROUTE_MASTER) {
        // Convert float to int16 for effects
        std::vector<int16_t> int16_buf(frames * 2);
        for (int i = 0; i < frames; i++) {
            int16_buf[i * 2] = static_cast<int16_t>(left[i] * 32767.0f);
            int16_buf[i * 2 + 1] = static_cast<int16_t>(right[i] * 32767.0f);
        }

        regroove_effects_process(effects, int16_buf.data(), frames, 44100);

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

int main(int argc, char* argv[]) {
    // Check for SFZ or RSX file argument
    const char* sfz_file = "assets/example.sfz";  // default
    std::string sfz_filename = "example.sfz";  // Just the filename for display
    bool is_rsx = false;

    if (argc > 1) {
        const char* input_file = argv[1];
        size_t len = strlen(input_file);

        // Check if file ends with .rsx
        if (len > 4 && strcmp(input_file + len - 4, ".rsx") == 0) {
            is_rsx = true;

            // Load RSX file
            rsx = samplecrate_rsx_create();
            if (samplecrate_rsx_load(rsx, input_file) == 0) {
                rsx_file_path = input_file;  // Store RSX path for saving later
                std::cout << "Loaded RSX file: " << input_file << std::endl;

                // Use programs if defined, otherwise fall back to legacy sfz_file
                const char* sfz_to_load = nullptr;
                if (rsx->num_programs > 0 && rsx->program_files[0][0] != '\0') {
                    sfz_to_load = rsx->program_files[0];  // Load program 1 (scene 1) by default
                    std::cout << "  Programs: " << rsx->num_programs << std::endl;
                    for (int i = 0; i < rsx->num_programs; i++) {
                        if (rsx->program_files[i][0] != '\0') {
                            std::cout << "    Program " << (i+1) << ": " << rsx->program_files[i] << std::endl;
                        }
                    }
                } else {
                    sfz_to_load = rsx->sfz_file;  // Legacy single SFZ file
                    std::cout << "  SFZ file: " << rsx->sfz_file << std::endl;
                }
                std::cout << "  Note pads: " << rsx->num_pads << std::endl;

                // Get full SFZ path
                char sfz_path[512];
                samplecrate_rsx_get_sfz_path(input_file, sfz_to_load, sfz_path, sizeof(sfz_path));
                sfz_file = strdup(sfz_path);  // Make a copy

                // Extract filename for display
                const char* last_slash = strrchr(sfz_to_load, '/');
                if (last_slash) {
                    sfz_filename = last_slash + 1;
                } else {
                    sfz_filename = sfz_to_load;
                }
            } else {
                std::cerr << "Failed to load RSX file: " << input_file << std::endl;
                samplecrate_rsx_destroy(rsx);
                rsx = nullptr;
            }
        } else {
            // Direct SFZ file
            sfz_file = input_file;
            // Extract just the filename from the path
            const char* last_slash = strrchr(input_file, '/');
            if (last_slash) {
                sfz_filename = last_slash + 1;
            } else {
                sfz_filename = input_file;
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

    SDL_Window* window = SDL_CreateWindow("samplecrate", 100, 100, 1200, 640, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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

    // Initialize mixer and effects
    samplecrate_mixer_init(&mixer);
    samplecrate_config_init(&config);
    samplecrate_config_load(&config, "samplecrate.ini");

    // Load expanded pads setting from config
    expanded_pads = (config.expanded_pads != 0);

    // Apply config defaults to mixer
    mixer.master_volume = config.default_master_volume;
    mixer.master_pan = config.default_master_pan;
    mixer.playback_volume = config.default_playback_volume;
    mixer.playback_pan = config.default_playback_pan;

    // Create effects and apply defaults from config
    effects = regroove_effects_create();
    if (effects) {
        regroove_effects_set_distortion_drive(effects, config.fx_distortion_drive);
        regroove_effects_set_distortion_mix(effects, config.fx_distortion_mix);
        regroove_effects_set_filter_cutoff(effects, config.fx_filter_cutoff);
        regroove_effects_set_filter_resonance(effects, config.fx_filter_resonance);
        regroove_effects_set_eq_low(effects, config.fx_eq_low);
        regroove_effects_set_eq_mid(effects, config.fx_eq_mid);
        regroove_effects_set_eq_high(effects, config.fx_eq_high);
        regroove_effects_set_compressor_threshold(effects, config.fx_compressor_threshold);
        regroove_effects_set_compressor_ratio(effects, config.fx_compressor_ratio);
        regroove_effects_set_compressor_attack(effects, config.fx_compressor_attack);
        regroove_effects_set_compressor_release(effects, config.fx_compressor_release);
        regroove_effects_set_compressor_makeup(effects, config.fx_compressor_makeup);
        regroove_effects_set_phaser_rate(effects, config.fx_phaser_rate);
        regroove_effects_set_phaser_depth(effects, config.fx_phaser_depth);
        regroove_effects_set_phaser_feedback(effects, config.fx_phaser_feedback);
        regroove_effects_set_reverb_room_size(effects, config.fx_reverb_room_size);
        regroove_effects_set_reverb_damping(effects, config.fx_reverb_damping);
        regroove_effects_set_reverb_mix(effects, config.fx_reverb_mix);
        regroove_effects_set_delay_time(effects, config.fx_delay_time);
        regroove_effects_set_delay_feedback(effects, config.fx_delay_feedback);
        regroove_effects_set_delay_mix(effects, config.fx_delay_mix);
    }

    // Init sfizz - create main synth and program synths
    synth = sfizz_create_synth();
    sfizz_set_sample_rate(synth, 44100);
    sfizz_set_samples_per_block(synth, 512);

    // If RSX has multiple programs, load them all into separate synth instances
    if (rsx && rsx->num_programs > 0) {
        std::cout << "Loading " << rsx->num_programs << " programs into separate synth instances" << std::endl;

        for (int i = 0; i < rsx->num_programs; i++) {
            if (rsx->program_files[i][0] == '\0') continue;

            // Get full SFZ path
            char sfz_path[512];
            samplecrate_rsx_get_sfz_path(rsx_file_path.c_str(), rsx->program_files[i], sfz_path, sizeof(sfz_path));

            // Create synth instance for this program
            program_synths[i] = sfizz_create_synth();
            sfizz_set_sample_rate(program_synths[i], 44100);
            sfizz_set_samples_per_block(program_synths[i], 512);

            std::cout << "Program " << (i + 1) << " (" << rsx->program_files[i] << "): loading..." << std::endl;
            if (!sfizz_load_file(program_synths[i], sfz_path)) {
                std::cerr << "ERROR: Failed to load program " << (i + 1) << ": " << sfz_path << std::endl;
                error_message = "Failed to load\nProgram " + std::to_string(i + 1);
                sfizz_free(program_synths[i]);
                program_synths[i] = nullptr;
            } else {
                int num_regions = sfizz_get_num_regions(program_synths[i]);
                std::cout << "  SUCCESS: Loaded " << num_regions << " regions" << std::endl;

                if (num_regions == 0) {
                    std::cerr << "  WARNING: No regions loaded for program " << (i + 1) << std::endl;
                    error_message = "No samples in\nProgram " + std::to_string(i + 1);
                }
            }
        }

        // Set main synth to point to first program synth
        if (program_synths[0]) {
            synth = program_synths[0];
            current_program = 0;
            midi_target_program[0] = 0;  // Initialize MIDI routing to first program
            midi_target_program[1] = 0;
            error_message = "";  // Clear error if first program loaded successfully
        }

        // Apply program volume and pan settings from RSX
        for (int i = 0; i < rsx->num_programs; i++) {
            mixer.program_volumes[i] = rsx->program_volumes[i];
            mixer.program_pans[i] = rsx->program_pans[i];
            std::cout << "Applied program " << (i + 1) << " settings: volume=" << mixer.program_volumes[i]
                      << " pan=" << mixer.program_pans[i] << std::endl;
        }
    } else {
        // No programs defined, load single SFZ file into main synth
        std::cout << "Loading SFZ file into sfizz: " << sfz_file << std::endl;
        if (!sfizz_load_file(synth, sfz_file)) {
            std::cerr << "ERROR: Failed to load SFZ file: " << sfz_file << std::endl;
            std::cerr << "Check that the file exists and paths are correct" << std::endl;
            error_message = "Failed to load:\n" + std::string(sfz_file);
        } else {
            std::cout << "SUCCESS: SFZ file loaded: " << sfz_file << std::endl;
            int num_regions = sfizz_get_num_regions(synth);
            std::cout << "Number of regions loaded: " << num_regions << std::endl;

            if (num_regions == 0) {
                std::cerr << "WARNING: No regions loaded! This usually means:" << std::endl;
                std::cerr << "  - Sample files referenced in the SFZ couldn't be found" << std::endl;
                std::cerr << "  - Sample paths are relative to the SFZ file location" << std::endl;
                std::cerr << "  - Check that sample files exist relative to: " << sfz_file << std::endl;
                error_message = "No samples loaded\nCheck SFZ paths";
            }
        }
    }

    // Init SDL audio
    SDL_AudioSpec spec, obtained;
    spec.freq = 44100;
    spec.format = AUDIO_F32SYS;
    spec.channels = 2;
    spec.samples = 512;
    spec.callback = audioCallback;

    if (SDL_OpenAudio(&spec, &obtained) < 0) {
        std::cerr << "Failed to open audio: " << SDL_GetError() << std::endl;
    } else {
        std::cout << "Audio opened successfully" << std::endl;
        std::cout << "Sample rate: " << obtained.freq << " Hz" << std::endl;
        std::cout << "Channels: " << (int)obtained.channels << std::endl;
        std::cout << "Buffer size: " << obtained.samples << " samples" << std::endl;
    }
    SDL_PauseAudio(0);

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
        // If not configured (-1), use port 0 for first device
        midi_device_ports[0] = config.midi_device_0;
        midi_device_ports[1] = config.midi_device_1;

        // If first device not configured, default to port 0
        if (midi_device_ports[0] == -1 && num_midi_ports > 0) {
            midi_device_ports[0] = 0;
            std::cout << "MIDI device 0 not configured, defaulting to port 0" << std::endl;
        }

        if (midi_init_multi(midi_event_callback, nullptr, midi_device_ports, MIDI_MAX_DEVICES) == 0) {
            std::cout << "MIDI initialized successfully" << std::endl;
            if (midi_device_ports[0] >= 0) {
                std::cout << "  Device 0: port " << midi_device_ports[0] << std::endl;
            }
            if (midi_device_ports[1] >= 0) {
                std::cout << "  Device 1: port " << midi_device_ports[1] << std::endl;
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

        // Update note pad fade effects
        for (int i = 0; i < RSX_MAX_NOTE_PADS; i++) {
            if (note_pad_fade[i] > 0.0f) {
                note_pad_fade[i] -= 0.02f;
                if (note_pad_fade[i] < 0.0f) note_pad_fade[i] = 0.0f;
            }
        }

        // Main window
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("samplecrate", nullptr, rootFlags);

        // Layout constants (matching mock-ui.cpp)
        const float SIDE_MARGIN = 10.0f;
        const float TOP_MARGIN = 8.0f;
        const float LEFT_PANEL_WIDTH = 190.0f;
        const float LCD_HEIGHT = 90.0f;
        const float BOTTOM_MARGIN = 8.0f;

        float fullW = io.DisplaySize.x;
        float fullH = io.DisplaySize.y;

        // Calculate panel heights accounting for padding and borders (like mock-ui.cpp)
        ImGuiStyle& style = ImGui::GetStyle();
        float childPaddingY = style.WindowPadding.y * 2.0f;
        float childBorderY = style.ChildBorderSize * 2.0f;

        // No sequencer, so just account for top and bottom margins
        float channelAreaHeight = fullH - TOP_MARGIN - BOTTOM_MARGIN - childPaddingY - childBorderY;
        float leftPanelHeight = channelAreaHeight;

        // LEFT PANEL (hidden in fullscreen pads mode)
        if (!fullscreen_pads_mode) {
            ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, TOP_MARGIN));
            ImGui::BeginChild("left_panel", ImVec2(LEFT_PANEL_WIDTH, leftPanelHeight),
                              false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
            // Update LCD display with current state
            if (lcd_display) {
                char lcd_text[256];

                // Show error if present
                if (!error_message.empty()) {
                    snprintf(lcd_text, sizeof(lcd_text),
                             "ERROR:\n%s",
                             error_message.c_str());
                }
                // Show program information if RSX loaded
                else if (rsx && rsx->num_programs > 0) {
                    // Use program name if available, otherwise show program number
                    const char* prog_display = rsx->program_names[current_program][0] != '\0'
                        ? rsx->program_names[current_program]
                        : "";

                    if (current_note >= 0) {
                        if (prog_display[0] != '\0') {
                            snprintf(lcd_text, sizeof(lcd_text),
                                     "Prg %d/%d: %s\nNote: %d Vel: %d",
                                     current_program + 1, rsx->num_programs,
                                     prog_display, current_note, current_velocity);
                        } else {
                            snprintf(lcd_text, sizeof(lcd_text),
                                     "Prg %d/%d\nNote: %d Vel: %d",
                                     current_program + 1, rsx->num_programs,
                                     current_note, current_velocity);
                        }
                    } else {
                        if (prog_display[0] != '\0') {
                            snprintf(lcd_text, sizeof(lcd_text),
                                     "Prg %d/%d: %s\n[Ready]",
                                     current_program + 1, rsx->num_programs,
                                     prog_display);
                        } else {
                            snprintf(lcd_text, sizeof(lcd_text),
                                     "Prg %d/%d\n[Ready]",
                                     current_program + 1, rsx->num_programs);
                        }
                    }
                } else {
                    // No programs, show simple display
                    if (current_note >= 0) {
                        snprintf(lcd_text, sizeof(lcd_text),
                                 "File: %s\n\nNote: %d\nVelocity: %d",
                                 sfz_filename.c_str(), current_note, current_velocity);
                    } else {
                        snprintf(lcd_text, sizeof(lcd_text),
                                 "File: %s\n\n[Ready]",
                                 sfz_filename.c_str());
                    }
                }
                lcd_write(lcd_display, lcd_text);

                // Draw LCD
                DrawLCD(lcd_get_buffer(lcd_display), LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // Program selection buttons (if RSX with programs loaded) - at top under LCD
            if (rsx && rsx->num_programs > 1) {
                const float PROG_BUTTON_SIZE = 48.0f;
                ImGui::Text("PROGRAM:");
                ImGui::Spacing();

                // P- button (disabled if at first program)
                bool at_first = (current_program == 0);
                if (at_first) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
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
                ImVec4 p1Col = (current_program == 0) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, p1Col);
                if (ImGui::Button("P1", ImVec2(PROG_BUTTON_SIZE, PROG_BUTTON_SIZE))) {
                    switch_program(0);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();

                // P+ button (disabled if at last program)
                bool at_last = (current_program == rsx->num_programs - 1);
                if (at_last) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.17f, 0.18f, 1.0f));
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
            const float BUTTON_SIZE = 48.0f;
            ImVec4 fxCol = (ui_mode == UI_MODE_EFFECTS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
            if (ImGui::Button("FX", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_EFFECTS;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImVec4 mixCol = (ui_mode == UI_MODE_MIX) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, mixCol);
            if (ImGui::Button("MIX", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_MIX;
            }
            ImGui::PopStyleColor();

            ImVec4 crateCol = (ui_mode == UI_MODE_INSTRUMENT) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, crateCol);
            if (ImGui::Button("CRATE", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_INSTRUMENT;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImVec4 padsCol = (ui_mode == UI_MODE_PADS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, padsCol);
            if (ImGui::Button("PADS", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_PADS;
            }
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 16.0f));

            // Additional mode buttons (second row)
            // LEARN button
            ImVec4 learnCol = learn_mode_active ? ImVec4(0.90f, 0.15f, 0.18f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
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
            ImVec4 midiCol = (ui_mode == UI_MODE_MIDI) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, midiCol);
            if (ImGui::Button("MIDI", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_MIDI;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Application  settings
            ImVec4 settingsCol = (ui_mode == UI_MODE_SETTINGS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, settingsCol);
            if (ImGui::Button("SETUP", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                ui_mode = UI_MODE_SETTINGS;
            }
            ImGui::PopStyleColor();

            }
            ImGui::EndChild();
        }

        // RIGHT PANEL - Switch based on mode
        // In fullscreen pads mode, use full width
        float rightX = fullscreen_pads_mode ? 0.0f : (SIDE_MARGIN + LEFT_PANEL_WIDTH + SIDE_MARGIN);
        float rightW = fullscreen_pads_mode ? fullW : (fullW - rightX - SIDE_MARGIN);

        // Calculate dynamic slider width and spacing based on available width (like mock-ui.cpp)
        const float BASE_SLIDER_W = 44.0f;
        const float BASE_SPACING = 26.0f;
        float baseTotal = BASE_SLIDER_W * 9.0f + BASE_SPACING * 8.0f;  // Assumes 9 channels worth of space
        float widthScale = rightW / baseTotal;
        if (widthScale > 1.40f) widthScale = 1.40f;  // Cap maximum scale
        if (widthScale < 1.0f) widthScale = 1.0f;    // Don't scale smaller than base
        float sliderW = BASE_SLIDER_W * widthScale;
        float spacing = BASE_SPACING * widthScale;

        // Use channelAreaHeight for right panel (same as mock-ui.cpp)
        ImGui::SetCursorPos(ImVec2(rightX, TOP_MARGIN));
        ImGui::BeginChild("right_panel", ImVec2(rightW, channelAreaHeight), false, ImGuiWindowFlags_NoScrollbar);
        {
            float labelH = ImGui::GetTextLineHeight();
            float contentHeight = channelAreaHeight - childPaddingY;
            float panSliderH = 20.0f;  // Height for horizontal pan slider
            const float SOLO_SIZE = 34.0f;
            const float MUTE_SIZE = 34.0f;

            // Calculate space needed above and below the slider
            // Above: Title (labelH) + spacing (4) + solo/fx button (SOLO_SIZE) + spacing (2) + pan slider (panSliderH + labelH) + spacing (2)
            float sliderTop = labelH + 4.0f + SOLO_SIZE + 2.0f + panSliderH + labelH + 2.0f;
            // Below: spacing (8) + mute button (MUTE_SIZE) + bottom margin (12)
            float bottomStack = 8.0f + MUTE_SIZE + 12.0f;
            // Available height for slider (with some padding for panel border/padding)
            float sliderH = leftPanelHeight - sliderTop - bottomStack - 30.0f;
            if (sliderH < 200.0f) sliderH = 200.0f;  // Minimum height
            if (sliderH > 400.0f) sliderH = 400.0f;  // Maximum height to prevent running off screen

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

                        // File input with label
                        ImGui::Text("File:");
                        ImGui::SameLine(80);
                        char file_label[32];
                        snprintf(file_label, sizeof(file_label), "##prog%d_file", i);
                        ImGui::PushItemWidth(300);
                        if (ImGui::InputText(file_label, rsx->program_files[i], sizeof(rsx->program_files[i]))) {
                            // Autosave on change
                            if (!rsx_file_path.empty()) {
                                samplecrate_rsx_save(rsx, rsx_file_path.c_str());
                            }
                        }
                        ImGui::PopItemWidth();

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

                    // Add program button
                    if (rsx->num_programs < RSX_MAX_PROGRAMS) {
                        if (ImGui::Button("Add Program")) {
                            // Add a new empty program
                            rsx->program_files[rsx->num_programs][0] = '\0';
                            rsx->program_names[rsx->num_programs][0] = '\0';
                            rsx->program_volumes[rsx->num_programs] = 1.0f;  // Default volume (100%)
                            rsx->program_pans[rsx->num_programs] = 0.5f;     // Center pan
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
                            bool is_configured = (rsx->pads[pad_idx].note >= 0);

                            ImVec4 btn_col = is_selected ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) :
                                            (is_configured ? ImVec4(0.26f, 0.27f, 0.30f, 1.0f) :
                                                            ImVec4(0.16f, 0.17f, 0.18f, 1.0f));

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
                                sfizz_send_note_on(target_synth, 0, pad->note, velocity);
                                note_pad_fade[selected_pad] = 1.0f;
                            }
                        }
                    }

                    // Show autosave status
                    if (!rsx_file_path.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "[Autosave]");
                    }
                }
            }
            else if (ui_mode == UI_MODE_MIX) {
                // MIX MODE: Master and playback mixing
                ImGui::Text("MIX");
                ImGui::Separator();
                ImGui::Spacing();

                // sliderW, spacing, sliderH, SOLO_SIZE, MUTE_SIZE are all calculated above

                ImVec2 origin = ImGui::GetCursorScreenPos();
                int col_index = 0;

                // MASTER channel
                {
                    float colX = origin.x + col_index * (sliderW + spacing);
                    ImGui::SetCursorScreenPos(ImVec2(colX, origin.y));
                    ImGui::BeginGroup();
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "MASTER");
                    ImGui::Dummy(ImVec2(0, 4.0f));

                    // FX routing button (mutually exclusive with all other FX routes)
                    ImVec4 fxCol = (mixer.fx_route == FX_ROUTE_MASTER) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
                    if (ImGui::Button("FX##master_fx", ImVec2(sliderW, SOLO_SIZE))) {
                        if (mixer.fx_route == FX_ROUTE_MASTER) {
                            mixer.fx_route = FX_ROUTE_NONE;
                        } else {
                            mixer.fx_route = FX_ROUTE_MASTER;
                            // Disable per-program FX
                            for (int i = 0; i < 4; i++) {
                                mixer.program_fx_routes[i] = 0;
                            }
                        }
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
                    ImVec4 muteCol = mixer.master_mute ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
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
                        ImGui::SetCursorScreenPos(ImVec2(colX, origin.y));
                        ImGui::BeginGroup();

                        // Program name or number
                        if (rsx->program_names[i][0] != '\0') {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", rsx->program_names[i]);
                        } else {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "PRG %d", i + 1);
                        }
                        ImGui::Dummy(ImVec2(0, 4.0f));

                        // FX routing button (mutually exclusive with all other FX routes)
                        char fx_id[32];
                        snprintf(fx_id, sizeof(fx_id), "FX##prog%d_fx", i);
                        ImVec4 fxCol = (mixer.program_fx_routes[i] == 1) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
                        if (ImGui::Button(fx_id, ImVec2(sliderW, SOLO_SIZE))) {
                            if (mixer.program_fx_routes[i] == 1) {
                                mixer.program_fx_routes[i] = 0;
                            } else {
                                // Disable all other FX routes (mutex)
                                mixer.fx_route = FX_ROUTE_NONE;
                                for (int j = 0; j < 4; j++) {
                                    mixer.program_fx_routes[j] = 0;
                                }
                                // Enable this program's FX
                                mixer.program_fx_routes[i] = 1;
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 2.0f));

                        // Pan slider
                        char pan_id[32];
                        snprintf(pan_id, sizeof(pan_id), "##prog%d_pan", i);
                        ImGui::PushItemWidth(sliderW);
                        if (ImGui::SliderFloat(pan_id, &mixer.program_pans[i], 0.0f, 1.0f, "")) {
                            // Could add MIDI learn here later
                        }
                        ImGui::PopItemWidth();
                        ImGui::Dummy(ImVec2(0, 2.0f));

                        // Volume fader
                        char slider_id[32];
                        snprintf(slider_id, sizeof(slider_id), "##prog%d_vol", i);
                        if (ImGui::VSliderFloat(slider_id, ImVec2(sliderW, sliderH), &mixer.program_volumes[i], 0.0f, 1.0f, "")) {
                            // Could add MIDI learn here later
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));

                        // Mute button
                        char mute_id[32];
                        snprintf(mute_id, sizeof(mute_id), "M##prog%d_mute", i);
                        ImVec4 muteCol = mixer.program_mutes[i] ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
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
                    ImGui::SetCursorScreenPos(ImVec2(colX, origin.y));
                    ImGui::BeginGroup();
                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "PLAYBACK");
                    ImGui::Dummy(ImVec2(0, 4.0f));

                    // FX routing button (mutually exclusive with all other FX routes)
                    ImVec4 fxCol = (mixer.fx_route == FX_ROUTE_PLAYBACK) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
                    if (ImGui::Button("FX##playback_fx", ImVec2(sliderW, SOLO_SIZE))) {
                        if (mixer.fx_route == FX_ROUTE_PLAYBACK) {
                            mixer.fx_route = FX_ROUTE_NONE;
                        } else {
                            mixer.fx_route = FX_ROUTE_PLAYBACK;
                            // Disable per-program FX
                            for (int i = 0; i < 4; i++) {
                                mixer.program_fx_routes[i] = 0;
                            }
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2.0f));

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
                    ImVec4 muteCol = mixer.playback_mute ? ImVec4(0.80f, 0.20f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
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
                if (!effects) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Effects system not initialized");
                } else {
                    const float fx_spacing = 16.0f;
                    // sliderW, spacing, sliderH, SOLO_SIZE, MUTE_SIZE are all calculated above

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
                        ImVec4 enCol = dist_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##dist_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_DISTORTION_TOGGLE);
                            } else {
                                regroove_effects_set_distortion_enabled(effects, !dist_en);
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float drive = regroove_effects_get_distortion_drive(effects);
                        if (ImGui::VSliderFloat("##fx_drive", ImVec2(sliderW, sliderH), &drive, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DISTORTION_DRIVE);
                            } else {
                                regroove_effects_set_distortion_drive(effects, drive);
                                config.fx_distortion_drive = drive;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##dist_drive_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_distortion_drive(effects, 0.5f);
                            config.fx_distortion_drive = 0.5f;
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
                        if (ImGui::VSliderFloat("##fx_dist_mix", ImVec2(sliderW, sliderH), &mix, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DISTORTION_MIX);
                            } else {
                                regroove_effects_set_distortion_mix(effects, mix);
                                config.fx_distortion_mix = mix;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##dist_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_distortion_mix(effects, 0.0f);
                            config.fx_distortion_mix = 0.0f;
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
                        ImVec4 enCol = filt_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##filt_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_FILTER_TOGGLE);
                            } else {
                                regroove_effects_set_filter_enabled(effects, !filt_en);
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float cutoff = regroove_effects_get_filter_cutoff(effects);
                        if (ImGui::VSliderFloat("##fx_cutoff", ImVec2(sliderW, sliderH), &cutoff, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_FILTER_CUTOFF);
                            } else {
                                regroove_effects_set_filter_cutoff(effects, cutoff);
                                config.fx_filter_cutoff = cutoff;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##filt_cutoff_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_filter_cutoff(effects, 1.0f);
                            config.fx_filter_cutoff = 1.0f;
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
                        if (ImGui::VSliderFloat("##fx_reso", ImVec2(sliderW, sliderH), &reso, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_FILTER_RESONANCE);
                            } else {
                                regroove_effects_set_filter_resonance(effects, reso);
                                config.fx_filter_resonance = reso;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##filt_reso_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_filter_resonance(effects, 0.0f);
                            config.fx_filter_resonance = 0.0f;
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
                        ImVec4 enCol = eq_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##eq_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_EQ_TOGGLE);
                            } else {
                                regroove_effects_set_eq_enabled(effects, !eq_en);
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float eq_low = regroove_effects_get_eq_low(effects);
                        if (ImGui::VSliderFloat("##fx_eq_low", ImVec2(sliderW, sliderH), &eq_low, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_LOW);
                            } else {
                                regroove_effects_set_eq_low(effects, eq_low);
                                config.fx_eq_low = eq_low;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_low_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_low(effects, 0.5f);
                            config.fx_eq_low = 0.5f;
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
                        if (ImGui::VSliderFloat("##fx_eq_mid", ImVec2(sliderW, sliderH), &eq_mid, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_MID);
                            } else {
                                regroove_effects_set_eq_mid(effects, eq_mid);
                                config.fx_eq_mid = eq_mid;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_mid_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_mid(effects, 0.5f);
                            config.fx_eq_mid = 0.5f;
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
                        if (ImGui::VSliderFloat("##fx_eq_high", ImVec2(sliderW, sliderH), &eq_high, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_EQ_HIGH);
                            } else {
                                regroove_effects_set_eq_high(effects, eq_high);
                                config.fx_eq_high = eq_high;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##eq_high_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_eq_high(effects, 0.5f);
                            config.fx_eq_high = 0.5f;
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
                        ImVec4 enCol = comp_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##comp_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_TOGGLE);
                            } else {
                                regroove_effects_set_compressor_enabled(effects, !comp_en);
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float thresh = regroove_effects_get_compressor_threshold(effects);
                        if (ImGui::VSliderFloat("##fx_comp_thresh", ImVec2(sliderW, sliderH), &thresh, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_THRESHOLD);
                            } else {
                                regroove_effects_set_compressor_threshold(effects, thresh);
                                config.fx_compressor_threshold = thresh;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##comp_thresh_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_compressor_threshold(effects, 0.8f);
                            config.fx_compressor_threshold = 0.8f;
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
                        if (ImGui::VSliderFloat("##fx_comp_ratio", ImVec2(sliderW, sliderH), &ratio, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_COMPRESSOR_RATIO);
                            } else {
                                regroove_effects_set_compressor_ratio(effects, ratio);
                                config.fx_compressor_ratio = ratio;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##comp_ratio_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_compressor_ratio(effects, 0.2f);
                            config.fx_compressor_ratio = 0.2f;
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
                        ImVec4 enCol = delay_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                        if (ImGui::Button("E##delay_en", ImVec2(sliderW, SOLO_SIZE))) {
                            if (learn_mode_active) {
                                start_learn_for_action(ACTION_FX_DELAY_TOGGLE);
                            } else {
                                regroove_effects_set_delay_enabled(effects, !delay_en);
                            }
                        }
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 6.0f));

                        float delay_time = regroove_effects_get_delay_time(effects);
                        if (ImGui::VSliderFloat("##fx_delay_time", ImVec2(sliderW, sliderH), &delay_time, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_TIME);
                            } else {
                                regroove_effects_set_delay_time(effects, delay_time);
                                config.fx_delay_time = delay_time;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_time_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_time(effects, 0.3f);
                            config.fx_delay_time = 0.3f;
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
                        if (ImGui::VSliderFloat("##fx_delay_fb", ImVec2(sliderW, sliderH), &feedback, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_FEEDBACK);
                            } else {
                                regroove_effects_set_delay_feedback(effects, feedback);
                                config.fx_delay_feedback = feedback;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_fb_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_feedback(effects, 0.3f);
                            config.fx_delay_feedback = 0.3f;
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
                        if (ImGui::VSliderFloat("##fx_delay_mix", ImVec2(sliderW, sliderH), &delay_mix, 0.0f, 1.0f, "")) {
                            if (learn_mode_active && ImGui::IsItemActive()) {
                                start_learn_for_action(ACTION_FX_DELAY_MIX);
                            } else {
                                regroove_effects_set_delay_mix(effects, delay_mix);
                                config.fx_delay_mix = delay_mix;
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 8.0f));
                        if (ImGui::Button("R##delay_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                            regroove_effects_set_delay_mix(effects, 0.0f);
                            config.fx_delay_mix = 0.0f;
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

                ImVec2 origin = ImGui::GetCursorPos();

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
                if (padSize > 140.0f) padSize = 140.0f; // Max pad size
                if (padSize < 60.0f) padSize = 60.0f;   // Min pad size

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
                        bool pad_configured = (pad && pad->note >= 0 && pad->enabled);

                        // Pad button with fade effect
                        float fade = note_pad_fade[pad_idx];
                        ImVec4 pad_col;
                        if (fade > 0.0f) {
                            // Active (red with fade)
                            pad_col = ImVec4(0.90f * fade + 0.26f * (1.0f - fade),
                                           0.15f * fade + 0.27f * (1.0f - fade),
                                           0.18f * fade + 0.30f * (1.0f - fade),
                                           1.0f);
                        } else if (!pad_configured) {
                            // Not configured (darker gray)
                            pad_col = ImVec4(0.16f, 0.17f, 0.18f, 1.0f);
                        } else {
                            // Configured but inactive
                            pad_col = ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                        }

                        ImGui::PushStyleColor(ImGuiCol_Button, pad_col);
                        ImGui::PushID(pad_idx);

                        // Pad label
                        char pad_label[128];
                        if (pad_configured) {
                            // Show program assignment if pad is assigned to a specific program
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
                        } else {
                            snprintf(pad_label, sizeof(pad_label), "N%d\n\n[Not Set]", pad_idx + 1);
                        }

                        if (ImGui::Button(pad_label, ImVec2(padSize, padSize))) {
                            if (learn_mode_active) {
                                // Enter learn mode for this pad
                                start_learn_for_action(ACTION_TRIGGER_NOTE_PAD, pad_idx);
                            } else if (pad_configured) {
                                // Trigger note
                                int velocity = pad->velocity > 0 ? pad->velocity : 100;

                                // Determine which synth to use based on pad's program setting
                                // For UI-clicked pads, use current_program as default (no device context)
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

                        ImGui::PopID();
                        ImGui::PopStyleColor();
                    }
                }

                // Add vertical bar on left side to toggle fullscreen mode (drawn after pads)
                // Save current cursor position to restore after drawing bar
                float barWidth = 12.0f;
                ImVec2 cursorBeforeBar = ImGui::GetCursorPos();

                // Bar should span from origin.y to the available height
                float barHeight = contentHeight - origin.y;

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
            else if (ui_mode == UI_MODE_MIDI) {
                // MIDI MODE: MIDI device configuration
                ImGui::Text("MIDI DEVICE CONFIGURATION");
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

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // MIDI Program Change enable/disable (per-device)
                ImGui::Text("MIDI Program Change:");

                // Device 0
                bool pc_enabled_0 = (config.midi_program_change_enabled[0] != 0);
                if (ImGui::Checkbox("Device 0: Respond to Program Change", &pc_enabled_0)) {
                    config.midi_program_change_enabled[0] = pc_enabled_0 ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }

                // Device 1
                bool pc_enabled_1 = (config.midi_program_change_enabled[1] != 0);
                if (ImGui::Checkbox("Device 1: Respond to Program Change", &pc_enabled_1)) {
                    config.midi_program_change_enabled[1] = pc_enabled_1 ? 1 : 0;
                    samplecrate_config_save(&config, "samplecrate.ini");
                }

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "When disabled: UI selection leads. When enabled: routes per program change");

                ImGui::Spacing();
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
                    ImGui::Text("Use LEARN mode to create new MIDI mappings");
                }
            }
            else if (ui_mode == UI_MODE_SETTINGS) {
                // SETTINGS MODE: Audio device configuration
                ImGui::Text("AUDIO SETTINGS");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Sample Rate: %d Hz", obtained.freq);
                ImGui::Text("Channels: %d", (int)obtained.channels);
                ImGui::Text("Buffer Size: %d samples", obtained.samples);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Audio device selection requires restart");
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Current device: SDL default");
            }
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
    SDL_CloseAudio();

    // Safely destroy synths
    {
        std::lock_guard<std::mutex> lock(synth_mutex);

        // If we loaded multiple programs, free all program synths
        if (rsx && rsx->num_programs > 0) {
            for (int i = 0; i < rsx->num_programs; i++) {
                if (program_synths[i]) {
                    sfizz_free(program_synths[i]);
                    program_synths[i] = nullptr;
                }
            }
            synth = nullptr;  // Was pointing to one of the program synths
        } else {
            // Single synth mode
            if (synth) {
                sfizz_free(synth);
                synth = nullptr;
            }
        }
    }

    // Cleanup MIDI and input mappings
    midi_deinit();
    if (input_mappings) {
        input_mappings_destroy(input_mappings);
    }
    if (rsx) {
        samplecrate_rsx_destroy(rsx);
    }
    if (lcd_display) {
        lcd_destroy(lcd_display);
    }
    if (effects) {
        regroove_effects_destroy(effects);
    }
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
