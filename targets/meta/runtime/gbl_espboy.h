/* ESPboy flash map (ESP-12F, 4 MiB) shared by the runtime, the platform and
 * the converter (recompiler/src/codegen/meta_emitter.cpp keeps a copy of the
 * numbers; tests/test_espboy_target.py compares them).
 *
 *   0x000000  application: Arduino core, runtime, generated code, and the
 *             ROM itself when it is at most ESPBOY_EMBED_BYTES, else bank 0
 *   0x100000  rom.bin: sectors 32 on of a larger ROM, raw or LZSS
 *   0x3C0000  cartridge RAM (two copies) and save states
 *   0x3FB000  Arduino/SDK: EEPROM, RF calibration, Wi-Fi settings
 *
 * The CPU maps only the first MiB of flash (0x40200000): everything from
 * ESPBOY_ROM_OFF on is reached with spi_flash_read. */
#ifndef GBL_ESPBOY_H
#define GBL_ESPBOY_H

#define ESPBOY_APP_BYTES 0x100000u
#define ESPBOY_ROM_OFF 0x100000u
#define ESPBOY_ROM_BYTES 0x2C0000u
#define ESPBOY_EMBED_BYTES 0x40000u
#define ESPBOY_SAVE_OFF 0x3C0000u
#define ESPBOY_SAVE_BYTES 0x03B000u
#define ESPBOY_BLOCK 4096u

#endif
