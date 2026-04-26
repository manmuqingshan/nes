/*
 * Copyright PeakRacing
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "nes.h"
#include "nes_mapper.h"

/* https://www.nesdev.org/wiki/INES_Mapper_015 */

static void nes_mapper_init(nes_t* nes) {
    // CPU $8000-$BFFF: First 16 KB PRG ROM bank.
    nes_load_prgrom_16k(nes, 0, 0);
    // CPU $C000-$FFFF: Last 16 KB PRG ROM bank.
    nes_load_prgrom_16k(nes, 1, nes->nes_rom.prg_rom_size - 1);
    // CHR-RAM: 8 KiB.
    nes_load_chrrom_8k(nes, 0, 0);
}

/*
    Bank select register at $8000-$FFFF:
    7  bit  0
    ---- ----
    BBBB BBSM
    ||||||||||
    ||||||+--- M: Mirroring (0: vertical, 1: horizontal)
    |||||+---- S: Swap bit (0: normal order, 1: swapped)
    ++++++---- B: 6-bit bank number (selects 32KB-aligned pair of 16KB banks)
*/
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    (void)address;
    uint8_t B = (data >> 2) & 0x3F;
    uint8_t S = (data >> 1) & 0x01;
    uint8_t M = data & 0x01;
    uint8_t base = B * 2;

    if (S == 0) {
        nes_load_prgrom_16k(nes, 0, base);
        nes_load_prgrom_16k(nes, 1, base + 1);
    } else {
        nes_load_prgrom_16k(nes, 0, base + 1);
        nes_load_prgrom_16k(nes, 1, base);
    }

    nes_ppu_screen_mirrors(nes, M ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL);
}

int nes_mapper15_init(nes_t* nes) {
    nes->nes_mapper.mapper_init  = nes_mapper_init;
    nes->nes_mapper.mapper_write = nes_mapper_write;
    return NES_OK;
}
