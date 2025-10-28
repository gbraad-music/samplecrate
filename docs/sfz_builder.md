# SFZ Builder API

A utility library for creating sfizz synths programmatically from wave files without needing to manually write SFZ files.

## Overview

The SFZ Builder API provides two approaches:

1. **Simple API** - Quick one-liner for loading a single sample
2. **Builder API** - Flexible multi-sample construction (drum kits, velocity layers, etc.)

## Quick Start

### Load a Single Sample

```c
#include "sfz_builder.h"
#include <sfizz.h>

// Create synth
sfizz_synth_t* synth = sfizz_create_synth();
sfizz_set_sample_rate(synth, 44100);

// Load kick drum on MIDI note 36
sfz_load_simple_sample(synth, "/path/to/kick.wav",
                       36, 36, 36,  // key range (note 36 only)
                       0, 127,      // all velocities
                       1.0f);       // full volume

// Use it!
sfizz_send_note_on(synth, 0, 36, 100);
```

### Build a Drum Kit

```c
// Create synth and builder
sfizz_synth_t* synth = sfizz_create_synth();
sfizz_set_sample_rate(synth, 44100);
SFZBuilder* builder = sfz_builder_create(44100);

// Add samples
sfz_builder_add_region(builder, "/path/to/kick.wav",
                       36, 36, 36,  // MIDI note 36
                       0, 127,      // full velocity
                       1.0f, 0.0f); // volume, pan

sfz_builder_add_region(builder, "/path/to/snare.wav",
                       38, 38, 38,  // MIDI note 38
                       0, 127,
                       1.0f, 0.0f);

sfz_builder_add_region(builder, "/path/to/hihat.wav",
                       42, 42, 42,  // MIDI note 42
                       0, 127,
                       0.8f, 0.0f); // quieter

// Load into synth
sfz_builder_load(builder, synth);

// Clean up builder (synth keeps the content)
sfz_builder_destroy(builder);
```

## API Reference

### Simple API

#### `sfz_load_simple_sample()`

Load a single wave file as a one-shot sample.

```c
int sfz_load_simple_sample(sfizz_synth_t* synth,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude);
```

**Parameters:**
- `synth` - The sfizz synth (must be created first)
- `sample_path` - Full path to the wave file
- `key_low` - Lowest MIDI note (0-127)
- `key_high` - Highest MIDI note (0-127)
- `root_key` - Root pitch (usually same as key_low for drums)
- `vel_low` - Lowest velocity (0-127)
- `vel_high` - Highest velocity (0-127)
- `amplitude` - Volume (0.0-1.0, where 1.0 = full volume)

**Returns:** 0 on success, -1 on error

**Example:**
```c
// Load kick on note 36 only
sfz_load_simple_sample(synth, "kick.wav", 36, 36, 36, 0, 127, 1.0f);

// Load bass sample across an octave (will pitch-shift)
sfz_load_simple_sample(synth, "bass_c.wav", 36, 47, 36, 0, 127, 1.0f);
```

### Builder API

#### `sfz_builder_create()`

Create a new SFZ builder.

```c
SFZBuilder* sfz_builder_create(int sample_rate);
```

**Parameters:**
- `sample_rate` - Sample rate (e.g., 44100)

**Returns:** Builder instance, or NULL on error

---

#### `sfz_builder_add_region()`

Add a region (sample) to the builder.

```c
int sfz_builder_add_region(SFZBuilder* builder,
                           const char* sample_path,
                           int key_low, int key_high, int root_key,
                           int vel_low, int vel_high,
                           float amplitude, float pan);
```

**Parameters:**
- `builder` - The builder instance
- `sample_path` - Full path to the wave file
- `key_low` - Lowest MIDI note (0-127)
- `key_high` - Highest MIDI note (0-127)
- `root_key` - Root pitch
- `vel_low` - Lowest velocity (0-127)
- `vel_high` - Highest velocity (0-127)
- `amplitude` - Volume (0.0-1.0)
- `pan` - Pan position (-1.0=left, 0.0=center, 1.0=right)

**Returns:** 0 on success, -1 on error

---

#### `sfz_builder_load()`

Build the SFZ and load it into a synth.

```c
int sfz_builder_load(SFZBuilder* builder, sfizz_synth_t* synth);
```

**Parameters:**
- `builder` - The builder instance
- `synth` - The sfizz synth to load into

**Returns:** 0 on success, -1 on error

---

#### `sfz_builder_destroy()`

Free the builder.

```c
void sfz_builder_destroy(SFZBuilder* builder);
```

**Parameters:**
- `builder` - The builder instance to free

## Common Use Cases

### Velocity Layers

```c
SFZBuilder* builder = sfz_builder_create(44100);

// Soft hit (velocity 0-63)
sfz_builder_add_region(builder, "snare_soft.wav",
                       38, 38, 38, 0, 63, 1.0f, 0.0f);

// Hard hit (velocity 64-127)
sfz_builder_add_region(builder, "snare_hard.wav",
                       38, 38, 38, 64, 127, 1.0f, 0.0f);

sfz_builder_load(builder, synth);
```

### Panned Samples

```c
// Kick - center
sfz_builder_add_region(builder, "kick.wav", 36, 36, 36, 0, 127, 1.0f, 0.0f);

// Hi-hat - left
sfz_builder_add_region(builder, "hihat.wav", 42, 42, 42, 0, 127, 0.8f, -0.5f);

// Crash - right
sfz_builder_add_region(builder, "crash.wav", 49, 49, 49, 0, 127, 0.9f, 0.5f);
```

### Chromatic Sample Mapping

```c
// Map a single bass sample across an octave
// sfizz will pitch-shift automatically
sfz_builder_add_region(builder, "bass_c.wav",
                       36, 47,  // C1 to B1
                       36,      // root at C1
                       0, 127, 1.0f, 0.0f);
```

## MIDI Note Reference

Common drum notes (General MIDI):
- 35 = Bass Drum 2
- 36 = Bass Drum 1 (Kick)
- 37 = Side Stick
- 38 = Snare
- 42 = Closed Hi-Hat
- 44 = Pedal Hi-Hat
- 46 = Open Hi-Hat
- 49 = Crash Cymbal 1
- 51 = Ride Cymbal 1

## Implementation Notes

- The API generates SFZ content in memory using `sfizz_load_string()`
- No temporary files are created
- The builder can hold up to 256 regions
- Volume is automatically converted from linear (0-1) to dB (-100 to 0)
- Pan is converted from normalized (-1 to 1) to SFZ format (-100 to 100)
- Default envelope: 1ms attack, 300ms release

## Thread Safety

The builder itself is not thread-safe. Create separate builders for each thread, or use external synchronization.

The sfizz synth itself has its own thread-safety characteristics - refer to sfizz documentation.

## Error Handling

All functions return -1 on error (or NULL for create functions). Common errors:
- NULL parameters
- Invalid file paths
- Out of memory
- Too many regions (>256)
- Invalid MIDI note ranges (must be 0-127)

## Performance

- Building an SFZ with 64 regions takes < 1ms
- Loading into sfizz depends on sample sizes (sfizz loads samples into memory)
- No performance overhead during playback compared to file-based SFZ

## Example: Complete Drum Machine

See `sfz_builder_example.c` for complete examples including:
- Simple single samples
- Multi-sample drum kits
- Velocity layers
- Chromatic mapping
