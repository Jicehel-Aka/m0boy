/* SAMD21 DMAC descriptors shared by the display (channel 0) and the audio
 * output (channel 1).  The descriptor table is one array for every channel
 * (DMAC BASEADDR), so the two drivers cannot keep their own. */
#ifndef META_DMA_H
#define META_DMA_H

#include <stdint.h>

#define META_DMA_LCD 0
#define META_DMA_AUDIO 1
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
