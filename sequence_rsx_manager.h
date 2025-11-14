#ifndef SEQUENCE_RSX_MANAGER_H
#define SEQUENCE_RSX_MANAGER_H

#include "samplecrate_rsx.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Add or update a sequence entry in RSX structure for an uploaded MIDI file
 *
 * @param rsx Pointer to RSX structure
 * @param slot Slot number (0-15)
 * @param program Program number to target (0-3 for programs 1-4)
 * @param rsx_path Path to RSX file (for saving changes)
 * @return 0 on success, -1 on error
 */
int sequence_rsx_add_uploaded(SamplecrateRSX* rsx, uint8_t slot, uint8_t program, const char* rsx_path);

/**
 * Remove a sequence entry from RSX structure
 *
 * @param rsx Pointer to RSX structure
 * @param slot Slot number (0-15)
 * @param rsx_path Path to RSX file (for saving changes)
 * @return 0 on success, -1 on error
 */
int sequence_rsx_remove(SamplecrateRSX* rsx, uint8_t slot, const char* rsx_path);

/**
 * Find sequence index for a given slot
 *
 * @param rsx Pointer to RSX structure
 * @param slot Slot number (0-15)
 * @return Sequence index (0-based), or -1 if not found
 */
int sequence_rsx_find_slot(SamplecrateRSX* rsx, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif // SEQUENCE_RSX_MANAGER_H
