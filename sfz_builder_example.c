/**
 * Example usage of the SFZ builder API
 *
 * This demonstrates how to programmatically create sfizz synths
 * from wave files without needing to write SFZ files manually.
 */

#include "sfz_builder.h"
#include <sfizz.h>
#include <stdio.h>

void example_simple_sample() {
    printf("=== Example 1: Simple single sample ===\n");

    // Create synth
    sfizz_synth_t* synth = sfizz_create_synth();
    sfizz_set_sample_rate(synth, 44100);
    sfizz_set_samples_per_block(synth, 512);

    // Load a single kick drum sample on MIDI note 36
    if (sfz_load_simple_sample(synth, "/path/to/kick.wav",
                                36, 36, 36,  // key range (C1)
                                0, 127,      // full velocity range
                                1.0f) == 0) { // full volume
        printf("Loaded kick drum on note 36\n");
    }

    // Now you can use the synth...
    // sfizz_send_note_on(synth, 0, 36, 100);

    sfizz_free(synth);
}

void example_drum_kit() {
    printf("\n=== Example 2: Multi-sample drum kit ===\n");

    // Create synth
    sfizz_synth_t* synth = sfizz_create_synth();
    sfizz_set_sample_rate(synth, 44100);
    sfizz_set_samples_per_block(synth, 512);

    // Create builder
    SFZBuilder* builder = sfz_builder_create(44100);

    // Add kick drum
    sfz_builder_add_region(builder, "/path/to/kick.wav",
                          36, 36, 36,  // MIDI note 36 (C1)
                          0, 127,      // all velocities
                          1.0f, 0.0f); // full volume, center pan

    // Add snare drum
    sfz_builder_add_region(builder, "/path/to/snare.wav",
                          38, 38, 38,  // MIDI note 38 (D1)
                          0, 127,
                          1.0f, 0.0f);

    // Add closed hi-hat
    sfz_builder_add_region(builder, "/path/to/hihat_closed.wav",
                          42, 42, 42,  // MIDI note 42 (F#1)
                          0, 127,
                          0.8f, 0.0f); // slightly quieter

    // Add open hi-hat
    sfz_builder_add_region(builder, "/path/to/hihat_open.wav",
                          46, 46, 46,  // MIDI note 46 (A#1)
                          0, 127,
                          0.8f, 0.0f);

    // Add crash cymbal (panned slightly right)
    sfz_builder_add_region(builder, "/path/to/crash.wav",
                          49, 49, 49,  // MIDI note 49 (C#2)
                          0, 127,
                          0.9f, 0.3f); // pan slightly right

    // Load into synth
    if (sfz_builder_load(builder, synth) == 0) {
        printf("Loaded drum kit with %d samples\n", 5);
    }

    // Clean up builder (synth keeps the loaded content)
    sfz_builder_destroy(builder);

    // Now you can use the synth...
    // sfizz_send_note_on(synth, 0, 36, 100);  // kick
    // sfizz_send_note_on(synth, 0, 38, 100);  // snare

    sfizz_free(synth);
}

void example_velocity_layers() {
    printf("\n=== Example 3: Velocity-layered sample ===\n");

    sfizz_synth_t* synth = sfizz_create_synth();
    sfizz_set_sample_rate(synth, 44100);
    sfizz_set_samples_per_block(synth, 512);

    SFZBuilder* builder = sfz_builder_create(44100);

    // Low velocity layer (soft hit)
    sfz_builder_add_region(builder, "/path/to/snare_soft.wav",
                          38, 38, 38,
                          0, 63,       // velocity 0-63
                          1.0f, 0.0f);

    // High velocity layer (hard hit)
    sfz_builder_add_region(builder, "/path/to/snare_hard.wav",
                          38, 38, 38,
                          64, 127,     // velocity 64-127
                          1.0f, 0.0f);

    sfz_builder_load(builder, synth);
    sfz_builder_destroy(builder);

    printf("Loaded velocity-layered snare\n");

    sfizz_free(synth);
}

void example_multi_note_sample() {
    printf("\n=== Example 4: Sample across multiple notes ===\n");

    sfizz_synth_t* synth = sfizz_create_synth();
    sfizz_set_sample_rate(synth, 44100);
    sfizz_set_samples_per_block(synth, 512);

    SFZBuilder* builder = sfz_builder_create(44100);

    // Map a bass sample across an octave
    // The sample will be pitch-shifted by sfizz
    sfz_builder_add_region(builder, "/path/to/bass_c.wav",
                          36, 47, 36,  // C1 to B1, root at C1
                          0, 127,
                          1.0f, 0.0f);

    sfz_builder_load(builder, synth);
    sfz_builder_destroy(builder);

    printf("Loaded bass sample across octave\n");

    sfizz_free(synth);
}

int main() {
    example_simple_sample();
    example_drum_kit();
    example_velocity_layers();
    example_multi_note_sample();

    printf("\nAll examples completed!\n");
    return 0;
}
