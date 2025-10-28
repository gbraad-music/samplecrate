# Sample Mode Integration Guide

This guide shows how to integrate sample-based program creation into the CRATE panel UI.

## Overview

The RSX structure now supports two modes for each program:
1. **SFZ File Mode** - Load an existing SFZ file (original behavior)
2. **Samples Mode** - Build SFZ programmatically from individual wave files

## Data Structures

### RSX Fields (already added)

```c
// In samplecrate_rsx.h:
RSXProgramMode program_modes[RSX_MAX_PROGRAMS];  // Mode for each program
RSXSampleMapping program_samples[RSX_MAX_PROGRAMS][RSX_MAX_SAMPLES_PER_PROGRAM];
int program_sample_counts[RSX_MAX_PROGRAMS];
```

### Sample Mapping Structure

```c
typedef struct {
    char sample_path[RSX_MAX_PATH];  // Path to wave file
    int key_low;                      // Low MIDI note (0-127)
    int key_high;                     // High MIDI note (0-127)
    int root_key;                     // Root pitch
    int vel_low;                      // Low velocity (0-127)
    int vel_high;                     // High velocity (0-127)
    float amplitude;                  // Volume (0.0-1.0)
    float pan;                        // Pan (-1.0 to 1.0)
    int enabled;                      // 1=enabled, 0=disabled
} RSXSampleMapping;
```

## UI Integration (main.cpp)

### Step 1: Add Mode Selector to Program Configuration

In the CRATE panel where each program is configured (around line 1766-1778):

```cpp
// After the program name input, add mode selector
ImGui::Text("Mode:");
ImGui::SameLine(80);

const char* mode_items[] = {"SFZ File", "Samples"};
int current_mode = (int)rsx->program_modes[i];

if (ImGui::Combo("##prog_mode", &current_mode, mode_items, 2)) {
    rsx->program_modes[i] = (RSXProgramMode)current_mode;
    if (!rsx_file_path.empty()) {
        samplecrate_rsx_save(rsx, rsx_file_path.c_str());
    }
}
```

### Step 2: Conditional UI Based on Mode

```cpp
if (rsx->program_modes[i] == PROGRAM_MODE_SFZ_FILE) {
    // Original file input (existing code)
    ImGui::Text("File:");
    ImGui::SameLine(80);
    ImGui::InputText("##prog_file", rsx->program_files[i], sizeof(rsx->program_files[i]));
}
else if (rsx->program_modes[i] == PROGRAM_MODE_SAMPLES) {
    // Sample list UI
    ImGui::Text("Samples: %d", rsx->program_sample_counts[i]);

    if (ImGui::Button("Add Sample")) {
        if (rsx->program_sample_counts[i] < RSX_MAX_SAMPLES_PER_PROGRAM) {
            int idx = rsx->program_sample_counts[i];
            RSXSampleMapping* sample = &rsx->program_samples[i][idx];

            // Initialize defaults
            sample->sample_path[0] = '\0';
            sample->key_low = 60;   // Middle C
            sample->key_high = 60;
            sample->root_key = 60;
            sample->vel_low = 0;
            sample->vel_high = 127;
            sample->amplitude = 1.0f;
            sample->pan = 0.0f;
            sample->enabled = 1;

            rsx->program_sample_counts[i]++;
        }
    }

    // Show sample list
    for (int s = 0; s < rsx->program_sample_counts[i]; s++) {
        RSXSampleMapping* sample = &rsx->program_samples[i][s];

        ImGui::PushID(s);
        ImGui::Text("Sample %d:", s + 1);
        ImGui::InputText("Path", sample->sample_path, sizeof(sample->sample_path));
        ImGui::SliderInt("Note Low", &sample->key_low, 0, 127);
        ImGui::SliderInt("Note High", &sample->key_high, 0, 127);
        ImGui::SliderFloat("Volume", &sample->amplitude, 0.0f, 1.0f);

        if (ImGui::Button("Remove")) {
            // Shift samples down
            for (int j = s; j < rsx->program_sample_counts[i] - 1; j++) {
                rsx->program_samples[i][j] = rsx->program_samples[i][j + 1];
            }
            rsx->program_sample_counts[i]--;
        }
        ImGui::PopID();
        ImGui::Separator();
    }
}
```

### Step 3: Build SFZ from Samples When Loading Program

In the function that loads programs (find where `sfizz_load_file` is called):

```cpp
#include "sfz_builder.h"

// When loading a program
if (rsx->program_modes[program_idx] == PROGRAM_MODE_SFZ_FILE) {
    // Original: load from file
    char full_path[1024];
    samplecrate_rsx_get_sfz_path(rsx_file_path.c_str(),
                                  rsx->program_files[program_idx],
                                  full_path, sizeof(full_path));
    sfizz_load_file(program_synths[program_idx], full_path);
}
else if (rsx->program_modes[program_idx] == PROGRAM_MODE_SAMPLES) {
    // New: build from samples
    SFZBuilder* builder = sfz_builder_create(44100);

    for (int s = 0; s < rsx->program_sample_counts[program_idx]; s++) {
        RSXSampleMapping* sample = &rsx->program_samples[program_idx][s];

        if (sample->enabled) {
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

    sfz_builder_load(builder, program_synths[program_idx]);
    sfz_builder_destroy(builder);
}
```

## Example Workflow

1. **User adds a new program** in the CRATE panel
2. **User selects "Samples" mode** from the dropdown
3. **User clicks "Add Sample"** button multiple times
4. **For each sample**, user enters:
   - Path to .wav file (e.g., `/path/to/kick.wav`)
   - MIDI note range (e.g., 36-36 for kick on C1)
   - Volume and pan
5. **When program is loaded**, the SFZ builder creates the instrument on-the-fly
6. **No .sfz file needed** - everything is stored in the .rsx file

## Quick Start Example

```cpp
// In UI code - adding a kick drum sample
RSXSampleMapping* kick = &rsx->program_samples[0][0];
strcpy(kick->sample_path, "/assets/kick.wav");
kick->key_low = 36;
kick->key_high = 36;
kick->root_key = 36;
kick->vel_low = 0;
kick->vel_high = 127;
kick->amplitude = 1.0f;
kick->pan = 0.0f;
kick->enabled = 1;
rsx->program_sample_counts[0] = 1;

// When loading the program
SFZBuilder* builder = sfz_builder_create(44100);
sfz_builder_add_region(builder, kick->sample_path,
                      kick->key_low, kick->key_high, kick->root_key,
                      kick->vel_low, kick->vel_high,
                      kick->amplitude, kick->pan);
sfz_builder_load(builder, synth);
sfz_builder_destroy(builder);
```

## File Format (RSX)

The sample data is saved in the .rsx file. Example format:

```
prog_1_mode=samples
prog_1_sample_count=3
prog_1_sample_0_path=kick.wav
prog_1_sample_0_key_low=36
prog_1_sample_0_key_high=36
prog_1_sample_0_root_key=36
prog_1_sample_0_vel_low=0
prog_1_sample_0_vel_high=127
prog_1_sample_0_amplitude=1.0
prog_1_sample_0_pan=0.0
prog_1_sample_0_enabled=1
```

(Update `samplecrate_rsx_save()` and `samplecrate_rsx_load()` to read/write these fields)

## Benefits

- **No manual SFZ editing** - users just add wave files
- **Visual configuration** - all settings in UI
- **Portable** - everything saved in .rsx file
- **Flexible** - can mix SFZ files and sample-based programs
- **Quick prototyping** - rapidly build drum kits

## Next Steps

1. Implement the UI changes in main.cpp (CRATE panel section)
2. Update `samplecrate_rsx_save()` to write sample data
3. Update `samplecrate_rsx_load()` to read sample data
4. Add file browser for selecting wave files
5. Add preset templates (e.g., "GM Drum Kit" with pre-mapped notes)

## Testing

```cpp
// Test creating a simple drum kit
rsx->program_modes[0] = PROGRAM_MODE_SAMPLES;
rsx->program_sample_counts[0] = 3;

// Kick
strcpy(rsx->program_samples[0][0].sample_path, "kick.wav");
rsx->program_samples[0][0].key_low = 36;
rsx->program_samples[0][0].key_high = 36;
rsx->program_samples[0][0].root_key = 36;

// Snare
strcpy(rsx->program_samples[0][1].sample_path, "snare.wav");
rsx->program_samples[0][1].key_low = 38;
rsx->program_samples[0][1].key_high = 38;
rsx->program_samples[0][1].root_key = 38;

// Hi-hat
strcpy(rsx->program_samples[0][2].sample_path, "hihat.wav");
rsx->program_samples[0][2].key_low = 42;
rsx->program_samples[0][2].key_high = 42;
rsx->program_samples[0][2].root_key = 42;
```

Then load the program and test!
