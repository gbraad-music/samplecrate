#ifndef SAMPLECRATE_COMMON_H
#define SAMPLECRATE_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

// Mixer state
typedef struct {
    // Master channel
    float master_volume;
    float master_pan;
    int master_mute;

    // Playback channel (SFZ/synth output)
    float playback_volume;
    float playback_pan;
    int playback_mute;

    // Per-program volumes (for multi-program RSX files)
    float program_volumes[4];  // Volume for each program (0-3)
    float program_pans[4];     // Pan for each program (0.0=left, 0.5=center, 1.0=right)
    int program_mutes[4];      // Mute for each program
    int program_fx_enable[4];  // FX enable per program (0=disabled, 1=enabled)

    // FX enable toggles (independent)
    int master_fx_enable;      // 0 = disabled, 1 = enabled
} SamplecrateMixer;

// Configuration structure (saved to .ini file)
typedef struct {
    // Device configuration
    int midi_device_0;   // MIDI device 0 port (-1 = not configured)
    int midi_device_1;   // MIDI device 1 port (-1 = not configured)
    int midi_channel[2]; // MIDI channel filter per device (-1 = Omni/all channels, 0-15 = specific channel)
    int audio_device;    // Audio output device (-1 = default)
    int expanded_pads;   // 0 = 16 pads, 1 = 32 pads
    int lock_ui_program_selection;  // 0 = allow UI control, 1 = lock to MIDI only
    int midi_program_change_enabled[2];  // Per-device: 0 = ignore (UI selection leads), 1 = receive but don't change UI

    // Mixer defaults
    float default_master_volume;
    float default_master_pan;
    float default_playback_volume;
    float default_playback_pan;

    // Effect defaults
    float fx_distortion_drive;
    float fx_distortion_mix;
    float fx_filter_cutoff;
    float fx_filter_resonance;
    float fx_eq_low;
    float fx_eq_mid;
    float fx_eq_high;
    float fx_compressor_threshold;
    float fx_compressor_ratio;
    float fx_compressor_attack;
    float fx_compressor_release;
    float fx_compressor_makeup;
    float fx_phaser_rate;
    float fx_phaser_depth;
    float fx_phaser_feedback;
    float fx_reverb_room_size;
    float fx_reverb_damping;
    float fx_reverb_mix;
    float fx_delay_time;
    float fx_delay_feedback;
    float fx_delay_mix;
} SamplecrateConfig;

// Initialize mixer with default values
void samplecrate_mixer_init(SamplecrateMixer* mixer);

// Initialize config with default values
void samplecrate_config_init(SamplecrateConfig* config);

// Load configuration from .ini file
int samplecrate_config_load(SamplecrateConfig* config, const char* filepath);

// Save configuration to .ini file
int samplecrate_config_save(const SamplecrateConfig* config, const char* filepath);

#ifdef __cplusplus
}
#endif

#endif // SAMPLECRATE_COMMON_H
