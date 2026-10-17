/*
 * Board settings of the META edition, shared by the two profiles that can
 * build a META firmware: runtime/gbl_profile.h (the Game Boy frontend) and
 * the gbl_profile.h the SG-1000/Game Gear generator writes (generator.cpp).
 * Both include this file, so the SPI rates and the display pipeline are
 * defined once for the three firmwares (the META's panel, card and pad all
 * hang on SERCOM4).
 *
 * Same contract as the profiles: a definition on the command line wins
 * (PlatformIO build_flags, the Pokitto Makefile's EXTRA_CFLAGS, m0_estimate.py
 * --cflag).  A console whose panel does not take the default display rate is
 * therefore served by a firmware rebuilt with -DMETA_LCD_SPI_MHZ=24, without
 * editing a source.  Definitions only: the Pokitto linker script
 * (pokitto.ld.in) preprocesses gbl_profile.h through this file.
 */
#ifndef GBL_BOARD_H
#define GBL_BOARD_H

/* META_LCD_SPI_MHZ: the display's clock at boot: 12, 24, 36 or 48 (the
 * option menu's "SPI ecran" changes it in the Game Boy firmware; the SG-1000
 * and Game Gear frontends keep the boot value).  Default 48, the most the
 * clock tree gives: 12 and 24 come from the DFLL48M, 36 and 48 from the
 * FDPLL96M (past the SERCOM's 48 MHz clock rating and the ST7735's
 * datasheet; 48 shown fine on a META, 2026-10-17).  The PLL has one
 * frequency: with the CPU overclocked (70 MHz, META_OVERCLOCK) the display
 * gets 35 (the menu's "36"), at 48 MHz CPU it gets 48; when the PLL does
 * not lock apply() falls back to 24 (meta_clock.cpp).  A full frame is
 * 40,960 bytes: 13.7 ms at 24 MHz, 9.4 at 35, 6.8 at 48.  A rate the panel
 * does not take scrambles the screen, menu included: rebuild with
 * -DMETA_LCD_SPI_MHZ=24 in build_flags. */
#ifndef META_LCD_SPI_MHZ
#define META_LCD_SPI_MHZ 48
#endif

/* META_SD_SPI_HZ: the card's clock outside identification, 12 MHz.  The
 * SAMD21's SPI master does not sample MISO reliably above about 12 MHz: at
 * 24 MHz every sector read failed on device (2026-10-16; identification at
 * 250 kHz was fine).  The display only writes and is not affected.  The
 * Arduino core capped every rate at 12 MHz, so the Gamebuino library read
 * its card at that rate.  The rate is derived from the SERCOM's actual clock
 * (meta_spi_rate).  A 512-byte sector takes 341 us to shift at 12 MHz (card
 * latency on top). */
#ifndef META_SD_SPI_HZ
#define META_SD_SPI_HZ 12000000
#endif

/* META_BUTTONS_SPI_HZ: the pad's 74HC165 shift register (CS PB03), 12 MHz,
 * the rate the Gamebuino library reads it at.
 * META_BUTTONS_PL_US: wait after CS low for the register's parallel load, in
 * microseconds (the library's 1).  Measured on device (2026-10-16): 12 MHz
 * and 1 us read all eight buttons in the library's order.
 * META_BUTTONS_LAPINOU_ORDER: define it to read meta-lapinou's bit order
 * (left, right, up, A, B, MENU, down, HOME) instead; not needed on the META
 * tested so far. */
#ifndef META_BUTTONS_SPI_HZ
#define META_BUTTONS_SPI_HZ 12000000
#endif
#ifndef META_BUTTONS_PL_US
#define META_BUTTONS_PL_US 1
#endif

/* META_LCD_BGR: 1 sets the ST7735's BGR order (MADCTL).  0 on the META
 * tested (2026-10-17): with it, red and blue came out swapped (error
 * screens of both frontends, Game Gear games).  It was set in 0.10.1 from
 * an emulator; a panel that shows swapped colours: rebuild with 1. */
#ifndef META_LCD_BGR
#define META_LCD_BGR 0
#endif

/* META_USB: 1 makes the firmware a USB serial port (meta_usb.c): the
 * 1200-baud touch of upload tools resets it into the bootloader (no double
 * press on RESET), and sam-ba's "R<addr>,<count>#" reads memory while the
 * game runs.  About 450 bytes of RAM and 2 KiB of flash. */
#ifndef META_USB
#define META_USB 1
#endif

/* META_LCD_DMA: 1 streams pixels with DMA while emulation continues (queue
 * in meta_lcd_queue.h); 0 writes them from the CPU (simpler, blocks for the
 * transfer). */
#ifndef META_LCD_DMA
#define META_LCD_DMA 1
#endif

/* META_LCD_12BPP: the ST7735 in 12-bit colour (RGB444, COLMOD 03h): 25%
 * fewer bytes per row on the SPI (a full frame 10.2 ms instead of 13.7 at
 * 24 MHz).  The pipeline renders 0x0RGB pixels (META_COLOR) and each row
 * is packed in Thumb-1 as it goes out (meta_lcd_pack_asm.S, ~5 cycles per
 * pixel).  meta-emu-sdl estimate on Super Mario Land: waits for the display
 * 8.5% -> 1.3% of the CPU, audio restarts 7 -> 1 per minute; colours keep 4
 * bits per channel. */
#ifndef META_LCD_12BPP
#define META_LCD_12BPP 0
#endif

#endif /* GBL_BOARD_H */
