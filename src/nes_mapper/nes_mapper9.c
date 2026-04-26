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

/* https://www.nesdev.org/wiki/MMC2 */

typedef struct {
    uint8_t prg;        /* 8KB PRG bank index for $8000-$9FFF */
    uint8_t creg[4];    /* CHR 4KB bank regs: [0,1]=left slot, [2,3]=right slot */
    uint8_t latch0;     /* 0 → use creg[0], 1 → use creg[1] for $0000-$0FFF */
    uint8_t latch1;     /* 0 → use creg[2], 1 → use creg[3] for $1000-$1FFF */
} mapper9_reg_t;

static mapper9_reg_t mapper_reg = {0};

static void mapper9_update_prg(nes_t* nes) {
    uint16_t num_8k = (uint16_t)(nes->nes_rom.prg_rom_size * 2);
    /* $8000-$9FFF: switchable */
    nes_load_prgrom_8k(nes, 0, mapper_reg.prg % num_8k);
    /* $A000-$BFFF: fixed to 3rd-to-last 8KB bank */
    nes_load_prgrom_8k(nes, 1, (num_8k - 3) % num_8k);
    /* $C000-$DFFF: fixed to 2nd-to-last 8KB bank */
    nes_load_prgrom_8k(nes, 2, (num_8k - 2) % num_8k);
    /* $E000-$FFFF: fixed to last 8KB bank */
    nes_load_prgrom_8k(nes, 3, num_8k - 1);
}

static void nes_mapper_init(nes_t* nes) {
    nes_memset(&mapper_reg, 0, sizeof(mapper_reg));
    mapper_reg.latch0 = 1;
    mapper_reg.latch1 = 1;

    mapper9_update_prg(nes);
    nes_load_chrrom_4k(nes, 0, mapper_reg.creg[mapper_reg.latch0]);
    nes_load_chrrom_4k(nes, 1, mapper_reg.creg[2 + mapper_reg.latch1]);

    nes_ppu_screen_mirrors(nes, nes->nes_rom.mirroring_type ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL);
}

/*
    Registers ($A000-$FFFF):
    $A000: PRG bank select    — selects 8KB bank for $8000-$9FFF
    $B000: CHR bank FD/$0000  — 4KB bank used when latch0=0
    $C000: CHR bank FE/$0000  — 4KB bank used when latch0=1
    $D000: CHR bank FD/$1000  — 4KB bank used when latch1=0
    $E000: CHR bank FE/$1000  — 4KB bank used when latch1=1
    $F000: Mirroring          — bit 0: 0=vertical, 1=horizontal
*/
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    switch (address & 0xF000) {
        case 0xA000:
            mapper_reg.prg = data & 0x0F;
            mapper9_update_prg(nes);
            break;
        case 0xB000:
            mapper_reg.creg[0] = data & 0x1F;
            nes_load_chrrom_4k(nes, 0, mapper_reg.creg[mapper_reg.latch0]);
            break;
        case 0xC000:
            mapper_reg.creg[1] = data & 0x1F;
            nes_load_chrrom_4k(nes, 0, mapper_reg.creg[mapper_reg.latch0]);
            break;
        case 0xD000:
            mapper_reg.creg[2] = data & 0x1F;
            nes_load_chrrom_4k(nes, 1, mapper_reg.creg[2 + mapper_reg.latch1]);
            break;
        case 0xE000:
            mapper_reg.creg[3] = data & 0x1F;
            nes_load_chrrom_4k(nes, 1, mapper_reg.creg[2 + mapper_reg.latch1]);
            break;
        case 0xF000:
            nes_ppu_screen_mirrors(nes, (data & 1) ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL);
            break;
        default:
            break;
    }
}

/*
    PPU latch — triggered when the PPU fetches tile $FD or $FE.
    Address is the PPU tile start address (pattern_id * 16 + pattern_table_base).

    Latch trigger conditions (same as FCEUX MMC2and4PPUHook):
      high byte 0x0F (pattern table 0):
        bits[7:4] == 0xD → latch0=0, load creg[0] at $0000
        bits[7:4] == 0xE → latch0=1, load creg[1] at $0000
      high byte 0x1F (pattern table 1):
        bits[7:4] == 0xD → latch1=0, load creg[2] at $1000
        bits[7:4] == 0xE → latch1=1, load creg[3] at $1000
*/
static void nes_mapper_ppu(nes_t* nes, uint16_t address) {
    const uint8_t h = (uint8_t)(address >> 8);
    if (h >= 0x20 || (h & 0x0F) != 0x0F) return;
    const uint8_t l = (uint8_t)(address & 0xF0);
    if (h < 0x10) {
        if (l == 0xD0) {
            mapper_reg.latch0 = 0;
            nes_load_chrrom_4k(nes, 0, mapper_reg.creg[0]);
        } else if (l == 0xE0) {
            mapper_reg.latch0 = 1;
            nes_load_chrrom_4k(nes, 0, mapper_reg.creg[1]);
        }
    } else {
        if (l == 0xD0) {
            mapper_reg.latch1 = 0;
            nes_load_chrrom_4k(nes, 1, mapper_reg.creg[2]);
        } else if (l == 0xE0) {
            mapper_reg.latch1 = 1;
            nes_load_chrrom_4k(nes, 1, mapper_reg.creg[3]);
        }
    }
}

int nes_mapper9_init(nes_t* nes) {
    nes->nes_mapper.mapper_init  = nes_mapper_init;
    nes->nes_mapper.mapper_write = nes_mapper_write;
    nes->nes_mapper.mapper_ppu   = nes_mapper_ppu;
    return 0;
}
