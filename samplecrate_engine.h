#ifndef SAMPLECRATE_ENGINE_H
#define SAMPLECRATE_ENGINE_H

#include <sfizz.h>
#include "samplecrate_rsx.h"
#include "midi_file_pad_player.h"
#include "regroove_effects.h"
#include "samplecrate_common.h"
#include <string>

// Forward declarations
struct MednessSequencer;

// Engine state structure
typedef struct {
    // RSX file and path
    SamplecrateRSX* rsx;
    std::string rsx_file_path;

    // Synths
    sfizz_synth_t* synth;                        // Legacy/main synth
    sfizz_synth_t* program_synths[RSX_MAX_PROGRAMS];  // Per-program synths

    // MIDI file player
    MidiFilePadPlayer* midi_pad_player;
    int midi_pad_indices[RSX_MAX_NOTE_PADS];

    // Effects
    RegrooveEffects* effects_master;
    RegrooveEffects* effects_program[RSX_MAX_PROGRAMS];  // Per-program FX chains

    // Mixer
    SamplecrateMixer mixer;

    // Note suppression state
    bool note_suppressed[128][RSX_MAX_PROGRAMS + 1];  // [note][program] (0=global, 1-128=programs)

    // Current state
    int current_program;
    std::string error_message;
} SamplecrateEngine;

// Engine lifecycle
SamplecrateEngine* samplecrate_engine_create(MednessSequencer* sequencer);
void samplecrate_engine_destroy(SamplecrateEngine* engine);

// File loading
int samplecrate_engine_load_rsx(SamplecrateEngine* engine, const char* rsx_path);
int samplecrate_engine_reload_program(SamplecrateEngine* engine, int program_idx);
void samplecrate_engine_load_note_suppression(SamplecrateEngine* engine);
void samplecrate_engine_save_note_suppression(SamplecrateEngine* engine);

// Program switching
void samplecrate_engine_switch_program(SamplecrateEngine* engine, int program_idx);

// Effects management
void samplecrate_engine_autosave_effects(SamplecrateEngine* engine);

// Audio rendering
void samplecrate_engine_render_audio(SamplecrateEngine* engine, float* left, float* right, int num_frames);

#endif // SAMPLECRATE_ENGINE_H
