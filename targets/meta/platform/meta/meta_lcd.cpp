/*
 * ST7735 + DMA output for the Gamebuino META.
 *
 * Derived from the META Display_ST7735 driver and manitou48 DMA helpers used
 * in the meta-lapinou project; the ST7735 init sequence comes from the
 * Adafruit ST7735 library (MIT license, Limor Fried/Ladyada, Adafruit
 * Industries).
 */
#include "meta_lcd.h"
#include "meta_target.h"

/* META_LCD_DMA (gbl_profile.h): 1 streams pixels with DMA while emulation
 * continues; 0 writes them from the CPU (simpler, blocks for the
 * transfer). */

#if META_LCD_DMA
#include "meta_lcd_queue.h"
#include "meta_dma.h"
#endif

#include "meta_clock.h"
#include "meta_hw.h"
#include "meta_lcd_pack.h"

/* META_LCD_12BPP (gbl_profile.h): the panel takes RGB444 (COLMOD 03h); the
 * pipeline renders 0x0RGB halfwords (META_COLOR) and each row is packed in
 * place just before it is sent (meta_lcd_pack.h, meta_lcd_pack_asm.S): 240
 * bytes per row instead of 320. */
#if META_LCD_12BPP
#define LCD_ROW_BYTES 240u
#define LCD_COLMOD 0x03
#else
#define LCD_ROW_BYTES 320u
#define LCD_COLMOD 0x05
#endif

#define TFT_CS_MASK (1u << 22) /* PB22 */
#define TFT_DC_MASK (1u << 23) /* PB23 */
#define TFT_PORT PORT->Group[1]

#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT 0x11
#define ST7735_NORON 0x13
#define ST7735_INVOFF 0x20
#define ST7735_DISPON 0x29
#define ST7735_CASET 0x2A
#define ST7735_RASET 0x2B
#define ST7735_RAMWR 0x2C
#define ST7735_MADCTL 0x36
#define ST7735_COLMOD 0x3A
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR 0xB4
#define ST7735_PWCTR1 0xC0
#define ST7735_PWCTR2 0xC1
#define ST7735_PWCTR3 0xC2
#define ST7735_PWCTR4 0xC3
#define ST7735_PWCTR5 0xC4
#define ST7735_VMCTR1 0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1
#define MADCTL_MX 0x40
#define MADCTL_MV 0x20
#define DELAY 0x80

static const uint8_t cmd_reset[] = {
    2,
    ST7735_SWRESET, DELAY, 150,
    ST7735_SLPOUT, DELAY, 150,
};

static const uint8_t cmd_init[] = {
    17,
    ST7735_FRMCTR1, 3, 0x01, 0x2C, 0x2D,
    ST7735_FRMCTR2, 3, 0x01, 0x2C, 0x2D,
    ST7735_FRMCTR3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
    ST7735_INVCTR, 1, 0x07,
    ST7735_PWCTR1, 3, 0xA2, 0x02, 0x84,
    ST7735_PWCTR2, 1, 0xC5,
    ST7735_PWCTR3, 2, 0x0A, 0x00,
    ST7735_PWCTR4, 2, 0x8A, 0x2A,
    ST7735_PWCTR5, 2, 0x8A, 0xEE,
    ST7735_VMCTR1, 1, 0x0E,
    ST7735_INVOFF, 0,
    ST7735_MADCTL, 1, MADCTL_MX | MADCTL_MV, /* landscape, as ST7735_ROTATE_DOWN */
    ST7735_COLMOD, 1, LCD_COLMOD,
    ST7735_GMCTRP1, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
                        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    ST7735_GMCTRN1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
    ST7735_NORON, 0,
    ST7735_DISPON, 0,
};

/* The option menu's rate (meta_clock.cpp: 12 to 48 MHz, default
 * META_LCD_SPI_MHZ; meta-lapinou drives this panel at 24). */
#define TFT_SPI_HZ ((uint32_t)meta_clock_lcd_spi() * 1000000u)

/* TXC only rises after a transmission, so track whether one is in flight
 * before waiting for the shifter to drain. */
static uint8_t tx_pending;
static inline void spi_wait_dre(void) { while (!(SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_DRE)) {} }
static inline void spi_wait_txc(void) {
    if (tx_pending) {
        while (!(SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_TXC)) {}
        tx_pending = 0;
    }
}
static inline void spi_send(uint8_t b) { spi_wait_dre(); SERCOM4->SPI.DATA.reg = b; tx_pending = 1; }
static inline void command_mode(void) { spi_wait_txc(); TFT_PORT.OUTCLR.reg = TFT_CS_MASK | TFT_DC_MASK; }
static inline void data_mode(void) { spi_wait_txc(); TFT_PORT.OUTSET.reg = TFT_DC_MASK; TFT_PORT.OUTCLR.reg = TFT_CS_MASK; }
static inline void idle_mode(void) { spi_wait_txc(); TFT_PORT.OUTSET.reg = TFT_CS_MASK; }

/* Commands at 24 MHz whatever the menu's pixel rate, as meta-lapinou sends
 * them on a META. */
static void command_list(const uint8_t *p) {
    meta_spi_rate(24000000u);
    uint8_t n = *p++;
    while (n--) {
        command_mode();
        spi_send(*p++);
        uint8_t args = *p++;
        uint8_t ms = args & DELAY;
        args &= (uint8_t)~DELAY;
        data_mode();
        while (args--) spi_send(*p++);
        if (ms) delay(*p++);
    }
    idle_mode();
}

#if META_LCD_DMA
/* ---- DMA (SERCOM4 TX, channel 0) ---------------------------------------- */

extern "C" void DMAC_Handler(void) {
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_LCD); /* the only channel with interrupts */
    DMAC->CHINTFLAG.reg = DMAC_CHINTFLAG_TCMPL | DMAC_CHINTFLAG_TERR | DMAC_CHINTFLAG_SUSP;
    lcdq_on_complete(); /* starts the next queued strip, if any */
}

#endif

#if META_LCD_DMA
static void dma_init(void) {
    meta_dma_init();
    NVIC_EnableIRQ(DMAC_IRQn);
}
#endif

static void set_window(uint8_t row0, uint8_t row1);

#if META_LCD_DMA
static void start_dma(const void *buf, uint32_t bytes) {
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_LCD);
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
    DMAC->CHCTRLB.reg = DMAC_CHCTRLB_LVL(0) | DMAC_CHCTRLB_TRIGSRC(SERCOM4_DMAC_ID_TX) |
                        DMAC_CHCTRLB_TRIGACT_BEAT;
    DMAC->CHINTENSET.reg = DMAC_CHINTENSET_TCMPL;
    meta_dma_desc[META_DMA_LCD].descaddr = 0;
    meta_dma_desc[META_DMA_LCD].dstaddr = (uint32_t)&SERCOM4->SPI.DATA.reg;
    meta_dma_desc[META_DMA_LCD].btcnt = (uint16_t)bytes;
    meta_dma_desc[META_DMA_LCD].srcaddr = (uint32_t)buf + bytes;
    meta_dma_desc[META_DMA_LCD].btctrl = DMAC_BTCTRL_VALID | DMAC_BTCTRL_SRCINC;
    tx_pending = 1;
    DMAC->CHCTRLA.reg |= DMAC_CHCTRLA_ENABLE;
}

/* Main loop or DMA interrupt: nothing else is using the bus. */
static void lcdq_start(const lcdq_entry *e) {
    set_window(e->row0, (uint8_t)(e->row0 + e->rows - 1));
    start_dma(e->buf, (uint32_t)e->rows * LCD_ROW_BYTES);
}
#endif

void meta_lcd_wait(void) {
#if META_LCD_DMA
    while (lcdq_busy()) {}
#endif
}

int meta_lcd_busy(void) {
#if META_LCD_DMA
    return lcdq_busy();
#else
    return 0;
#endif
}

uint32_t meta_lcd_done_seq(void) {
#if META_LCD_DMA
    return lcdq_done_seq();
#else
    return 0xFFFFFFFFu;
#endif
}

/* ---- public ------------------------------------------------------------- */

void meta_lcd_init(void) {
    TFT_PORT.DIRSET.reg = TFT_CS_MASK | TFT_DC_MASK;
    TFT_PORT.OUTSET.reg = TFT_CS_MASK;
    meta_spi_init();
    if (!PM->RCAUSE.bit.SYST) command_list(cmd_reset);
    command_list(cmd_init);
#if META_LCD_DMA
    dma_init();
#endif
}

static volatile uint8_t in_transaction;

static void set_window(uint8_t row0, uint8_t row1) {
    command_mode();
    spi_send(ST7735_CASET);
    data_mode();
    spi_send(0); spi_send(0); spi_send(0); spi_send(159);
    command_mode();
    spi_send(ST7735_RASET);
    data_mode();
    spi_send(0); spi_send(row0); spi_send(0); spi_send(row1);
    command_mode();
    spi_send(ST7735_RAMWR);
    data_mode();
}

static void open_bus(void) {
    if (in_transaction) return;
    meta_spi_rate(TFT_SPI_HZ);
    in_transaction = 1;
}

uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
#if META_LCD_12BPP
    meta_lcd_pack444_asm((uint8_t *)buf, (unsigned)rows * 160u); /* the buffer is the caller's scratch */
#endif
#if META_LCD_DMA
    for (;;) {
        int start_now;
        __disable_irq();
        uint32_t seq = lcdq_submit_locked(buf, row0, rows, &start_now);
        __enable_irq();
        if (!seq) continue; /* queue full: a completion will make room */
        if (start_now) {
            /* Idle bus: this context owns it until the interrupt chain ends. */
            open_bus();
            lcdq_entry e = {buf, row0, rows};
            lcdq_start(&e);
        }
        return seq;
    }
#else
    open_bus();
    set_window(row0, (uint8_t)(row0 + rows - 1));
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < (uint32_t)rows * LCD_ROW_BYTES; i++) spi_send(p[i]);
    return 0;
#endif
}

int meta_lcd_release(void) {
    if (meta_lcd_busy()) return 0;
    if (in_transaction) {
        idle_mode();
        in_transaction = 0;
    }
    return 1;
}

void meta_lcd_end_frame(void) {
    meta_lcd_wait();
    meta_lcd_release();
}

void meta_lcd_fill(uint16_t color_be) {
    open_bus();
    set_window(0, 127);
#if META_LCD_12BPP
    uint8_t px[4] = {(uint8_t)color_be, (uint8_t)(color_be >> 8), (uint8_t)color_be, (uint8_t)(color_be >> 8)};
    meta_lcd_pack444(px, 2);
    for (uint32_t i = 0; i < 80u * 128u; i++) {
        spi_send(px[0]);
        spi_send(px[1]);
        spi_send(px[2]);
    }
#else
    for (uint32_t i = 0; i < 160u * 128u; i++) {
        spi_send((uint8_t)color_be);
        spi_send((uint8_t)(color_be >> 8));
    }
#endif
    meta_lcd_end_frame();
}
