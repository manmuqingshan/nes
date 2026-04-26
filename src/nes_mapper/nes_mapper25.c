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

/* https://www.nesdev.org/wiki/VRC4
 * Mapper 25 - VRC4c/VRC4d variant.
 * Address bits A0 and A1 are swapped relative to mapper 23:
 *   A1 = nibble select, A0 = bank-within-block for CHR regs.
 * Mirroring sub-register is therefore at $9001/$9003 (A0=1) instead of $9002/$9003.
 * IRQ and PRG layout identical to mapper 23.
 */

typedef struct {
    uint8_t prg[2];
    uint8_t chr[8];
    uint8_t mirror;
    uint8_t irq_enable;
    uint8_t irq_latch;
    uint8_t irq_counter;
    uint8_t irq_mode;
} mapper25_register_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(mapper25_register_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    mapper25_register_t* r = (mapper25_register_t*)nes->nes_mapper.mapper_register;
    nes_memset(r, 0, sizeof(mapper25_register_t));

    uint16_t prg_banks = (uint16_t)(nes->nes_rom.prg_rom_size * 2);
    nes_load_prgrom_8k(nes, 0, 0);
    nes_load_prgrom_8k(nes, 1, 1);
    nes_load_prgrom_8k(nes, 2, (uint8_t)(prg_banks - 2));
    nes_load_prgrom_8k(nes, 3, (uint8_t)(prg_banks - 1));

    if (nes->nes_rom.chr_rom_size == 0) {
        nes_load_chrrom_8k(nes, 0, 0);
    } else {
        for (int i = 0; i < 8; i++) {
            nes_load_chrrom_1k(nes, (uint8_t)i, 0);
        }
    }
}

/*
 * CHR write handler for $B000-$E003 (VRC4c/VRC4d variant).
 * A1 selects nibble (0=low4, 1=high4); A0 selects bank-within-block.
 */
static void mapper25_write_chr(mapper25_register_t* r, nes_t* nes,
                                uint16_t addr, uint8_t data) {
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t nibble = (uint8_t)((addr >> 1) & 1); /* A1 */
    uint8_t sub    = (uint8_t)(addr & 1);         /* A0 */
    uint8_t block  = (uint8_t)((addr >> 12) - 0xBU);
    uint8_t idx    = (uint8_t)(block * 2u + sub);
    if (nibble == 0) {
        r->chr[idx] = (r->chr[idx] & 0xF0u) | (data & 0x0Fu);
    } else {
        r->chr[idx] = (r->chr[idx] & 0x0Fu) | (uint8_t)((data & 0x0Fu) << 4);
    }
    nes_load_chrrom_1k(nes, idx, r->chr[idx]);
}

static const nes_mirror_type_t vrc4_mirror_table[4] = {
    NES_MIRROR_VERTICAL,
    NES_MIRROR_HORIZONTAL,
    NES_MIRROR_ONE_SCREEN0,
    NES_MIRROR_ONE_SCREEN1,
};

/*
 * $8000        : 8KB PRG bank select for $8000-$9FFF, bits[4:0]
 * $9000/$9002  : 8KB PRG bank select for $A000-$BFFF (A0=0, due to swap)
 * $9001/$9003  : mirroring bits[1:0] (A0=1, due to swap)
 * $B000-$E003  : 8x1KB CHR banks
 * $F000        : IRQ latch (8-bit)
 * $F001        : IRQ control (bit1=enable, bit2=mode)
 * $F002        : IRQ acknowledge
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    mapper25_register_t* r = (mapper25_register_t*)nes->nes_mapper.mapper_register;
    switch (address & 0xF000u) {
    case 0x8000u:
        r->prg[0] = data & 0x1Fu;
        nes_load_prgrom_8k(nes, 0, r->prg[0]);
        break;
    case 0x9000u:
        if (address & 0x0001u) { /* $9001/$9003: mirroring (A0=1) */
            r->mirror = data & 0x03u;
            if (nes->nes_rom.four_screen == 0) {
                nes_ppu_screen_mirrors(nes, vrc4_mirror_table[r->mirror]);
            }
        } else { /* $9000/$9002: PRG bank 1 (A0=0) */
            r->prg[1] = data & 0x1Fu;
            nes_load_prgrom_8k(nes, 1, r->prg[1]);
        }
        break;
    case 0xB000u:
    case 0xC000u:
    case 0xD000u:
    case 0xE000u:
        mapper25_write_chr(r, nes, address, data);
        break;
    case 0xF000u:
        switch (address & 0x0003u) {
        case 0: /* IRQ latch */
            r->irq_latch = data;
            break;
        case 1: /* IRQ control */
            r->irq_mode = (data >> 2) & 1u;
            if (data & 0x02u) {
                r->irq_enable  = 1;
                r->irq_counter = r->irq_latch;
            } else {
                r->irq_enable = 0;
            }
            break;
        case 2: /* IRQ acknowledge */
            r->irq_enable = 0;
            nes->nes_cpu.irq_pending = 0;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static void nes_mapper_hsync(nes_t* nes) {
    mapper25_register_t* r = (mapper25_register_t*)nes->nes_mapper.mapper_register;
    if (!r->irq_enable) return;
    if (nes->nes_ppu.MASK_b == 0 && nes->nes_ppu.MASK_s == 0) return;
    if (r->irq_counter == 0) {
        r->irq_counter = r->irq_latch;
        nes_cpu_irq(nes);
    } else {
        r->irq_counter--;
    }
}

int nes_mapper25_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_write  = nes_mapper_write;
    nes->nes_mapper.mapper_hsync  = nes_mapper_hsync;
    return NES_OK;
}
