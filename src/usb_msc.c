#include "usb_msc.h"
#include "sd.h"
#include "meta_hw.h"
#include <string.h>

/* USB 2.0 full-speed Mass Storage, Bulk-Only Transport, one removable LUN. */
#define EP_OUT 1
#define EP_IN 2
#define EP(n) USB->DEVICE.DeviceEndpoint[n]
#define CBW_MAGIC 0x43425355u
#define CSW_MAGIC 0x53425355u

static UsbDeviceDescriptor desc[3] __attribute__((aligned(4)));
static uint8_t ep0_out[64] __attribute__((aligned(4)));
static uint8_t ep0_in[64] __attribute__((aligned(4)));
static uint8_t bulk_out[64] __attribute__((aligned(4)));
static uint8_t bulk_in[64] __attribute__((aligned(4)));
static uint8_t sector[512];
static uint8_t configured, state, sense_key, sense_asc;
static uint32_t tag, residue, current_lba, remaining_blocks;
static uint16_t fill;

enum { WAIT_CBW, WRITE_DATA };

static const uint8_t device_desc[] = {
    18, 1, 0, 2, 0, 0, 0, 64,
    0x41, 0x23, 0x4d, 0x80, /* Gamebuino META VID/PID (existing native port) */
    0, 1, 1, 2, 3, 1
};
static const uint8_t config_desc[] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, EP_OUT, 2, 64, 0, 0,
    7, 5, 0x80 | EP_IN, 2, 64, 0, 0
};
static const uint8_t language[] = {4, 3, 9, 4};
static const char *const strings[] = {"Gamebuino", "META SD Bridge", "0001"};

static int wait_in(unsigned ep) {
    uint32_t start = millis();
    while (!(EP(ep).EPINTFLAG.reg & USB_DEVICE_EPINTFLAG_TRCPT1)) {
        if (millis() - start > 1000) return 0;
    }
    EP(ep).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT1;
    return 1;
}
static int send(unsigned ep, uint8_t *buffer, const void *data, unsigned length) {
    if (length) memcpy(buffer, data, length);
    desc[ep].DeviceDescBank[1].ADDR.reg = (uint32_t)buffer;
    desc[ep].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3) |
                                             USB_DEVICE_PCKSIZE_BYTE_COUNT(length);
    EP(ep).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT1;
    EP(ep).EPSTATUSSET.reg = USB_DEVICE_EPSTATUSSET_BK1RDY;
    return wait_in(ep);
}
static void control_in(const void *data, unsigned size, unsigned requested) {
    const uint8_t *p = (const uint8_t *)data;
    if (size > requested) size = requested;
    unsigned sent = 0;
    do {
        unsigned n = size - sent > 64 ? 64 : size - sent;
        if (!send(0, ep0_in, p + sent, n)) return;
        sent += n;
        if (n < 64 || (sent == size && size == requested)) break;
    } while (1);
    EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
}
static void control_ack(void) { send(0, ep0_in, 0, 0); }
static void control_stall(void) {
    EP(0).EPSTATUSSET.reg = USB_DEVICE_EPSTATUSSET_STALLRQ0 | USB_DEVICE_EPSTATUSSET_STALLRQ1;
}
static void arm_out(void) {
    desc[EP_OUT].DeviceDescBank[0].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
    EP(EP_OUT).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
}
static void configure(void) {
    memset(&desc[1], 0, sizeof desc - sizeof desc[0]);
    EP(EP_OUT).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE0(3);
    desc[EP_OUT].DeviceDescBank[0].ADDR.reg = (uint32_t)bulk_out;
    EP(EP_OUT).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT0;
    arm_out();
    EP(EP_IN).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE1(3);
    desc[EP_IN].DeviceDescBank[1].ADDR.reg = (uint32_t)bulk_in;
    desc[EP_IN].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
    EP(EP_IN).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK1RDY;
    state = WAIT_CBW;
}
static void descriptor(uint16_t value, unsigned requested) {
    unsigned type = value >> 8, index = value & 255u;
    if (type == 1) control_in(device_desc, sizeof device_desc, requested);
    else if (type == 2) control_in(config_desc, sizeof config_desc, requested);
    else if (type == 3 && index == 0) control_in(language, sizeof language, requested);
    else if (type == 3 && index <= 3) {
        uint8_t s[64];
        const char *p = strings[index - 1];
        unsigned n = 0;
        while (p[n] && n < 31) { s[2 + n * 2] = (uint8_t)p[n]; s[3 + n * 2] = 0; ++n; }
        s[0] = (uint8_t)(2 + 2 * n); s[1] = 3;
        control_in(s, s[0], requested);
    } else control_stall();
}
static void setup(void) {
    uint8_t type = ep0_out[0], req = ep0_out[1];
    uint16_t value = (uint16_t)(ep0_out[2] | ep0_out[3] << 8);
    uint16_t length = (uint16_t)(ep0_out[6] | ep0_out[7] << 8);
    if ((type & 0x60) == 0) {
        static const uint8_t zero[2] = {0, 0};
        switch (req) {
        case 0: control_in(zero, 2, length); return;
        case 1: /* CLEAR_FEATURE(ENDPOINT_HALT) */
            if ((type & 31u) == 2 && value == 0) {
                unsigned ep = ep0_out[4] & 15u;
                if (ep == EP_OUT || ep == EP_IN) {
                    if (ep0_out[4] & 0x80u)
                        EP(ep).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ1;
                    else EP(ep).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ0;
                }
            }
            control_ack(); return;
        case 3: case 11: control_ack(); return;
        case 5:
            control_ack();
            USB->DEVICE.DADD.reg = (uint8_t)(USB_DEVICE_DADD_ADDEN | (value & 127u));
            return;
        case 6: descriptor(value, length); return;
        case 8: control_in(&configured, 1, length); return;
        case 9:
            configured = (uint8_t)value;
            if (configured) configure();
            control_ack(); return;
        case 10: control_in(zero, 1, length); return;
        }
    } else if ((type & 0x60) == 0x20) {
        static const uint8_t lun = 0;
        if (type == 0xa1 && req == 0xfe) { control_in(&lun, 1, length); return; }
        if (type == 0x21 && req == 0xff) {
            state = WAIT_CBW; fill = 0;
            EP(EP_OUT).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ0;
            EP(EP_IN).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ1;
            arm_out(); control_ack(); return;
        }
    }
    control_stall();
}
void USB_Handler(void) {
    if (USB->DEVICE.INTFLAG.reg & USB_DEVICE_INTFLAG_EORST) {
        USB->DEVICE.INTFLAG.reg = USB_DEVICE_INTFLAG_EORST;
        configured = 0;
        for (unsigned ep = 1; ep < 3; ++ep) {
            EP(ep).EPCFG.reg = 0;
            EP(ep).EPINTENCLR.reg = 0x7f;
            EP(ep).EPINTFLAG.reg = 0x7f;
        }
        memset(desc, 0, sizeof desc);
        EP(0).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE0(1) | USB_DEVICE_EPCFG_EPTYPE1(1);
        desc[0].DeviceDescBank[0].ADDR.reg = (uint32_t)ep0_out;
        desc[0].DeviceDescBank[0].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
        desc[0].DeviceDescBank[1].ADDR.reg = (uint32_t)ep0_in;
        desc[0].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
        EP(0).EPINTENSET.reg = USB_DEVICE_EPINTENSET_RXSTP;
    }
    if (EP(0).EPINTFLAG.reg & USB_DEVICE_EPINTFLAG_RXSTP) {
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ0 | USB_DEVICE_EPSTATUSCLR_STALLRQ1;
        EP(0).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_RXSTP;
        setup();
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
    }
}
void usb_msc_init(void) {
    PORT->Group[0].PMUX[12].reg = PORT_PMUX_PMUXE_G | PORT_PMUX_PMUXO_G;
    PORT->Group[0].PINCFG[24].reg = PORT_PINCFG_PMUXEN;
    PORT->Group[0].PINCFG[25].reg = PORT_PINCFG_PMUXEN;
    PM->APBBMASK.reg |= PM_APBBMASK_USB;
    PM->AHBMASK.reg |= PM_AHBMASK_USB;
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(7) | GCLK_GENDIV_DIV(1);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(7) | GCLK_GENCTRL_SRC_DFLL48M |
                        GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_USB_Val) |
                                   GCLK_CLKCTRL_GEN_GCLK7 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    USB->DEVICE.CTRLA.reg = USB_CTRLA_SWRST;
    while (USB->DEVICE.SYNCBUSY.reg & USB_SYNCBUSY_SWRST) {}
    uint32_t f = *(const uint32_t *)(NVMCTRL_OTP4 + 4);
    uint32_t n = (f & USB_FUSES_TRANSN_Msk) >> USB_FUSES_TRANSN_Pos;
    uint32_t p = (f & USB_FUSES_TRANSP_Msk) >> USB_FUSES_TRANSP_Pos;
    uint32_t t = (f & USB_FUSES_TRIM_Msk) >> USB_FUSES_TRIM_Pos;
    if (n == 31) n = 5;
    if (p == 31) p = 29;
    if (t == 7) t = 3;
    USB->DEVICE.PADCAL.reg = (uint16_t)(USB_PADCAL_TRANSN(n) |
                                         USB_PADCAL_TRANSP(p) | USB_PADCAL_TRIM(t));
    memset(desc, 0, sizeof desc);
    USB->DEVICE.DESCADD.reg = (uint32_t)desc;
    USB->DEVICE.QOSCTRL.reg = USB_QOSCTRL_CQOS(3) | USB_QOSCTRL_DQOS(3);
    USB->DEVICE.CTRLA.reg = USB_CTRLA_MODE_DEVICE | USB_CTRLA_RUNSTDBY;
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_SPDCONF_FS | USB_DEVICE_CTRLB_DETACH;
    USB->DEVICE.INTENSET.reg = USB_DEVICE_INTENSET_EORST;
    USB->DEVICE.CTRLA.reg |= USB_CTRLA_ENABLE;
    while (USB->DEVICE.SYNCBUSY.reg & USB_SYNCBUSY_ENABLE) {}
    delay(200);
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_SPDCONF_FS;
    NVIC_SetPriority(USB_IRQn, (1u << __NVIC_PRIO_BITS) - 1u);
    NVIC_EnableIRQ(USB_IRQn);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static int data_in(const void *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    if (n > residue) n = residue;
    uint32_t original = n;
    while (n) {
        unsigned k = n > 64 ? 64 : n;
        if (!send(EP_IN, bulk_in, s, k)) return 0;
        s += k; n -= k;
    }
    residue -= original;
    return 1;
}
static void csw(uint8_t status) {
    uint8_t r[13];
    put_le32(r, CSW_MAGIC); put_le32(r + 4, tag); put_le32(r + 8, residue);
    r[12] = status;
    send(EP_IN, bulk_in, r, sizeof r);
    state = WAIT_CBW;
}
static void fail(uint8_t key, uint8_t asc) {
    sense_key = key; sense_asc = asc;
    csw(1);
}
static void scsi(const uint8_t *b) {
    const uint8_t *c = b + 15;
    tag = le32(b + 4); residue = le32(b + 8);
    uint8_t in = b[12] & 0x80u;
    if (b[13] != 0 || b[14] == 0 || b[14] > 16) { fail(5, 0x24); return; }
    uint8_t r[36] = {0};
    switch (c[0]) {
    case 0x00: /* TEST UNIT READY */
        if (!sd_sectors()) fail(2, 0x3a); else csw(0);
        return;
    case 0x03: /* REQUEST SENSE */
        r[0] = 0x70; r[2] = sense_key; r[7] = 10; r[12] = sense_asc;
        if (!in || !data_in(r, 18)) { fail(5, 0x24); return; }
        sense_key = sense_asc = 0;
        csw(0); return;
    case 0x12: /* INQUIRY */
        r[0] = 0; r[1] = 0x80; r[2] = 5; r[3] = 2; r[4] = 31;
        memcpy(r + 8, "GAMEBUINO", 9);
        memcpy(r + 16, "META SD BRIDGE  ", 16);
        memcpy(r + 32, "1.0 ", 4);
        if (!in || !data_in(r, 36)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x1a: /* MODE SENSE(6), write caching disabled */
        r[0] = 3; r[2] = 0;
        if (!in || !data_in(r, 4)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x5a: /* MODE SENSE(10), no mode pages */
        r[1] = 6;
        if (!in || !data_in(r, 8)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0xa0: /* REPORT LUNS: one LUN, number zero */
        r[3] = 8;
        if (!in || !data_in(r, 16)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x1b: case 0x1e: case 0x35: /* START STOP, PREVENT/ALLOW, SYNC CACHE */
        csw(0); return;
    case 0x2f: /* VERIFY(10), no comparison data */
        if (c[1] & 2u) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x23: /* READ FORMAT CAPACITIES */
        r[3] = 8; put_be32(r + 4, sd_sectors()); r[8] = 2;
        r[10] = 2; /* 512 bytes */
        if (!in || !data_in(r, 12)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x25: /* READ CAPACITY(10) */
        if (!sd_sectors()) { fail(2, 0x3a); return; }
        put_be32(r, sd_sectors() - 1u); put_be32(r + 4, 512);
        if (!in || !data_in(r, 8)) { fail(5, 0x24); return; }
        csw(0); return;
    case 0x28: case 0x2a: { /* READ(10), WRITE(10) */
        current_lba = be32(c + 2); remaining_blocks = be16(c + 7);
        if (current_lba > sd_sectors() || remaining_blocks > sd_sectors() - current_lba) {
            fail(5, 0x21); return;
        }
        if (residue != remaining_blocks * 512u || (in != (c[0] == 0x28))) {
            fail(5, 0x24); return;
        }
        if (c[0] == 0x2a) {
            if (!remaining_blocks) { csw(0); return; }
            state = WRITE_DATA; fill = 0; return;
        }
        while (remaining_blocks--) {
            if (!sd_read(current_lba++, sector)) { fail(3, 0x11); return; }
            if (!data_in(sector, 512)) { fail(4, 0x44); return; }
            meta_watchdog_feed();
        }
        csw(0); return;
    }
    default: fail(5, 0x20); return;
    }
}
void usb_msc_poll(void) {
    if (!configured || !(EP(EP_OUT).EPINTFLAG.reg & USB_DEVICE_EPINTFLAG_TRCPT0)) return;
    EP(EP_OUT).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT0;
    unsigned n = desc[EP_OUT].DeviceDescBank[0].PCKSIZE.bit.BYTE_COUNT;
    if (state == WAIT_CBW) {
        if (n == 31 && le32(bulk_out) == CBW_MAGIC) scsi(bulk_out);
        else { state = WAIT_CBW; }
    } else if (state == WRITE_DATA) {
        if (n > 512u - fill || n == 0) { fail(5, 0x24); }
        else {
            memcpy(sector + fill, bulk_out, n);
            fill += (uint16_t)n;
            residue -= n;
            if (fill == 512) {
                if (!sd_write(current_lba++, sector)) fail(3, 0x0c);
                else {
                    fill = 0;
                    --remaining_blocks;
                    if (!remaining_blocks) csw(0);
                }
                meta_watchdog_feed();
            }
        }
    }
    arm_out();
}
