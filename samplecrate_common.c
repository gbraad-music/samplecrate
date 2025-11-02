#include "samplecrate_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void samplecrate_mixer_init(SamplecrateMixer* mixer) {
    if (!mixer) return;

    mixer->master_volume = 0.7f;
    mixer->master_pan = 0.5f;
    mixer->master_mute = 0;

    mixer->playback_volume = 0.8f;
    mixer->playback_pan = 0.5f;
    mixer->playback_mute = 0;

    // Initialize per-program volumes
    for (int i = 0; i < 4; i++) {
        mixer->program_volumes[i] = 1.0f;  // 100% volume
        mixer->program_pans[i] = 0.5f;     // Center
        mixer->program_mutes[i] = 0;
        mixer->program_fx_enable[i] = 0;   // FX disabled by default
    }

    mixer->master_fx_enable = 1;  // Master FX enabled by default
}

void samplecrate_config_init(SamplecrateConfig* config) {
    if (!config) return;

    // Device defaults
    config->midi_device_0 = -1;  // Not configured
    config->midi_device_1 = -1;  // Not configured
    config->midi_device_2 = -1;  // Not configured
    config->midi_channel[0] = -1;  // Device 0: Omni (all channels)
    config->midi_channel[1] = -1;  // Device 1: Omni (all channels)
    config->midi_channel[2] = -1;  // Device 2: Omni (all channels)
    config->audio_device = -1;   // Use default
    config->expanded_pads = 0;   // Normal 16 pads by default
    config->lock_ui_program_selection = 0;  // Allow UI control by default
    config->midi_program_change_enabled[0] = 1;  // Device 0: Accept MIDI program changes by default
    config->midi_program_change_enabled[1] = 0;  // Device 1: Ignore program changes by default (use UI)
    config->midi_program_change_enabled[2] = 0;  // Device 2: Ignore program changes by default (use UI)

    // MIDI sync defaults
    config->midi_clock_tempo_sync = 1;  // Enabled by default (adjust tempo to MIDI clock)
    config->midi_spp_receive = 1;       // Enabled by default (sync to SPP)

    // SysEx defaults
    config->sysex_device_id = 0;        // Device ID 0 by default

    // Mixer defaults
    config->default_master_volume = 0.7f;
    config->default_master_pan = 0.5f;
    config->default_playback_volume = 0.8f;
    config->default_playback_pan = 0.5f;

    // Effect defaults (neutral/off positions)
    config->fx_distortion_drive = 0.5f;
    config->fx_distortion_mix = 0.0f;
    config->fx_filter_cutoff = 1.0f;      // Fully open
    config->fx_filter_resonance = 0.0f;
    config->fx_eq_low = 0.5f;             // Centered (no boost/cut)
    config->fx_eq_mid = 0.5f;
    config->fx_eq_high = 0.5f;
    config->fx_compressor_threshold = 0.8f;
    config->fx_compressor_ratio = 0.2f;
    config->fx_compressor_attack = 0.3f;
    config->fx_compressor_release = 0.5f;
    config->fx_compressor_makeup = 0.5f;
    config->fx_phaser_rate = 0.3f;
    config->fx_phaser_depth = 0.5f;
    config->fx_phaser_feedback = 0.3f;
    config->fx_reverb_room_size = 0.5f;
    config->fx_reverb_damping = 0.5f;
    config->fx_reverb_mix = 0.0f;
    config->fx_delay_time = 0.3f;
    config->fx_delay_feedback = 0.3f;
    config->fx_delay_mix = 0.0f;
}

int samplecrate_config_load(SamplecrateConfig* config, const char* filepath) {
    if (!config || !filepath) return 0;

    FILE* f = fopen(filepath, "r");
    if (!f) {
        // File doesn't exist, use defaults
        samplecrate_config_init(config);
        return 0;
    }

    // Initialize with defaults first
    samplecrate_config_init(config);

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        line[strcspn(line, "\r\n")] = 0;

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;

        // Check for section header
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        // Parse key=value
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char* key = line;
        const char* value = eq + 1;

        // Trim whitespace from key
        while (*key == ' ' || *key == '\t') key++;
        char* key_end = eq - 1;
        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
            *key_end = '\0';
            key_end--;
        }

        // Trim whitespace from value
        while (*value == ' ' || *value == '\t') value++;

        // Parse based on section
        if (strcmp(section, "devices") == 0) {
            if (strcmp(key, "midi_device_0") == 0) config->midi_device_0 = atoi(value);
            else if (strcmp(key, "midi_device_1") == 0) config->midi_device_1 = atoi(value);
            else if (strcmp(key, "midi_device_2") == 0) config->midi_device_2 = atoi(value);
            else if (strcmp(key, "midi_channel_device_0") == 0) config->midi_channel[0] = atoi(value);
            else if (strcmp(key, "midi_channel_device_1") == 0) config->midi_channel[1] = atoi(value);
            else if (strcmp(key, "midi_channel_device_2") == 0) config->midi_channel[2] = atoi(value);
            else if (strcmp(key, "audio_device") == 0) config->audio_device = atoi(value);
            else if (strcmp(key, "expanded_pads") == 0) config->expanded_pads = atoi(value);
            else if (strcmp(key, "lock_ui_program_selection") == 0) config->lock_ui_program_selection = atoi(value);
            else if (strcmp(key, "midi_program_change_enabled_device_0") == 0) config->midi_program_change_enabled[0] = atoi(value);
            else if (strcmp(key, "midi_program_change_enabled_device_1") == 0) config->midi_program_change_enabled[1] = atoi(value);
            else if (strcmp(key, "midi_program_change_enabled_device_2") == 0) config->midi_program_change_enabled[2] = atoi(value);
            else if (strcmp(key, "midi_clock_tempo_sync") == 0) config->midi_clock_tempo_sync = atoi(value);
            else if (strcmp(key, "midi_spp_receive") == 0) config->midi_spp_receive = atoi(value);
            else if (strcmp(key, "sysex_device_id") == 0) config->sysex_device_id = atoi(value);
            // Legacy support for old config files
            else if (strcmp(key, "midi_program_change_enabled") == 0) {
                int val = atoi(value);
                config->midi_program_change_enabled[0] = val;
                config->midi_program_change_enabled[1] = val;
                config->midi_program_change_enabled[2] = val;
            }
        }
        else if (strcmp(section, "Mixer") == 0) {
            if (strcmp(key, "master_volume") == 0) config->default_master_volume = atof(value);
            else if (strcmp(key, "master_pan") == 0) config->default_master_pan = atof(value);
            else if (strcmp(key, "playback_volume") == 0) config->default_playback_volume = atof(value);
            else if (strcmp(key, "playback_pan") == 0) config->default_playback_pan = atof(value);
        }
        else if (strcmp(section, "Effects") == 0) {
            if (strcmp(key, "distortion_drive") == 0) config->fx_distortion_drive = atof(value);
            else if (strcmp(key, "distortion_mix") == 0) config->fx_distortion_mix = atof(value);
            else if (strcmp(key, "filter_cutoff") == 0) config->fx_filter_cutoff = atof(value);
            else if (strcmp(key, "filter_resonance") == 0) config->fx_filter_resonance = atof(value);
            else if (strcmp(key, "eq_low") == 0) config->fx_eq_low = atof(value);
            else if (strcmp(key, "eq_mid") == 0) config->fx_eq_mid = atof(value);
            else if (strcmp(key, "eq_high") == 0) config->fx_eq_high = atof(value);
            else if (strcmp(key, "compressor_threshold") == 0) config->fx_compressor_threshold = atof(value);
            else if (strcmp(key, "compressor_ratio") == 0) config->fx_compressor_ratio = atof(value);
            else if (strcmp(key, "compressor_attack") == 0) config->fx_compressor_attack = atof(value);
            else if (strcmp(key, "compressor_release") == 0) config->fx_compressor_release = atof(value);
            else if (strcmp(key, "compressor_makeup") == 0) config->fx_compressor_makeup = atof(value);
            else if (strcmp(key, "phaser_rate") == 0) config->fx_phaser_rate = atof(value);
            else if (strcmp(key, "phaser_depth") == 0) config->fx_phaser_depth = atof(value);
            else if (strcmp(key, "phaser_feedback") == 0) config->fx_phaser_feedback = atof(value);
            else if (strcmp(key, "reverb_room_size") == 0) config->fx_reverb_room_size = atof(value);
            else if (strcmp(key, "reverb_damping") == 0) config->fx_reverb_damping = atof(value);
            else if (strcmp(key, "reverb_mix") == 0) config->fx_reverb_mix = atof(value);
            else if (strcmp(key, "delay_time") == 0) config->fx_delay_time = atof(value);
            else if (strcmp(key, "delay_feedback") == 0) config->fx_delay_feedback = atof(value);
            else if (strcmp(key, "delay_mix") == 0) config->fx_delay_mix = atof(value);
        }
    }

    fclose(f);
    return 1;
}

int samplecrate_config_save(const SamplecrateConfig* config, const char* filepath) {
    if (!config || !filepath) return 0;

    FILE* f = fopen(filepath, "w");
    if (!f) return 0;

    fprintf(f, "; samplecrate configuration file\n\n");

    fprintf(f, "[devices]\n");
    fprintf(f, "midi_device_0=%d\n", config->midi_device_0);
    fprintf(f, "midi_device_1=%d\n", config->midi_device_1);
    fprintf(f, "midi_device_2=%d\n", config->midi_device_2);
    fprintf(f, "midi_channel_device_0=%d  ; -1 = Omni (all channels), 0-15 = MIDI channel 1-16\n", config->midi_channel[0]);
    fprintf(f, "midi_channel_device_1=%d  ; -1 = Omni (all channels), 0-15 = MIDI channel 1-16\n", config->midi_channel[1]);
    fprintf(f, "midi_channel_device_2=%d  ; -1 = Omni (all channels), 0-15 = MIDI channel 1-16\n", config->midi_channel[2]);
    fprintf(f, "audio_device=%d\n", config->audio_device);
    fprintf(f, "expanded_pads=%d\n", config->expanded_pads);
    fprintf(f, "midi_program_change_enabled_device_0=%d\n", config->midi_program_change_enabled[0]);
    fprintf(f, "midi_program_change_enabled_device_1=%d\n", config->midi_program_change_enabled[1]);
    fprintf(f, "midi_program_change_enabled_device_2=%d\n", config->midi_program_change_enabled[2]);
    fprintf(f, "midi_clock_tempo_sync=%d  ; 0 = visual only, 1 = adjust playback tempo\n", config->midi_clock_tempo_sync);
    fprintf(f, "midi_spp_receive=%d  ; 0 = ignore SPP, 1 = sync to SPP\n", config->midi_spp_receive);
    fprintf(f, "sysex_device_id=%d  ; SysEx device ID (0-127) for remote control\n", config->sysex_device_id);
    fprintf(f, "\n");

    fprintf(f, "[Mixer]\n");
    fprintf(f, "master_volume=%.3f\n", config->default_master_volume);
    fprintf(f, "master_pan=%.3f\n", config->default_master_pan);
    fprintf(f, "playback_volume=%.3f\n", config->default_playback_volume);
    fprintf(f, "playback_pan=%.3f\n", config->default_playback_pan);
    fprintf(f, "\n");

    fprintf(f, "[Effects]\n");
    fprintf(f, "distortion_drive=%.3f\n", config->fx_distortion_drive);
    fprintf(f, "distortion_mix=%.3f\n", config->fx_distortion_mix);
    fprintf(f, "filter_cutoff=%.3f\n", config->fx_filter_cutoff);
    fprintf(f, "filter_resonance=%.3f\n", config->fx_filter_resonance);
    fprintf(f, "eq_low=%.3f\n", config->fx_eq_low);
    fprintf(f, "eq_mid=%.3f\n", config->fx_eq_mid);
    fprintf(f, "eq_high=%.3f\n", config->fx_eq_high);
    fprintf(f, "compressor_threshold=%.3f\n", config->fx_compressor_threshold);
    fprintf(f, "compressor_ratio=%.3f\n", config->fx_compressor_ratio);
    fprintf(f, "compressor_attack=%.3f\n", config->fx_compressor_attack);
    fprintf(f, "compressor_release=%.3f\n", config->fx_compressor_release);
    fprintf(f, "compressor_makeup=%.3f\n", config->fx_compressor_makeup);
    fprintf(f, "phaser_rate=%.3f\n", config->fx_phaser_rate);
    fprintf(f, "phaser_depth=%.3f\n", config->fx_phaser_depth);
    fprintf(f, "phaser_feedback=%.3f\n", config->fx_phaser_feedback);
    fprintf(f, "reverb_room_size=%.3f\n", config->fx_reverb_room_size);
    fprintf(f, "reverb_damping=%.3f\n", config->fx_reverb_damping);
    fprintf(f, "reverb_mix=%.3f\n", config->fx_reverb_mix);
    fprintf(f, "delay_time=%.3f\n", config->fx_delay_time);
    fprintf(f, "delay_feedback=%.3f\n", config->fx_delay_feedback);
    fprintf(f, "delay_mix=%.3f\n", config->fx_delay_mix);

    fclose(f);
    return 1;
}
