#ifndef SFZ_BUILDER_H
#define SFZ_BUILDER_H

#include <sfizz.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a simple single-sample SFZ and load it into sfizz
 *
 * This creates a minimal SFZ file in memory that maps a single wave file
 * to a MIDI note range.
 *
 * @param synth The sfizz synth to load into (must be created first)
 * @param sample_path Full path to the wave file
 * @param key_low Low MIDI note (0-127)
 * @param key_high High MIDI note (0-127)
 * @param root_key Root key/pitch (usually same as key_low for one-shot samples)
 * @param vel_low Low velocity (0-127)
 * @param vel_high High velocity (0-127)
 * @param amplitude Volume (0.0-1.0, 1.0 = full volume)
 * @return 0 on success, -1 on error
 *
 * Example:
 *   sfizz_synth_t* synth = sfizz_create_synth();
 *   sfz_load_simple_sample(synth, "/path/to/kick.wav", 36, 36, 36, 0, 127, 1.0f);
 */
int sfz_load_simple_sample(sfizz_synth_t* synth,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude);

/**
 * Multi-region SFZ builder for creating drum kits programmatically
 */
typedef struct SFZBuilder SFZBuilder;

/**
 * Create a new SFZ builder
 *
 * @param sample_rate Sample rate (e.g., 44100)
 * @return Builder instance, or NULL on error
 */
SFZBuilder* sfz_builder_create(int sample_rate);

/**
 * Add a region (sample) to the SFZ
 *
 * @param builder The builder instance
 * @param sample_path Full path to the wave file
 * @param key_low Low MIDI note (0-127)
 * @param key_high High MIDI note (0-127)
 * @param root_key Root key/pitch
 * @param vel_low Low velocity (0-127)
 * @param vel_high High velocity (0-127)
 * @param amplitude Volume (0.0-1.0)
 * @param pan Pan position (-1.0=left, 0.0=center, 1.0=right)
 * @return 0 on success, -1 on error
 */
int sfz_builder_add_region(SFZBuilder* builder,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude, float pan);

/**
 * Write the built SFZ to a temporary file
 *
 * @param builder The builder instance
 * @param base_path Directory to write the temp file to
 * @param out_filename Buffer to receive the temp filename (relative, e.g., ".samplecrate_temp.sfz")
 * @param out_size Size of out_filename buffer
 * @return 0 on success, -1 on error
 */
int sfz_builder_write_temp(SFZBuilder* builder, const char* base_path, char* out_filename, size_t out_size);

/**
 * Build and load the SFZ into a sfizz synth
 *
 * @param builder The builder instance
 * @param synth The sfizz synth to load into
 * @param base_path Base directory for resolving relative sample paths (can be NULL to use current directory)
 * @return 0 on success, -1 on error
 */
int sfz_builder_load(SFZBuilder* builder, sfizz_synth_t* synth, const char* base_path);

/**
 * Free the builder
 *
 * @param builder The builder instance
 */
void sfz_builder_destroy(SFZBuilder* builder);

#ifdef __cplusplus
}
#endif

#endif // SFZ_BUILDER_H
