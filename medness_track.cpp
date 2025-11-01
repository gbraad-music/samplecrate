#include "medness_track.h"
#include "MidiFile.h"
#include <vector>
#include <algorithm>

using namespace smf;

struct MednessTrack {
    std::vector<MednessTrackEvent> events;
    int ticks_per_quarter;
    int duration_ticks;
};

MednessTrack* medness_track_create(void) {
    MednessTrack* track = new MednessTrack();
    track->ticks_per_quarter = 480;  // Default TPQN
    track->duration_ticks = 0;
    return track;
}

void medness_track_destroy(MednessTrack* track) {
    if (track) {
        delete track;
    }
}

int medness_track_load_midi_file(MednessTrack* track, const char* filename) {
    if (!track || !filename) return -1;

    MidiFile midifile;

    // Load the MIDI file
    if (!midifile.read(filename)) {
        return -1;
    }

    // Make absolute ticks
    midifile.doTimeAnalysis();
    midifile.linkNotePairs();

    track->ticks_per_quarter = midifile.getTicksPerQuarterNote();

    // Extract note events from all tracks
    track->events.clear();

    for (int t = 0; t < midifile.getTrackCount(); t++) {
        for (int e = 0; e < midifile[t].size(); e++) {
            MidiEvent& me = midifile[t][e];

            if (me.isNoteOn()) {
                MednessTrackEvent evt;
                evt.tick = me.tick;
                evt.note = me.getKeyNumber();
                evt.velocity = me.getVelocity();
                evt.on = 1;
                track->events.push_back(evt);
            } else if (me.isNoteOff()) {
                MednessTrackEvent evt;
                evt.tick = me.tick;
                evt.note = me.getKeyNumber();
                evt.velocity = 0;
                evt.on = 0;
                track->events.push_back(evt);
            }
        }
    }

    // Sort events by tick, with NOTE OFFs before NOTE ONs at the same tick
    std::sort(track->events.begin(), track->events.end(),
              [](const MednessTrackEvent& a, const MednessTrackEvent& b) {
                  if (a.tick == b.tick) {
                      // At same tick: OFF (0) before ON (1)
                      return a.on < b.on;
                  }
                  return a.tick < b.tick;
              });

    // Calculate duration
    if (!track->events.empty()) {
        track->duration_ticks = track->events.back().tick;
    } else {
        track->duration_ticks = 0;
    }

    return 0;
}

int medness_track_get_event_count(MednessTrack* track) {
    if (!track) return 0;
    return (int)track->events.size();
}

const MednessTrackEvent* medness_track_get_event(MednessTrack* track, int index) {
    if (!track || index < 0 || index >= (int)track->events.size()) {
        return nullptr;
    }
    return &track->events[index];
}

const MednessTrackEvent* medness_track_get_events(MednessTrack* track, int* out_count) {
    if (!track) {
        if (out_count) *out_count = 0;
        return nullptr;
    }
    if (out_count) *out_count = (int)track->events.size();
    return track->events.data();
}

int medness_track_get_duration_ticks(MednessTrack* track) {
    if (!track) return 0;
    return track->duration_ticks;
}

int medness_track_get_tpqn(MednessTrack* track) {
    if (!track) return 480;
    return track->ticks_per_quarter;
}
