#include "gbrt.h"
#include "ppu.h"
#include "audio.h"
#include "platform_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gbrt_debug.h"

/* ============================================================================
 * Definitions
 * ========================================================================== */

#define WRAM_BANK_SIZE 0x1000
#define VRAM_SIZE      0x2000
#define OAM_SIZE       0xA0
#define IO_SIZE        0x80
#define HRAM_SIZE      0x7F

/* ============================================================================
 * Globals
 * ========================================================================== */

bool gbrt_trace_enabled = false;
uint64_t gbrt_instruction_count = 0;
uint64_t gbrt_instruction_limit = 0;


/* ============================================================================
 * Context Management
 * ========================================================================== */

GBContext* gb_context_create(const GBConfig* config) {
    GBContext* ctx = (GBContext*)calloc(1, sizeof(GBContext));
    if (!ctx) return NULL;
    
    ctx->wram = (uint8_t*)calloc(1, WRAM_BANK_SIZE * 8);
    ctx->vram = (uint8_t*)calloc(1, VRAM_SIZE * 2);
    ctx->oam = (uint8_t*)calloc(1, OAM_SIZE);
    ctx->hram = (uint8_t*)calloc(1, HRAM_SIZE);
    ctx->io = (uint8_t*)calloc(1, IO_SIZE + 1);
    
    if (!ctx->wram || !ctx->vram || !ctx->oam || !ctx->hram || !ctx->io) {
        gb_context_destroy(ctx);
        return NULL;
    }
    
    GBPPU* ppu = (GBPPU*)calloc(1, sizeof(GBPPU));
    if (ppu) {
        ppu_init(ppu);
        ctx->ppu = ppu;
    }
    
    ctx->apu = gb_audio_create();
    gb_context_reset(ctx, true);
    (void)config;
    return ctx;
}

void gb_context_destroy(GBContext* ctx) {
    if (!ctx) return;
    free(ctx->wram);
    free(ctx->vram);
    free(ctx->oam);
    free(ctx->hram);
    free(ctx->io);
    if (ctx->ppu) free(ctx->ppu);
    if (ctx->apu) gb_audio_destroy(ctx->apu);
    if (ctx->rom) free(ctx->rom);
    free(ctx);
}

void gb_context_reset(GBContext* ctx, bool skip_bootrom) {
    if (skip_bootrom) {
        ctx->pc = 0x0100;
        ctx->sp = 0xFFFE;
        ctx->af = 0x01B0;
        ctx->bc = 0x0013;
        ctx->de = 0x00D8;
        ctx->hl = 0x014D;
        gb_unpack_flags(ctx);
        ctx->rom_bank = 1;
        ctx->wram_bank = 1;
        
        ctx->io[0x05] = 0x00; /* TIMA */
        ctx->io[0x06] = 0x00; /* TMA */
        ctx->io[0x07] = 0x00; /* TAC */
        ctx->io[0x10] = 0x80; /* NR10 */
        ctx->io[0x11] = 0xBF; /* NR11 */
        ctx->io[0x12] = 0xF3; /* NR12 */
        ctx->io[0x14] = 0xBF; /* NR14 */
        ctx->io[0x16] = 0x3F; /* NR21 */
        ctx->io[0x17] = 0x00; /* NR22 */
        ctx->io[0x19] = 0xBF; /* NR24 */
        ctx->io[0x1A] = 0x7F; /* NR30 */
        ctx->io[0x1B] = 0xFF; /* NR31 */
        ctx->io[0x1C] = 0x9F; /* NR32 */
        ctx->io[0x1E] = 0xBF; /* NR34 */
        ctx->io[0x20] = 0xFF; /* NR41 */
        ctx->io[0x21] = 0x00; /* NR42 */
        ctx->io[0x22] = 0x00; /* NR43 */
        ctx->io[0x23] = 0xBF; /* NR44 */
        ctx->io[0x24] = 0x77; /* NR50 */
        ctx->io[0x25] = 0xF3; /* NR51 */
        ctx->io[0x26] = 0xF1; /* NR52 */
        ctx->io[0x40] = 0x91; /* LCDC */
        ctx->io[0x42] = 0x00; /* SCY */
        ctx->io[0x43] = 0x00; /* SCX */
        ctx->io[0x45] = 0x00; /* LYC */
        ctx->io[0x47] = 0xFC; /* BGP */
        ctx->io[0x48] = 0xFF; /* OBP0 */
        ctx->io[0x49] = 0xFF; /* OBP1 */
        ctx->io[0x4A] = 0x00; /* WY */
        ctx->io[0x4B] = 0x00; /* WX */
        ctx->io[0x80] = 0x00; /* IE */
    }
}

bool gb_context_load_rom(GBContext* ctx, const uint8_t* data, size_t size) {
    if (ctx->rom) free(ctx->rom);
    ctx->rom = (uint8_t*)malloc(size);
    if (!ctx->rom) return false;
    memcpy(ctx->rom, data, size);
    ctx->rom_size = size;
    return true;
}

/* ============================================================================
 * Memory Access
 * ========================================================================== */

uint8_t gb_read8(GBContext* ctx, uint16_t addr) {
    if (addr < 0x4000) return ctx->rom[addr];
    if (addr < 0x8000) return ctx->rom[(ctx->rom_bank * 0x4000) + (addr - 0x4000)];
    if (addr < 0xA000) {
        if ((ctx->io[0x41] & 3) == 3) return 0xFF;
        return ctx->vram[(ctx->vram_bank * VRAM_SIZE) + (addr - 0x8000)];
    }
    if (addr < 0xC000) {
        if (!ctx->ram_enabled) return 0xFF;
        
        if (ctx->rtc_mode) {
             switch (ctx->rtc_reg) {
                case 0x08: return ctx->rtc.latched_s;
                case 0x09: return ctx->rtc.latched_m;
                case 0x0A: return ctx->rtc.latched_h;
                case 0x0B: return ctx->rtc.latched_dl;
                case 0x0C: return ctx->rtc.latched_dh;
                default: return 0xFF;
             }
        }
        
        if (ctx->eram) return ctx->eram[(ctx->ram_bank * 0x2000) + (addr - 0xA000)];
        return 0xFF;
    }
    if (addr < 0xD000) return ctx->wram[addr - 0xC000];
    if (addr < 0xE000) return ctx->wram[(ctx->wram_bank * WRAM_BANK_SIZE) + (addr - 0xD000)];
    if (addr < 0xFE00) return gb_read8(ctx, addr - 0x2000);
    if (addr < 0xFEA0) {
        uint8_t stat = ctx->io[0x41] & 3;
        if (stat == 2 || stat == 3) return 0xFF;
        return ctx->oam[addr - 0xFE00];
    }
    if (addr < 0xFF00) return 0xFF;
    if (addr < 0xFF80) {
        if (addr == 0xFF00) {
             // DBG_GENERAL("Reading JOYP 0xFF00");
             uint8_t joyp = ctx->io[0x00];
             // Bits 6-7 always 1. Bits 4-5 return what was written.
             uint8_t res = 0xC0 | (joyp & 0x30) | 0x0F;
             if (!(joyp & 0x10)) res &= g_joypad_dpad;
             if (!(joyp & 0x20)) res &= g_joypad_buttons;
             return res;
        }
        if (addr == 0xFF04) return (uint8_t)(ctx->div_counter >> 8);
        if (addr >= 0xFF40 && addr <= 0xFF4B) return ppu_read_register((GBPPU*)ctx->ppu, addr);
        if (addr >= 0xFF10 && addr <= 0xFF3F) return gb_audio_read(ctx, addr);
        return ctx->io[addr - 0xFF00];
    }
    if (addr < 0xFFFF) return ctx->hram[addr - 0xFF80];
    if (addr == 0xFFFF) return ctx->io[0x80];
    return 0xFF;
}

void gb_write8(GBContext* ctx, uint16_t addr, uint8_t value) {
    /* MBC Write Handling */
    if (addr < 0x8000) {
        /* MBC3 (0x0F, 0x10, 0x11, 0x12, 0x13) */
        if (ctx->mbc_type >= 0x0F && ctx->mbc_type <= 0x13) {
            if (addr < 0x2000) {
                /* RAM/RTC Enable */
                ctx->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x4000) {
                /* ROM Bank Number (1-127) */
                ctx->rom_bank = value & 0x7F;
                if (ctx->rom_bank == 0) ctx->rom_bank = 1;
            } else if (addr < 0x6000) {
                /* RAM Bank Number or RTC Register Select */
                if (value <= 0x03) {
                    ctx->rtc_mode = 0;
                    ctx->ram_bank = value;
                } else if (value >= 0x08 && value <= 0x0C) {
                    ctx->rtc_mode = 1;
                    ctx->rtc_reg = value;
                }
            } else if (addr < 0x8000) {
                /* Latch Clock Data */
                if (ctx->rtc.latch_state == 0 && value == 0) {
                    ctx->rtc.latch_state = 1;
                } else if (ctx->rtc.latch_state == 1 && value == 1) {
                    ctx->rtc.latch_state = 0;
                    /* Latch current time */
                    ctx->rtc.latched_s = ctx->rtc.s;
                    ctx->rtc.latched_m = ctx->rtc.m;
                    ctx->rtc.latched_h = ctx->rtc.h;
                    ctx->rtc.latched_dl = ctx->rtc.dl;
                    ctx->rtc.latched_dh = ctx->rtc.dh;
                } else {
                    ctx->rtc.latch_state = 0;
                }
            }
        }
        /* Default / MBC1 behavior for others (simplified) */
        else {
            if (addr >= 0x2000 && addr <= 0x3FFF) {
                ctx->rom_bank = value & 0x1F;
                if (ctx->rom_bank == 0) ctx->rom_bank = 1;
            }
        }
        return;
    }
    if (addr < 0xA000) {
        /* VRAM Write - Check STAT mode 3 */
        if ((ctx->io[0x41] & 3) == 3) return;
        
        ctx->vram[(ctx->vram_bank * VRAM_SIZE) + (addr - 0x8000)] = value;
        return;
    }
    if (addr < 0xC000) {
        /* External RAM / RTC Write */
        if (!ctx->ram_enabled) return;
        
        if (ctx->rtc_mode) {
            /* RTC Register Write */
            switch (ctx->rtc_reg) {
                case 0x08: ctx->rtc.s = value % 60; break;
                case 0x09: ctx->rtc.m = value % 60; break;
                case 0x0A: ctx->rtc.h = value % 24; break;
                case 0x0B: ctx->rtc.dl = value; break;
                case 0x0C: 
                    ctx->rtc.dh = value; 
                    ctx->rtc.active = !(value & 0x40); /* Bit 6 is Halt */
                    break;
            }
        } else if (ctx->eram) {
            /* SRAM Write */
            ctx->eram[(ctx->ram_bank * 0x2000) + (addr - 0xA000)] = value;
        }
        return;
    }
    if (addr < 0xD000) { ctx->wram[addr - 0xC000] = value; return; }
    if (addr < 0xE000) { ctx->wram[(ctx->wram_bank * WRAM_BANK_SIZE) + (addr - 0xD000)] = value; return; }
    if (addr < 0xFE00) { gb_write8(ctx, addr - 0x2000, value); return; }
    if (addr < 0xFEA0) { 
        /* OAM Write - Check STAT mode 2 or 3 */
        uint8_t stat = ctx->io[0x41] & 3;
        if (stat == 2 || stat == 3) return;
        
        ctx->oam[addr - 0xFE00] = value; 
        return; 
    }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) {
        if (addr >= 0xFF40 && addr <= 0xFF4B) { ppu_write_register((GBPPU*)ctx->ppu, ctx, addr, value); return; }
        if (addr >= 0xFF10 && addr <= 0xFF3F) { gb_audio_write(ctx, addr, value); return; }
        if (addr == 0xFF04) { 
            ctx->div_counter = 0; 
            ctx->io[0x04] = 0; /* Update register view immediately */
            if (ctx->apu) gb_audio_div_reset(ctx->apu);
            return; 
        }
        if (addr == 0xFF46) {
             uint16_t src = value << 8;
             for (int i=0; i<0xA0; i++) ctx->oam[i] = gb_read8(ctx, src + i);
             return;
        }
        if (addr == 0xFF02 && (value & 0x80)) {
            /* printf("%c", ctx->io[0x01]); fflush(stdout); */
            ctx->io[0x0F] |= 0x08;
        }
        ctx->io[addr - 0xFF00] = value;
        return;
    }
    if (addr < 0xFFFF) { 
        // if (addr >= 0xFF80 && addr <= 0xFF8F) {
        //      DBG_GENERAL("Writing to HRAM[%04X]: %02X", addr, value);
        // }
        ctx->hram[addr - 0xFF80] = value; return; 
    }
    if (addr == 0xFFFF) { ctx->io[0x80] = value; return; }
}

uint16_t gb_read16(GBContext* ctx, uint16_t addr) {
    return (uint16_t)gb_read8(ctx, addr) | ((uint16_t)gb_read8(ctx, addr + 1) << 8);
}

void gb_write16(GBContext* ctx, uint16_t addr, uint16_t value) {
    gb_write8(ctx, addr, value & 0xFF);
    gb_write8(ctx, addr + 1, value >> 8);
}

void gb_push16(GBContext* ctx, uint16_t value) {
    ctx->sp -= 2;
    gb_write16(ctx, ctx->sp, value);
}

uint16_t gb_pop16(GBContext* ctx) {
    uint16_t val = gb_read16(ctx, ctx->sp);
    ctx->sp += 2;
    return val;
}

/* ============================================================================
 * ALU
 * ========================================================================== */

void gb_add8(GBContext* ctx, uint8_t value) {
    uint32_t res = (uint32_t)ctx->a + value;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F)) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_adc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    uint32_t res = (uint32_t)ctx->a + value + carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F) + carry) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_sub8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
    ctx->a -= value;
}
void gb_sbc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    int res = (int)ctx->a - (int)value - carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 1;
    ctx->f_h = ((int)(ctx->a & 0x0F) - (int)(value & 0x0F) - (int)carry) < 0;
    ctx->f_c = res < 0;
    ctx->a = (uint8_t)res;
}
void gb_and8(GBContext* ctx, uint8_t value) { ctx->a &= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 1; ctx->f_c = 0; }
void gb_or8(GBContext* ctx, uint8_t value) { ctx->a |= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_xor8(GBContext* ctx, uint8_t value) { ctx->a ^= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_cp8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
}
uint8_t gb_inc8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0x0F;
    val++;
    ctx->f_z = val == 0;
    ctx->f_n = 0;
    return val;
}
uint8_t gb_dec8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0;
    val--;
    ctx->f_z = val == 0;
    ctx->f_n = 1;
    return val;
}
void gb_add16(GBContext* ctx, uint16_t val) {
    uint32_t res = (uint32_t)ctx->hl + val;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->hl & 0x0FFF) + (val & 0x0FFF)) > 0x0FFF;
    ctx->f_c = res > 0xFFFF;
    ctx->hl = (uint16_t)res;
}
void gb_add_sp(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->sp += off;
}
void gb_ld_hl_sp_n(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->hl = ctx->sp + off;
}

uint8_t gb_rlc(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v = (v << 1) | ctx->f_c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rrc(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (v >> 1) | (ctx->f_c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rl(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v >> 7; v = (v << 1) | c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rr(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v & 1; v = (v >> 1) | (c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sla(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v <<= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sra(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (uint8_t)((int8_t)v >> 1); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_swap(GBContext* ctx, uint8_t v) { v = (uint8_t)((v << 4) | (v >> 4)); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; return v; }
uint8_t gb_srl(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v >>= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
void gb_bit(GBContext* ctx, uint8_t bit, uint8_t v) { ctx->f_z = !(v & (1 << bit)); ctx->f_n = 0; ctx->f_h = 1; }

void gb_rlca(GBContext* ctx) { ctx->a = gb_rlc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rrca(GBContext* ctx) { ctx->a = gb_rrc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rla(GBContext* ctx) { ctx->a = gb_rl(ctx, ctx->a); ctx->f_z = 0; }
void gb_rra(GBContext* ctx) { ctx->a = gb_rr(ctx, ctx->a); ctx->f_z = 0; }

void gb_daa(GBContext* ctx) {
   int a = ctx->a;
   if (!ctx->f_n) {
       if (ctx->f_h || (a & 0xF) > 9) a += 0x06;
       if (ctx->f_c || a > 0x9F) a += 0x60;
   } else {
       if (ctx->f_h) a = (a - 6) & 0xFF;
       if (ctx->f_c) a -= 0x60;
   }
   
   ctx->f_h = 0;
   if ((a & 0x100) == 0x100) ctx->f_c = 1;
   
   a &= 0xFF;
   ctx->f_z = (a == 0);
   ctx->a = (uint8_t)a;
}

/* ============================================================================
 * Control Flow helpers
 * ========================================================================== */

void gb_ret(GBContext* ctx) { ctx->pc = gb_pop16(ctx); }
void gbrt_jump_hl(GBContext* ctx) { ctx->pc = ctx->hl; }
void gb_rst(GBContext* ctx, uint8_t vec) { gb_push16(ctx, ctx->pc); ctx->pc = vec; }

__attribute__((weak)) void gb_dispatch(GBContext* ctx, uint16_t addr) { ctx->pc = addr; gb_interpret(ctx, addr); }
__attribute__((weak)) void gb_dispatch_call(GBContext* ctx, uint16_t addr) { ctx->pc = addr; }

/* ============================================================================
 * Timing & Hardware Sync
 * ========================================================================== */

static inline void gb_sync(GBContext* ctx) {
    uint32_t current = ctx->cycles;
    uint32_t delta = current - ctx->last_sync_cycles;
    if (delta > 0) {
        ctx->last_sync_cycles = current;
        if (ctx->ppu) ppu_tick((GBPPU*)ctx->ppu, ctx, delta);
    }
}

void gb_add_cycles(GBContext* ctx, uint32_t cycles) {
    ctx->cycles += cycles;
    ctx->frame_cycles += cycles;
}



static void gb_rtc_tick(GBContext* ctx, uint32_t cycles) {
    if (!ctx->rtc.active) return;
    
    /* Update RTC time */
    ctx->rtc.last_time += cycles;
    while (ctx->rtc.last_time >= 4194304) { /* 1 second at 4.194304 MHz */
        ctx->rtc.last_time -= 4194304;
        
        ctx->rtc.s++;
        if (ctx->rtc.s >= 60) {
            ctx->rtc.s = 0;
            ctx->rtc.m++;
            if (ctx->rtc.m >= 60) {
                ctx->rtc.m = 0;
                ctx->rtc.h++;
                if (ctx->rtc.h >= 24) {
                    ctx->rtc.h = 0;
                    uint16_t d = ctx->rtc.dl | ((ctx->rtc.dh & 1) << 8);
                    d++;
                    ctx->rtc.dl = d & 0xFF;
                    if (d > 0x1FF) {
                        ctx->rtc.dh |= 0x80; /* Overflow */
                        ctx->rtc.dh &= 0xFE; /* Clear 9th bit */
                    } else {
                        ctx->rtc.dh = (ctx->rtc.dh & 0xFE) | ((d >> 8) & 1);
                    }
                }
            }
        }
    }
}

void gb_tick(GBContext* ctx, uint32_t cycles) {
    static uint32_t last_log = 0;
    
    // Check limit
    if (gbrt_instruction_limit > 0) {
        gbrt_instruction_count++;
        if (gbrt_instruction_count >= gbrt_instruction_limit) {
            printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
            exit(0);
        }
    }

    if (gbrt_trace_enabled && ctx->cycles - last_log >= 10000) {
        last_log = ctx->cycles;
        fprintf(stderr, "[TICK] Cycles: %u, PC: 0x%04X, IME: %d, IF: 0x%02X, IE: 0x%02X\n", 
                ctx->cycles, ctx->pc, ctx->ime, ctx->io[0x0F], ctx->io[0x80]);
    }
    gb_add_cycles(ctx, cycles);
    
    /* RTC Tick */
    gb_rtc_tick(ctx, cycles);

    ctx->div_counter += cycles;
    ctx->io[0x04] = (uint8_t)(ctx->div_counter >> 8);
    
    uint8_t tac = ctx->io[0x07];
    if (tac & 0x04) {
        uint32_t thr = 1024;
        switch (tac & 3) { case 1: thr = 16; break; case 2: thr = 64; break; case 3: thr = 256; break; }
        ctx->timer_counter += cycles;
        while (ctx->timer_counter >= thr) {
            ctx->timer_counter -= thr;
            if (ctx->io[0x05] == 0xFF) { ctx->io[0x05] = ctx->io[0x06]; ctx->io[0x0F] |= 0x04; }
            else ctx->io[0x05]++;
        }
    }
    
    if ((ctx->cycles & 0xFF) < cycles || (ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1F))) {
        gb_sync(ctx);
        if (ctx->frame_done || (ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1F))) ctx->stopped = 1;
    }
    if (ctx->apu) gb_audio_step(ctx, cycles);
    if (ctx->ime_pending) { ctx->ime = 1; ctx->ime_pending = 0; }
}

void gb_handle_interrupts(GBContext* ctx) {
    if (!ctx->ime) return;
    uint8_t if_reg = ctx->io[0x0F];
    uint8_t ie_reg = ctx->io[0x80];
    uint8_t pending = if_reg & ie_reg & 0x1F;
    if (pending) {
        ctx->ime = 0; ctx->halted = 0;
        uint16_t vec = 0; uint8_t bit = 0;
        if (pending & 0x01) { vec = 0x0040; bit = 0x01; }
        else if (pending & 0x02) { vec = 0x0048; bit = 0x02; }
        else if (pending & 0x04) { vec = 0x0050; bit = 0x04; }
        else if (pending & 0x08) { vec = 0x0058; bit = 0x08; }
        else if (pending & 0x10) { vec = 0x0060; bit = 0x10; }
        if (vec) {
            ctx->io[0x0F] &= ~bit;
            gb_push16(ctx, ctx->pc);
            ctx->pc = vec;
            ctx->stopped = 1;
        }
    }
}

/* ============================================================================
 * Execution
 * ========================================================================== */

uint32_t gb_run_frame(GBContext* ctx) {
    gb_reset_frame(ctx);
    uint32_t start = ctx->cycles;
    
    static int fcount = 0;
    fcount++;
    if (fcount % 60 == 0) {
        fprintf(stderr, "[FRAME] Frame %d, Cycles: %u\n", fcount, ctx->cycles);
    }

    while (!ctx->frame_done) {
        gb_handle_interrupts(ctx);
        
        /* Check for HALT exit condition (even if IME=0) */
        if (ctx->halted) {
             if (ctx->io[0x0F] & ctx->io[0x80] & 0x1F) {
                 ctx->halted = 0;
             }
        }
        
        ctx->stopped = 0;
        if (ctx->halted) gb_tick(ctx, 4);
        else gb_step(ctx);
        gb_sync(ctx);
    }
    return ctx->cycles - start;
}

uint32_t gb_step(GBContext* ctx) {
    if (gbrt_instruction_limit > 0 && ++gbrt_instruction_count >= gbrt_instruction_limit) {
        printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
        exit(0);
    }
    uint32_t start = ctx->cycles;
    gb_dispatch(ctx, ctx->pc);
    return ctx->cycles - start;
}

void gb_reset_frame(GBContext* ctx) {
    ctx->frame_done = 0;
    ctx->frame_cycles = 0;
    if (ctx->ppu) ppu_clear_frame_ready((GBPPU*)ctx->ppu);
}

const uint32_t* gb_get_framebuffer(GBContext* ctx) {
    if (ctx->ppu) return ppu_get_framebuffer((GBPPU*)ctx->ppu);
    return NULL;
}

void gb_halt(GBContext* ctx) { ctx->halted = 1; }
void gb_stop(GBContext* ctx) { ctx->stopped = 1; }
bool gb_frame_complete(GBContext* ctx) { return ctx->frame_done != 0; }

void gb_set_platform_callbacks(GBContext* ctx, const GBPlatformCallbacks* c) {
    if (ctx && c) {
        ctx->callbacks = *c;
    }
}

void gb_audio_callback(GBContext* ctx, int16_t l, int16_t r) {
    if (ctx && ctx->callbacks.on_audio_sample) {
        ctx->callbacks.on_audio_sample(ctx, l, r);
    }
}
