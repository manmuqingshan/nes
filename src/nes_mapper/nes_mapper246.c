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

/* https://www.nesdev.org/wiki/INES_Mapper_246
 * Mapper 246 - Fong Shen Bang (封神榜).
 * PRG: 4x8KB switchable banks at $8000-$FFFF via writes to $6000-$6003.
 * CHR: 4x2KB switchable banks at $0000-$1FFF via writes to $6004-$6007.
 * All switching done through mapper_sram ($6000-$7FFF writes).
 */

typedef struct {
    uint8_t prg[4];
    uint8_t chr[4];
} mapper246_register_t;

static void nes_mapper_deinit(nes_t* nes) {
    nes_free(nes->nes_mapper.mapper_register);
    nes->nes_mapper.mapper_register = NULL;
}

static void nes_mapper_init(nes_t* nes) {
    if (nes->nes_mapper.mapper_register == NULL) {
        nes->nes_mapper.mapper_register = nes_malloc(sizeof(mapper246_register_t));
        if (nes->nes_mapper.mapper_register == NULL) return;
    }
    mapper246_register_t* r = (mapper246_register_t*)nes->nes_mapper.mapper_register;
    nes_memset(r, 0, sizeof(mapper246_register_t));

    uint8_t prg_banks = (uint8_t)(nes->nes_rom.prg_rom_size * 2u);
    nes_load_prgrom_8k(nes, 0, 0);
    nes_load_prgrom_8k(nes, 1, 1);
    nes_load_prgrom_8k(nes, 2, (uint8_t)(prg_banks - 2u));
    nes_load_prgrom_8k(nes, 3, (uint8_t)(prg_banks - 1u));

    if (nes->nes_rom.chr_rom_size == 0)
        nes_load_chrrom_8k(nes, 0, 0);
}

static void nes_mapper_sram(nes_t* nes, uint16_t address, uint8_t data) {
    mapper246_register_t* r = (mapper246_register_t*)nes->nes_mapper.mapper_register;
    if (address >= 0x6000u && address <= 0x6003u) {
        uint8_t slot = (uint8_t)(address - 0x6000u);
        r->prg[slot] = data;
        nes_load_prgrom_8k(nes, slot, data);
    } else if (address >= 0x6004u && address <= 0x6007u) {
        uint8_t slot = (uint8_t)(address - 0x6004u);
        r->chr[slot] = data;
        if (nes->nes_rom.chr_rom_size > 0) {
            nes_load_chrrom_1k(nes, (uint8_t)(slot * 2u),       (uint8_t)(data * 2u));
            nes_load_chrrom_1k(nes, (uint8_t)(slot * 2u + 1u),  (uint8_t)(data * 2u + 1u));
        }
    }
}

int nes_mapper246_init(nes_t* nes) {
    nes->nes_mapper.mapper_init   = nes_mapper_init;
    nes->nes_mapper.mapper_deinit = nes_mapper_deinit;
    nes->nes_mapper.mapper_sram   = nes_mapper_sram;
    return NES_OK;
}
