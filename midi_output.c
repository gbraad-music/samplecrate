#include "midi_output.h"
#include <stdio.h>
#include <rtmidi_c.h>
#include <unistd.h>

// MIDI output state
static RtMidiOutPtr midi_out = NULL;
static int midi_out_device_id = -1;

int midi_output_list_ports(void) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return 0;
#endif
    RtMidiOutPtr temp = rtmidi_out_create_default();
    if (!temp) return 0;
    unsigned int nports = rtmidi_get_port_count(temp);
    rtmidi_out_free(temp);
    return nports;
}

int midi_output_get_port_name(int port, char *name_out, int bufsize) {
    if (!name_out || bufsize <= 0) return -1;
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) return -1;
#endif

    RtMidiOutPtr temp = rtmidi_out_create_default();
    if (!temp) return -1;

    unsigned int nports = rtmidi_get_port_count(temp);
    if (port < 0 || port >= (int)nports) {
        rtmidi_out_free(temp);
        return -1;
    }

    rtmidi_get_port_name(temp, port, name_out, &bufsize);
    rtmidi_out_free(temp);
    return 0;
}

int midi_output_init(int device_id) {
#ifndef _WIN32
    // On Linux, check if ALSA sequencer is available
    if (access("/dev/snd/seq", F_OK) != 0) {
        fprintf(stderr, "MIDI output: ALSA sequencer not available\n");
        return -1;
    }
#endif

    if (midi_out != NULL) {
        midi_output_deinit();
    }

    // Create RtMidi output
    midi_out = rtmidi_out_create_default();
    if (!midi_out) {
        fprintf(stderr, "Failed to create RtMidi output\n");
        return -1;
    }

    // Get device count
    unsigned int num_devices = rtmidi_get_port_count(midi_out);
    if (device_id < 0 || device_id >= (int)num_devices) {
        fprintf(stderr, "Invalid MIDI output device ID: %d (available: %u)\n", device_id, num_devices);
        rtmidi_out_free(midi_out);
        midi_out = NULL;
        return -1;
    }

    // Open the device
    char port_name[256];
    int bufsize = sizeof(port_name);
    int name_len = rtmidi_get_port_name(midi_out, device_id, port_name, &bufsize);
    if (name_len < 0) {
        snprintf(port_name, sizeof(port_name), "Port %d", device_id);
    }

    rtmidi_open_port(midi_out, device_id, "samplecrate-midi-out");

    midi_out_device_id = device_id;

    printf("MIDI output initialized on device %d: %s\n", device_id, port_name);
    return 0;
}

void midi_output_deinit(void) {
    if (midi_out) {
        rtmidi_close_port(midi_out);
        rtmidi_out_free(midi_out);
        midi_out = NULL;
        midi_out_device_id = -1;
    }
}

int midi_output_send_sysex(const unsigned char *msg, size_t msg_len) {
    if (!midi_out) return -1;
    if (!msg || msg_len < 2) return -1;

    // Validate SysEx message format
    if (msg[0] != 0xF0 || msg[msg_len - 1] != 0xF7) {
        fprintf(stderr, "[MIDI Output] Invalid SysEx message (must start with 0xF0 and end with 0xF7)\n");
        return -1;
    }

    rtmidi_out_send_message(midi_out, msg, msg_len);
    printf("[MIDI Output] Sent SysEx message (%zu bytes)\n", msg_len);
    return 0;
}
