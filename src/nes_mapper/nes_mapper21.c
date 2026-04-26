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

/* https://www.nesdev.org/wiki/INES_Mapper_021
 * Mapper 21 - VRC4a/VRC4e (Konami).
 * PRG: 8KB switchable at $8000 and $A000; $C000-$FFFF fixed (last 16KB).
 * CHR: 8x1KB banks via $B000-$E007 with nibble-pair registers.
 * Address decode differs from mapper 23: A1=nibble select, A2=bank-within-block.
 * Scanline IRQ identical to mapper 23/25.
 */

typedef struct {
    uint8_t prg[2];       /* 5-bit PRG bank indices for $8000 and $A000 */
    uint8_t chr[8];       /* 8-bit assembled CHR 1KB bank indices [0-7] */
    uint8_t mirror;       /* mirroring mode index into vrc4_mirror_table */
    uint8_t irq_enable;   /* IRQ enabled flag */
    uint8_t irq_latch;    /* IRQ reload value */
    uint8_t irq_counter;  /* IRQ scanline countdown */
    uint8_t irq_mode;     /* IRQ mode (unused by this impl, reserved) */
} mapper21_register_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(mapper21_register_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    mapper21_register_t* r = (mapper21_register_t*)nes->nes_mapper.mapper_register;
    nes_memset(r, 0, sizeof(mapper21_register_t));

    uint16_t prg_banks = (uint16_t)(nes->nes_rom.prg_rom_size * 2u);
    nes_load_prgrom_8k(nes, 0, 0);
    nes_load_prgrom_8k(nes, 1, 1);
    nes_load_prgrom_8k(nes, 2, (uint16_t)(prg_banks - 2u));
    nes_load_prgrom_8k(nes, 3, (uint16_t)(prg_banks - 1u));

    if (nes->nes_rom.chr_rom_size == 0) {
        nes_load_chrrom_8k(nes, 0, 0);
    } else {
        for (int i = 0; i < 8; i++) {
            nes_load_chrrom_1k(nes, (uint8_t)i, 0);
        }
    }
}

/*
 * CHR bank write for $B000-$E007 (VRC4a/VRC4e address encoding).
 * A1 selects nibble (0=low4, 1=high4); A2 selects bank within block.
 */
static void mapper21_write_chr(mapper21_register_t* r, nes_t* nes,
                                uint16_t addr, uint8_t data) {
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t nibble = (uint8_t)((addr >> 1) & 1u); /* A1: hi/lo nibble select */
    uint8_t sub    = (uint8_t)((addr >> 2) & 1u); /* A2: bank within block */
    uint8_t block  = (uint8_t)((addr >> 12) - 0xBu);
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
 * $8000        : 8KB PRG bank at $8000, bits[4:0]
 * $9000 (A1=0) : 8KB PRG bank at $A000, bits[4:0]
 * $9002 (A1=1) : mirroring bits[1:0] (0=V, 1=H, 2=1scr0, 3=1scr1)
 * $B000-$E007  : 8x1KB CHR banks via nibble pairs (A1=nibble, A2=sub)
 * $F000-$F006  : Scanline IRQ (latch/control/acknowledge)
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    mapper21_register_t* r = (mapper21_register_t*)nes->nes_mapper.mapper_register;
    switch (address & 0xF000u) {
    case 0x8000u:
        r->prg[0] = data & 0x1Fu;
        nes_load_prgrom_8k(nes, 0, r->prg[0]);
        break;
    case 0x9000u:
        if (address & 0x0002u) { /* A1=1: mirroring sub-register */
            r->mirror = data & 0x03u;
            if (nes->nes_rom.four_screen == 0) {
                nes_ppu_screen_mirrors(nes, vrc4_mirror_table[r->mirror]);
            }
        } else { /* A1=0: PRG bank at $A000 */
            r->prg[1] = data & 0x1Fu;
            nes_load_prgrom_8k(nes, 1, r->prg[1]);
        }
        break;
    case 0xB000u:
    case 0xC000u:
    case 0xD000u:
    case 0xE000u:
        mapper21_write_chr(r, nes, address, data);
        break;
    case 0xF000u:
        switch (address & 0x0003u) {
        case 0: /* IRQ latch */
            r->irq_latch = data;
            break;
        case 1: /* IRQ control: bit1=enable now, bit2=mode */
            r->irq_mode = (data >> 2) & 0x01u;
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

/* Decrement scanline IRQ counter; fire when it wraps through zero. */
static void nes_mapper_hsync(nes_t* nes) {
    mapper21_register_t* r = (mapper21_register_t*)nes->nes_mapper.mapper_register;
    if (!r->irq_enable) return;
    if (nes->nes_ppu.MASK_b == 0 && nes->nes_ppu.MASK_s == 0) return;
    if (r->irq_counter == 0) {
        r->irq_counter = r->irq_latch;
        nes_cpu_irq(nes);
    } else {
        r->irq_counter--;
    }
}

int nes_mapper21_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_write  = nes_mapper_write;
    nes->nes_mapper.mapper_hsync  = nes_mapper_hsync;
    return NES_OK;
}
