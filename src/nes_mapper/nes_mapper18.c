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

/* https://www.nesdev.org/wiki/INES_Mapper_018 */

typedef struct {
    uint8_t  prg[3];        /* 8KB PRG bank indices for $8000/$A000/$C000 */
    uint8_t  chr[8];        /* 1KB CHR bank indices */
    uint8_t  irq_enable;
    uint16_t irq_counter;   /* Live down-counter (scanline-based) */
    uint16_t irq_latch;     /* Reloaded on $F001 acknowledge */
} nes_mapper18_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void mapper18_update_prg(nes_t* nes) {
    nes_mapper18_t* m = (nes_mapper18_t*)nes->nes_mapper.mapper_register;
    uint8_t prg_count = (uint8_t)(nes->nes_rom.prg_rom_size * 2);
    nes_load_prgrom_8k(nes, 0, m->prg[0] % prg_count);
    nes_load_prgrom_8k(nes, 1, m->prg[1] % prg_count);
    nes_load_prgrom_8k(nes, 2, m->prg[2] % prg_count);
    nes_load_prgrom_8k(nes, 3, (uint16_t)(prg_count - 1)); /* fixed last 8KB */
}

static void mapper18_update_chr(nes_t* nes) {
    nes_mapper18_t* m = (nes_mapper18_t*)nes->nes_mapper.mapper_register;
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t chr_count = (uint8_t)(nes->nes_rom.chr_rom_size * 8);
    for (int i = 0; i < 8; i++) {
        nes_load_chrrom_1k(nes, (uint8_t)i, m->chr[i] % chr_count);
    }
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(nes_mapper18_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    nes_mapper18_t* m = (nes_mapper18_t*)nes->nes_mapper.mapper_register;
    nes_memset(m, 0, sizeof(nes_mapper18_t));

    m->prg[0] = 0; m->prg[1] = 1; m->prg[2] = 2;
    for (int i = 0; i < 8; i++) m->chr[i] = (uint8_t)i;

    mapper18_update_prg(nes);
    mapper18_update_chr(nes);

    if (nes->nes_rom.mirroring_type) {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);
    } else {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);
    }
}

/*
 * Each register is 8-bits wide but written in two 4-bit nibble writes.
 * Address bit[0] selects low (0) or high (1) nibble; bits[3:1] select sub-reg.
 *
 * $8000/$8001: 8KB PRG bank at $8000 (prg[0])
 * $8002/$8003: 8KB PRG bank at $A000 (prg[1])
 * $9000/$9001: 8KB PRG bank at $C000 (prg[2])
 * $9002/$9003: reserved
 * $A000-$D003: 1KB CHR banks 0-7 (two nibble-writes per CHR register)
 * $E000/$E001: IRQ latch bits [7:0]  (low/high nibble)
 * $E002/$E003: IRQ latch bits [15:8] (low/high nibble)
 * $F000:       IRQ enable (bit 0)
 * $F001:       IRQ acknowledge (reload counter from latch)
 * $F002:       IRQ mode (ignored — scanline-only on MCU)
 * $F003:       Mirroring (0=H, 1=V, 2=single0, 3=single1)
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    nes_mapper18_t* m = (nes_mapper18_t*)nes->nes_mapper.mapper_register;
    uint8_t nibble_sel = address & 1u;  /* 0=low nibble, 1=high nibble */

    switch (address & 0xF000) {
    case 0x8000: {
        /* $8000/$8001: prg[0],  $8002/$8003: prg[1] */
        uint8_t idx = (address >> 1) & 1u;
        if (!nibble_sel)
            m->prg[idx] = (uint8_t)((m->prg[idx] & 0xF0u) | (data & 0x0Fu));
        else
            m->prg[idx] = (uint8_t)((m->prg[idx] & 0x0Fu) | ((data & 0x0Fu) << 4));
        mapper18_update_prg(nes);
        break;
    }
    case 0x9000:
        /* $9000/$9001: prg[2];  $9002/$9003: reserved */
        if ((address & 0x3u) <= 1u) {
            if (!nibble_sel)
                m->prg[2] = (uint8_t)((m->prg[2] & 0xF0u) | (data & 0x0Fu));
            else
                m->prg[2] = (uint8_t)((m->prg[2] & 0x0Fu) | ((data & 0x0Fu) << 4));
            mapper18_update_prg(nes);
        }
        break;
    case 0xA000: case 0xB000: case 0xC000: case 0xD000: {
        /*
         * Each 0x1000-aligned block covers 2 CHR banks, each bank taking 2 nibble writes.
         * chr_idx = ((address - 0xA000) >> 11) | ((address >> 1) & 1)
         */
        uint8_t chr_idx = (uint8_t)(((address - 0xA000u) >> 11) | ((address >> 1) & 1u));
        if (!nibble_sel)
            m->chr[chr_idx] = (uint8_t)((m->chr[chr_idx] & 0xF0u) | (data & 0x0Fu));
        else
            m->chr[chr_idx] = (uint8_t)((m->chr[chr_idx] & 0x0Fu) | ((data & 0x0Fu) << 4));
        mapper18_update_chr(nes);
        break;
    }
    case 0xE000: {
        /* IRQ latch nibble writes:
         * $E000: bits[3:0],  $E001: bits[7:4],  $E002: bits[11:8],  $E003: bits[15:12] */
        uint8_t shift = (uint8_t)((address & 0x3u) * 4u);
        uint16_t mask = (uint16_t)(~(0x000Fu << shift));
        m->irq_latch = (uint16_t)((m->irq_latch & mask) | ((uint16_t)(data & 0x0Fu) << shift));
        break;
    }
    case 0xF000:
        switch (address & 0x3u) {
        case 0:
            m->irq_enable = data & 0x1u;
            break;
        case 1:
            /* Acknowledge: clear IRQ, reload counter from latch */
            nes->nes_cpu.irq_pending = 0;
            m->irq_counter = m->irq_latch;
            break;
        case 2:
            break; /* IRQ mode — scanline only, ignore */
        case 3:
            switch (data & 0x3u) {
            case 0: nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);  break;
            case 1: nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);    break;
            case 2: nes_ppu_screen_mirrors(nes, NES_MIRROR_ONE_SCREEN0); break;
            case 3: nes_ppu_screen_mirrors(nes, NES_MIRROR_ONE_SCREEN1); break;
            }
            break;
        }
        break;
    default:
        break;
    }
}

/* Scanline IRQ: counter decrements each scanline; fires at 0. */
static void nes_mapper_hsync(nes_t* nes) {
    nes_mapper18_t* m = (nes_mapper18_t*)nes->nes_mapper.mapper_register;
    if (!m->irq_enable) return;
    if (m->irq_counter > 0) {
        m->irq_counter--;
        if (m->irq_counter == 0) {
            nes_cpu_irq(nes);
        }
    }
}

int nes_mapper18_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_write  = nes_mapper_write;
    nes->nes_mapper.mapper_hsync  = nes_mapper_hsync;
    return NES_OK;
}
