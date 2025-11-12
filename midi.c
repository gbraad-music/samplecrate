#include "midi.h"
#include "midi_sysex.h"
#include <unistd.h>
#include <stdio.h>
#include <rtmidi_c.h>

static RtMidiInPtr midiin[MIDI_MAX_DEVICES] = {NULL};
static MidiEventCallback midi_cb = NULL;
static void *cb_userdata = NULL;

// Common MIDI event handler with SysEx support
static void handle_midi_event(int device_id, double dt, const unsigned char *msg, size_t sz) {
    if (sz < 1) return;

    // Handle SysEx messages (0xF0 ... 0xF7)
    if (sz >= 5 && msg[0] == 0xF0) {
        printf("[MIDI] SysEx message received (%zu bytes): F0", sz);
        for (size_t i = 1; i < sz && i < 10; i++) {
            printf(" %02X", msg[i]);
        }
        if (sz > 10) printf(" ...");
        printf(" on MIDI port %d\n", device_id);

        // Try to parse as Samplecrate SysEx message
        if (sysex_parse_message(msg, sz)) {
            // Message was handled by SysEx subsystem
            printf("[MIDI] SysEx message was handled\n");
            return;
        }
        printf("[MIDI] SysEx message not recognized (not for us or wrong manufacturer)\n");
        // Otherwise, fall through to regular handling
    }

    // Handle regular MIDI messages
    if (midi_cb) {
        // Handle Song Position Pointer (0xF2 + LSB + MSB)
        if (sz == 3 && msg[0] == 0xF2) {
            // SPP is a special 3-byte message: combine the two 7-bit data bytes
            // Position is in "MIDI beats" (1/16th notes), LSB first
            unsigned char data1 = msg[1] & 0x7F;  // LSB
            unsigned char data2 = msg[2] & 0x7F;  // MSB
            midi_cb(msg[0], data1, data2, device_id, cb_userdata);
            return;
        }

        unsigned char data1 = (sz >= 2) ? msg[1] : 0;
        unsigned char data2 = (sz >= 3) ? msg[2] : 0;
        midi_cb(msg[0], data1, data2, device_id, cb_userdata);
    }
}

// Device-specific callback wrappers
static void rtmidi_event_callback_0(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    handle_midi_event(0, dt, msg, sz);
}

static void rtmidi_event_callback_1(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    handle_midi_event(1, dt, msg, sz);
}

static void rtmidi_event_callback_2(double dt, const unsigned char *msg, size_t sz, void *userdata) {
    handle_midi_event(2, dt, msg, sz);
}

int midi_list_ports(void) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
#endif
    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return 0;
    unsigned int nports = rtmidi_get_port_count(temp);
    rtmidi_in_free(temp);
    return nports;
}

int midi_get_port_name(int port, char *name_out, int bufsize) {
    if (!name_out || bufsize <= 0) return -1;
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
#endif
    RtMidiInPtr temp = rtmidi_in_create_default();
    if (!temp) return -1;

    unsigned int nports = rtmidi_get_port_count(temp);
    if (port < 0 || port >= (int)nports) {
        rtmidi_in_free(temp);
        return -1;
    }

    rtmidi_get_port_name(temp, port, name_out, &bufsize);
    rtmidi_in_free(temp);
    return 0;
}

int midi_init(MidiEventCallback cb, void *userdata, int port) {
    int ports[1] = {port};
    return midi_init_multi(cb, userdata, ports, 1);
}

int midi_init_multi(MidiEventCallback cb, void *userdata, const int *ports, int num_ports) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
#endif
    if (num_ports > MIDI_MAX_DEVICES) num_ports = MIDI_MAX_DEVICES;

    midi_cb = cb;
    cb_userdata = userdata;

    int opened = 0;
    RtMidiCCallback callbacks[MIDI_MAX_DEVICES] = {rtmidi_event_callback_0, rtmidi_event_callback_1, rtmidi_event_callback_2};

    for (int dev = 0; dev < num_ports; dev++) {
        if (ports[dev] < 0) continue;  // Skip if port is -1

        midiin[dev] = rtmidi_in_create_default();
        if (!midiin[dev]) continue;

        unsigned int nports = rtmidi_get_port_count(midiin[dev]);
        if (nports == 0 || ports[dev] >= (int)nports) {
            rtmidi_in_free(midiin[dev]);
            midiin[dev] = NULL;
            continue;
        }

        char port_name[64];
        snprintf(port_name, sizeof(port_name), "samplecrate-midi-in-%d", dev);
        rtmidi_open_port(midiin[dev], ports[dev], port_name);
        // Use callback matching device index (dev), not port number
        // This ensures device_id in callback matches the device slot (0 or 1)
        rtmidi_in_set_callback(midiin[dev], callbacks[dev], NULL);
        rtmidi_in_ignore_types(midiin[dev], 0, 0, 0);

        printf("MIDI: Opened device %d on port %d with callback %d\n", dev, ports[dev], dev);
        opened++;
    }

    return opened > 0 ? 0 : -1;
}

void midi_deinit(void) {
    for (int i = 0; i < MIDI_MAX_DEVICES; i++) {
        if (midiin[i]) {
            rtmidi_in_free(midiin[i]);
            midiin[i] = NULL;
        }
    }
    midi_cb = NULL;
    cb_userdata = NULL;
}