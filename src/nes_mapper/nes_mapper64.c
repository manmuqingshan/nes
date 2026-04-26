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

/* https://www.nesdev.org/wiki/INES_Mapper_064 — Tengen RAMBO-1 */

typedef struct {
    uint8_t reg_select;     /* $8000 bank select register */
    uint8_t chr[8];         /* CHR bank values (regs 0-5; 6/7 unused) */
    uint8_t prg[2];         /* 8KB PRG bank indices for $8000/$A000 */
    uint8_t irq_enable;
    uint8_t irq_counter;    /* 8-bit scanline down-counter */
    uint8_t irq_latch;      /* Reloaded on $C001 write */
    uint8_t irq_reload;     /* Flag: reload counter on next scanline */
} nes_mapper64_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void mapper64_update_banks(nes_t* nes) {
    nes_mapper64_t* m = (nes_mapper64_t*)nes->nes_mapper.mapper_register;
    uint8_t prg_count = (uint8_t)(nes->nes_rom.prg_rom_size * 2);

    /* PRG: regs 8/9 swappable at $8000/$A000; $C000/$E000 fixed to last two */
    nes_load_prgrom_8k(nes, 0, m->prg[0] % prg_count);
    nes_load_prgrom_8k(nes, 1, m->prg[1] % prg_count);
    nes_load_prgrom_8k(nes, 2, (uint16_t)(prg_count - 2));
    nes_load_prgrom_8k(nes, 3, (uint16_t)(prg_count - 1));

    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t chr_count = (uint8_t)(nes->nes_rom.chr_rom_size * 8);

    /*
     * CHR layout (regs 0/1 = 2KB banks, regs 2-5 = 1KB banks):
     *   Reg 0: 2KB at $0000 (slots 0,1)
     *   Reg 1: 2KB at $0800 (slots 2,3)
     *   Reg 2: 1KB at $1000 (slot 4)
     *   Reg 3: 1KB at $1400 (slot 5)
     *   Reg 4: 1KB at $1800 (slot 6)
     *   Reg 5: 1KB at $1C00 (slot 7)
     */
    nes_load_chrrom_1k(nes, 0, (uint8_t)((m->chr[0] & 0xFEu) % chr_count));
    nes_load_chrrom_1k(nes, 1, (uint8_t)((m->chr[0] | 0x01u) % chr_count));
    nes_load_chrrom_1k(nes, 2, (uint8_t)((m->chr[1] & 0xFEu) % chr_count));
    nes_load_chrrom_1k(nes, 3, (uint8_t)((m->chr[1] | 0x01u) % chr_count));
    nes_load_chrrom_1k(nes, 4, m->chr[2] % chr_count);
    nes_load_chrrom_1k(nes, 5, m->chr[3] % chr_count);
    nes_load_chrrom_1k(nes, 6, m->chr[4] % chr_count);
    nes_load_chrrom_1k(nes, 7, m->chr[5] % chr_count);
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(nes_mapper64_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    nes_mapper64_t* m = (nes_mapper64_t*)nes->nes_mapper.mapper_register;
    nes_memset(m, 0, sizeof(nes_mapper64_t));

    /* Init CHR regs so 2KB banks cover the first 4KB, 1KB regs cover the rest */
    m->chr[0] = 0; m->chr[1] = 2;
    m->chr[2] = 4; m->chr[3] = 5; m->chr[4] = 6; m->chr[5] = 7;
    m->prg[0] = 0; m->prg[1] = 1;

    mapper64_update_banks(nes);

    if (nes->nes_rom.mirroring_type) {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);
    } else {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);
    }
}

/*
 * $8000 (even): Bank select
 *   bits[3:0]: register index (0-9 used; 6/7 extra CHR skipped for MCU)
 *   bit[5]:    mode flag (simplified/ignored for MCU)
 * $8001 (odd):  Bank data written to register indexed by last $8000 write
 * $A000 (even): Mirroring — bit[0]: 0=V, 1=H
 * $C000 (even): IRQ latch (8-bit)
 * $C001 (odd):  IRQ reload (set reload flag)
 * $E000 (even): IRQ disable + acknowledge
 * $E001 (odd):  IRQ enable
 */
static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    nes_mapper64_t* m = (nes_mapper64_t*)nes->nes_mapper.mapper_register;
    switch (address & 0xE001u) {
    case 0x8000:
        m->reg_select = data & 0x0Fu;
        break;
    case 0x8001: {
        uint8_t reg = m->reg_select;
        if (reg <= 5u) {
            m->chr[reg] = data;
        } else if (reg == 8u) {
            m->prg[0] = data & 0x3Fu;
        } else if (reg == 9u) {
            m->prg[1] = data & 0x3Fu;
        }
        /* Regs 6/7 (extra 1KB CHR) and 0xA/0xB/0xF skipped for MCU */
        mapper64_update_banks(nes);
        break;
    }
    case 0xA000:
        nes_ppu_screen_mirrors(nes, (data & 0x1u) ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL);
        break;
    case 0xC000:
        m->irq_latch = data;
        break;
    case 0xC001:
        m->irq_reload = 1;
        break;
    case 0xE000:
        m->irq_enable = 0;
        nes->nes_cpu.irq_pending = 0;
        break;
    case 0xE001:
        m->irq_enable = 1;
        break;
    default:
        break;
    }
}

/* Scanline-based IRQ counter (same logic as MMC3). */
static void nes_mapper_hsync(nes_t* nes) {
    nes_mapper64_t* m = (nes_mapper64_t*)nes->nes_mapper.mapper_register;
    if (nes->nes_ppu.MASK_b == 0 && nes->nes_ppu.MASK_s == 0) return;

    if (m->irq_counter == 0 || m->irq_reload) {
        m->irq_counter = m->irq_latch;
    } else {
        m->irq_counter--;
    }

    if (m->irq_counter == 0 && m->irq_enable) {
        nes_cpu_irq(nes);
    }

    m->irq_reload = 0;
}

int nes_mapper64_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_write  = nes_mapper_write;
    nes->nes_mapper.mapper_hsync  = nes_mapper_hsync;
    return NES_OK;
}
