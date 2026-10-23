/*
 * LPC11U68 registers used by the Pokitto frontend (offsets from the NXP
 * LPC11U6x user manual, UM10732).  Only what the frontend touches.
 */
#ifndef LPC11U6X_H
#define LPC11U6X_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG8(a) (*(volatile uint8_t *)(a))

/* ---- SYSCON ------------------------------------------------------------- */
#define SYSCON_BASE 0x40048000u
#define SYSCON_PRESETCTRL REG32(SYSCON_BASE + 0x004)
#define SYSCON_SYSPLLCTRL REG32(SYSCON_BASE + 0x008)
#define SYSCON_SYSPLLSTAT REG32(SYSCON_BASE + 0x00C)
#define SYSCON_SYSOSCCTRL REG32(SYSCON_BASE + 0x020)
#define SYSCON_SYSPLLCLKSEL REG32(SYSCON_BASE + 0x040)
#define SYSCON_SYSPLLCLKUEN REG32(SYSCON_BASE + 0x044)
#define SYSCON_MAINCLKSEL REG32(SYSCON_BASE + 0x070)
#define SYSCON_MAINCLKUEN REG32(SYSCON_BASE + 0x074)
#define SYSCON_SYSAHBCLKDIV REG32(SYSCON_BASE + 0x078)
#define SYSCON_SYSAHBCLKCTRL REG32(SYSCON_BASE + 0x080)
#define SYSCON_SSP0CLKDIV REG32(SYSCON_BASE + 0x094)
#define SYSCON_PDRUNCFG REG32(SYSCON_BASE + 0x238)

/* SYSAHBCLKCTRL bits */
#define AHB_GPIO (1u << 6)
#define AHB_CT32B0 (1u << 9)
#define AHB_SSP0 (1u << 11)
#define AHB_IOCON (1u << 16)
#define AHB_RAM1 (1u << 26)
#define AHB_USBRAM (1u << 27)
/* PRESETCTRL bits (1 = out of reset) */
#define PRESET_SSP0 (1u << 0)
/* PDRUNCFG: bits that may change, bits that must read as one */
#define PDRUN_VALID_BITS 0x000025FFu
#define PDRUN_RESERVED_ONE 0x0000C800u
#define PDRUN_SYSOSC (1u << 5)
#define PDRUN_SYSPLL (1u << 7)

/* ---- IOCON: PIOp_n at base + offset of the port + 4 * n ------------------ */
#define IOCON_BASE 0x40044000u
#define IOCON_PIO0(n) REG32(IOCON_BASE + 0x000 + 4u * (n))
#define IOCON_PIO1(n) REG32(IOCON_BASE + 0x060 + 4u * (n))
#define IOCON_PIO2(n) REG32(IOCON_BASE + 0x0F0 + 4u * (n))
#define IOCON_FUNC(f) ((uint32_t)(f))
#define IOCON_MODE_NONE (0u << 3)
#define IOCON_MODE_PULLDOWN (1u << 3)
#define IOCON_MODE_PULLUP (2u << 3)
#define IOCON_DIGITAL (1u << 7) /* ADMODE: digital (pins with an analog function) */
/* Function and pull of a pin, the other bits (digital mode, reserved ones)
 * kept as they are, like mbed's pin_function / pin_mode. */
#define IOCON_SET(reg, func, mode) ((reg) = ((reg) & ~0x1Fu) | (func) | (mode))

/* ---- GPIO ---------------------------------------------------------------- */
#define GPIO_BASE 0xA0000000u
#define GPIO_B(port, pin) REG8(GPIO_BASE + 0x0000 + 0x20u * (port) + (pin)) /* byte pin */
#define GPIO_DIR(port) REG32(GPIO_BASE + 0x2000 + 4u * (port))
#define GPIO_MASK(port) REG32(GPIO_BASE + 0x2080 + 4u * (port))
#define GPIO_PIN(port) REG32(GPIO_BASE + 0x2100 + 4u * (port))
#define GPIO_MPIN(port) REG32(GPIO_BASE + 0x2180 + 4u * (port))
#define GPIO_SET(port) REG32(GPIO_BASE + 0x2200 + 4u * (port))
#define GPIO_CLR(port) REG32(GPIO_BASE + 0x2280 + 4u * (port))
#define GPIO_PIN_INT_IENR REG32(0xA0004004u)
#define GPIO_PIN_INT_IENF REG32(0xA0004010u)

/* ---- CT32B0 (audio sample clock) ----------------------------------------- */
#define CT32B0_BASE 0x40014000u
#define CT32B0_IR REG32(CT32B0_BASE + 0x00)
#define CT32B0_TCR REG32(CT32B0_BASE + 0x04)
#define CT32B0_TC REG32(CT32B0_BASE + 0x08)
#define CT32B0_PR REG32(CT32B0_BASE + 0x0C)
#define CT32B0_MCR REG32(CT32B0_BASE + 0x14)
#define CT32B0_MR1 REG32(CT32B0_BASE + 0x1C)

/* ---- SSP0 (SD card) ------------------------------------------------------- */
#define SSP0_BASE 0x40040000u
#define SSP0_CR0 REG32(SSP0_BASE + 0x00)
#define SSP0_CR1 REG32(SSP0_BASE + 0x04)
#define SSP0_DR REG32(SSP0_BASE + 0x08)
#define SSP0_SR REG32(SSP0_BASE + 0x0C)
#define SSP0_CPSR REG32(SSP0_BASE + 0x10)
#define SSP_SR_TNF (1u << 1)
#define SSP_SR_RNE (1u << 2)
#define SSP_SR_BSY (1u << 4)

/* ---- Cortex-M0+ core ----------------------------------------------------- */
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define NVIC_ISER REG32(0xE000E100u)
#define NVIC_ICER REG32(0xE000E180u)
#define NVIC_ICPR REG32(0xE000E280u)
#define NVIC_IPR(n) REG32(0xE000E400u + 4u * ((n) >> 2))
#define SCB_ICSR REG32(0xE000ED04u)
#define SCB_VTOR REG32(0xE000ED08u)
#define SCB_ICSR_PENDSTSET (1u << 26)

#define IRQ_CT32B0 18

static inline void irq_disable(void) { __asm__ volatile("cpsid i" ::: "memory"); }
static inline void irq_enable(void) { __asm__ volatile("cpsie i" ::: "memory"); }

#endif
