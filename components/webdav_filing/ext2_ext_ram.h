// SPDX-License-Identifier: MIT
#pragma once

/* A few ext2 scratch buffers would sit in external RAM when available. On
 * ESPHome we keep them in ordinary BSS; 16 KiB is fine. */

#define EXT2_EXT_RAM_BSS
