/*
 * USB CDC-ACM device of the Gamebuino META, bare metal (META_USB,
 * gbl_board.h): the firmware shows up as a serial port, like the Arduino
 * core's.  It serves three things:
 *
 *  - the 1200-baud touch (open at 1200 baud, drop DTR) that upload tools use
 *    to get the console into its bootloader: the frontend saves, then resets
 *    with the bootloader's sam-ba request in RAM (meta_reset_to_bootloader);
 *  - sam-ba's commands, hexadecimal: "V#" version, "R<addr>,<count>#" read
 *    RAM or flash, "W<addr>,<word>#" and "O<addr>,<byte>#" write RAM, while
 *    the game runs (tools/meta-usb.py reads and writes by ELF symbol);
 *  - profiling: SysTick writes the interrupted PC into a ring of 32 every
 *    millisecond (meta_usb_prof: count, then the samples); the host drains
 *    it every 20 ms or so and builds the histogram (tools/meta-usb.py prof);
 *  - tests: meta_usb_buttons is ORed into every pad read (META_MB_* bits);
 *  - meta_usb_write, for debug output.
 *
 * Interrupt driven, at the lowest priority: control transfers are answered in
 * the interrupt, which waits for the host to take each IN packet (bounded).
 * Endpoints: 0 control, 1 interrupt IN (CDC notifications, never sent),
 * 2 bulk OUT, 3 bulk IN, 64 bytes.  USB runs on generator 7 from the DFLL48M,
 * which stays at 48 MHz whatever clock the CPU uses (meta_clock.cpp).
 */
#include "meta_usb.h"

#include <string.h>

#include "meta_hw.h"
#include "meta_target.h"

#if META_USB

#define VID 0x2341u /* the Arduino Zero's native port, like the Gamebuino library */
#define PID 0x804Du

#define EP_NOTIFY 1
#define EP_OUT 2
#define EP_IN 3

static UsbDeviceDescriptor ep_desc[4] __attribute__((aligned(4)));
static uint8_t ep0_out[64] __attribute__((aligned(4)));
static uint8_t ep0_in[64] __attribute__((aligned(4)));
static uint8_t rx_buf[64] __attribute__((aligned(4)));
static uint8_t tx_buf[64] __attribute__((aligned(4)));

static uint8_t configured, line_state;
static uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8}; /* 115200 8N1 */
volatile uint8_t meta_usb_bootloader_request;
volatile uint8_t meta_usb_buttons;
/* The SysTick sampler's ring (meta_startup.c reads it through a weak
 * pointer): only in a profiling build (-DMETA_USB_PROF=1, meta-usb.py
 * prof), 132 bytes of RAM the games keep otherwise. */
#ifdef META_USB_PROF
volatile uint32_t meta_usb_prof[1 + META_USB_PROF_SAMPLES];
#endif

#ifdef META_USB_DEBUG
/* Read through sam-ba after a double press on RESET. */
struct {
    uint32_t isr, eorst, setup, in_ok, in_timeout, out_ok, out_timeout, stall;
    uint8_t last_setup[8];
    uint32_t intflag, ep0flag;
} meta_usb_dbg;
#define DBG(x) (meta_usb_dbg.x)
#else
#define DBG(x) (void)0
#endif

/* Command parser (V#, R<addr>,<count>#). */
static char cmd[24];
static uint8_t cmd_len;

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
    VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8, 0x00, 0x01,
    1, 2, 0, 1,
};

static const uint8_t conf_desc[75] = {
    9, 2, 75, 0, 2, 1, 0, 0x80, 50,
    /* interface association: CDC, two interfaces */
    8, 0x0B, 0, 2, 2, 2, 0, 0,
    /* interface 0: communication */
    9, 4, 0, 0, 1, 2, 2, 0, 0,
    5, 0x24, 0x00, 0x10, 0x01,  /* header */
    5, 0x24, 0x01, 0x01, 1,     /* call management: data interface 1 */
    4, 0x24, 0x02, 0x06,        /* ACM: line coding, break */
    5, 0x24, 0x06, 0, 1,        /* union */
    7, 5, 0x80 | EP_NOTIFY, 3, 16, 0, 16,
    /* interface 1: data */
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
    7, 5, EP_OUT, 2, 64, 0, 0,
    7, 5, 0x80 | EP_IN, 2, 64, 0, 0,
};

static const uint8_t str0[4] = {4, 3, 0x09, 0x04};
static const char *const strings[] = {"Gamebuino", "Gamebuino META m0boy"};

#define EP(n) USB->DEVICE.DeviceEndpoint[n]

/* Bounded wait for an endpoint interrupt flag (the host may never come). */
static int wait_flag(unsigned ep, uint8_t flag) {
    for (uint32_t t = 0; t < 400000u; t++)
        if (EP(ep).EPINTFLAG.reg & flag) {
            EP(ep).EPINTFLAG.reg = flag;
#ifdef META_USB_DEBUG
            if (flag & USB_DEVICE_EPINTFLAG_TRCPT1) DBG(in_ok)++;
            else DBG(out_ok)++;
#endif
            return 1;
        }
#ifdef META_USB_DEBUG
    if (flag & USB_DEVICE_EPINTFLAG_TRCPT1) DBG(in_timeout)++;
    else DBG(out_timeout)++;
#endif
    return 0;
}

/* One IN packet of at most 64 bytes on endpoint 0 or EP_IN. */
static int send_packet(unsigned ep, uint8_t *buf, const void *data, unsigned n) {
    if (n) memcpy(buf, data, n);
    ep_desc[ep].DeviceDescBank[1].ADDR.reg = (uint32_t)buf;
    ep_desc[ep].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3) | USB_DEVICE_PCKSIZE_BYTE_COUNT(n);
    EP(ep).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT1;
    EP(ep).EPSTATUSSET.reg = USB_DEVICE_EPSTATUSSET_BK1RDY;
    return wait_flag(ep, USB_DEVICE_EPINTFLAG_TRCPT1);
}

/* Control IN data stage, cut to the host's wLength, in 64-byte packets (a
 * last full packet is followed by a zero-length one when shorter than asked). */
static void control_in(const uint8_t *data, unsigned n, unsigned wlength) {
    if (n > wlength) n = wlength;
    unsigned sent = 0;
    for (;;) {
        unsigned k = n - sent > 64 ? 64 : n - sent;
        if (!send_packet(0, ep0_in, data + sent, k)) return;
        sent += k;
        if (k < 64 || (sent == n && n == wlength)) break;
    }
    /* Status stage: the host's zero-length OUT. */
    EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
}

static void control_ack(void) { send_packet(0, ep0_in, 0, 0); }

static void control_stall(void) {
#ifdef META_USB_DEBUG
    DBG(stall)++;
#endif
    EP(0).EPSTATUSSET.reg = USB_DEVICE_EPSTATUSSET_STALLRQ0 | USB_DEVICE_EPSTATUSSET_STALLRQ1;
}

static void configure_endpoints(void) {
    memset(&ep_desc[1], 0, sizeof ep_desc - sizeof ep_desc[0]);
    EP(EP_NOTIFY).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE1(4); /* interrupt IN */
    ep_desc[EP_NOTIFY].DeviceDescBank[1].ADDR.reg = (uint32_t)tx_buf;
    ep_desc[EP_NOTIFY].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(1);
    EP(EP_NOTIFY).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK1RDY;

    EP(EP_OUT).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE0(3); /* bulk OUT */
    ep_desc[EP_OUT].DeviceDescBank[0].ADDR.reg = (uint32_t)rx_buf;
    ep_desc[EP_OUT].DeviceDescBank[0].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
    EP(EP_OUT).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT0;
    EP(EP_OUT).EPINTENSET.reg = USB_DEVICE_EPINTENSET_TRCPT0;
    EP(EP_OUT).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY; /* ready to receive */

    EP(EP_IN).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE1(3); /* bulk IN */
    ep_desc[EP_IN].DeviceDescBank[1].ADDR.reg = (uint32_t)tx_buf;
    ep_desc[EP_IN].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
    EP(EP_IN).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK1RDY;
}

static void get_descriptor(uint16_t wvalue, uint16_t wlength) {
    uint8_t type = (uint8_t)(wvalue >> 8), index = (uint8_t)wvalue;
    if (type == 1) {
        control_in(dev_desc, sizeof dev_desc, wlength);
    } else if (type == 2) {
        control_in(conf_desc, sizeof conf_desc, wlength);
    } else if (type == 3 && index == 0) {
        control_in(str0, sizeof str0, wlength);
    } else if (type == 3 && index <= 2) {
        uint8_t s[2 + 2 * 31];
        const char *p = strings[index - 1];
        unsigned n = 0;
        while (p[n] && n < 31) {
            s[2 + 2 * n] = (uint8_t)p[n];
            s[3 + 2 * n] = 0;
            n++;
        }
        s[0] = (uint8_t)(2 + 2 * n);
        s[1] = 3;
        control_in(s, s[0], wlength);
    } else {
        control_stall(); /* device qualifier and the rest: full-speed only */
    }
}

static void setup_request(void) {
    const uint8_t *s = ep0_out;
    uint8_t type = s[0], req = s[1];
    uint16_t wvalue = (uint16_t)(s[2] | s[3] << 8), wlength = (uint16_t)(s[6] | s[7] << 8);
    if ((type & 0x60) == 0x00) { /* standard */
        switch (req) {
        case 0x00: { /* GET_STATUS */
            static const uint8_t zero[2] = {0, 0};
            control_in(zero, 2, wlength);
            return;
        }
        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
        case 0x0B: /* SET_INTERFACE */
            control_ack();
            return;
        case 0x05: /* SET_ADDRESS: after the status stage */
            control_ack();
            USB->DEVICE.DADD.reg = (uint8_t)(USB_DEVICE_DADD_ADDEN | (wvalue & 0x7F));
            return;
        case 0x06:
            get_descriptor(wvalue, wlength);
            return;
        case 0x08: { /* GET_CONFIGURATION */
            uint8_t c = configured;
            control_in(&c, 1, wlength);
            return;
        }
        case 0x09: /* SET_CONFIGURATION */
            configured = (uint8_t)(wvalue & 0xFF);
            if (configured) configure_endpoints();
            control_ack();
            return;
        case 0x0A: { /* GET_INTERFACE */
            uint8_t z = 0;
            control_in(&z, 1, wlength);
            return;
        }
        }
    } else if ((type & 0x60) == 0x20) { /* class (CDC) */
        switch (req) {
        case 0x20: /* SET_LINE_CODING: 7 bytes of data */
            ep_desc[0].DeviceDescBank[0].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
            EP(0).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT0;
            EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
            if (wait_flag(0, USB_DEVICE_EPINTFLAG_TRCPT0)) memcpy(line_coding, ep0_out, sizeof line_coding);
            control_ack();
            return;
        case 0x21: /* GET_LINE_CODING */
            control_in(line_coding, sizeof line_coding, wlength);
            return;
        case 0x22: /* SET_CONTROL_LINE_STATE */
            line_state = (uint8_t)wvalue;
            control_ack();
            /* The Arduino upload touch: 1200 baud, then DTR dropped. */
            if (!(line_state & 1) && line_coding[0] == 0xB0 && line_coding[1] == 0x04 && line_coding[2] == 0 &&
                line_coding[3] == 0)
                meta_usb_bootloader_request = 1;
            return;
        case 0x23: /* SEND_BREAK */
            control_ack();
            return;
        }
    }
    control_stall();
}

static void reply(const void *data, unsigned n) {
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        unsigned k = n > 64 ? 64 : n;
        if (!send_packet(EP_IN, tx_buf, p, k)) return;
        p += k;
        n -= k;
    }
}

static uint32_t hex(const char *p, const char **end) {
    uint32_t v = 0;
    for (;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') v = v << 4 | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v << 4 | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v << 4 | (uint32_t)(c - 'A' + 10);
        else break;
    }
    *end = p;
    return v;
}

static void command(void) {
    cmd[cmd_len] = 0;
    if (cmd[0] == 'V') {
        static const char v[] = "m0boy META\n\r";
        reply(v, sizeof v - 1);
    } else if (cmd[0] == 'R') {
        const char *p;
        uint32_t addr = hex(cmd + 1, &p);
        uint32_t n = *p == ',' ? hex(p + 1, &p) : 0;
        /* RAM, flash (bootloader included), the NVM calibration rows, and,
         * by words, the peripherals and the core's registers (reading some,
         * a SERCOM's DATA for one, has side effects). */
        int ok = (addr >= 0x20000000u && addr + n <= 0x20008000u) || addr + n <= 0x40000u ||
                 (addr >= 0x00800000u && addr + n <= 0x00806040u) ||
                 (((addr >= 0x40000000u && addr + n <= 0x43000000u) ||
                   (addr >= 0xE0000000u && addr + n <= 0xE0100000u)) && !(addr & 3u) && !(n & 3u) && n <= 64u);
        if (ok && addr >= 0x40000000u) {
            uint32_t words[16];
            for (uint32_t i = 0; i < n / 4u; i++) words[i] = ((const volatile uint32_t *)addr)[i];
            reply(words, n);
            return;
        }
        if (ok) reply((const void *)addr, n);
    } else if (cmd[0] == 'W' || cmd[0] == 'O') {
        const char *p;
        uint32_t addr = hex(cmd + 1, &p);
        uint32_t v = *p == ',' ? hex(p + 1, &p) : 0;
        unsigned size = cmd[0] == 'W' ? 4u : 1u;
        if (addr >= 0x20000000u && addr + size <= 0x20008000u && !(addr & (size - 1u))) {
            if (size == 4) *(volatile uint32_t *)addr = v;
            else *(volatile uint8_t *)addr = (uint8_t)v;
        }
    }
}

static void receive(void) {
    unsigned n = ep_desc[EP_OUT].DeviceDescBank[0].PCKSIZE.bit.BYTE_COUNT;
    for (unsigned i = 0; i < n; i++) {
        char c = (char)rx_buf[i];
        if (c == '#') {
            command();
            cmd_len = 0;
        } else if (c == 'N' || c == '\r' || c == '\n') {
            /* sam-ba's "N#" (binary mode) and line ends: nothing to do */
        } else if (cmd_len < sizeof cmd - 1) {
            cmd[cmd_len++] = c;
        }
    }
    EP(EP_OUT).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
}

void USB_Handler(void) {
    uint16_t flags = USB->DEVICE.INTFLAG.reg;
#ifdef META_USB_DEBUG
    DBG(isr)++;
    DBG(intflag) = flags;
    DBG(ep0flag) = EP(0).EPINTFLAG.reg;
    if (flags & USB_DEVICE_INTFLAG_EORST) DBG(eorst)++;
#endif
    if (flags & USB_DEVICE_INTFLAG_EORST) {
        USB->DEVICE.INTFLAG.reg = USB_DEVICE_INTFLAG_EORST;
        configured = 0;
        /* The previous configuration's endpoints: off, flags and interrupts
         * cleared (a pending one would re-enter this handler forever). */
        for (unsigned ep = 1; ep < 4; ep++) {
            EP(ep).EPCFG.reg = 0;
            EP(ep).EPINTENCLR.reg = 0x7F;
            EP(ep).EPINTFLAG.reg = 0x7F;
        }
        memset(ep_desc, 0, sizeof ep_desc);
        EP(0).EPCFG.reg = USB_DEVICE_EPCFG_EPTYPE0(1) | USB_DEVICE_EPCFG_EPTYPE1(1);
        ep_desc[0].DeviceDescBank[0].ADDR.reg = (uint32_t)ep0_out;
        ep_desc[0].DeviceDescBank[0].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
        ep_desc[0].DeviceDescBank[1].ADDR.reg = (uint32_t)ep0_in;
        ep_desc[0].DeviceDescBank[1].PCKSIZE.reg = USB_DEVICE_PCKSIZE_SIZE(3);
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
        EP(0).EPINTENSET.reg = USB_DEVICE_EPINTENSET_RXSTP;
    }
    if (EP(0).EPINTFLAG.reg & USB_DEVICE_EPINTFLAG_RXSTP) {
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_STALLRQ0 | USB_DEVICE_EPSTATUSCLR_STALLRQ1;
        EP(0).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_RXSTP;
#ifdef META_USB_DEBUG
        DBG(setup)++;
        memcpy(meta_usb_dbg.last_setup, ep0_out, 8);
#endif
        setup_request();
        EP(0).EPSTATUSCLR.reg = USB_DEVICE_EPSTATUSCLR_BK0RDY;
    }
    if (EP(EP_OUT).EPINTFLAG.reg & USB_DEVICE_EPINTFLAG_TRCPT0) {
        EP(EP_OUT).EPINTFLAG.reg = USB_DEVICE_EPINTFLAG_TRCPT0;
        if (configured) receive();
    }
    /* Anything else that may be pending (flags without an enabled interrupt
     * do not call this handler): device flags other than EORST. */
    USB->DEVICE.INTFLAG.reg = (uint16_t)(USB->DEVICE.INTFLAG.reg & ~USB_DEVICE_INTFLAG_EORST);
}

void meta_usb_init(void) {
    /* D- PA24, D+ PA25: peripheral function G. */
    PORT->Group[0].PMUX[12].reg = PORT_PMUX_PMUXE_G | PORT_PMUX_PMUXO_G;
    PORT->Group[0].PINCFG[24].reg = PORT_PINCFG_PMUXEN;
    PORT->Group[0].PINCFG[25].reg = PORT_PINCFG_PMUXEN;

    PM->APBBMASK.reg |= PM_APBBMASK_USB;
    PM->AHBMASK.reg |= PM_AHBMASK_USB;
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(7) | GCLK_GENDIV_DIV(1);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(7) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_USB_Val) | GCLK_CLKCTRL_GEN_GCLK7 |
                                   GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}

    USB->DEVICE.CTRLA.reg = USB_CTRLA_SWRST;
    while (USB->DEVICE.SYNCBUSY.reg & USB_SYNCBUSY_SWRST) {}

    /* Pad calibration from the NVM software calibration row. */
    uint32_t fuses = *(const uint32_t *)(NVMCTRL_OTP4 + 4);
    uint32_t transn = (fuses & USB_FUSES_TRANSN_Msk) >> USB_FUSES_TRANSN_Pos;
    uint32_t transp = (fuses & USB_FUSES_TRANSP_Msk) >> USB_FUSES_TRANSP_Pos;
    uint32_t trim = (fuses & USB_FUSES_TRIM_Msk) >> USB_FUSES_TRIM_Pos;
    if (transn == 0x1F) transn = 5;
    if (transp == 0x1F) transp = 29;
    if (trim == 0x7) trim = 3;
    USB->DEVICE.PADCAL.reg = (uint16_t)(USB_PADCAL_TRANSN(transn) | USB_PADCAL_TRANSP(transp) | USB_PADCAL_TRIM(trim));

    memset(ep_desc, 0, sizeof ep_desc);
    USB->DEVICE.DESCADD.reg = (uint32_t)ep_desc;
    USB->DEVICE.QOSCTRL.reg = USB_QOSCTRL_CQOS(3) | USB_QOSCTRL_DQOS(3);
    USB->DEVICE.CTRLA.reg = USB_CTRLA_MODE_DEVICE | USB_CTRLA_RUNSTDBY;
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_SPDCONF_FS | USB_DEVICE_CTRLB_DETACH;
    USB->DEVICE.INTENSET.reg = USB_DEVICE_INTENSET_EORST;
    USB->DEVICE.CTRLA.reg = USB_CTRLA_MODE_DEVICE | USB_CTRLA_RUNSTDBY | USB_CTRLA_ENABLE;
    while (USB->DEVICE.SYNCBUSY.reg & USB_SYNCBUSY_ENABLE) {}
    /* Stay detached long enough for the host to see the device go: a reset
     * from the bootloader (sam-ba or the RESET button) reaches this a few
     * milliseconds later, and the host (measured: Linux behind a hub) then
     * kept the bootloader's device and never enumerated this one. */
    delay(200);
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_SPDCONF_FS; /* attach */

    NVIC_SetPriority(USB_IRQn, (1u << __NVIC_PRIO_BITS) - 1u);
    NVIC_EnableIRQ(USB_IRQn);
}

void meta_usb_stop(void) {
    NVIC_DisableIRQ(USB_IRQn);
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_DETACH;
    USB->DEVICE.CTRLA.reg = 0;
    while (USB->DEVICE.SYNCBUSY.reg & USB_SYNCBUSY_ENABLE) {}
    configured = 0;
}

void meta_usb_write(const void *data, unsigned n) {
    if (!configured || !(line_state & 1)) return; /* nobody listening */
    NVIC_DisableIRQ(USB_IRQn);
    reply(data, n);
    NVIC_EnableIRQ(USB_IRQn);
}

/* The META's bootloader (read from flash, 0x1964), on any reset but a
 * watchdog one, first checks 0x20007FFC: 0x3aecfd88 there starts sam-ba (the
 * Gamebuino launcher's "enter bootloader").  A watchdog reset starts the
 * launcher instead, and the Arduino double-tap magic is only honoured after
 * an external reset. */
void meta_reset_to_bootloader(void) {
    __disable_irq();
    USB->DEVICE.CTRLB.reg = USB_DEVICE_CTRLB_DETACH;
    *(volatile uint32_t *)0x20007FFCu = 0x3AECFD88u;
    NVIC_SystemReset();
}

#endif /* META_USB */
