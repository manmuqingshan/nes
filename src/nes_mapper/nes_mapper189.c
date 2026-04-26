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

/* https://www.nesdev.org/wiki/INES_Mapper_189
 * Mapper 189 - TXC Board (82242I / 01-22017-400).
 * PRG: 32KB switchable at $8000-$FFFF, selected by write to $4120.
 *   bank = (data | (data >> 4)) & 0x0F
 * CHR: 8KB switchable (from ROM header CHR bank; no runtime CHR switching needed).
 * Write to $4120 via mapper_apu callback.
 */

typedef struct {
    uint8_t prg_bank;
} mapper189_register_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(mapper189_register_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    mapper189_register_t* r = (mapper189_register_t*)nes->nes_mapper.mapper_register;
    nes_memset(r, 0, sizeof(mapper189_register_t));

    nes_load_prgrom_32k(nes, 0, 0);

    if (nes->nes_rom.chr_rom_size == 0)
        nes_load_chrrom_8k(nes, 0, 0);
    else
        nes_load_chrrom_8k(nes, 0, 0);
}

static void nes_mapper_apu(nes_t* nes, uint16_t address, uint8_t data) {
    if (address != 0x4120u) return;
    mapper189_register_t* r = (mapper189_register_t*)nes->nes_mapper.mapper_register;
    r->prg_bank = (uint8_t)((data | (data >> 4u)) & 0x0Fu);
    nes_load_prgrom_32k(nes, 0, r->prg_bank);
}

int nes_mapper189_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_apu    = nes_mapper_apu;
    return NES_OK;
}
