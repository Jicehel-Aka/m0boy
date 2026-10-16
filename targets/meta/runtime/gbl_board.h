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

/* META_LCD_SPI_MHZ: the display's clock at boot: 12, 24, 36 or 48.  The
 * option menu's "SPI ecran" changes it in the Game Boy firmware; the SG-1000
 * and Game Gear frontends have no menu and keep the boot value.
 *
 * 24 is the boot default: the rate meta-lapinou drives this panel at, and
 * the only panel rate with runs on a real META.  12 comes from the DFLL48M,
 * 24 from the DFLL48M too (half its SERCOM clock), and 36 and 48 only from
 * the FDPLL96M at 72 or 96 MHz -- past the SERCOM's 48 MHz clock rating and
 * past the ST7735's 15 MHz write cycle.  gm0's overclocked META port ran 36;
 * nothing has been seen at 48 on a panel, so the default stays at 24 and
 * 36/48 are opt-in: "SPI ecran" in the Game Boy firmware's menu (SG-1000 and
 * Game Gear frontends have no menu and keep the boot value), or a game's own
 * `-DMETA_LCD_SPI_MHZ=36|48` in its build_flags.  The PLL has one frequency,
 * so the CPU at 72 MHz caps the display at 36 (meta_clock.cpp reduces it),
 * and when the PLL does not lock apply() falls back to 24.
 * A full frame is 40,960 bytes: 13.7 ms at 24 MHz, 9.1 at 36, 6.8 at 48
 * (30,720 / 10.2 / 6.8 / 5.1 in 12-bit colour, META_LCD_12BPP).  A rate the
 * panel does not take scrambles the screen, menu included. */
#ifndef META_LCD_SPI_MHZ
#define META_LCD_SPI_MHZ 24
#endif

/* META_SD_SPI_HZ: the card's clock outside identification, 24 MHz -- the
 * fastest divider output at or below the SD's 25 MHz Default-Speed limit for
 * SPI mode.  Identification itself runs at 250 kHz (meta_sd_bus.cpp).  The
 * rate is derived from the SERCOM's actual clock (meta_spi_rate), so it is
 * 24 MHz from the DFLL48M and from the PLL at 96 MHz (display at 48), and
 * 18 MHz from the PLL at 72 MHz (display at 36) -- the same pair the
 * Pokitto's SSP gives at 48 and 72 MHz.  A 512-byte sector takes 341 us to
 * shift at 12 MHz and 171 us at 24 (card latency on top). */
#ifndef META_SD_SPI_HZ
#define META_SD_SPI_HZ 24000000
#endif

/* META_BUTTONS_SPI_HZ: the pad's 74HC165 shift register (CS PB03), 12 MHz,
 * the rate the Gamebuino library reads it at.  24 MHz returned shifted
 * buttons on the META tested here (device, 2026-10-15): keep 12.
 * META_BUTTONS_LAPINOU_ORDER: on the META tested here the byte comes back in
 * meta-lapinou's bit order (left, right, up, A, B, MENU, down, HOME) at both
 * rates; the frontends remap it (meta_main.cpp).
 * META_BUTTONS_PL_US: wait after CS low for the register's parallel load, in
 * microseconds; the library's 1 us -- without it the load is missed when CS
 * falls right after a display burst (measured on device 2026-10-15). */
#ifndef META_BUTTONS_SPI_HZ
#define META_BUTTONS_SPI_HZ 24000000
#endif
#ifndef META_BUTTONS_PL_US
#define META_BUTTONS_PL_US 1
#endif
#ifndef META_BUTTONS_LAPINOU_ORDER
#define META_BUTTONS_LAPINOU_ORDER 1
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
