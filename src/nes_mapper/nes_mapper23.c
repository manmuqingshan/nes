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
 * Mapper 23 - VRC4b/VRC4e variant.
 * Address bits: A0 = nibble select, A1 = bank-within-block for CHR regs.
 * 8KB PRG banks: slot0 and slot1 switchable, slot2/3 fixed to last two banks.
 * 8x1KB CHR banks via $B000-$E003 (two banks per $1000 block).
 * Scanline IRQ via $F000-$F002.
 */

typedef struct {
    uint8_t prg[2];
    uint8_t chr[8];
    uint8_t mirror;
    uint8_t irq_enable;
    uint8_t irq_latch;
    uint8_t irq_counter;
    uint8_t irq_mode;
} mapper23_register_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(mapper23_register_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    mapper23_register_t* r = (mapper23_register_t*)nes->nes_mapper.mapper_register;
    nes_memset(r, 0, sizeof(mapper23_register_t));

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
 * CHR write handler for $B000-$E003.
 * Each $1000 block covers two 1KB CHR banks.
 * A0 selects nibble (0=low4, 1=high4); A1 selects which bank within the block.
 */
static void mapper23_write_chr(mapper23_register_t* r, nes_t* nes,
                                uint16_t addr, uint8_t data) {
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t nibble = (uint8_t)(addr & 1);
    uint8_t sub    = (uint8_t)((addr >> 1) & 1);
    uint8_t block  = (uint8_t)((addr >> 12) - 0xBU); /* 0=$B, 1=$C, 2=$D, 3=$E */
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
 * $9000/$9001  : 8KB PRG bank select for $A000-$BFFF, bits[4:0]
 * $9002/$9003  : mirroring bits[1:0] (0=V, 1=H, 2=1scr0, 3=1scr1)
 * $B000-$E003  : 8x1KB CHR banks (two per $1000 block, nibble pairs)
 * $F000        : IRQ latch (8-bit)
 * $F001        : IRQ control (bit1=enable, bit2=mode, bit0=enable-after-ack)
 * $F002        : IRQ acknowledge
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    mapper23_register_t* r = (mapper23_register_t*)nes->nes_mapper.mapper_register;
    switch (address & 0xF000u) {
    case 0x8000u:
        r->prg[0] = data & 0x1Fu;
        nes_load_prgrom_8k(nes, 0, r->prg[0]);
        break;
    case 0x9000u:
        if (address & 0x0002u) { /* $9002/$9003: mirroring */
            r->mirror = data & 0x03u;
            if (nes->nes_rom.four_screen == 0) {
                nes_ppu_screen_mirrors(nes, vrc4_mirror_table[r->mirror]);
            }
        } else { /* $9000/$9001: PRG bank 1 */
            r->prg[1] = data & 0x1Fu;
            nes_load_prgrom_8k(nes, 1, r->prg[1]);
        }
        break;
    case 0xB000u:
    case 0xC000u:
    case 0xD000u:
    case 0xE000u:
        mapper23_write_chr(r, nes, address, data);
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

/* Decrement scanline IRQ counter; fire when it wraps through zero. */
static void nes_mapper_hsync(nes_t* nes) {
    mapper23_register_t* r = (mapper23_register_t*)nes->nes_mapper.mapper_register;
    if (!r->irq_enable) return;
    if (nes->nes_ppu.MASK_b == 0 && nes->nes_ppu.MASK_s == 0) return;
    if (r->irq_counter == 0) {
        r->irq_counter = r->irq_latch;
        nes_cpu_irq(nes);
    } else {
        r->irq_counter--;
    }
}

int nes_mapper23_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_write  = nes_mapper_write;
    nes->nes_mapper.mapper_hsync  = nes_mapper_hsync;
    return NES_OK;
}
