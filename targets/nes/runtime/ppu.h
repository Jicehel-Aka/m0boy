/* The 2C02: registers $2000-$2007, the loopy v/t scroll model, background
 * and sprite rendering per scanline into a 256-pixel RGB565 row, sprite-0
 * hit (Super Mario Bros' status-bar split depends on it).  The machine
 * runs the CPU a scanline at a time, then asks for the row; mid-frame
 * register writes apply from the next row, which is where SMB changes
 * them anyway. */
#ifndef NES_PPU_H
#define NES_PPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    /* state the program reaches (the registers and the loopy pair) */
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;
    uint8_t x, w;
    uint8_t buffer; /* $2007's read-ahead */
    uint8_t open_bus;
    /* memory the PPU owns or borrows */
    uint8_t *nametable;  /* 2 KiB, mirrored per the cartridge */
    uint8_t *palette;    /* 32 bytes */
    uint8_t *oam;        /* 256 bytes (the machine's, DMA'd) */
    const uint8_t *chr;  /* 8 KiB CHR (ROM on NROM) */
    unsigned vertical_mirror;
    /* output: the scanline's fetched stream, 264 pixels in stream order —
     * screen pixel c is row[c + x]; sprites overlay at their own stream
     * positions, the left-column mask clears x..x+7.  compose (want_row)
     * decides whether a line is built at all. */
    uint16_t row[264];
    uint16_t frame;
    /* the callbacks the machine renders through.  want_row, when set,
     * answers whether line y reaches the panel: a dropped line still walks
     * v and keeps sprite-0 honest, it only skips composing the pixels. */
    void (*render_row)(void *, unsigned y); /* may be null: video off */
    int (*want_row)(void *, unsigned y);    /* may be null: every line */
    void *host;
} nes_ppu_t;

void nes_ppu_reset(nes_ppu_t *ppu);
uint8_t nes_ppu_read(nes_ppu_t *ppu, unsigned reg);
void nes_ppu_write(nes_ppu_t *ppu, unsigned reg, uint8_t value);
/* One scanline: sets vblank/sprite0 at their lines, renders y < 240 when
 * enabled, advances v; returns 1 when an NMI must be raised. */
int nes_ppu_line(nes_ppu_t *ppu, unsigned line);

#endif


#ifdef __cplusplus
}
#endif