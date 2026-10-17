/* SAMD21 DMAC descriptors shared by the audio output (channel 0) and the
 * display (channel 1).  The descriptor table is one array for every channel
 * (DMAC BASEADDR), so the two drivers cannot keep their own.
 *
 * Channel numbers matter: SAMD21 erratum (DMAC, linked descriptors) --
 * enabling a channel while a channel using linked descriptors is active
 * can end in a fetch error or a wrong descriptor fetch when the channel
 * enabled has the lower number.  The audio chain (linked, always running)
 * had channel 1 and the display, enabled for every strip, channel 0: on
 * the META (2026-10-17) the display channel then stopped with 350 bytes
 * left and its trigger present, the audio channel pending, until the
 * watchdog (about every 7 minutes of Super Mario Land 2 with two-row
 * strips).  The display now has the higher number. */
#ifndef META_DMA_H
#define META_DMA_H

#include <stdint.h>

#define META_DMA_AUDIO 0
#define META_DMA_LCD 1
#define META_DMA_CHANNELS 2

typedef struct {
    uint16_t btctrl;
    uint16_t btcnt;
    uint32_t srcaddr; /* end address when incremented */
    uint32_t dstaddr;
    uint32_t descaddr;
} meta_dma_descriptor;

extern meta_dma_descriptor meta_dma_desc[META_DMA_CHANNELS];
extern volatile meta_dma_descriptor meta_dma_wb[META_DMA_CHANNELS];

/* Clock the DMAC and set its descriptor tables (once; later calls return). */
void meta_dma_init(void);

#endif
