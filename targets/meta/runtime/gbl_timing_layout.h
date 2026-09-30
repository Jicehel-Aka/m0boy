/*
 * Field offsets shared by the C runtime and gbl_timing_asm.S: the PPU state
 * structure (gbl_ppu.c), the timer state (gbl_core.c) and gbl_cpu_t
 * (gbl.h).  Every one is checked with _Static_assert, so a layout change
 * fails to compile instead of breaking the assembly.
 */
#ifndef GBL_TIMING_LAYOUT_H
#define GBL_TIMING_LAYOUT_H

#define GBL_P_LINE_START 0
#define GBL_P_OFF_START 4
#define GBL_P_NEXT_STAGE_OFF 8
#define GBL_P_LY 12
#define GBL_P_STAGE 13
#define GBL_P_MODE 14
#define GBL_P_STAT_LINE 15
#define GBL_P_WIN_LINE 16
#define GBL_P_LCD_ON 17
#define GBL_P_MOVED 18
#define GBL_P_STAT_MODES 19
#define GBL_P_LYC_LINE 20
#define GBL_P_EVT_DIRTY 25
#define GBL_P_DEV_NEXT 28

#define GBL_T_DIV_BASE 0
#define GBL_T_TIMA_SYNC 4
#define GBL_T_SERIAL_BUSY 12
#define GBL_T_CORE_HAS_NEXT 13
#define GBL_T_CORE_DIRTY 14
#define GBL_T_CORE_NEXT 16

#define GBL_GBR_IME 15
#define GBL_GBR_CYC 20
#define GBL_GBR_NEXT_EVT 24

#endif
