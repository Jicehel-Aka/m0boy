/*
 * The option menu's palettes, shared by the consoles' display pipelines
 * (meta_display.h).
 *
 * The GBC entry reproduces the Game Boy Color boot ROM's colouring of DMG
 * games: when a non-color cartridge is booted, the ROM sums the 16 title
 * bytes, looks the checksum up in its compatibility table and loads one of
 * its palettes (three per game: OBJ0, OBJ1 and BG — the DMG's four shades
 * mapped to colours; sprites shared the BG palette here, as on the DMG).
 * Titles that share a checksum are told apart by their 4th letter, and
 * unknown titles (or a non-Nintendo licensee) get the boot ROM's default.
 * Tables transcribed from the SameBoy boot ROM reimplementation (LIJI32/
 * SameBoy, BootROMs/cgb_boot.asm), which matches the original BIOS.
 */
#include "meta_display.h"

/* Lightest shade first (0xRRGGBB). */
const uint32_t meta_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
    {0xFFFFFF, 0x7BFF31, 0x003A19, 0x000000}, /* GBC: filled by meta_palettes_gbc_auto */
};
const char *const meta_palette_names[META_PALETTE_COUNT] = {"Vert", "Gris", "Pocket", "Ambre", "Bleu", "GBC"};

/* ---- the boot ROM's palette machinery ------------------------------------- */

/* 32 base palettes, BGR555 (0BBGGGGRRRRR... i.e. bits 0-4 red), the colour
 * the boot ROM loads for one of the DMG's four shades, lightest first as
 * the games define them (a few games order them differently: kept as is). */
static const uint16_t cgb_base[32][4] = {
    {0x7FFF, 0x32BF, 0x00D0, 0x0000}, /*  0 */
    {0x639F, 0x4279, 0x15B0, 0x04CB}, /*  1 */
    {0x7FFF, 0x6E31, 0x454A, 0x0000}, /*  2 */
    {0x7FFF, 0x1BEF, 0x0200, 0x0000}, /*  3 Tetris */
    {0x7FFF, 0x421F, 0x1CF2, 0x0000}, /*  4 */
    {0x7FFF, 0x5294, 0x294A, 0x0000}, /*  5 */
    {0x7FFF, 0x03FF, 0x012F, 0x0000}, /*  6 */
    {0x7FFF, 0x03EF, 0x01D6, 0x0000}, /*  7 */
    {0x7FFF, 0x42B5, 0x3DC8, 0x0000}, /*  8 */
    {0x7E74, 0x03FF, 0x0180, 0x0000}, /*  9 */
    {0x67FF, 0x77AC, 0x1A13, 0x2D6B}, /* 10 */
    {0x7ED6, 0x4BFF, 0x2175, 0x0000}, /* 11 */
    {0x53FF, 0x4A5F, 0x7E52, 0x0000}, /* 12 */
    {0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0}, /* 13 */
    {0x03ED, 0x7FFF, 0x255F, 0x0000}, /* 14 */
    {0x036A, 0x021F, 0x03FF, 0x7FFF}, /* 15 */
    {0x7FFF, 0x01DF, 0x0112, 0x0000}, /* 16 */
    {0x231F, 0x035F, 0x00F2, 0x0009}, /* 17 */
    {0x7FFF, 0x03EA, 0x011F, 0x0000}, /* 18 */
    {0x299F, 0x001A, 0x000C, 0x0000}, /* 19 */
    {0x7FFF, 0x027F, 0x001F, 0x0000}, /* 20 */
    {0x7FFF, 0x03E0, 0x0206, 0x0120}, /* 21 */
    {0x7FFF, 0x7EEB, 0x001F, 0x7C00}, /* 22 */
    {0x7FFF, 0x3FFF, 0x7E00, 0x001F}, /* 23 */
    {0x7FFF, 0x03FF, 0x001F, 0x0000}, /* 24 */
    {0x03FF, 0x001F, 0x000C, 0x0000}, /* 25 */
    {0x7FFF, 0x033F, 0x0193, 0x0000}, /* 26 */
    {0x0000, 0x4200, 0x037F, 0x7FFF}, /* 27 */
    {0x7FFF, 0x7E8C, 0x7C00, 0x0000}, /* 28 */
    {0x7FFF, 0x1BEF, 0x6180, 0x0000}, /* 29 (the boot ROM's default) */
};

/* For each game (by title checksum): which of the boot ROM's palette
 * combinations it gets.  A combination is (OBJ0, OBJ1, BG) base palettes;
 * only BG reaches the DMG shades here. */
static const uint8_t cgb_title_checksums[] = {
    0x00, /* Default */
    0x88, /* ALLEY WAY */
    0x16, /* YAKUMAN */
    0x36, /* BASEBALL, (Game and Watch 2) */
    0xD1, /* TENNIS */
    0xDB, /* TETRIS */
    0xF2, /* QIX */
    0x3C, /* DR.MARIO */
    0x8C, /* RADARMISSION */
    0x92, /* F1RACE */
    0x3D, /* YOSSY NO TAMAGO */
    0x5C,
    0x58, /* X */
    0xC9, /* MARIOLAND2 */
    0x3E, /* YOSSY NO COOKIE */
    0x70, /* ZELDA */
    0x1D,
    0x59,
    0x69, /* TETRIS FLASH */
    0x19, /* DONKEY KONG */
    0x35, /* MARIO'S PICROSS */
    0xA8,
    0x14, /* POKEMON RED, (GAMEBOYCAMERA G) */
    0xAA, /* POKEMON GREEN */
    0x75, /* PICROSS 2 */
    0x95, /* YOSSY NO PANEPON */
    0x99, /* KIRAKIRA KIDS */
    0x34, /* GAMEBOY GALLERY */
    0x6F, /* POCKETCAMERA */
    0x15,
    0xFF, /* BALLOON KID */
    0x97, /* KINGOFTHEZOO */
    0x4B, /* DMG FOOTBALL */
    0x90, /* WORLD CUP */
    0x17, /* OTHELLO */
    0x10, /* SUPER RC PRO-AM */
    0x39, /* DYNABLASTER */
    0xF7, /* BOY AND BLOB GB2 */
    0xF6, /* MEGAMAN */
    0xA2, /* STAR WARS-NOA */
    0x49,
    0x4E, /* WAVERACE */
    0x43,
    0x68, /* LOLO2 */
    0xE0, /* YOSHI'S COOKIE */
    0x8B, /* MYSTIC QUEST */
    0xF0,
    0xCE, /* TOPRANKINGTENNIS */
    0x0C, /* MANSELL */
    0x29, /* MEGAMAN3 */
    0xE8, /* SPACE INVADERS */
    0xB7, /* GAME&WATCH */
    0x86, /* DONKEYKONGLAND95 */
    0x9A, /* ASTEROIDS/MISCMD */
    0x52, /* STREET FIGHTER 2 */
    0x01, /* DEFENDER/JOUST */
    0x9D, /* KILLERINSTINCT95 */
    0x71, /* TETRIS BLAST */
    0x9C, /* PINOCCHIO */
    0xBD,
    0x5D, /* BA.TOSHINDEN */
    0x6D, /* NETTOU KOF 95 */
    0x67,
    0x3F, /* TETRIS PLUS */
    0x6B, /* DONKEYKONGLAND 3 */
};
#define CGB_UNIQUE_COUNT \
    (sizeof(cgb_title_checksums) / sizeof(cgb_title_checksums[0]))
/* From here the same checksum can appear again: the 4th title letter tells
 * the games apart (cgb_dup_letters, aligned with these entries). */
static const uint8_t cgb_dup_checksums[] = {
    0xB3, /* ???[B]???????? */
    0x46, /* SUP[E]R MARIOLAND */
    0x28, /* GOL[F] */
    0xA5, /* SOL[A]RSTRIKER */
    0xC6, /* GBW[A]RS */
    0xD3, /* KAE[R]UNOTAMENI */
    0x27, /* ???[B]???????? */
    0x61, /* POK[E]MON BLUE */
    0x18, /* DON[K]EYKONGLAND */
    0x66, /* GAM[E]BOY GALLERY2 */
    0x6A, /* DON[K]EYKONGLAND 2 */
    0xBF, /* KID[ ]ICARUS */
    0x0D, /* TET[R]IS2 */
    0xF4, /* ???[-]???????? */
    0xB3, /* MOG[U]RANYA */
    0x46, /* ???[R]???????? */
    0x28, /* GAL[A]GA&GALAXIAN */
    0xA5, /* BT2[R]AGNAROKWORLD */
    0xC6, /* KEN[ ]GRIFFEY JR */
    0xD3, /* ???[I]???????? */
    0x27, /* MAG[N]ETIC SOCCER */
    0x61, /* VEG[A]S STAKES */
    0x18, /* ???[I]???????? */
    0x66, /* MIL[L]I/CENTI/PEDE */
    0x6A, /* MAR[I]O & YOSHI */
    0xBF, /* SOC[C]ER */
    0x0D, /* POK[E]BOM */
    0xF4, /* G&W[ ]GALLERY */
    0xB3, /* TET[R]IS ATTACK */
};
static const char cgb_dup_letters[] = "BEFAARBEKEK R-URAR INAILICE R";

/* BG palette per title, indexed like the checksum tables above. */
static const uint8_t cgb_bg_palette[] = {
    0,  /* Default */
    4,  /* ALLEY WAY */
    5,  /* YAKUMAN */
    35, /* BASEBALL, (Game and Watch 2) */
    34, /* TENNIS */
    3,  /* TETRIS */
    31, /* QIX */
    15, /* DR.MARIO */
    10, /* RADARMISSION */
    5,  /* F1RACE */
    19, /* YOSSY NO TAMAGO */
    36,
    7,  /* X */
    37, /* MARIOLAND2 */
    30, /* YOSSY NO COOKIE */
    44, /* ZELDA */
    21,
    32,
    31, /* TETRIS FLASH */
    20, /* DONKEY KONG */
    5,  /* MARIO'S PICROSS */
    33,
    13, /* POKEMON RED, (GAMEBOYCAMERA G) */
    14, /* POKEMON GREEN */
    5,  /* PICROSS 2 */
    29, /* YOSSY NO PANEPON */
    5,  /* KIRAKIRA KIDS */
    18, /* GAMEBOY GALLERY */
    9,  /* POCKETCAMERA */
    3,
    2,  /* BALLOON KID */
    26, /* KINGOFTHEZOO */
    25, /* DMG FOOTBALL */
    25, /* WORLD CUP */
    41, /* OTHELLO */
    42, /* SUPER RC PRO-AM */
    26, /* DYNABLASTER */
    45, /* BOY AND BLOB GB2 */
    42, /* MEGAMAN */
    45, /* STAR WARS-NOA */
    36,
    38, /* WAVERACE */
    26,
    42, /* LOLO2 */
    30, /* YOSHI'S COOKIE */
    41, /* MYSTIC QUEST */
    34,
    34, /* TOPRANKINGTENNIS */
    5,  /* MANSELL */
    42, /* MEGAMAN3 */
    6,  /* SPACE INVADERS */
    5,  /* GAME&WATCH */
    33, /* DONKEYKONGLAND95 */
    25, /* ASTEROIDS/MISCMD */
    42, /* STREET FIGHTER 2 */
    42, /* DEFENDER/JOUST */
    40, /* KILLERINSTINCT95 */
    2,  /* TETRIS BLAST */
    16, /* PINOCCHIO */
    25,
    42, /* BA.TOSHINDEN */
    42, /* NETTOU KOF 95 */
    5,
    0,  /* TETRIS PLUS */
    39, /* DONKEYKONGLAND 3 */
    36,
    22, /* SUPER MARIOLAND */
    25, /* GOLF */
    6,  /* SOLARSTRIKER */
    32, /* GBWARS */
    12, /* KAERUNOTAMENI */
    36,
    11, /* POKEMON BLUE */
    39, /* DONKEYKONGLAND */
    18, /* GAMEBOY GALLERY2 */
    39, /* DONKEYKONGLAND 2 */
    24, /* KID ICARUS */
    31, /* TETRIS2 */
    50,
    17, /* MOGURANYA */
    46,
    6,  /* GALAGA&GALAXIAN */
    27, /* BT2RAGNAROKWORLD */
    0,  /* KEN GRIFFEY JR */
    47,
    41, /* MAGNETIC SOCCER */
    41, /* VEGAS STAKES */
    0,
    0,  /* MILLI/CENTI/PEDE */
    19, /* MARIO & YOSHI */
    34, /* SOCCER */
    23, /* POKEBOM */
    18, /* G&W GALLERY */
    29, /* TETRIS ATTACK */
};

/* The boot ROM's palette combinations, as the BG byte offset into the
 * flattened base palettes (palette_comb n -> n*8; the boot ROM's four raw
 * combinations 22, 34, 35 and 36 point between two base palettes and read
 * across them: kept as is).  Only the BG third is used here; combo 0 is
 * what the boot ROM loads for titles it does not know. */
static const uint8_t cgb_comb_bg[] = {
    232, /*  0 (default: base 29) */
    144, /*  1, Right */
    160, /*  2 */
    192, /*  3, Down + A */
    72,  /*  4 */
    0,   /*  5, Up */
    216, /*  6, Right + B */
    40,  /*  7, Left + B */
    96,  /*  8, Down */
    208, /*  9 */
    64,  /* 10 */
    224, /* 11 */
    16,  /* 12 */
    32,  /* 13 */
    232, /* 14 */
    224, /* 15 */
    16,  /* 16 */
    64,  /* 17 */
    56,  /* 18 */
    144, /* 19 */
    160, /* 20 */
    72,  /* 21 */
    44,  /* 22, raw (base 5 lines 2-3, then base 6 lines 0-1) */
    16,  /* 23 */
    16,  /* 24 */
    24,  /* 25 */
    0,   /* 26 */
    0,   /* 27 */
    8,   /* 28, Up + B */
    144, /* 29 */
    160, /* 30 */
    192, /* 31 */
    64,  /* 32 */
    104, /* 33 */
    56,  /* 34, raw (base 7) */
    60,  /* 35, raw (base 7 words 2-3, then base 8 lines 0-1) */
    36,  /* 36, raw (base 4 words 2-3, then base 5 lines 0-1) */
    80,  /* 37 */
    224, /* 38 */
    16,  /* 39 */
    16,  /* 40, Left + A */
    24,  /* 41 */
    0,   /* 42 */
    32,  /* 43, Up + A */
    32,  /* 44 */
    0,   /* 45 */
    224, /* 46 */
    64,  /* 47 */
    224, /* 48, Left */
    48,  /* 49, Down + B */
    232, /* 50 */
};

static uint8_t gbc_checksum;      /* 0 when no title was given */
static const char *gbc_title;

void meta_palettes_set_title(const char *title, uint8_t checksum) {
    gbc_title = title;
    gbc_checksum = checksum;
}

static uint32_t cgb_rgb(uint16_t bgr555) {
    unsigned r = (bgr555 & 31) * 255 / 31;
    unsigned g = ((bgr555 >> 5) & 31) * 255 / 31;
    unsigned b = ((bgr555 >> 10) & 31) * 255 / 31;
    return r << 16 | g << 8 | b;
}

const uint32_t *meta_palettes_gbc_auto(void) {
    static uint32_t colours[4];
    const uint16_t *flat = &cgb_base[0][0];
    unsigned combo = 0; /* the boot ROM's default */
    if (gbc_title && gbc_checksum) {
        for (unsigned i = 0; i < CGB_UNIQUE_COUNT + sizeof(cgb_dup_checksums); i++) {
            uint8_t ck = i < CGB_UNIQUE_COUNT ? cgb_title_checksums[i]
                                              : cgb_dup_checksums[i - CGB_UNIQUE_COUNT];
            if (ck != gbc_checksum) continue;
            if (i >= CGB_UNIQUE_COUNT &&
                cgb_dup_letters[i - CGB_UNIQUE_COUNT] != gbc_title[3]) continue;
            combo = cgb_bg_palette[i];
            break;
        }
    }
    unsigned off = cgb_comb_bg[combo < sizeof(cgb_comb_bg) ? combo : 0];
    for (unsigned i = 0; i < 4; i++) colours[i] = cgb_rgb(flat[off / 2 + i]);
    return colours;
}
