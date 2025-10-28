#include "sfz_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define MAX_REGIONS 256
#define MAX_SFZ_SIZE (1024 * 64)  // 64KB should be enough for most cases

struct SFZBuilder {
    int sample_rate;
    char* sfz_content;
    size_t content_size;
    size_t content_capacity;
    int region_count;
};

/**
 * Helper to append text to the SFZ content buffer
 */
static int sfz_append(SFZBuilder* builder, const char* text) {
    size_t len = strlen(text);
    size_t new_size = builder->content_size + len;

    if (new_size >= builder->content_capacity) {
        // Need to expand buffer
        size_t new_capacity = builder->content_capacity * 2;
        if (new_capacity < new_size + 1024) {
            new_capacity = new_size + 1024;
        }

        char* new_content = (char*)realloc(builder->sfz_content, new_capacity);
        if (!new_content) {
            return -1;
        }

        builder->sfz_content = new_content;
        builder->content_capacity = new_capacity;
    }

    memcpy(builder->sfz_content + builder->content_size, text, len);
    builder->content_size = new_size;
    builder->sfz_content[builder->content_size] = '\0';

    return 0;
}

/**
 * Create a simple single-sample SFZ and load it into sfizz
 */
int sfz_load_simple_sample(sfizz_synth_t* synth,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude) {
    if (!synth || !sample_path) {
        return -1;
    }

    // Build minimal SFZ content
    char sfz_content[2048];
    snprintf(sfz_content, sizeof(sfz_content),
        "<region>\n"
        "sample=%s\n"
        "lokey=%d\n"
        "hikey=%d\n"
        "pitch_keycenter=%d\n"
        "lovel=%d\n"
        "hivel=%d\n"
        "volume=%.2f\n",
        sample_path,
        key_low,
        key_high,
        root_key,
        vel_low,
        vel_high,
        amplitude * 100.0f - 100.0f  // Convert 0-1 to -100dB to 0dB range
    );

    // Load the SFZ from string
    if (sfizz_load_string(synth, "", sfz_content) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Create a new SFZ builder
 */
SFZBuilder* sfz_builder_create(int sample_rate) {
    SFZBuilder* builder = (SFZBuilder*)calloc(1, sizeof(SFZBuilder));
    if (!builder) {
        return NULL;
    }

    builder->sample_rate = sample_rate;
    builder->content_capacity = 4096;
    builder->sfz_content = (char*)malloc(builder->content_capacity);
    if (!builder->sfz_content) {
        free(builder);
        return NULL;
    }

    builder->sfz_content[0] = '\0';
    builder->content_size = 0;
    builder->region_count = 0;

    // Add SFZ header (no <control> section - let sfizz use the base path parameter)
    char header[256];
    snprintf(header, sizeof(header),
        "// Auto-generated SFZ\n"
        "<global>\n"
        "sample_quality=2\n"
        "ampeg_attack=0.001\n"
        "ampeg_release=0.3\n"
        "\n");

    sfz_append(builder, header);

    return builder;
}

/**
 * Add a region to the SFZ builder
 */
int sfz_builder_add_region(SFZBuilder* builder,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude, float pan) {
    if (!builder || !sample_path) {
        return -1;
    }

    if (builder->region_count >= MAX_REGIONS) {
        return -1;
    }

    char region[1024];
    int offset = 0;

    offset += snprintf(region + offset, sizeof(region) - offset,
        "<region>\n"
        "sample=%s\n"
        "lokey=%d\n"
        "hikey=%d\n",
        sample_path,
        key_low,
        key_high
    );

    // Only add pitch_keycenter if it differs from key_low (needed for transposition)
    // When root_key == key_low, it's a one-shot with no pitch change
    if (root_key != key_low) {
        offset += snprintf(region + offset, sizeof(region) - offset,
            "pitch_keycenter=%d\n", root_key);
    }

    offset += snprintf(region + offset, sizeof(region) - offset,
        "lovel=%d\n"
        "hivel=%d\n",
        vel_low,
        vel_high
    );

    // Add volume if not default
    if (amplitude != 1.0f) {
        // Convert amplitude (0-1) to dB (-100 to 0)
        float volume_db = amplitude > 0.0f
            ? 20.0f * log10f(amplitude)
            : -100.0f;
        offset += snprintf(region + offset, sizeof(region) - offset,
            "volume=%.2f\n", volume_db);
    }

    // Add pan if not center
    if (pan != 0.0f) {
        // Convert pan (-1 to 1) to SFZ pan (-100 to 100)
        offset += snprintf(region + offset, sizeof(region) - offset,
            "pan=%.0f\n", pan * 100.0f);
    }

    offset += snprintf(region + offset, sizeof(region) - offset, "\n");

    if (sfz_append(builder, region) != 0) {
        return -1;
    }

    builder->region_count++;
    return 0;
}

/**
 * Write the built SFZ to a temporary file
 */
int sfz_builder_write_temp(SFZBuilder* builder, const char* base_path, char* out_filename, size_t out_size) {
    if (!builder || !base_path || !out_filename || out_size == 0) {
        fprintf(stderr, "sfz_builder_write_temp: Invalid parameters\n");
        return -1;
    }

    if (builder->region_count == 0) {
        fprintf(stderr, "sfz_builder_write_temp: No regions added\n");
        return -1;
    }

    // Debug: print the generated SFZ content
    fprintf(stderr, "Generated SFZ content (%zu bytes, %d regions):\n%s\n",
            builder->content_size, builder->region_count, builder->sfz_content);

    // Construct temp file path (will be set by caller with program name)
    char temp_path[1024];
    snprintf(temp_path, sizeof(temp_path), "%s/%s", base_path, out_filename);

    // Write to file
    FILE* f = fopen(temp_path, "w");
    if (!f) {
        fprintf(stderr, "sfz_builder_write_temp: Failed to write temp SFZ file: %s\n", temp_path);
        return -1;
    }

    fwrite(builder->sfz_content, 1, builder->content_size, f);
    fclose(f);

    fprintf(stderr, "Wrote temporary SFZ to: %s\n", temp_path);

    // out_filename already contains the filename passed in, just return success
    return 0;
}

/**
 * Build and load the SFZ into sfizz
 */
int sfz_builder_load(SFZBuilder* builder, sfizz_synth_t* synth, const char* base_path) {
    if (!builder || !synth) {
        fprintf(stderr, "sfz_builder_load: NULL builder or synth\n");
        return -1;
    }

    if (builder->region_count == 0) {
        fprintf(stderr, "sfz_builder_load: No regions added\n");
        return -1;  // No regions added
    }

    // Use current directory if base_path is NULL
    const char* path_to_use = base_path ? base_path : ".";

    // Build virtual path (must be absolute for sfizz to resolve relative sample paths)
    char virtual_sfz_path[1024];
    snprintf(virtual_sfz_path, sizeof(virtual_sfz_path), "%s/virtual.sfz", path_to_use);

    // Load from string using sfizz
    fprintf(stderr, "Loading SFZ from string (%zu bytes, %d regions)\n",
            builder->content_size, builder->region_count);
    fprintf(stderr, "Virtual path: %s\n", virtual_sfz_path);

    bool load_result = sfizz_load_string(synth, virtual_sfz_path, builder->sfz_content);

    if (!load_result) {
        fprintf(stderr, "sfz_builder_load: sfizz_load_string failed\n");
        return -1;
    }

    int num_loaded = sfizz_get_num_regions(synth);
    fprintf(stderr, "sfizz_load_string SUCCESS! %d regions loaded\n", num_loaded);

    if (num_loaded == 0) {
        fprintf(stderr, "sfz_builder_load: Warning - 0 regions loaded\n");
        return -1;
    }

    return 0;
}

/**
 * Free the builder
 */
void sfz_builder_destroy(SFZBuilder* builder) {
    if (builder) {
        if (builder->sfz_content) {
            free(builder->sfz_content);
        }
        free(builder);
    }
}
