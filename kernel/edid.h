/*
 * (65e-34) EDID: what a monitor says about itself (VESA E-EDID 1.3/1.4, and
 * CEA-861 extension blocks), as the firmware passed it on: its name, its
 * native mode, and every mode it lists with a refresh rate. Pure C: no
 * kernel calls, so the Mac can check it too.
 */
#ifndef RITTEROS_EDID_H
#define RITTEROS_EDID_H

#include <stdbool.h>
#include <stdint.h>

#include "driver_gpu.h"     /* (67f) struct edid_timing, edid_detailed, edid_info: the drivers' too */

/* Parse size bytes of EDID; false (and valid false) if it is not one. */
bool edid_parse(const uint8_t *edid, uint32_t size, struct edid_info *out);

/* The refresh of the width x height mode as the EDID lists it, in
 * millihertz, 0 if it does not: the native mode's if it is that size, else
 * 60 Hz if that is listed for it (what firmware sets), else the lowest rate
 * listed for it. */
uint32_t edid_refresh(const struct edid_info *e, uint32_t width, uint32_t height);

#endif
