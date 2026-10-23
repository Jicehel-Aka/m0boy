/* Constant DMG-line layouts, used with META_DISPLAY_COMPACT. Dropped lines
 * use row 0 (never read). Every expression folds at compile time. */
#ifndef META_DISPLAY_LAYOUT_H
#define META_DISPLAY_LAYOUT_H
#define DL_KIND(m, y) ((m) == META_MODE_CROP ? ((y) < 8 || (y) >= 136 ? L_DROP : L_ROW) : \
    (m) == META_MODE_SKIP ? ((y) % 9 == 8 ? L_DROP : L_ROW) : \
    (m) == META_MODE_TILE ? (((y) & 7) == 7 && (y) != 63 && (y) != 95 ? L_DROP : L_ROW) : \
    (m) == META_MODE_TBLEND ? ((y) == 143 ? L_DROP : \
        (((y) & 7) == 7 && (y) != 63 && (y) != 95 ? L_HOLD : \
         ((y) & 7) == 0 && (y) && (y) != 64 && (y) != 96 ? L_MERGE : L_ROW)) : \
    (m) == META_MODE_SMOOTH ? L_ROW : ((y) % 9 == 7 ? L_HOLD : (y) % 9 == 8 ? L_MERGE : L_ROW))
#define DL_ROW(m, y) (DL_KIND(m, y) == L_DROP ? 0 : \
    (m) == META_MODE_CROP ? (y) - 8 : \
    (m) == META_MODE_SMOOTH ? (y) : \
    (m) == META_MODE_TILE ? (y) - (y) / 8 + ((y) > 63) + ((y) > 95) : \
    (m) == META_MODE_TBLEND ? (y) - (y) / 8 + ((y) >= 64) + ((y) >= 96) : \
    (m) == META_MODE_SKIP ? (y) - (y) / 9 : (y) - (y) / 9 - ((y) % 9 == 8))
#define DL_8(f, m, b) f(m, b), f(m, (b)+1), f(m, (b)+2), f(m, (b)+3), \
    f(m, (b)+4), f(m, (b)+5), f(m, (b)+6), f(m, (b)+7)
#define DL_144(f, m) DL_8(f,m,0), DL_8(f,m,8), DL_8(f,m,16), DL_8(f,m,24), \
    DL_8(f,m,32), DL_8(f,m,40), DL_8(f,m,48), DL_8(f,m,56), DL_8(f,m,64), \
    DL_8(f,m,72), DL_8(f,m,80), DL_8(f,m,88), DL_8(f,m,96), DL_8(f,m,104), \
    DL_8(f,m,112), DL_8(f,m,120), DL_8(f,m,128), DL_8(f,m,136)
#define DL_MODE(m) {{DL_144(DL_KIND, m)}, {DL_144(DL_ROW, m)}}
static const uint8_t display_layout[META_MODE_COUNT][2][144] = {
    DL_MODE(META_MODE_SMOOTH), DL_MODE(META_MODE_TILE), DL_MODE(META_MODE_BLEND),
    DL_MODE(META_MODE_DARK), DL_MODE(META_MODE_CROP), DL_MODE(META_MODE_SKIP),
    DL_MODE(META_MODE_TBLEND)
};
#undef DL_MODE
#undef DL_144
#undef DL_8
#undef DL_ROW
#undef DL_KIND
#endif
