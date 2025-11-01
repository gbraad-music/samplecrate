#ifndef MEDNESS_TRACK_H
#define MEDNESS_TRACK_H

#ifdef __cplusplus
extern "C" {
#endif

// A track is a passive container for MIDI event data
// The sequencer plays tracks - tracks don't play themselves

typedef struct MednessTrack MednessTrack;

// MIDI event in a track
typedef struct {
    int tick;           // MIDI tick when event occurs
    int note;           // MIDI note number
    int velocity;       // Note velocity (0-127)
    int on;             // 1=note_on, 0=note_off
} MednessTrackEvent;

// Create a new empty track
MednessTrack* medness_track_create(void);

// Destroy a track
void medness_track_destroy(MednessTrack* track);

// Load MIDI file data into the track
// Returns 0 on success, -1 on error
int medness_track_load_midi_file(MednessTrack* track, const char* filename);

// Get the number of events in the track
int medness_track_get_event_count(MednessTrack* track);

// Get a specific event by index
// Returns NULL if index is out of bounds
const MednessTrackEvent* medness_track_get_event(MednessTrack* track, int index);

// Get all events in the track (for iteration)
// Returns pointer to event array, count is written to out_count
const MednessTrackEvent* medness_track_get_events(MednessTrack* track, int* out_count);

// Get track duration in ticks
int medness_track_get_duration_ticks(MednessTrack* track);

// Get ticks per quarter note (from MIDI file)
int medness_track_get_tpqn(MednessTrack* track);

#ifdef __cplusplus
}
#endif

#endif // MEDNESS_TRACK_H
