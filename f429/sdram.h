// W9825G6KH-6 SDRAM (32 MiB, 16-bit, 4 banks x 8192 rows x 512 columns) on
// FMC SDRAM bank 1 (SDNE0 = PH3, SDCKE0 = PH2), mapped at 0xC0000000.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SDRAM_BASE 0xC0000000u
#define SDRAM_SIZE (32u * 1024u * 1024u)

void sdram_init(void);
// Quick power-on test (data bus, every address line, and a 256 KiB pattern).
// Returns true if the SDRAM works. Destroys the SDRAM contents.
bool sdram_test(void);
