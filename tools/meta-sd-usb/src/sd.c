#include "sd.h"
#include "meta_hw.h"

/* META: SD CS PA27; SERCOM4 MISO PA12/PAD0, MOSI PB10/PAD2, SCK PB11/PAD3. */
#define SPI SERCOM4->SPI
#define CS (1u << 27)
static uint8_t high_capacity;
static uint32_t capacity;
static uint16_t crc16(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (unsigned i = 0; i < 8; ++i)
        crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0));
    return crc;
}

static uint8_t xfer(uint8_t b) {
    SPI.DATA.reg = b;
    while (!(SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
    return (uint8_t)SPI.DATA.reg;
}
static void select_card(void) { PORT->Group[0].OUTCLR.reg = CS; xfer(0xff); }
static void release_card(void) { PORT->Group[0].OUTSET.reg = CS; xfer(0xff); }
static uint8_t command(uint8_t op, uint32_t arg, uint8_t crc) {
    xfer(0xff); /* required gap between SD commands, even with CS held low */
    xfer((uint8_t)(0x40u | op));
    xfer((uint8_t)(arg >> 24)); xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8)); xfer((uint8_t)arg); xfer(crc);
    uint8_t r = 0xff;
    for (unsigned n = 0; n < 16 && (r & 0x80u); ++n) r = xfer(0xff);
    return r;
}
static int token(uint32_t timeout_ms) {
    uint32_t start = millis();
    uint8_t b;
    do { b = xfer(0xff); } while (b == 0xff && millis() - start < timeout_ms);
    return b == 0xfe;
}
static void pin_d(unsigned group, unsigned pin) {
    PortGroup *g = &PORT->Group[group];
    if (pin & 1u) g->PMUX[pin >> 1].bit.PMUXO = PORT_PMUX_PMUXO_D_Val;
    else g->PMUX[pin >> 1].bit.PMUXE = PORT_PMUX_PMUXE_D_Val;
    g->PINCFG[pin].reg |= PORT_PINCFG_PMUXEN;
}
static void rate(uint8_t baud) {
    SPI.CTRLA.bit.ENABLE = 0;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
    SPI.BAUD.reg = baud;
    SPI.CTRLA.bit.ENABLE = 1;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
}
static void bus_init(void) {
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM4;
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(6) | GCLK_GENDIV_DIV(1);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(6) | GCLK_GENCTRL_SRC_DFLL48M |
                        GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM4_CORE_Val) |
                                   GCLK_CLKCTRL_GEN_GCLK6 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    pin_d(0, 12); pin_d(1, 10); pin_d(1, 11);
    PORT->Group[0].DIRCLR.reg = 1u << 12;
    PORT->Group[0].PINCFG[12].reg = PORT_PINCFG_PMUXEN | PORT_PINCFG_INEN;
    /* The panel and button shift register share SPI. Keep both deselected. */
    PORT->Group[1].PINCFG[22].reg = 0;
    PORT->Group[1].OUTSET.reg = 1u << 22;
    PORT->Group[1].DIRSET.reg = 1u << 22;
    PORT->Group[1].PINCFG[3].reg = 0;
    PORT->Group[1].OUTSET.reg = 1u << 3;
    PORT->Group[1].DIRSET.reg = 1u << 3;
    PORT->Group[0].PINCFG[27].reg = 0;
    PORT->Group[0].OUTSET.reg = CS;
    PORT->Group[0].DIRSET.reg = CS;
    SPI.CTRLA.reg = SERCOM_SPI_CTRLA_MODE_SPI_MASTER | SERCOM_SPI_CTRLA_DOPO(1) |
                    SERCOM_SPI_CTRLA_DIPO(0);
    SPI.CTRLB.reg = SERCOM_SPI_CTRLB_CHSIZE(0) | SERCOM_SPI_CTRLB_RXEN;
    while (SPI.SYNCBUSY.bit.CTRLB) {}
    rate(95); /* 250 kHz at 48 MHz */
}
/* The pad's 74HC165 (CS PB03) shares the card's bus: 2 MHz for it, then the card's rate again. */
uint8_t sd_pad(void) {
    rate(11);
    for (unsigned i = 0; i < 4 && (SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); ++i) (void)SPI.DATA.reg;
    PORT->Group[1].OUTCLR.reg = 1u << 3;
    for (volatile unsigned t = 0; t < 100; ++t) {} /* latch time */
    uint8_t raw = xfer(1);
    PORT->Group[1].OUTSET.reg = 1u << 3;
    rate(1);
    return (uint8_t)~raw; /* active low */
}
uint32_t sd_sectors(void) { return capacity; }
int sd_init(void) {
    bus_init();
    for (unsigned i = 0; i < 10; ++i) xfer(0xff);
    select_card();
    uint32_t start = millis();
    while (command(0, 0, 0x95) != 1) {
        if (millis() - start > 500) goto fail;
    }
    uint8_t v2 = command(8, 0x1aa, 0x87) == 1;
    if (v2) {
        uint8_t r[4];
        for (unsigned i = 0; i < 4; ++i) r[i] = xfer(0xff);
        if (r[2] != 1 || r[3] != 0xaa) goto fail;
    }
    start = millis();
    for (;;) {
        command(55, 0, 0x65);
        if (command(41, v2 ? 0x40000000u : 0, 0x77) == 0) break;
        if (millis() - start > 1500) goto fail;
    }
    high_capacity = 0;
    if (v2 && command(58, 0, 0xfd) == 0) {
        uint8_t ocr = xfer(0xff);
        for (unsigned i = 0; i < 3; ++i) xfer(0xff);
        high_capacity = (ocr & 0x40u) != 0;
    }
    /* CMD9 CSD gives total logical 512-byte blocks. */
    if (command(9, 0, 1) != 0 || !token(100)) goto fail;
    uint8_t csd[16];
    for (unsigned i = 0; i < 16; ++i) csd[i] = xfer(0xff);
    xfer(0xff); xfer(0xff);
    if ((csd[0] >> 6) == 1) {
        uint32_t c = ((uint32_t)(csd[7] & 63u) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        capacity = (c + 1u) * 1024u;
    } else if ((csd[0] >> 6) == 0) {
        uint32_t c = ((uint32_t)(csd[6] & 3u) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((uint32_t)(csd[9] & 3u) << 1) | (csd[10] >> 7);
        uint32_t bytes = (c + 1u) << (mult + 2u + (csd[5] & 15u));
        capacity = bytes / 512u;
        if (command(16, 512, 1) != 0) goto fail;
    } else goto fail;
    release_card();
    rate(1); /* 12 MHz, conservative on physical META hardware */
    return capacity != 0;
fail:
    release_card();
    capacity = 0;
    return 0;
}
int sd_read(uint32_t lba, uint8_t *dst) {
    if (lba >= capacity) return 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        select_card();
        int ok = command(17, high_capacity ? lba : lba << 9, 1) == 0 && token(120);
        if (ok) {
            uint16_t crc = 0;
            for (unsigned i = 0; i < 512; ++i) {
                dst[i] = xfer(0xff);
                crc = crc16(crc, dst[i]);
            }
            uint16_t received = (uint16_t)xfer(0xff) << 8;
            received |= xfer(0xff);
            ok = crc == received;
        }
        release_card();
        if (ok) return 1;
    }
    return 0;
}
int sd_write(uint32_t lba, const uint8_t *src) {
    if (lba >= capacity) return 0;
    select_card();
    int ok = command(24, high_capacity ? lba : lba << 9, 1) == 0;
    if (ok) {
        xfer(0xff); xfer(0xfe);
        for (unsigned i = 0; i < 512; ++i) xfer(src[i]);
        xfer(0xff); xfer(0xff);
        uint8_t r = 0xff;
        for (unsigned i = 0; i < 16 && r == 0xff; ++i) r = xfer(0xff);
        ok = (r & 31u) == 5u;
        uint32_t start = millis();
        while (ok && xfer(0xff) != 0xff) {
            if (millis() - start > 700) { ok = 0; break; }
        }
    }
    release_card();
    return ok;
}
