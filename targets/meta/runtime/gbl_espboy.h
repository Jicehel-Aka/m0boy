/* ESPboy flash map (ESP-12F, 4 MiB) shared by the runtime, the platform and
 * the converter (recompiler/src/codegen/meta_emitter.cpp keeps a copy of the
 * numbers; tests/test_espboy_target.py compares them).
 *
 *   0x000000  application (Arduino core, runtime, generated code)
 *   0x0A0000  rom.bin: bank 0 raw, then raw or LZSS sectors
 *   0x3C0000  cartridge RAM (two copies) and save states
 *   0x3FB000  Arduino/SDK: EEPROM, RF calibration, Wi-Fi settings
 *
 * The CPU only maps the first MiB of flash (0x40200000): a ROM image that
 * fits between ESPBOY_ROM_OFF and that limit is read in place; a larger one
 * has its bank 0 mapped and its other sectors read with spi_flash_read. */
#ifndef GBL_ESPBOY_H
#define GBL_ESPBOY_H

#define ESPBOY_MAP_BASE 0x40200000u
#define ESPBOY_MAP_BYTES 0x100000u
#define ESPBOY_APP_BYTES 0x0A0000u
#define ESPBOY_ROM_OFF 0x0A0000u
#define ESPBOY_ROM_BYTES 0x320000u
#define ESPBOY_SAVE_OFF 0x3C0000u
#define ESPBOY_SAVE_BYTES 0x03B000u
#define ESPBOY_ROM_MAPPED_BYTES (ESPBOY_MAP_BYTES - ESPBOY_ROM_OFF)
#define ESPBOY_BLOCK 4096u

#endif
