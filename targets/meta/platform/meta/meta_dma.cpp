/* SAMD21 DMAC setup shared by meta_lcd.cpp and meta_audio.cpp. */
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
    /* BASEADDR and WRBADDR can only be written while the DMAC is off. */
    DMAC->BASEADDR.reg = (uint32_t)meta_dma_desc;
    DMAC->WRBADDR.reg = (uint32_t)meta_dma_wb;
    DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xF);
}
