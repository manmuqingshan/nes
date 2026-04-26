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

/* https://www.nesdev.org/wiki/Bandai_FCG_board */

typedef struct {
    uint8_t  chr[8];        /* 1KB CHR bank indices */
    uint8_t  prg;           /* 16KB PRG bank at $8000 */
    uint8_t  irq_enable;    /* IRQ enable flag */
    uint16_t irq_counter;   /* 16-bit down-counter */
    uint16_t irq_latch;     /* Reloaded into counter on $x00B write */
} nes_mapper16_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void mapper16_update_prg(nes_t* nes) {
    nes_mapper16_t* m = (nes_mapper16_t*)nes->nes_mapper.mapper_register;
    uint16_t last = (uint16_t)(nes->nes_rom.prg_rom_size - 1);
    nes_load_prgrom_16k(nes, 0, m->prg % nes->nes_rom.prg_rom_size);
    nes_load_prgrom_16k(nes, 1, last);
}

static void mapper16_update_chr(nes_t* nes) {
    nes_mapper16_t* m = (nes_mapper16_t*)nes->nes_mapper.mapper_register;
    if (nes->nes_rom.chr_rom_size == 0) return;
    uint8_t chr_count = (uint8_t)(nes->nes_rom.chr_rom_size * 8);
    for (int i = 0; i < 8; i++) {
        nes_load_chrrom_1k(nes, (uint8_t)i, m->chr[i] % chr_count);
    }
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(nes_mapper16_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    nes_mapper16_t* m = (nes_mapper16_t*)nes->nes_mapper.mapper_register;
    nes_memset(m, 0, sizeof(nes_mapper16_t));
    for (int i = 0; i < 8; i++) m->chr[i] = (uint8_t)i;

    mapper16_update_prg(nes);
    mapper16_update_chr(nes);

    if (nes->nes_rom.mirroring_type) {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);
    } else {
        nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);
    }
}

/*
 * Registers mapped at both $6000-$7FFF (mapper_sram) and $8000-$FFFF (mapper_write).
 * Address bits [3:0] select the register:
 *   $x0: CHR 1KB bank 0    $x1: CHR 1KB bank 1    ...    $x7: CHR 1KB bank 7
 *   $x8: PRG 16KB bank select (bits[3:0])
 *   $x9: Mirroring  (0=H, 1=V, 2=single-screen 0, 3=single-screen 1)
 *   $xA: IRQ enable (bit 0)
 *   $xB: IRQ acknowledge — clear flag + reload counter from latch
 *   $xC: IRQ latch low byte
 *   $xD: IRQ latch high byte
 */
static void mapper16_do_write(nes_t* nes, uint16_t address, uint8_t data) {
    nes_mapper16_t* m = (nes_mapper16_t*)nes->nes_mapper.mapper_register;
    switch (address & 0x000F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
    case 0x4: case 0x5: case 0x6: case 0x7:
        m->chr[address & 0x7] = data;
        mapper16_update_chr(nes);
        break;
    case 0x8:
        m->prg = data & 0x0F;
        mapper16_update_prg(nes);
        break;
    case 0x9:
        switch (data & 0x3) {
        case 0: nes_ppu_screen_mirrors(nes, NES_MIRROR_HORIZONTAL);  break;
        case 1: nes_ppu_screen_mirrors(nes, NES_MIRROR_VERTICAL);    break;
        case 2: nes_ppu_screen_mirrors(nes, NES_MIRROR_ONE_SCREEN0); break;
        case 3: nes_ppu_screen_mirrors(nes, NES_MIRROR_ONE_SCREEN1); break;
        }
        break;
    case 0xA:
        m->irq_enable = data & 0x1;
        nes->nes_cpu.irq_pending = 0;
        break;
    case 0xB:
        /* Acknowledge: clear IRQ, reload counter from latch */
        nes->nes_cpu.irq_pending = 0;
        m->irq_counter = m->irq_latch;
        break;
    case 0xC:
        m->irq_latch = (m->irq_latch & 0xFF00) | data;
        break;
    case 0xD:
        m->irq_latch = (m->irq_latch & 0x00FF) | ((uint16_t)data << 8);
        break;
    default:
        break;
    }
}

static void nes_mapper_sram(nes_t* nes, uint16_t address, uint8_t data) {
    mapper16_do_write(nes, address, data);
}

static void nes_mapper_write(nes_t* nes, uint16_t address, uint8_t data) {
    mapper16_do_write(nes, address, data);
}

/* 16-bit counter decrements every CPU cycle; fires IRQ on unsigned underflow. */
static void nes_mapper_cpu_clock(nes_t* nes, uint16_t cycles) {
    nes_mapper16_t* m = (nes_mapper16_t*)nes->nes_mapper.mapper_register;
    if (!m->irq_enable) return;
    uint16_t prev = m->irq_counter;
    m->irq_counter -= cycles;
    if (m->irq_counter > prev) { /* unsigned underflow */
        nes_cpu_irq(nes);
    }
}

int nes_mapper16_init(nes_t* nes) {
    nes->nes_mapper.mapper_init      = nes_mapper_init;
    nes->nes_mapper.mapper_deinit    = nes_mapper_deinit;
    nes->nes_mapper.mapper_write     = nes_mapper_write;
    nes->nes_mapper.mapper_sram      = nes_mapper_sram;
    nes->nes_mapper.mapper_cpu_clock = nes_mapper_cpu_clock;
    return NES_OK;
}
