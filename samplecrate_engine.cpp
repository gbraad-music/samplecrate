#include "samplecrate_engine.h"
#include "sfz_builder.h"
#include "medness_sequencer.h"
#include "medness_performance.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <libgen.h>

extern "C" {
#include "samplecrate_rsx.h"
}

// Mutex for thread-safe synth access
static std::mutex synth_mutex;

// Cross-platform realpath wrapper
static char* cross_platform_realpath(const char* path, char* resolved_path) {
#ifdef _WIN32
    return _fullpath(resolved_path, path, 1024);
#else
    return realpath(path, resolved_path);
#endif
}

SamplecrateEngine* samplecrate_engine_create(MednessSequencer* sequencer) {
    SamplecrateEngine* engine = new SamplecrateEngine();
    if (!engine) return nullptr;

    // Initialize pointers
    engine->rsx = nullptr;
    engine->synth = nullptr;
    engine->performance = nullptr;
    engine->effects_master = nullptr;
    engine->current_program = 0;

    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        engine->program_synths[i] = nullptr;
        engine->effects_program[i] = nullptr;
    }

    for (int i = 0; i < RSX_MAX_NOTE_PADS; i++) {
        engine->pad_program_numbers[i] = 0;  // Default to program 1
    }

    // Initialize note suppression
    for (int note = 0; note < 128; note++) {
        for (int prog = 0; prog < RSX_MAX_PROGRAMS + 1; prog++) {  // +1 for global (index 0)
            engine->note_suppressed[note][prog] = false;
        }
    }

    // Initialize mixer
    samplecrate_mixer_init(&engine->mixer);

    // Create main synth
    engine->synth = sfizz_create_synth();
    sfizz_set_sample_rate(engine->synth, 44100);
    sfizz_set_samples_per_block(engine->synth, 512);

    // Create performance manager (handles both pads and sequences)
    engine->performance = medness_performance_create();
    if (engine->performance) {
        medness_performance_set_sequencer(engine->performance, sequencer);
        medness_performance_set_tempo(engine->performance, 125.0f);
        // Set to IMMEDIATE mode for pads (start right away, not quantized)
        medness_performance_set_start_mode(engine->performance, SEQUENCE_START_IMMEDIATE);
    }

    // Create effects
    engine->effects_master = regroove_effects_create();
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        engine->effects_program[i] = regroove_effects_create();
    }

    return engine;
}

void samplecrate_engine_destroy(SamplecrateEngine* engine) {
    if (!engine) return;

    // Free RSX
    if (engine->rsx) {
        samplecrate_rsx_destroy(engine->rsx);
    }

    // Free synths
    if (engine->synth) {
        sfizz_free(engine->synth);
    }
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        if (engine->program_synths[i]) {
            sfizz_free(engine->program_synths[i]);
        }
    }

    // Free performance manager
    if (engine->performance) {
        medness_performance_destroy(engine->performance);
    }

    // Free effects
    if (engine->effects_master) {
        regroove_effects_destroy(engine->effects_master);
    }
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        if (engine->effects_program[i]) {
            regroove_effects_destroy(engine->effects_program[i]);
        }
    }

    delete engine;
}

void samplecrate_engine_load_note_suppression(SamplecrateEngine* engine) {
    if (!engine || !engine->rsx) return;

    // Copy global suppression
    for (int note = 0; note < 128; note++) {
        engine->note_suppressed[note][0] = (engine->rsx->note_suppressed_global[note] != 0);
    }

    // Copy per-program suppression
    for (int prog = 0; prog < RSX_MAX_PROGRAMS; prog++) {
        for (int note = 0; note < 128; note++) {
            engine->note_suppressed[note][prog + 1] = (engine->rsx->note_suppressed_program[prog][note] != 0);
        }
    }
}

void samplecrate_engine_save_note_suppression(SamplecrateEngine* engine) {
    if (!engine || !engine->rsx || engine->rsx_file_path.empty()) return;

    // Copy global suppression
    for (int note = 0; note < 128; note++) {
        engine->rsx->note_suppressed_global[note] = engine->note_suppressed[note][0] ? 1 : 0;
    }

    // Copy per-program suppression
    for (int prog = 0; prog < RSX_MAX_PROGRAMS; prog++) {
        for (int note = 0; note < 128; note++) {
            engine->rsx->note_suppressed_program[prog][note] = engine->note_suppressed[note][prog + 1] ? 1 : 0;
        }
    }

    // Save to file
    samplecrate_rsx_save(engine->rsx, engine->rsx_file_path.c_str());
}

int samplecrate_engine_reload_program(SamplecrateEngine* engine, int program_idx) {
    if (!engine || !engine->rsx || program_idx < 0 || program_idx >= RSX_MAX_PROGRAMS) return -1;

    std::lock_guard<std::mutex> lock(synth_mutex);

    // Free existing synth if it exists
    if (engine->program_synths[program_idx]) {
        sfizz_free(engine->program_synths[program_idx]);
        engine->program_synths[program_idx] = nullptr;
    }

    // Skip if no content to load
    if (engine->rsx->program_modes[program_idx] == PROGRAM_MODE_SFZ_FILE && engine->rsx->program_files[program_idx][0] == '\0') return 0;
    if (engine->rsx->program_modes[program_idx] == PROGRAM_MODE_SAMPLES && engine->rsx->program_sample_counts[program_idx] == 0) return 0;

    // Create new synth instance
    engine->program_synths[program_idx] = sfizz_create_synth();
    sfizz_set_sample_rate(engine->program_synths[program_idx], 44100);
    sfizz_set_samples_per_block(engine->program_synths[program_idx], 512);

    bool load_success = false;

    if (engine->rsx->program_modes[program_idx] == PROGRAM_MODE_SFZ_FILE) {
        // Load from SFZ file
        char sfz_path[512];
        samplecrate_rsx_get_sfz_path(engine->rsx_file_path.c_str(), engine->rsx->program_files[program_idx], sfz_path, sizeof(sfz_path));

        std::cout << "Reloading Program " << (program_idx + 1) << " (SFZ File: " << engine->rsx->program_files[program_idx] << ")" << std::endl;
        if (sfizz_load_file(engine->program_synths[program_idx], sfz_path)) {
            load_success = true;
            int num_regions = sfizz_get_num_regions(engine->program_synths[program_idx]);
            std::cout << "  SUCCESS: Loaded " << num_regions << " regions" << std::endl;
        } else {
            std::cerr << "ERROR: Failed to load program " << (program_idx + 1) << ": " << sfz_path << std::endl;
        }
    }
    else if (engine->rsx->program_modes[program_idx] == PROGRAM_MODE_SAMPLES) {
        // Build from samples
        std::cout << "Reloading Program " << (program_idx + 1) << " (Samples: " << engine->rsx->program_sample_counts[program_idx] << ")" << std::endl;

        SFZBuilder* builder = sfz_builder_create(44100);
        if (builder) {
            for (int s = 0; s < engine->rsx->program_sample_counts[program_idx]; s++) {
                RSXSampleMapping* sample = &engine->rsx->program_samples[program_idx][s];

                if (sample->enabled && sample->sample_path[0] != '\0') {
                    std::cout << "  Sample " << (s + 1) << ": " << sample->sample_path << std::endl;

                    sfz_builder_add_region(builder,
                                          sample->sample_path,
                                          sample->key_low,
                                          sample->key_high,
                                          sample->root_key,
                                          sample->vel_low,
                                          sample->vel_high,
                                          sample->amplitude,
                                          sample->pan);
                }
            }

            // Get RSX directory to write temp file
            char rsx_dir[512];
            strncpy(rsx_dir, engine->rsx_file_path.c_str(), sizeof(rsx_dir) - 1);
            rsx_dir[sizeof(rsx_dir) - 1] = '\0';

            char* dir = dirname(rsx_dir);
            char absolute_dir[1024];
            char* resolved = cross_platform_realpath(dir, absolute_dir);
            const char* base_path = resolved ? absolute_dir : dir;

            if (sfz_builder_load(builder, engine->program_synths[program_idx], base_path) == 0) {
                load_success = true;
                int num_regions = sfizz_get_num_regions(engine->program_synths[program_idx]);
                std::cout << "  SUCCESS: Built " << num_regions << " regions" << std::endl;
            } else {
                std::cerr << "ERROR: Failed to build program " << (program_idx + 1) << " from samples" << std::endl;
            }

            sfz_builder_destroy(builder);
        }
    }

    if (!load_success) {
        engine->error_message = "Failed to load\nProgram " + std::to_string(program_idx + 1);
        sfizz_free(engine->program_synths[program_idx]);
        engine->program_synths[program_idx] = nullptr;
        return -1;
    } else {
        engine->error_message = "";  // Clear error on success

        // Update main synth pointer if this is the current program
        if (engine->current_program == program_idx) {
            engine->synth = engine->program_synths[program_idx];
        }
        return 0;
    }
}

int samplecrate_engine_load_rsx(SamplecrateEngine* engine, const char* rsx_path) {
    if (!engine || !rsx_path) return -1;

    // Create RSX structure if not exists
    if (!engine->rsx) {
        engine->rsx = samplecrate_rsx_create();
    }

    // Clean up ALL existing program synths before loading new file
    // (in case the new file has fewer programs than the old one)
    std::cout << "Cleaning up existing programs..." << std::endl;
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        if (engine->program_synths[i]) {
            sfizz_free(engine->program_synths[i]);
            engine->program_synths[i] = nullptr;
        }
    }

    // Load the RSX file
    if (samplecrate_rsx_load(engine->rsx, rsx_path) != 0) {
        std::cerr << "Failed to load RSX file: " << rsx_path << std::endl;
        return -1;
    }

    std::cout << "Loaded RSX file: " << rsx_path << std::endl;
    engine->rsx_file_path = rsx_path;

    // Reload all programs from the new file
    for (int i = 0; i < engine->rsx->num_programs; i++) {
        samplecrate_engine_reload_program(engine, i);
    }

    // Load note suppression settings
    samplecrate_engine_load_note_suppression(engine);

    // Load MIDI files for all configured pads (using unified sequence system)
    if (engine->performance) {
        std::cout << "Loading MIDI files for pads..." << std::endl;
        for (int i = 0; i < engine->rsx->num_pads && i < RSX_MAX_NOTE_PADS; i++) {
            if (engine->rsx->pads[i].midi_file[0] != '\0') {
                char midi_path[512];
                samplecrate_rsx_get_sfz_path(engine->rsx_file_path.c_str(), engine->rsx->pads[i].midi_file, midi_path, sizeof(midi_path));

                // Determine which program this pad targets
                int prog = (engine->rsx->pads[i].program >= 0) ? engine->rsx->pads[i].program : engine->current_program;
                engine->pad_program_numbers[i] = prog;

                if (medness_performance_load_pad(engine->performance, i, midi_path, prog) == 0) {
                    // Success message already printed by medness_performance_load_pad
                } else {
                    std::cerr << "  Pad " << (i + 1) << ": Failed to load MIDI file " << midi_path << std::endl;
                }
            }
        }
    }

    // Reset to program 0
    engine->current_program = 0;
    if (engine->program_synths[0]) {
        engine->synth = engine->program_synths[0];
    }
    // Note: if program_synths[0] is NULL, engine->synth retains the default synth created during engine init

    return 0;
}

void samplecrate_engine_switch_program(SamplecrateEngine* engine, int program_idx) {
    if (!engine || !engine->rsx || program_idx < 0 || program_idx >= engine->rsx->num_programs) return;

    engine->current_program = program_idx;
    engine->synth = engine->program_synths[program_idx];

    std::cout << "Switched to program " << (program_idx + 1) << std::endl;
}

void samplecrate_engine_autosave_effects(SamplecrateEngine* engine) {
    if (!engine || !engine->rsx || engine->rsx_file_path.empty()) return;

    // This function would save effects state back to RSX
    // Implementation depends on how effects are stored in RSX
    // For now, just placeholder
}

void samplecrate_engine_render_audio(SamplecrateEngine* engine, float* left, float* right, int num_frames) {
    // This would be implemented for headless rendering
    // For now, just placeholder
}
