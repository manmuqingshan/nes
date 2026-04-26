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

/*
 * https://www.nesdev.org/wiki/Namco_163_audio
 * Namco 129/163 — bank switching + IRQ only (no audio expansion).
 */

typedef struct {
    uint8_t  prg[3];        /* 8KB PRG bank indices for $8000/$A000/$C000 */
    uint8_t  chr[8];        /* 1KB CHR bank indices for PPU $0000-$1FFF */
    uint8_t  irq_enable;
    uint16_t irq_counter;   /* 15-bit up-counter; fires at 0x7FFF */
} nes_mapper19_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void mapper19_update_prg(nes_t* nes) {
    nes_mapper19_t* m = (nes_mapper19_t*)nes->nes_mapper.mapper_register;
    uint8_t prg_count = (uint8_t)(nes->nes_rom.prg_rom_size * 2);
    nes_load_prgrom_8k(nes, 0, m->prg[0] % prg_count);
    nes_load_prgrom_8k(nes, 1, m->prg[1] % prg_count);
    nes_load_prgrom_8k(nes, 2, m->prg[2] % prg_count);
    nes_load_prgrom_8k(nes, 3, (uint16_t)(prg_count - 1)); /* fixed last 8KB */
}

static void mapper19_update_chr(nes_t* nes) {
    nes_mapper19_t* m = (nes_mapper19_t*)nes->nes_mapper.mapper_register;
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t chr_count = (uint8_t)(nes->nes_rom.chr_rom_size * 8);
    for (int i = 0; i < 8; i++) {
        nes_load_chrrom_1k(nes, (uint8_t)i, m->chr[i] % chr_count);
    }
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(nes_mapper19_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    nes_mapper19_t* m = (nes_mapper19_t*)nes->nes_mapper.mapper_register;
    nes_memset(m, 0, sizeof(nes_mapper19_t));

    m->prg[0] = 0; m->prg[1] = 1; m->prg[2] = 2;
    for (int i = 0; i < 8; i++) m->chr[i] = (uint8_t)i;

    mapper19_update_prg(nes);
    mapper19_update_chr(nes);

    if (nes->nes_rom.mirroring_type) {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);
    } else {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);
    }
}

/*
 * $8000-$BFFF: CHR 1KB banks 0-7 at PPU $0000-$1FFF
 *   Slot = (address - $8000) >> 11  (one slot per 0x800 bytes)
 * $C000-$DFFF: CHR/NT banks — skip NT handling for MCU target
 * $E000-$E7FF: 8KB PRG bank at $8000 (bits[5:0])
 * $E800-$EFFF: 8KB PRG bank at $A000 (bits[5:0])
 * $F000-$F7FF: 8KB PRG bank at $C000 (bits[5:0])
 * $F800-$FFFF: IRQ control — bit[7]=enable, bit[6]=disable (audio addr skipped)
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    nes_mapper19_t* m = (nes_mapper19_t*)nes->nes_mapper.mapper_register;

    if (address < 0xC000u) {
        /* CHR 1KB banks: $8000-$BFFF, slot = bits [12:11] relative to $8000 */
        uint8_t slot = (uint8_t)((address - 0x8000u) >> 11);
        m->chr[slot] = data;
        if (nes->nes_rom.chr_rom_size) {
            uint8_t chr_count = (uint8_t)(nes->nes_rom.chr_rom_size * 8);
            nes_load_chrrom_1k(nes, slot, data % chr_count);
        }
    } else if (address < 0xE000u) {
        /* $C000-$DFFF: NT/CHR-NT bank switching — skip for MCU target */
    } else if (address < 0xF800u) {
        /* PRG banks: $E000/$E800/$F000 → prg[0/1/2] */
        uint8_t prg_idx = (uint8_t)((address - 0xE000u) >> 11);
        if (prg_idx < 3u) {
            m->prg[prg_idx] = data & 0x3Fu;
            mapper19_update_prg(nes);
        }
    } else {
        /* $F800-$FFFF: IRQ control; bit[7]=enable, bit[6]=disable */
        if (data & 0x80u)
            m->irq_enable = 1;
        else if (data & 0x40u)
            m->irq_enable = 0;
    }
}

/*
 * 15-bit up-counter increments every CPU cycle.
 * Fires IRQ and resets when it reaches 0x7FFF.
 */
static void nes_mapper_cpu_clock(nes_t* nes, uint16_t cycles) {
    nes_mapper19_t* m = (nes_mapper19_t*)nes->nes_mapper.mapper_register;
    if (!m->irq_enable) return;
    m->irq_counter += cycles;
    if (m->irq_counter >= 0x7FFFu) {
        m->irq_counter = 0;
        nes_cpu_irq(nes);
    }
}

int nes_mapper19_init(nes_t* nes) {
    nes->nes_mapper.mapper_init      = nes_mapper_init;
    nes->nes_mapper.mapper_deinit    = nes_mapper_deinit;
    nes->nes_mapper.mapper_write     = nes_mapper_write;
    nes->nes_mapper.mapper_cpu_clock = nes_mapper_cpu_clock;
    return NES_OK;
}
