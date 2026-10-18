/* SAMD21 DMAC setup shared by meta_lcd.cpp and meta_audio.cpp.
 *
 * No channel uses linked descriptors: SAM D21 erratum 1.7.2 (silicon A to
 * F; the META tested is D): while a channel with linked descriptors is
 * active, enabling another one can fetch a wrong descriptor.  On device
 * (2026-10-18, Game Gear: the display channel enabled at every line) the
 * DMAC then wrote audio descriptors over unrelated RAM, and with the
 * channel numbers swapped the display channel also stalled.  The audio
 * driver chains its blocks from the transfer-complete interrupt instead. */
#include "meta_dma.h"

#include "meta_hw.h"

meta_dma_descriptor meta_dma_desc[META_DMA_CHANNELS] __attribute__((aligned(16)));
volatile meta_dma_descriptor meta_dma_wb[META_DMA_CHANNELS] __attribute__((aligned(16)));

void meta_dma_init(void) {
    static bool done;
    if (done) return;
    done = true;
    PM->AHBMASK.reg |= PM_AHBMASK_DMAC;
    PM->APBBMASK.reg |= PM_APBBMASK_DMAC;
    /* BASEADDR and WRBADDR can only be written while the DMAC is off: reset
     * it first in case whatever ran before left it on (the loader of the
     * META tested on 2026-10-16 left it off). */
    DMAC->CTRL.reg = 0;
    while (DMAC->CTRL.reg & DMAC_CTRL_DMAENABLE) {}
    DMAC->CTRL.reg = DMAC_CTRL_SWRST;
    while (DMAC->CTRL.reg & DMAC_CTRL_SWRST) {}
    DMAC->BASEADDR.reg = (uint32_t)meta_dma_desc;
    DMAC->WRBADDR.reg = (uint32_t)meta_dma_wb;
    DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xF);
}

extern "C" void DMAC_Handler(void) {
    uint8_t chid = DMAC->CHID.reg; /* the interrupted code may be using another channel */
    uint32_t pending = DMAC->INTSTATUS.reg;
    if ((pending & (1u << META_DMA_AUDIO)) && meta_dma_audio_irq) meta_dma_audio_irq();
    if ((pending & (1u << META_DMA_LCD)) && meta_dma_lcd_irq) meta_dma_lcd_irq();
    DMAC->CHID.reg = chid;
}
