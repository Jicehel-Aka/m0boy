/* Minimal read-only FAT16/FAT32 file lookup (see meta_fat.h). */
#include "meta_fat.h"

#include <string.h>

#define MAX_SUBDIRS 32

typedef struct {
    meta_fat_read_fn rd;
    uint8_t *buf;
    uint32_t fat_lba, root_lba, root_sectors, data_lba, root_cluster;
    uint32_t spc; /* sectors per cluster */
    uint8_t fat32;
} volume;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int is_fat_boot(const uint8_t *b) {
    return (b[0] == 0xEB || b[0] == 0xE9) && u16(b + 0x0B) == 512 && b[0x0D] &&
           !(b[0x0D] & (b[0x0D] - 1)) && (b[0x10] == 1 || b[0x10] == 2);
}

static int mount(volume *v) {
    uint8_t *b = v->buf;
    if (!v->rd(0, b)) return META_FAT_IO;
    if (b[510] != 0x55 || b[511] != 0xAA) return META_FAT_NO_FS;
    uint32_t part = 0;
    if (!is_fat_boot(b)) {
        /* MBR: first FAT partition. */
        int found = 0;
        for (int i = 0; i < 4 && !found; i++) {
            const uint8_t *e = b + 0x1BE + 16 * i;
            uint8_t t = e[4];
            if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E) {
                part = u32(e + 8);
                found = 1;
            }
        }
        if (!found) return META_FAT_NO_FS;
        if (!v->rd(part, b)) return META_FAT_IO;
        if (!is_fat_boot(b)) return META_FAT_NO_FS;
    }
    uint32_t spc = b[0x0D], rsvd = u16(b + 0x0E), nfats = b[0x10], root_ent = u16(b + 0x11);
    uint32_t total = u16(b + 0x13) ? u16(b + 0x13) : u32(b + 0x20);
    uint32_t fatsz = u16(b + 0x16) ? u16(b + 0x16) : u32(b + 0x24);
    uint32_t root_secs = (root_ent * 32u + 511u) / 512u;
    v->spc = spc;
    v->fat_lba = part + rsvd;
    v->root_lba = v->fat_lba + nfats * fatsz;
    v->root_sectors = root_secs;
    v->data_lba = v->root_lba + root_secs;
    uint32_t meta = rsvd + nfats * fatsz + root_secs;
    if (total <= meta) return META_FAT_NO_FS;
    uint32_t clusters = (total - meta) / spc;
    if (clusters < 4085) return META_FAT_NO_FS; /* FAT12 */
    v->fat32 = clusters >= 65525;
    v->root_cluster = v->fat32 ? u32(b + 0x2C) : 0;
    return META_FAT_OK;
}

static uint32_t cluster_lba(const volume *v, uint32_t c) { return v->data_lba + (c - 2u) * v->spc; }

/* Next cluster in the chain, 0 at the end, 0xFFFFFFFF on a read error. */
static uint32_t next_cluster(volume *v, uint32_t c) {
    uint32_t off = v->fat32 ? c * 4u : c * 2u;
    if (!v->rd(v->fat_lba + off / 512u, v->buf)) return 0xFFFFFFFFu;
    if (v->fat32) {
        uint32_t n = u32(v->buf + off % 512u) & 0x0FFFFFFFu;
        return (n < 2 || n >= 0x0FFFFFF8u) ? 0 : n;
    }
    uint32_t n = u16(v->buf + off % 512u);
    return (n < 2 || n >= 0xFFF8u) ? 0 : n;
}

/* Scan one directory (cluster 0 = FAT16 root).  Returns 1 when name83 was
 * found (cluster and size filled), 0 otherwise, <0 on error; subdirectories
 * are collected into subdirs when it is not NULL. */
static int scan_dir(volume *v, uint32_t cluster, const char *name83, uint32_t *file_cluster,
                    uint32_t *file_size, uint32_t *subdirs, int *n_subdirs) {
    uint32_t sector = 0;
    for (;;) {
        uint32_t lba;
        if (cluster == 0) {
            if (sector >= v->root_sectors) return 0;
            lba = v->root_lba + sector;
        } else {
            if (sector == v->spc) {
                cluster = next_cluster(v, cluster);
                if (cluster == 0xFFFFFFFFu) return META_FAT_IO;
                if (cluster == 0) return 0;
                sector = 0;
            }
            lba = cluster_lba(v, cluster) + sector;
        }
        if (!v->rd(lba, v->buf)) return META_FAT_IO;
        for (int i = 0; i < 16; i++) {
            const uint8_t *e = v->buf + 32 * i;
            if (e[0] == 0x00) return 0;
            if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
            uint32_t first = (uint32_t)u16(e + 20) << 16 | u16(e + 26);
            if (e[11] & 0x10) {
                if (subdirs && e[0] != '.' && *n_subdirs < MAX_SUBDIRS && first >= 2)
                    subdirs[(*n_subdirs)++] = first;
                continue;
            }
            if (!memcmp(e, name83, 11)) {
                *file_cluster = first;
                *file_size = u32(e + 28);
                return 1;
            }
        }
        sector++;
    }
}

int meta_fat_find(meta_fat_read_fn rd, uint8_t *buf, const char *name83, meta_extent *ext,
                  int max_ext, int *n_ext, uint32_t *size) {
    volume v;
    memset(&v, 0, sizeof(v));
    v.rd = rd;
    v.buf = buf;
    int r = mount(&v);
    if (r) return r;
    uint32_t subdirs[MAX_SUBDIRS], first = 0;
    int n_sub = 0;
    r = scan_dir(&v, v.root_cluster, name83, &first, size, subdirs, &n_sub);
    for (int i = 0; r == 0 && i < n_sub; i++) r = scan_dir(&v, subdirs[i], name83, &first, size, 0, 0);
    if (r < 0) return r;
    if (r == 0) return META_FAT_NOT_FOUND;

    /* Cluster chain -> runs of consecutive sectors. */
    uint32_t need = (*size + 511u) / 512u, have = 0;
    int n = 0;
    uint32_t c = first;
    while (have < need) {
        if (c < 2) return META_FAT_NO_FS; /* chain shorter than the file */
        uint32_t lba = cluster_lba(&v, c);
        if (n && ext[n - 1].lba + ext[n - 1].sectors == lba) {
            ext[n - 1].sectors += v.spc;
        } else {
            if (n == max_ext) return META_FAT_FRAGMENTED;
            ext[n].lba = lba;
            ext[n].sectors = v.spc;
            n++;
        }
        have += v.spc;
        if (have >= need) break;
        c = next_cluster(&v, c);
        if (c == 0xFFFFFFFFu) return META_FAT_IO;
    }
    *n_ext = n;
    return META_FAT_OK;
}

uint32_t meta_fat_lba(const meta_extent *ext, int n_ext, uint32_t sector) {
    for (int i = 0; i < n_ext; i++) {
        if (sector < ext[i].sectors) return ext[i].lba + sector;
        sector -= ext[i].sectors;
    }
    return 0;
}
