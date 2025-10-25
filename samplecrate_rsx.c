#include "samplecrate_rsx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <libgen.h>

// Helper: initialize effects settings with defaults (matching samplecrate_common.c)
static void init_effects_defaults(RSXEffectsSettings* fx) {
    if (!fx) return;

    // Distortion
    fx->distortion_enabled = 0;
    fx->distortion_drive = 0.5f;
    fx->distortion_mix = 0.0f;

    // Filter
    fx->filter_enabled = 0;
    fx->filter_cutoff = 1.0f;      // Fully open
    fx->filter_resonance = 0.0f;

    // EQ
    fx->eq_enabled = 0;
    fx->eq_low = 0.5f;             // Centered (no boost/cut)
    fx->eq_mid = 0.5f;
    fx->eq_high = 0.5f;

    // Compressor
    fx->compressor_enabled = 0;
    fx->compressor_threshold = 0.8f;
    fx->compressor_ratio = 0.2f;
    fx->compressor_attack = 0.3f;
    fx->compressor_release = 0.5f;
    fx->compressor_makeup = 0.5f;

    // Phaser
    fx->phaser_enabled = 0;
    fx->phaser_rate = 0.3f;
    fx->phaser_depth = 0.5f;
    fx->phaser_feedback = 0.3f;

    // Reverb
    fx->reverb_enabled = 0;
    fx->reverb_room_size = 0.5f;
    fx->reverb_damping = 0.5f;
    fx->reverb_mix = 0.0f;

    // Delay
    fx->delay_enabled = 0;
    fx->delay_time = 0.3f;
    fx->delay_feedback = 0.3f;
    fx->delay_mix = 0.0f;
}

SamplecrateRSX* samplecrate_rsx_create(void) {
    SamplecrateRSX* rsx = (SamplecrateRSX*)calloc(1, sizeof(SamplecrateRSX));
    if (!rsx) return NULL;

    rsx->version = 1;
    rsx->sfz_file[0] = '\0';
    rsx->num_programs = 0;
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        rsx->program_files[i][0] = '\0';
        rsx->program_names[i][0] = '\0';
        rsx->program_volumes[i] = 1.0f;  // Default volume (100%)
        rsx->program_pans[i] = 0.5f;     // Center pan
    }
    rsx->num_pads = 0;

    // Initialize FX chain enables (default ON)
    rsx->master_fx_enable = 1;
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        rsx->program_fx_enable[i] = 1;
    }

    // Initialize effects settings with defaults
    init_effects_defaults(&rsx->master_effects);
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        init_effects_defaults(&rsx->program_effects[i]);
    }

    // Initialize pads
    for (int i = 0; i < RSX_MAX_NOTE_PADS; i++) {
        rsx->pads[i].note = -1;
        rsx->pads[i].description[0] = '\0';
        rsx->pads[i].velocity = 0;  // 0 = use default
        rsx->pads[i].pitch_bend = 0.0f;
        rsx->pads[i].pan = NAN;  // Not set
        rsx->pads[i].volume = NAN;  // Not set
        rsx->pads[i].enabled = 1;
        rsx->pads[i].program = -1;  // -1 = use current program
    }

    // Initialize note suppression (all notes enabled by default)
    memset(rsx->note_suppressed_global, 0, sizeof(rsx->note_suppressed_global));
    for (int i = 0; i < RSX_MAX_PROGRAMS; i++) {
        memset(rsx->note_suppressed_program[i], 0, sizeof(rsx->note_suppressed_program[i]));
    }

    return rsx;
}

void samplecrate_rsx_destroy(SamplecrateRSX* rsx) {
    if (rsx) {
        free(rsx);
    }
}

// Helper: trim whitespace
static void trim(char* str) {
    char* start = str;
    while (isspace(*start)) start++;

    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    char* end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) end--;
    *(end + 1) = '\0';

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

// Helper: parse key=value line
static int parse_key_value(const char* line, char* key, char* value, size_t key_size, size_t value_size) {
    const char* eq = strchr(line, '=');
    if (!eq) return -1;

    size_t key_len = eq - line;
    if (key_len >= key_size) return -1;

    strncpy(key, line, key_len);
    key[key_len] = '\0';
    trim(key);

    strncpy(value, eq + 1, value_size - 1);
    value[value_size - 1] = '\0';

    // Remove quotes from value if present
    trim(value);
    if (value[0] == '"') {
        size_t len = strlen(value);
        if (len > 1 && value[len - 1] == '"') {
            memmove(value, value + 1, len - 2);
            value[len - 2] = '\0';
        }
    }

    return 0;
}

// Helper: load effects settings from key-value pairs
static void load_effects_setting(RSXEffectsSettings* fx, const char* key, const char* value) {
    if (!fx || !key || !value) return;

    // Distortion
    if (strcmp(key, "distortion_enabled") == 0) fx->distortion_enabled = atoi(value);
    else if (strcmp(key, "distortion_drive") == 0) fx->distortion_drive = atof(value);
    else if (strcmp(key, "distortion_mix") == 0) fx->distortion_mix = atof(value);

    // Filter
    else if (strcmp(key, "filter_enabled") == 0) fx->filter_enabled = atoi(value);
    else if (strcmp(key, "filter_cutoff") == 0) fx->filter_cutoff = atof(value);
    else if (strcmp(key, "filter_resonance") == 0) fx->filter_resonance = atof(value);

    // EQ
    else if (strcmp(key, "eq_enabled") == 0) fx->eq_enabled = atoi(value);
    else if (strcmp(key, "eq_low") == 0) fx->eq_low = atof(value);
    else if (strcmp(key, "eq_mid") == 0) fx->eq_mid = atof(value);
    else if (strcmp(key, "eq_high") == 0) fx->eq_high = atof(value);

    // Compressor
    else if (strcmp(key, "compressor_enabled") == 0) fx->compressor_enabled = atoi(value);
    else if (strcmp(key, "compressor_threshold") == 0) fx->compressor_threshold = atof(value);
    else if (strcmp(key, "compressor_ratio") == 0) fx->compressor_ratio = atof(value);
    else if (strcmp(key, "compressor_attack") == 0) fx->compressor_attack = atof(value);
    else if (strcmp(key, "compressor_release") == 0) fx->compressor_release = atof(value);
    else if (strcmp(key, "compressor_makeup") == 0) fx->compressor_makeup = atof(value);

    // Phaser
    else if (strcmp(key, "phaser_enabled") == 0) fx->phaser_enabled = atoi(value);
    else if (strcmp(key, "phaser_rate") == 0) fx->phaser_rate = atof(value);
    else if (strcmp(key, "phaser_depth") == 0) fx->phaser_depth = atof(value);
    else if (strcmp(key, "phaser_feedback") == 0) fx->phaser_feedback = atof(value);

    // Reverb
    else if (strcmp(key, "reverb_enabled") == 0) fx->reverb_enabled = atoi(value);
    else if (strcmp(key, "reverb_room_size") == 0) fx->reverb_room_size = atof(value);
    else if (strcmp(key, "reverb_damping") == 0) fx->reverb_damping = atof(value);
    else if (strcmp(key, "reverb_mix") == 0) fx->reverb_mix = atof(value);

    // Delay
    else if (strcmp(key, "delay_enabled") == 0) fx->delay_enabled = atoi(value);
    else if (strcmp(key, "delay_time") == 0) fx->delay_time = atof(value);
    else if (strcmp(key, "delay_feedback") == 0) fx->delay_feedback = atof(value);
    else if (strcmp(key, "delay_mix") == 0) fx->delay_mix = atof(value);
}

int samplecrate_rsx_load(SamplecrateRSX* rsx, const char* filepath) {
    if (!rsx || !filepath) return -1;

    FILE* f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "Failed to open RSX file: %s\n", filepath);
        return -1;
    }

    char line[512];
    char section[128] = "";
    int current_pad = -1;

    while (fgets(line, sizeof(line), f)) {
        trim(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        // Check for section header
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
                trim(section);
                current_pad = -1;  // Reset pad index when entering new section
            }
            continue;
        }

        // Parse key=value
        char key[128], value[384];
        if (parse_key_value(line, key, value, sizeof(key), sizeof(value)) != 0) {
            continue;
        }

        // Handle [Samplecrate] section (case-insensitive)
        if (strcasecmp(section, "Samplecrate") == 0) {
            if (strcmp(key, "version") == 0) {
                rsx->version = atoi(value);
            } else if (strcmp(key, "file") == 0) {
                strncpy(rsx->sfz_file, value, sizeof(rsx->sfz_file) - 1);
                rsx->sfz_file[sizeof(rsx->sfz_file) - 1] = '\0';
            }
        }
        // Handle [Programs] section (case-insensitive)
        else if (strcasecmp(section, "Programs") == 0) {
            // Parse prog_N_file or prog_N_name format
            if (strncmp(key, "prog_", 5) == 0) {
                int prog_num = atoi(key + 5);
                printf("DEBUG: Parsing Programs section: key='%s' prog_num=%d value='%s'\n", key, prog_num, value);
                if (prog_num >= 1 && prog_num <= RSX_MAX_PROGRAMS) {
                    int prog_idx = prog_num - 1;

                    // Check if it's _file, _name, _volume, or _pan
                    if (strstr(key, "_file") != NULL) {
                        // prog_N_file
                        strncpy(rsx->program_files[prog_idx], value, sizeof(rsx->program_files[prog_idx]) - 1);
                        rsx->program_files[prog_idx][sizeof(rsx->program_files[prog_idx]) - 1] = '\0';
                        if (prog_num > rsx->num_programs) {
                            rsx->num_programs = prog_num;
                        }
                        printf("DEBUG: Stored program %d file: '%s' (total programs: %d)\n", prog_num, rsx->program_files[prog_idx], rsx->num_programs);
                    } else if (strstr(key, "_name") != NULL) {
                        // prog_N_name
                        strncpy(rsx->program_names[prog_idx], value, sizeof(rsx->program_names[prog_idx]) - 1);
                        rsx->program_names[prog_idx][sizeof(rsx->program_names[prog_idx]) - 1] = '\0';
                        printf("DEBUG: Stored program %d name: '%s'\n", prog_num, rsx->program_names[prog_idx]);
                    } else if (strstr(key, "_volume") != NULL) {
                        // prog_N_volume
                        rsx->program_volumes[prog_idx] = atof(value);
                        printf("DEBUG: Stored program %d volume: %.3f\n", prog_num, rsx->program_volumes[prog_idx]);
                    } else if (strstr(key, "_pan") != NULL) {
                        // prog_N_pan
                        rsx->program_pans[prog_idx] = atof(value);
                        printf("DEBUG: Stored program %d pan: %.3f\n", prog_num, rsx->program_pans[prog_idx]);
                    } else if (strstr(key, "_fx_enable") != NULL) {
                        // prog_N_fx_enable
                        rsx->program_fx_enable[prog_idx] = atoi(value);
                        printf("DEBUG: Stored program %d fx_enable: %d\n", prog_num, rsx->program_fx_enable[prog_idx]);
                    }
                } else {
                    printf("DEBUG: prog_num %d out of range (1-%d)\n", prog_num, RSX_MAX_PROGRAMS);
                }
            }
        }
        // Handle [MasterEffects] section (case-insensitive)
        else if (strcasecmp(section, "MasterEffects") == 0) {
            if (strcmp(key, "fx_enable") == 0) {
                rsx->master_fx_enable = atoi(value);
            } else {
                load_effects_setting(&rsx->master_effects, key, value);
            }
        }
        // Handle [ProgramEffects1-4] sections (case-insensitive)
        else if (strncasecmp(section, "ProgramEffects", 14) == 0) {
            int prog_num = atoi(section + 14);  // Extract number from "ProgramEffects1" etc.
            if (prog_num >= 1 && prog_num <= RSX_MAX_PROGRAMS) {
                if (strcmp(key, "fx_enable") == 0) {
                    rsx->program_fx_enable[prog_num - 1] = atoi(value);
                } else {
                    load_effects_setting(&rsx->program_effects[prog_num - 1], key, value);
                }
            }
        }
        // Handle [NoteSuppression] section (case-insensitive)
        else if (strcasecmp(section, "NoteSuppression") == 0) {
            // Parse global_<note> or prog_<N>_<note> format
            if (strncmp(key, "global_", 7) == 0) {
                int note = atoi(key + 7);
                if (note >= 0 && note < 128) {
                    rsx->note_suppressed_global[note] = (unsigned char)atoi(value);
                }
            } else if (strncmp(key, "prog_", 5) == 0) {
                // Extract program number from "prog_N_<note>"
                int prog_num = atoi(key + 5);
                if (prog_num >= 1 && prog_num <= RSX_MAX_PROGRAMS) {
                    // Find the second underscore to get the note number
                    const char* note_str = strchr(key + 5, '_');
                    if (note_str) {
                        note_str++;  // Skip underscore
                        int note = atoi(note_str);
                        if (note >= 0 && note < 128) {
                            rsx->note_suppressed_program[prog_num - 1][note] = (unsigned char)atoi(value);
                        }
                    }
                }
            }
        }
        // Handle [NoteTriggerPads] section (case-insensitive)
        else if (strcasecmp(section, "NoteTriggerPads") == 0) {
            // Parse pad_N<number>_<property> format
            if (strncmp(key, "pad_N", 5) == 0) {
                // Extract pad number (1-16)
                int pad_num = atoi(key + 5);
                if (pad_num < 1 || pad_num > RSX_MAX_NOTE_PADS) continue;

                int pad_idx = pad_num - 1;
                if (pad_idx >= rsx->num_pads) {
                    rsx->num_pads = pad_idx + 1;
                }

                // Find property name after second underscore
                const char* prop = strchr(key + 5, '_');
                if (!prop) continue;
                prop++;  // Skip underscore

                // Parse property
                if (strcmp(prop, "note") == 0) {
                    rsx->pads[pad_idx].note = atoi(value);
                } else if (strcmp(prop, "description") == 0) {
                    strncpy(rsx->pads[pad_idx].description, value, sizeof(rsx->pads[pad_idx].description) - 1);
                    rsx->pads[pad_idx].description[sizeof(rsx->pads[pad_idx].description) - 1] = '\0';
                } else if (strcmp(prop, "velocity") == 0) {
                    rsx->pads[pad_idx].velocity = atoi(value);
                } else if (strcmp(prop, "pitch_bend") == 0) {
                    rsx->pads[pad_idx].pitch_bend = atof(value);
                } else if (strcmp(prop, "pan") == 0) {
                    rsx->pads[pad_idx].pan = atof(value);
                } else if (strcmp(prop, "volume") == 0) {
                    rsx->pads[pad_idx].volume = atof(value);
                } else if (strcmp(prop, "enabled") == 0) {
                    rsx->pads[pad_idx].enabled = atoi(value);
                } else if (strcmp(prop, "program") == 0) {
                    rsx->pads[pad_idx].program = atoi(value);
                }
            }
        }
    }

    fclose(f);
    return 0;
}

// Helper: save effects settings to file with prefix
static void save_effects_settings(FILE* f, const char* prefix, const RSXEffectsSettings* fx) {
    if (!f || !prefix || !fx) return;

    // Distortion
    fprintf(f, "%sdistortion_enabled=%d\n", prefix, fx->distortion_enabled);
    fprintf(f, "%sdistortion_drive=%.3f\n", prefix, fx->distortion_drive);
    fprintf(f, "%sdistortion_mix=%.3f\n", prefix, fx->distortion_mix);

    // Filter
    fprintf(f, "%sfilter_enabled=%d\n", prefix, fx->filter_enabled);
    fprintf(f, "%sfilter_cutoff=%.3f\n", prefix, fx->filter_cutoff);
    fprintf(f, "%sfilter_resonance=%.3f\n", prefix, fx->filter_resonance);

    // EQ
    fprintf(f, "%seq_enabled=%d\n", prefix, fx->eq_enabled);
    fprintf(f, "%seq_low=%.3f\n", prefix, fx->eq_low);
    fprintf(f, "%seq_mid=%.3f\n", prefix, fx->eq_mid);
    fprintf(f, "%seq_high=%.3f\n", prefix, fx->eq_high);

    // Compressor
    fprintf(f, "%scompressor_enabled=%d\n", prefix, fx->compressor_enabled);
    fprintf(f, "%scompressor_threshold=%.3f\n", prefix, fx->compressor_threshold);
    fprintf(f, "%scompressor_ratio=%.3f\n", prefix, fx->compressor_ratio);
    fprintf(f, "%scompressor_attack=%.3f\n", prefix, fx->compressor_attack);
    fprintf(f, "%scompressor_release=%.3f\n", prefix, fx->compressor_release);
    fprintf(f, "%scompressor_makeup=%.3f\n", prefix, fx->compressor_makeup);

    // Phaser
    fprintf(f, "%sphaser_enabled=%d\n", prefix, fx->phaser_enabled);
    fprintf(f, "%sphaser_rate=%.3f\n", prefix, fx->phaser_rate);
    fprintf(f, "%sphaser_depth=%.3f\n", prefix, fx->phaser_depth);
    fprintf(f, "%sphaser_feedback=%.3f\n", prefix, fx->phaser_feedback);

    // Reverb
    fprintf(f, "%sreverb_enabled=%d\n", prefix, fx->reverb_enabled);
    fprintf(f, "%sreverb_room_size=%.3f\n", prefix, fx->reverb_room_size);
    fprintf(f, "%sreverb_damping=%.3f\n", prefix, fx->reverb_damping);
    fprintf(f, "%sreverb_mix=%.3f\n", prefix, fx->reverb_mix);

    // Delay
    fprintf(f, "%sdelay_enabled=%d\n", prefix, fx->delay_enabled);
    fprintf(f, "%sdelay_time=%.3f\n", prefix, fx->delay_time);
    fprintf(f, "%sdelay_feedback=%.3f\n", prefix, fx->delay_feedback);
    fprintf(f, "%sdelay_mix=%.3f\n", prefix, fx->delay_mix);
}

int samplecrate_rsx_save(SamplecrateRSX* rsx, const char* filepath) {
    if (!rsx || !filepath) return -1;

    FILE* f = fopen(filepath, "w");
    if (!f) {
        fprintf(stderr, "Failed to create RSX file: %s\n", filepath);
        return -1;
    }

    // Write header
    fprintf(f, "[Samplecrate]\n");
    fprintf(f, "version=%d\n", rsx->version);
    if (rsx->sfz_file[0] != '\0') {
        fprintf(f, "file=\"%s\"\n", rsx->sfz_file);
    }
    fprintf(f, "\n");

    // Write programs
    if (rsx->num_programs > 0) {
        fprintf(f, "[Programs]\n");
        for (int i = 0; i < rsx->num_programs; i++) {
            if (rsx->program_names[i][0] != '\0') {
                fprintf(f, "prog_%d_name=\"%s\"\n", i + 1, rsx->program_names[i]);
            }
            if (rsx->program_files[i][0] != '\0') {
                fprintf(f, "prog_%d_file=\"%s\"\n", i + 1, rsx->program_files[i]);
            }
            fprintf(f, "prog_%d_volume=%.3f\n", i + 1, rsx->program_volumes[i]);
            fprintf(f, "prog_%d_pan=%.3f\n", i + 1, rsx->program_pans[i]);
            fprintf(f, "prog_%d_fx_enable=%d\n", i + 1, rsx->program_fx_enable[i]);
        }
        fprintf(f, "\n");
    }

    // Write master effects
    fprintf(f, "[MasterEffects]\n");
    fprintf(f, "fx_enable=%d\n", rsx->master_fx_enable);
    save_effects_settings(f, "", &rsx->master_effects);
    fprintf(f, "\n");

    // Write per-program effects
    for (int i = 0; i < rsx->num_programs; i++) {
        fprintf(f, "[ProgramEffects%d]\n", i + 1);
        fprintf(f, "fx_enable=%d\n", rsx->program_fx_enable[i]);
        save_effects_settings(f, "", &rsx->program_effects[i]);
        fprintf(f, "\n");
    }

    // Write note suppression
    fprintf(f, "[NoteSuppression]\n");
    fprintf(f, "# Global note suppression (affects all programs)\n");
    for (int note = 0; note < 128; note++) {
        if (rsx->note_suppressed_global[note]) {
            fprintf(f, "global_%d=1\n", note);
        }
    }
    fprintf(f, "\n# Per-program note suppression\n");
    for (int prog = 0; prog < rsx->num_programs; prog++) {
        int any_suppressed = 0;
        for (int note = 0; note < 128; note++) {
            if (rsx->note_suppressed_program[prog][note]) {
                any_suppressed = 1;
                break;
            }
        }
        if (any_suppressed) {
            fprintf(f, "# Program %d\n", prog + 1);
            for (int note = 0; note < 128; note++) {
                if (rsx->note_suppressed_program[prog][note]) {
                    fprintf(f, "prog_%d_%d=1\n", prog + 1, note);
                }
            }
        }
    }
    fprintf(f, "\n");

    // Write note trigger pads
    fprintf(f, "[NoteTriggerPads]\n");
    for (int i = 0; i < rsx->num_pads; i++) {
        if (rsx->pads[i].note < 0) continue;

        int pad_num = i + 1;
        fprintf(f, "pad_N%d_note=%d\n", pad_num, rsx->pads[i].note);

        if (rsx->pads[i].description[0] != '\0') {
            fprintf(f, "pad_N%d_description=\"%s\"\n", pad_num, rsx->pads[i].description);
        }

        if (rsx->pads[i].velocity > 0) {
            fprintf(f, "pad_N%d_velocity=%d\n", pad_num, rsx->pads[i].velocity);
        }

        if (rsx->pads[i].pitch_bend != 0.0f) {
            fprintf(f, "pad_N%d_pitch_bend=%.3f\n", pad_num, rsx->pads[i].pitch_bend);
        }

        if (!isnan(rsx->pads[i].pan)) {
            fprintf(f, "pad_N%d_pan=%.3f\n", pad_num, rsx->pads[i].pan);
        }

        if (!isnan(rsx->pads[i].volume)) {
            fprintf(f, "pad_N%d_volume=%.3f\n", pad_num, rsx->pads[i].volume);
        }

        if (!rsx->pads[i].enabled) {
            fprintf(f, "pad_N%d_enabled=0\n", pad_num);
        }

        if (rsx->pads[i].program >= 0) {
            fprintf(f, "pad_N%d_program=%d\n", pad_num, rsx->pads[i].program);
        }

        fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}

void samplecrate_rsx_get_sfz_path(const char* rsx_path, const char* sfz_relative,
                                   char* out_path, size_t out_size) {
    if (!rsx_path || !sfz_relative || !out_path || out_size == 0) return;

    // Make a copy of rsx_path for dirname (it may modify the string)
    char rsx_path_copy[RSX_MAX_PATH];
    strncpy(rsx_path_copy, rsx_path, sizeof(rsx_path_copy) - 1);
    rsx_path_copy[sizeof(rsx_path_copy) - 1] = '\0';

    // Get directory of RSX file
    char* dir = dirname(rsx_path_copy);

    // Make a copy of sfz_relative and convert backslashes to forward slashes
    char sfz_normalized[RSX_MAX_PATH];
    strncpy(sfz_normalized, sfz_relative, sizeof(sfz_normalized) - 1);
    sfz_normalized[sizeof(sfz_normalized) - 1] = '\0';
    for (char* p = sfz_normalized; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    // Combine directory with relative SFZ path
    snprintf(out_path, out_size, "%s/%s", dir, sfz_normalized);
}
