#ifndef MIDI_OUTPUT_H
#define MIDI_OUTPUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// List available MIDI output ports
// Returns the number of output ports found
int midi_output_list_ports(void);

// Get the name of a MIDI output port
// port: port index
// name_out: buffer to store the port name
// bufsize: size of name_out buffer
// Returns 0 on success, -1 on failure
int midi_output_get_port_name(int port, char *name_out, int bufsize);

// Initialize MIDI output device
// device_id: MIDI output port number
// Returns 0 on success, -1 on failure
int midi_output_init(int device_id);

// Cleanup MIDI output
void midi_output_deinit(void);

// Send SysEx message
// msg: SysEx message buffer (must start with 0xF0 and end with 0xF7)
// msg_len: length of message in bytes
// Returns 0 on success, -1 on failure
int midi_output_send_sysex(const unsigned char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif // MIDI_OUTPUT_H
