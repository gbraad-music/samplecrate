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

    NoteTriggerPad pads[RSX_MAX_NOTE_PADS];
    int num_pads;
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
