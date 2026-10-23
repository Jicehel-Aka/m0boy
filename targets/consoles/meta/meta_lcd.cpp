/*
 * ST7735 + DMA output for the Gamebuino META.
 *
 * Derived from the META Display_ST7735 driver and manitou48 DMA helpers used
 * in the meta-lapinou project; the ST7735 init sequence comes from the
 * Adafruit ST7735 library (MIT license, Limor Fried/Ladyada, Adafruit
 * Industries).
 */
#include "m0_lcd.h"
#include "m0_target.h"

/* META_LCD_DMA (gbl_profile.h): 1 streams pixels with DMA while emulation
 * continues; 0 writes them from the CPU (simpler, blocks for the
 * transfer). */

#if META_LCD_DMA
#include "meta_lcd_queue.h"
#include "meta_dma.h"
#endif

#include "m0_clock.h"
#include "meta_hw.h"
#include "meta_lcd_pack.h"

/* META_LCD_12BPP (gbl_profile.h): the panel takes RGB444 (COLMOD 03h); the
 * pipeline renders 0x0RGB halfwords (M0_COLOR) and each row is packed in
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
#define MADCTL_BGR 0x08 /* META_LCD_BGR (gbl_board.h) */
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
    ST7735_MADCTL, 1, MADCTL_MX | MADCTL_MV | (META_LCD_BGR ? MADCTL_BGR : 0), /* paysage */
    ST7735_COLMOD, 1, LCD_COLMOD,
    ST7735_GMCTRP1, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
                        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    ST7735_GMCTRN1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
    ST7735_NORON, 0,
    ST7735_DISPON, 0,
};

/* The option menu's rate (meta_clock.cpp: 12 to 48 MHz, boot default
 * META_LCD_SPI_MHZ, 48; meta-lapinou drives this panel at 24). */
#define TFT_SPI_HZ ((uint32_t)m0_clock_lcd_spi() * 1000000u)

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
/* ---- DMA (SERCOM4 TX, channel 1, meta_dma.h) ---------------------------- */

/* From DMAC_Handler (meta_dma.cpp), which restores CHID. */
extern "C" void meta_dma_lcd_irq(void) {
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_LCD);
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
static void place_rows(uint8_t row0, uint8_t rows);

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
    meta_spi_rate(TFT_SPI_HZ); /* a pad or SD read may have changed it */
    place_rows(e->row0, e->rows);
    start_dma(e->buf, (uint32_t)e->rows * LCD_ROW_BYTES);
}
#endif

void m0_lcd_wait(void) {
#if META_LCD_DMA
    while (lcdq_busy()) {}
#endif
}

int m0_lcd_busy(void) {
#if META_LCD_DMA
    return lcdq_busy();
#else
    return 0;
#endif
}

uint32_t m0_lcd_done_seq(void) {
#if META_LCD_DMA
    return lcdq_done_seq();
#else
    return 0xFFFFFFFFu;
#endif
}

/* ---- public ------------------------------------------------------------- */

void m0_lcd_init(void) {
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

/* The window reaches the panel's last row: rows that follow the previous
 * strip go on as data of the same RAMWR, which moves to the next row by
 * itself.  The window is set again for a strip that does not follow (the
 * frame's first, after rows not drawn) and after the bus was released (the
 * end of every frame, a card or pad access): a command must not be relied
 * on across the panel's CS going high.  Game Gear: one window a frame
 * instead of one per line (11 command bytes and the waits around them).
 * META_LCD_RUNS=0: a window for every strip, as before. */
#ifndef META_LCD_RUNS
#define META_LCD_RUNS 1
#endif
#define LAST_ROW 127u
static uint16_t win_next = 0xFFFFu; /* row the open RAMWR writes next */

static void place_rows(uint8_t row0, uint8_t rows) {
    if (!META_LCD_RUNS) set_window(row0, (uint8_t)(row0 + rows - 1));
    else if (row0 != win_next || !in_transaction) set_window(row0, (uint8_t)LAST_ROW);
    win_next = (uint16_t)(row0 + rows);
}

static void open_bus(void) {
    if (in_transaction) return;
    meta_spi_rate(TFT_SPI_HZ);
    in_transaction = 1;
}

uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
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
    place_rows(row0, rows);
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < (uint32_t)rows * LCD_ROW_BYTES; i++) spi_send(p[i]);
    return 0;
#endif
}

#if M0_DEBUG_BUS
/* Debug (meta-usb.py read meta_bus_violations): what should be idle when
 * the pad or the card takes the bus, counted per kind: 0 the display DMA
 * channel still enabled, 1 the display's CS low, 2 a byte waiting in DATA,
 * 3 the strip queue busy.  Index 4 counts the checks. */
extern "C" {
volatile uint32_t meta_bus_violations[5];
}
extern "C" void m0_lcd_bus_check(void) {
    unsigned bad = 0;
#if META_LCD_DMA
    __disable_irq();
    uint8_t chid = DMAC->CHID.reg;
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_LCD);
    if (DMAC->CHCTRLA.reg & DMAC_CHCTRLA_ENABLE) bad |= 1;
    DMAC->CHID.reg = chid;
    __enable_irq();
    if (lcdq_busy()) bad |= 8;
#endif
    if (!(TFT_PORT.OUT.reg & TFT_CS_MASK)) bad |= 2;
    if (!(SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_DRE)) bad |= 4;
    for (unsigned k = 0; k < 4; k++)
        if (bad & (1u << k)) meta_bus_violations[k]++;
    meta_bus_violations[4]++;
}
#endif

int m0_lcd_release(void) {
    if (m0_lcd_busy()) return 0;
    if (in_transaction) {
        idle_mode();
        in_transaction = 0;
    }
    win_next = 0xFFFFu;
    return 1;
}

void m0_lcd_end_frame(void) {
    m0_lcd_wait();
    m0_lcd_release();
}

void m0_lcd_fill(uint16_t color_be) {
    open_bus();
    set_window(0, 127);
    win_next = 0xFFFFu;
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
    m0_lcd_end_frame();
}
