/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "sps.h"

#include <string.h>

void gs_sps_init(int width, int height) {
  (void)width;
  (void)height;
  // Do nothing, PSP does not require SPS NAL rewriting.
}

void gs_sps_fix(PLENTRY sps, int flags, uint8_t* out_buf, uint32_t* out_offset) {
  (void)flags;
  // Copy the original SPS directly to the output buffer unmodified
  memcpy(out_buf + *out_offset, sps->data, sps->length);
  *out_offset += sps->length;
}
