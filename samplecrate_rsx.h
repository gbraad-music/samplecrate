#ifndef SAMPLECRATE_RSX_H
#define SAMPLECRATE_RSX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_MAX_NOTE_PADS 32  // Support expanded pads mode (2 sets of 16)
#define RSX_MAX_PROGRAMS 4    // Support up to 4 programs (scenes)
#define RSX_MAX_PATH 512
#define RSX_MAX_DESCRIPTION 64
#define RSX_MAX_SAMPLES_PER_PROGRAM 64  // Max samples per program

// Effects settings for one effects chain
typedef struct {
    // Distortion
    int distortion_enabled;
    float distortion_drive;
    float distortion_mix;

    // Filter
    int filter_enabled;
    float filter_cutoff;
    float filter_resonance;

    // EQ
    int eq_enabled;
    float eq_low;
    float eq_mid;
    float eq_high;

    // Compressor
    int compressor_enabled;
    float compressor_threshold;
    float compressor_ratio;
    float compressor_attack;
    float compressor_release;
    float compressor_makeup;

    // Phaser
    int phaser_enabled;
    float phaser_rate;
    float phaser_depth;
    float phaser_feedback;

    // Reverb
    int reverb_enabled;
    float reverb_room_size;
    float reverb_damping;
    float reverb_mix;

    // Delay
    int delay_enabled;
    float delay_time;
    float delay_feedback;
    float delay_mix;
} RSXEffectsSettings;

// Sample mapping for programmatic SFZ building
typedef struct {
    char sample_path[RSX_MAX_PATH];  // Path to wave file
    int key_low;                      // Low MIDI note (0-127)
    int key_high;                     // High MIDI note (0-127)
    int root_key;                     // Root pitch
    int vel_low;                      // Low velocity (0-127)
    int vel_high;                     // High velocity (0-127)
    float amplitude;                  // Volume (0.0-1.0)
    float pan;                        // Pan (-1.0=left, 0.0=center, 1.0=right)
    int enabled;                      // 1=enabled, 0=disabled
} RSXSampleMapping;

// Program mode enumeration
typedef enum {
    PROGRAM_MODE_SFZ_FILE = 0,   // Load from SFZ file
    PROGRAM_MODE_SAMPLES = 1      // Build from sample list
} RSXProgramMode;

// Note trigger pad configuration
typedef struct {
    int note;                           // MIDI note number
    char description[RSX_MAX_DESCRIPTION];  // Display text
    int velocity;                       // Default velocity (0-127, 0=use default)
    float pitch_bend;                   // Pitch bend (-1.0 to 1.0, 0=none)
    float pan;                          // Pan (-1.0 to 1.0, 0=center, use NaN for not set)
    float volume;                       // Volume (0.0 to 1.0, use NaN for not set)
    int enabled;                        // 1=enabled, 0=disabled
    int program;                        // Program index (0-3 for prog 1-4, -1=current program)
} NoteTriggerPad;

// RSX file content
typedef struct {
    int version;
    char sfz_file[RSX_MAX_PATH];        // Relative path to SFZ file (legacy, use programs[0])
    char program_files[RSX_MAX_PROGRAMS][RSX_MAX_PATH];  // Program SFZ files (prog_1 to prog_4)
    char program_names[RSX_MAX_PROGRAMS][RSX_MAX_DESCRIPTION];  // Program display names
    int num_programs;                   // Number of programs defined

    // Per-program mixing defaults
    float program_volumes[RSX_MAX_PROGRAMS];  // Default volume for each program (0.0-1.0)
    float program_pans[RSX_MAX_PROGRAMS];     // Default pan for each program (0.0-1.0, 0.5=center)

    // Per-program MIDI channel filtering (-1 = Omni/all channels, 0-15 = specific channel)
    int program_midi_channels[RSX_MAX_PROGRAMS];  // MIDI channel filter per program

    // Program mode (SFZ file or sample-based)
    RSXProgramMode program_modes[RSX_MAX_PROGRAMS];  // Mode for each program

    // Sample-based program data
    RSXSampleMapping program_samples[RSX_MAX_PROGRAMS][RSX_MAX_SAMPLES_PER_PROGRAM];  // Sample mappings
    int program_sample_counts[RSX_MAX_PROGRAMS];  // Number of samples per program

    // FX chain enable states
    int master_fx_enable;                    // Master FX chain enable (0=off, 1=on)
    int program_fx_enable[RSX_MAX_PROGRAMS]; // Per-program FX chain enables

    // Effects settings
    RSXEffectsSettings master_effects;                    // Master effects chain
    RSXEffectsSettings program_effects[RSX_MAX_PROGRAMS]; // Per-program effects chains

    NoteTriggerPad pads[RSX_MAX_NOTE_PADS];
    int num_pads;

    // Note suppression (128 MIDI notes, 0-127)
    // [note] = global suppression (affects all programs)
    // [note] = per-program suppression for programs 0-3
    unsigned char note_suppressed_global[128];      // 1=suppressed, 0=not suppressed
    unsigned char note_suppressed_program[4][128];  // Per-program suppression
} SamplecrateRSX;

// Create a new RSX structure with defaults
SamplecrateRSX* samplecrate_rsx_create(void);

// Free RSX structure
void samplecrate_rsx_destroy(SamplecrateRSX* rsx);

// Load RSX file
int samplecrate_rsx_load(SamplecrateRSX* rsx, const char* filepath);

// Save RSX file
int samplecrate_rsx_save(SamplecrateRSX* rsx, const char* filepath);

// Get full SFZ path from RSX file path
// (resolves relative sfz_file path relative to RSX directory)
void samplecrate_rsx_get_sfz_path(const char* rsx_path, const char* sfz_relative,
                                   char* out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // SAMPLECRATE_RSX_H
