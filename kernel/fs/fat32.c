// FAT32 filesystem driver (read + write). Owner: fs agent.
// - Superfloppy volumes or the first FAT32 partition of an MBR disk.
// - Long file names for reading and writing (short 8.3 alias with ~N tail).
// - Every FAT update is written to all FAT copies. FSInfo hints are invalidated
//   on the first modification (free count = unknown), which is always valid.
// All per-volume state lives in struct fat32 (one lock per volume later).
#include "fs_internal.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"
#include "rtc.h"

#define SECTOR      512
#define FAT_EOC     0x0ffffff8u
#define FAT_MASK    0x0fffffffu
#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0f
#define LFN_MAX_ENTRIES 20

struct fat32 {
    struct blockdev *bd;
    uint64_t part_lba;
    uint32_t spc;                 // sectors per cluster
    uint32_t cluster_bytes;
    uint32_t nfats;
    uint32_t fat_size;            // sectors per FAT
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t cluster_count;       // valid clusters are 2 .. cluster_count + 1
    uint32_t fsinfo_sector;       // relative to part_lba, 0 = none
    uint32_t alloc_hint;
    bool fsinfo_invalidated;
    uint64_t fat_cache_lba;
    bool fat_cache_valid;
    uint8_t fat_cache[SECTOR];
    uint8_t *clbuf;               // cluster scratch for read-modify-write
    uint8_t *zero;                // cluster of zeros
    char label[12];
};

struct dir_iter {
    struct fat32 *fs;
    uint32_t cluster;
    uint32_t sec, idx;
    bool loaded, end;
    uint64_t lba;
    uint8_t buf[SECTOR];
};

struct fat_entry {
    char name[VFS_NAME_MAX];
    uint8_t short_name[11];
    uint8_t attr;
    uint32_t first;
    uint32_t size;
    uint64_t lba;                 // location of the short entry
    uint16_t off;
    int nlfn;                     // LFN entries preceding it
    uint64_t lfn_lba[LFN_MAX_ENTRIES];
    uint16_t lfn_off[LFN_MAX_ENTRIES];
};

struct fat_node {
    uint32_t first;
    uint32_t size;
    bool is_dir;
    bool has_dirent;              // false for the root directory
    uint64_t dirent_lba;
    uint16_t dirent_off;
    uint32_t cache_idx, cache_clus;   // last (cluster index -> cluster) lookup
    struct dir_iter it;           // for readdir
    bool it_started;
};

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int sec_read(struct fat32 *fs, uint64_t lba, uint32_t n, void *buf) {
    return blockdev_read(fs->bd, lba, n, buf) ? VFS_EIO : 0;
}
static int sec_write(struct fat32 *fs, uint64_t lba, uint32_t n, const void *buf) {
    return blockdev_write(fs->bd, lba, n, buf) ? VFS_EIO : 0;
}

static bool valid_cluster(struct fat32 *fs, uint32_t c) {
    return c >= 2 && c < fs->cluster_count + 2;
}
static uint64_t cluster_lba(struct fat32 *fs, uint32_t c) {
    return fs->data_lba + (uint64_t)(c - 2) * fs->spc;
}

// ---- FAT table --------------------------------------------------------------

static int fat_load(struct fat32 *fs, uint64_t lba) {
    if (fs->fat_cache_valid && fs->fat_cache_lba == lba) return 0;
    fs->fat_cache_valid = false;
    int r = sec_read(fs, lba, 1, fs->fat_cache);
    if (r) return r;
    fs->fat_cache_lba = lba;
    fs->fat_cache_valid = true;
    return 0;
}

static int fat_get(struct fat32 *fs, uint32_t c, uint32_t *out) {
    if (!valid_cluster(fs, c)) return VFS_EIO;
    uint64_t lba = fs->fat_lba + (uint64_t)c * 4 / SECTOR;
    int r = fat_load(fs, lba);
    if (r) return r;
    *out = rd32(fs->fat_cache + (c * 4) % SECTOR) & FAT_MASK;
    return 0;
}

static int invalidate_fsinfo(struct fat32 *fs) {
    if (fs->fsinfo_invalidated || !fs->fsinfo_sector) return 0;
    fs->fsinfo_invalidated = true;
    uint8_t buf[SECTOR];
    uint64_t lba = fs->part_lba + fs->fsinfo_sector;
    if (sec_read(fs, lba, 1, buf)) return VFS_EIO;
    if (rd32(buf) != 0x41615252 || rd32(buf + 484) != 0x61417272) return 0;
    wr32(buf + 488, 0xffffffff);   // free count unknown
    wr32(buf + 492, 0xffffffff);   // no next-free hint
    return sec_write(fs, lba, 1, buf);
}

static int fat_set(struct fat32 *fs, uint32_t c, uint32_t v) {
    if (!valid_cluster(fs, c)) return VFS_EIO;
    int r = invalidate_fsinfo(fs);
    if (r) return r;
    uint64_t rel = (uint64_t)c * 4 / SECTOR;
    r = fat_load(fs, fs->fat_lba + rel);
    if (r) return r;
    uint8_t *p = fs->fat_cache + (c * 4) % SECTOR;
    wr32(p, (rd32(p) & ~FAT_MASK) | (v & FAT_MASK));
    for (uint32_t i = 0; i < fs->nfats; i++) {
        r = sec_write(fs, fs->fat_lba + (uint64_t)i * fs->fat_size + rel, 1, fs->fat_cache);
        if (r) { fs->fat_cache_valid = false; return r; }
    }
    return 0;
}

// Allocates one cluster, marks it end-of-chain and zeroes it.
static int alloc_cluster(struct fat32 *fs, uint32_t *out) {
    uint32_t c = fs->alloc_hint;
    for (uint32_t n = 0; n < fs->cluster_count; n++, c++) {
        if (!valid_cluster(fs, c)) c = 2;
        uint32_t v;
        int r = fat_get(fs, c, &v);
        if (r) return r;
        if (v != 0) continue;
        r = sec_write(fs, cluster_lba(fs, c), fs->spc, fs->zero);
        if (r) return r;
        r = fat_set(fs, c, FAT_MASK);
        if (r) return r;
        fs->alloc_hint = c + 1;
        *out = c;
        return 0;
    }
    return VFS_ENOSPC;
}

static int free_chain(struct fat32 *fs, uint32_t c) {
    for (uint32_t guard = 0; valid_cluster(fs, c) && guard <= fs->cluster_count; guard++) {
        uint32_t next;
        int r = fat_get(fs, c, &next);
        if (r) return r;
        r = fat_set(fs, c, 0);
        if (r) return r;
        if (c < fs->alloc_hint) fs->alloc_hint = c;
        if (next >= FAT_EOC) return 0;
        c = next;
    }
    return 0;
}

// ---- Time -------------------------------------------------------------------

static void fat_now(uint16_t *date, uint16_t *time) {
    struct rtc_time t;
    rtc_read(&t);
    uint16_t year = t.year >= 1980 ? (uint16_t)(t.year - 1980) : 0;
    *date = (uint16_t)((year << 9) | ((t.month & 0xf) << 5) | (t.day & 0x1f));
    *time = (uint16_t)((t.hour << 11) | ((t.minute & 0x3f) << 5) | ((t.second / 2) & 0x1f));
}

// ---- Directory iteration ----------------------------------------------------

static void dir_iter_init(struct dir_iter *it, struct fat32 *fs, uint32_t first) {
    it->fs = fs;
    it->cluster = first;
    it->sec = it->idx = 0;
    it->loaded = false;
    it->end = !valid_cluster(fs, first);
}

// Returns 1 and the next raw 32-byte entry (valid until the next call), 0 at
// the end of the cluster chain, or a negative error.
static int dir_raw_next(struct dir_iter *it, uint8_t **ent, uint64_t *lba, uint16_t *off) {
    if (it->end) return 0;
    if (!it->loaded) {
        it->lba = cluster_lba(it->fs, it->cluster) + it->sec;
        int r = sec_read(it->fs, it->lba, 1, it->buf);
        if (r) return r;
        it->loaded = true;
    }
    *ent = it->buf + it->idx * 32;
    *lba = it->lba;
    *off = (uint16_t)(it->idx * 32);
    if (++it->idx == SECTOR / 32) {
        it->idx = 0;
        it->loaded = false;
        if (++it->sec == it->fs->spc) {
            it->sec = 0;
            uint32_t next;
            int r = fat_get(it->fs, it->cluster, &next);
            if (r) return r;
            if (next >= FAT_EOC || !valid_cluster(it->fs, next)) it->end = true;
            else it->cluster = next;
        }
    }
    return 1;
}

static uint8_t lfn_checksum(const uint8_t *sn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

static const uint8_t lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void short_to_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
        out[n++] = (e[12] & 0x08) ? lower(c) : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (e[12] & 0x10) ? lower((char)e[i]) : (char)e[i];
    }
    out[n] = 0;
}

// Returns 1 and the next live entry (skipping ".", "..", volume labels and
// deleted entries), 0 at the end of the directory, or a negative error.
static int dir_next(struct dir_iter *it, struct fat_entry *out) {
    char lfn[LFN_MAX_ENTRIES * 13 + 1];
    int lfn_total = 0, lfn_seen = 0;
    uint8_t lfn_sum = 0;
    bool lfn_ok = false;
    out->nlfn = 0;
    for (;;) {
        uint8_t *e;
        uint64_t lba;
        uint16_t off;
        int r = dir_raw_next(it, &e, &lba, &off);
        if (r <= 0) return r;
        if (e[0] == 0x00) { it->end = true; return 0; }
        if (e[0] == 0xe5) { lfn_ok = false; out->nlfn = 0; continue; }
        if ((e[11] & 0x3f) == ATTR_LFN) {
            int seq = e[0] & 0x1f;
            if (e[0] & 0x40) {
                lfn_ok = seq >= 1 && seq <= LFN_MAX_ENTRIES;
                lfn_total = seq;
                lfn_seen = 0;
                lfn_sum = e[13];
                out->nlfn = 0;
                if (lfn_ok) memset(lfn, 0, sizeof lfn);
            }
            if (!lfn_ok || seq != lfn_total - lfn_seen || e[13] != lfn_sum) { lfn_ok = false; continue; }
            for (int i = 0; i < 13; i++) {
                uint16_t ch = rd16(e + lfn_pos[i]);
                char c = (ch == 0 || ch == 0xffff) ? 0 : (ch < 0x80 ? (char)ch : '?');
                lfn[(seq - 1) * 13 + i] = c;
            }
            if (out->nlfn < LFN_MAX_ENTRIES) {
                out->lfn_lba[out->nlfn] = lba;
                out->lfn_off[out->nlfn] = off;
                out->nlfn++;
            }
            lfn_seen++;
            continue;
        }
        if (e[11] & ATTR_VOLUME) { lfn_ok = false; out->nlfn = 0; continue; }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { lfn_ok = false; out->nlfn = 0; continue; }

        if (lfn_ok && lfn_seen == lfn_total && lfn_checksum(e) == lfn_sum && lfn[0]) {
            lfn[lfn_total * 13] = 0;
            strncpy(out->name, lfn, VFS_NAME_MAX - 1);
            out->name[VFS_NAME_MAX - 1] = 0;
        } else {
            short_to_name(e, out->name);
            out->nlfn = 0;
        }
        memcpy(out->short_name, e, 11);
        out->attr = e[11];
        out->first = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        out->size = rd32(e + 28);
        out->lba = lba;
        out->off = off;
        return 1;
    }
}

static bool name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (lower(*a) != lower(*b)) return false;
        a++, b++;
    }
    return *a == *b;
}

static int dir_find(struct fat32 *fs, uint32_t dir, const char *name, size_t len, struct fat_entry *out) {
    char want[VFS_NAME_MAX];
    if (len >= VFS_NAME_MAX) return VFS_ENAMETOOLONG;
    memcpy(want, name, len);
    want[len] = 0;
    struct dir_iter *it = kmalloc(sizeof *it);
    if (!it) return VFS_ENOMEM;
    dir_iter_init(it, fs, dir);
    int r;
    while ((r = dir_next(it, out)) > 0) {
        char sn[13];
        short_to_name(out->short_name, sn);
        if (name_eq(out->name, want) || name_eq(sn, want)) { kfree(it); return 0; }
    }
    kfree(it);
    return r < 0 ? r : VFS_ENOENT;
}

// Walks rel up to its last component. On success *parent is the cluster of the
// containing directory and *leaf points at the last component inside rel.
static int walk_parent(struct fat32 *fs, const char *rel, uint32_t *parent, const char **leaf) {
    uint32_t dir = fs->root_cluster;
    const char *p = rel;
    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) break;
        struct fat_entry *e = kmalloc(sizeof *e);
        if (!e) return VFS_ENOMEM;
        int r = dir_find(fs, dir, p, (size_t)(slash - p), e);
        if (!r && !(e->attr & ATTR_DIR)) r = VFS_ENOTDIR;
        if (!r) dir = e->first ? e->first : fs->root_cluster;
        kfree(e);
        if (r) return r;
        p = slash + 1;
    }
    *parent = dir;
    *leaf = p;
    return 0;
}

// Looks up rel. *is_root is set for "" (out is then untouched).
static int lookup(struct fat32 *fs, const char *rel, struct fat_entry *out, bool *is_root, uint32_t *parent) {
    *is_root = rel[0] == 0;
    if (*is_root) return 0;
    const char *leaf;
    uint32_t dir;
    int r = walk_parent(fs, rel, &dir, &leaf);
    if (r) return r;
    if (parent) *parent = dir;
    return dir_find(fs, dir, leaf, strlen(leaf), out);
}

// ---- Directory modification -------------------------------------------------

static int patch_entry(struct fat32 *fs, uint64_t lba, uint16_t off, const uint8_t *ent32) {
    uint8_t buf[SECTOR];
    int r = sec_read(fs, lba, 1, buf);
    if (r) return r;
    memcpy(buf + off, ent32, 32);
    return sec_write(fs, lba, 1, buf);
}

static bool valid_83_char(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL && c != 0;
}

static bool valid_lfn_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 255) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7f || strchr("\"*/:<>?\\|", c)) return false;
    }
    return true;
}

// Tries to represent name exactly as 8.3 (with NT lower-case flags).
static bool make_exact_83(const char *name, uint8_t sn[11], uint8_t *ntcase) {
    memset(sn, ' ', 11);
    *ntcase = 0;
    const char *dot = strchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3 || (dot && (elen == 0 || strchr(dot + 1, '.')))) return false;
    bool lo[2] = { false, false }, up[2] = { false, false };
    for (size_t i = 0; i < blen + (dot ? elen + 1 : 0); i++) {
        if (i == blen) continue;
        char c = name[i];
        int part = i > blen;
        if (c >= 'a' && c <= 'z') lo[part] = true;
        if (c >= 'A' && c <= 'Z') up[part] = true;
        if (!valid_83_char(upper(c))) return false;
        if (part) sn[8 + (i - blen - 1)] = (uint8_t)upper(c);
        else sn[i] = (uint8_t)upper(c);
    }
    if ((lo[0] && up[0]) || (lo[1] && up[1])) return false;
    if (lo[0]) *ntcase |= 0x08;
    if (lo[1]) *ntcase |= 0x10;
    return true;
}

static bool short_exists(struct fat32 *fs, uint32_t dir, const uint8_t sn[11]) {
    struct dir_iter *it = kmalloc(sizeof *it);
    if (!it) return true;
    dir_iter_init(it, fs, dir);
    uint8_t *e;
    uint64_t lba;
    uint16_t off;
    bool found = false;
    while (!found && dir_raw_next(it, &e, &lba, &off) > 0) {
        if (e[0] == 0) break;
        if (e[0] != 0xe5 && (e[11] & 0x3f) != ATTR_LFN && memcmp(e, sn, 11) == 0) found = true;
    }
    kfree(it);
    return found;
}

// Builds a unique "BASIS~N.EXT" alias for a long name.
static int make_alias(struct fat32 *fs, uint32_t dir, const char *name, uint8_t sn[11]) {
    char base[9] = {0}, ext[4] = {0};
    const char *last_dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') last_dot = p;
    if (last_dot == name) last_dot = NULL;
    size_t bl = 0, el = 0;
    for (const char *p = name; *p && (!last_dot || p < last_dot); p++) {
        if (*p == ' ' || *p == '.') continue;
        char c = upper(*p);
        if (bl < 8) base[bl++] = valid_83_char(c) ? c : '_';
    }
    if (last_dot)
        for (const char *p = last_dot + 1; *p && el < 3; p++) {
            if (*p == ' ' || *p == '.') continue;
            char c = upper(*p);
            ext[el++] = valid_83_char(c) ? c : '_';
        }
    if (bl == 0) base[bl++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[9];
        int tl = ksnprintf(tail, sizeof tail, "~%u", n);
        size_t keep = bl;
        if (keep + (size_t)tl > 8) keep = 8 - (size_t)tl;
        memset(sn, ' ', 11);
        memcpy(sn, base, keep);
        memcpy(sn + keep, tail, (size_t)tl);
        memcpy(sn + 8, ext, el);
        if (!short_exists(fs, dir, sn)) return 0;
    }
    return VFS_EEXIST;
}

// Adds a directory entry (with LFN entries if needed) to dir.
static int dir_add(struct fat32 *fs, uint32_t dir, const char *name, uint8_t attr,
                   uint32_t first, uint64_t *out_lba, uint16_t *out_off) {
    if (!valid_lfn_name(name)) return VFS_EINVAL;
    uint8_t sn[11], ntcase = 0;
    int nlfn = 0;
    if (!make_exact_83(name, sn, &ntcase)) {
        int r = make_alias(fs, dir, name, sn);
        if (r) return r;
        nlfn = (int)((strlen(name) + 12) / 13);
    }
    int need = nlfn + 1;

    // Find `need` consecutive free slots, extending the directory if necessary.
    uint64_t slot_lba[LFN_MAX_ENTRIES + 1];
    uint16_t slot_off[LFN_MAX_ENTRIES + 1];
    int run = 0;
    struct dir_iter *it = kmalloc(sizeof *it);
    if (!it) return VFS_ENOMEM;
    dir_iter_init(it, fs, dir);
    int r = 0;
    while (run < need) {
        uint32_t cur = it->cluster;
        uint8_t *e;
        uint64_t lba;
        uint16_t off;
        r = dir_raw_next(it, &e, &lba, &off);
        if (r < 0) break;
        if (r == 0) {
            uint32_t nc;
            r = alloc_cluster(fs, &nc);
            if (!r) r = fat_set(fs, cur, nc);
            if (r) break;
            it->cluster = nc;
            it->sec = it->idx = 0;
            it->loaded = false;
            it->end = false;
            continue;
        }
        if (e[0] == 0x00 || e[0] == 0xe5) {
            slot_lba[run] = lba;
            slot_off[run] = off;
            run++;
        } else {
            run = 0;
        }
    }
    kfree(it);
    if (r < 0) return r;

    uint8_t ent[32];
    uint8_t sum = lfn_checksum(sn);
    size_t nlen = strlen(name);
    for (int k = 0; k < nlfn; k++) {
        int seq = nlfn - k;
        memset(ent, 0, 32);
        ent[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        ent[11] = ATTR_LFN;
        ent[13] = sum;
        for (int i = 0; i < 13; i++) {
            size_t ci = (size_t)(seq - 1) * 13 + (size_t)i;
            uint16_t ch = ci < nlen ? (uint8_t)name[ci] : (ci == nlen ? 0 : 0xffff);
            wr16(ent + lfn_pos[i], ch);
        }
        r = patch_entry(fs, slot_lba[k], slot_off[k], ent);
        if (r) return r;
    }
    uint16_t date, time;
    fat_now(&date, &time);
    memset(ent, 0, 32);
    memcpy(ent, sn, 11);
    ent[11] = attr;
    ent[12] = ntcase;
    wr16(ent + 14, time);
    wr16(ent + 16, date);
    wr16(ent + 18, date);
    wr16(ent + 20, (uint16_t)(first >> 16));
    wr16(ent + 22, time);
    wr16(ent + 24, date);
    wr16(ent + 26, (uint16_t)first);
    r = patch_entry(fs, slot_lba[nlfn], slot_off[nlfn], ent);
    if (r) return r;
    *out_lba = slot_lba[nlfn];
    *out_off = slot_off[nlfn];
    return 0;
}

// Writes the node's first cluster, size and modification time to its dirent.
static int node_sync(struct fat32 *fs, struct fat_node *n) {
    if (!n->has_dirent) return 0;
    uint8_t buf[SECTOR];
    int r = sec_read(fs, n->dirent_lba, 1, buf);
    if (r) return r;
    uint8_t *e = buf + n->dirent_off;
    uint16_t date, time;
    fat_now(&date, &time);
    wr16(e + 20, (uint16_t)(n->first >> 16));
    wr16(e + 26, (uint16_t)n->first);
    if (!n->is_dir) wr32(e + 28, n->size);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 18, date);
    e[11] |= n->is_dir ? 0 : ATTR_ARCH;
    return sec_write(fs, n->dirent_lba, 1, buf);
}

// ---- File data --------------------------------------------------------------

// Finds the cluster holding cluster-index idx of the node, optionally growing
// the chain (new clusters are zeroed).
static int node_cluster(struct fat32 *fs, struct fat_node *n, uint32_t idx, bool grow, uint32_t *out) {
    if (!n->first) {
        if (!grow) return VFS_EIO;
        int r = alloc_cluster(fs, &n->first);
        if (r) return r;
        n->cache_idx = 0;
        n->cache_clus = n->first;
    }
    uint32_t i = 0, c = n->first;
    if (n->cache_clus && n->cache_idx <= idx) { i = n->cache_idx; c = n->cache_clus; }
    while (i < idx) {
        uint32_t next;
        int r = fat_get(fs, c, &next);
        if (r) return r;
        if (next >= FAT_EOC || !valid_cluster(fs, next)) {
            if (!grow) return VFS_EIO;
            r = alloc_cluster(fs, &next);
            if (!r) r = fat_set(fs, c, next);
            if (r) return r;
        }
        c = next;
        i++;
    }
    n->cache_idx = idx;
    n->cache_clus = c;
    *out = c;
    return 0;
}

static int64_t fat_read(struct vfs_file *f, uint64_t off, void *buf, size_t len) {
    struct fat32 *fs = f->fs;
    struct fat_node *n = f->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (size_t)(n->size - off);
    uint8_t *dst = buf;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / fs->cluster_bytes);
        uint32_t within = (uint32_t)(pos % fs->cluster_bytes);
        size_t chunk = fs->cluster_bytes - within;
        if (chunk > len - done) chunk = len - done;
        uint32_t c;
        int r = node_cluster(fs, n, idx, false, &c);
        if (r) return done ? (int64_t)done : r;
        if (within == 0 && chunk == fs->cluster_bytes) {
            r = sec_read(fs, cluster_lba(fs, c), fs->spc, dst + done);
        } else {
            r = sec_read(fs, cluster_lba(fs, c), fs->spc, fs->clbuf);
            if (!r) memcpy(dst + done, fs->clbuf + within, chunk);
        }
        if (r) return done ? (int64_t)done : r;
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t fat_write(struct vfs_file *f, uint64_t off, const void *buf, size_t len) {
    struct fat32 *fs = f->fs;
    struct fat_node *n = f->priv;
    if (off + len > 0xffffffffull) return VFS_ENOSPC;        // FAT32 file size limit
    const uint8_t *src = buf;
    size_t done = 0;
    int err = 0;
    uint32_t old_first = n->first;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / fs->cluster_bytes);
        uint32_t within = (uint32_t)(pos % fs->cluster_bytes);
        size_t chunk = fs->cluster_bytes - within;
        if (chunk > len - done) chunk = len - done;
        uint32_t c;
        err = node_cluster(fs, n, idx, true, &c);
        if (err) break;
        if (within == 0 && chunk == fs->cluster_bytes) {
            err = sec_write(fs, cluster_lba(fs, c), fs->spc, src + done);
        } else {
            err = sec_read(fs, cluster_lba(fs, c), fs->spc, fs->clbuf);
            if (!err) {
                memcpy(fs->clbuf + within, src + done, chunk);
                err = sec_write(fs, cluster_lba(fs, c), fs->spc, fs->clbuf);
            }
        }
        if (err) break;
        done += chunk;
    }
    if (off + done > n->size) n->size = (uint32_t)(off + done);
    if (done || n->first != old_first) {
        int r = node_sync(fs, n);
        if (r && !err) err = r;
    }
    f->size = n->size;
    if (done) return (int64_t)done;
    return err;
}

// ---- VFS operations ---------------------------------------------------------

static struct fat_node *node_from_entry(struct fat32 *fs, const struct fat_entry *e, bool is_root) {
    struct fat_node *n = kcalloc(1, sizeof *n);
    if (!n) return NULL;
    if (is_root) {
        n->first = fs->root_cluster;
        n->is_dir = true;
    } else {
        n->is_dir = (e->attr & ATTR_DIR) != 0;
        n->first = e->first;
        if (n->is_dir && !n->first) n->first = fs->root_cluster;   // ".." style root link
        n->size = n->is_dir ? 0 : e->size;
        n->has_dirent = true;
        n->dirent_lba = e->lba;
        n->dirent_off = e->off;
    }
    return n;
}

static int fat_open(void *fsp, const char *rel, int flags, struct vfs_file *f) {
    struct fat32 *fs = fsp;
    struct fat_entry *e = kmalloc(sizeof *e);
    if (!e) return VFS_ENOMEM;
    bool is_root;
    uint32_t parent = 0;
    int r = lookup(fs, rel, e, &is_root, &parent);
    if (r == VFS_ENOENT && (flags & VFS_O_CREATE)) {
        const char *leaf;
        r = walk_parent(fs, rel, &parent, &leaf);
        if (!r) {
            uint64_t lba;
            uint16_t off;
            r = dir_add(fs, parent, leaf, ATTR_ARCH, 0, &lba, &off);
            if (!r) r = lookup(fs, rel, e, &is_root, NULL);
        }
    } else if (!r && (flags & VFS_O_CREATE) && (flags & VFS_O_EXCL)) {
        r = VFS_EEXIST;
    }
    if (r) { kfree(e); return r; }
    struct fat_node *n = node_from_entry(fs, e, is_root);
    kfree(e);
    if (!n) return VFS_ENOMEM;
    if (!n->is_dir && (flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && (n->first || n->size)) {
        r = free_chain(fs, n->first);
        n->first = 0;
        n->size = 0;
        if (!r) r = node_sync(fs, n);
        if (r) { kfree(n); return r; }
    }
    f->priv = n;
    f->type = n->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    f->size = n->size;
    return 0;
}

static int fat_readdir(struct vfs_file *f, struct vfs_dirent *out) {
    struct fat_node *n = f->priv;
    if (!n->it_started) {
        dir_iter_init(&n->it, f->fs, n->first);
        n->it_started = true;
    }
    struct fat_entry *e = kmalloc(sizeof *e);
    if (!e) return VFS_ENOMEM;
    int r = dir_next(&n->it, e);
    if (r > 0) {
        memset(out, 0, sizeof *out);
        strncpy(out->name, e->name, VFS_NAME_MAX - 1);
        out->type = (e->attr & ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        out->size = (e->attr & ATTR_DIR) ? 0 : e->size;
    }
    kfree(e);
    return r;
}

static void fat_close(struct vfs_file *f) {
    kfree(f->priv);
    f->priv = NULL;
}

static int fat_stat(void *fsp, const char *rel, struct vfs_stat *st) {
    struct fat32 *fs = fsp;
    struct fat_entry *e = kmalloc(sizeof *e);
    if (!e) return VFS_ENOMEM;
    bool is_root;
    int r = lookup(fs, rel, e, &is_root, NULL);
    if (!r) {
        bool dir = is_root || (e->attr & ATTR_DIR);
        st->type = dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        st->size = dir ? 0 : e->size;
    }
    kfree(e);
    return r;
}

static int fat_mkdir(void *fsp, const char *rel) {
    struct fat32 *fs = fsp;
    struct fat_entry *e = kmalloc(sizeof *e);
    if (!e) return VFS_ENOMEM;
    bool is_root;
    int r = lookup(fs, rel, e, &is_root, NULL);
    kfree(e);
    if (!r) return VFS_EEXIST;
    if (r != VFS_ENOENT) return r;
    uint32_t parent;
    const char *leaf;
    r = walk_parent(fs, rel, &parent, &leaf);
    if (r) return r;
    if (!valid_lfn_name(leaf)) return VFS_EINVAL;
    uint32_t c;
    r = alloc_cluster(fs, &c);
    if (r) return r;

    uint8_t buf[SECTOR];
    memset(buf, 0, sizeof buf);
    uint16_t date, time;
    fat_now(&date, &time);
    uint32_t parent_link = parent == fs->root_cluster ? 0 : parent;
    for (int i = 0; i < 2; i++) {
        uint8_t *d = buf + i * 32;
        memset(d, ' ', 11);
        d[0] = '.';
        if (i) d[1] = '.';
        uint32_t cl = i ? parent_link : c;
        d[11] = ATTR_DIR;
        wr16(d + 14, time); wr16(d + 16, date); wr16(d + 18, date);
        wr16(d + 20, (uint16_t)(cl >> 16));
        wr16(d + 22, time); wr16(d + 24, date);
        wr16(d + 26, (uint16_t)cl);
    }
    r = sec_write(fs, cluster_lba(fs, c), 1, buf);
    uint64_t lba;
    uint16_t off;
    if (!r) r = dir_add(fs, parent, leaf, ATTR_DIR, c, &lba, &off);
    if (r) free_chain(fs, c);
    return r;
}

static int fat_unlink(void *fsp, const char *rel) {
    struct fat32 *fs = fsp;
    struct fat_entry *e = kmalloc(sizeof *e);
    if (!e) return VFS_ENOMEM;
    bool is_root;
    int r = lookup(fs, rel, e, &is_root, NULL);
    if (!r && is_root) r = VFS_EBUSY;
    if (!r && (e->attr & ATTR_DIR) && e->first) {
        struct dir_iter *it = kmalloc(sizeof *it);
        struct fat_entry *child = kmalloc(sizeof *child);
        if (!it || !child) r = VFS_ENOMEM;
        else {
            dir_iter_init(it, fs, e->first);
            int n = dir_next(it, child);
            if (n > 0) r = VFS_ENOTEMPTY;
            else if (n < 0) r = n;
        }
        kfree(it);
        kfree(child);
    }
    if (!r) {
        // Mark the short entry and its LFN entries deleted, then free the data.
        uint8_t buf[SECTOR];
        for (int i = -1; i < e->nlfn && !r; i++) {
            uint64_t lba = i < 0 ? e->lba : e->lfn_lba[i];
            uint16_t off = i < 0 ? e->off : e->lfn_off[i];
            r = sec_read(fs, lba, 1, buf);
            if (!r) {
                buf[off] = 0xe5;
                r = sec_write(fs, lba, 1, buf);
            }
        }
        if (!r && e->first) r = free_chain(fs, e->first);
    }
    kfree(e);
    return r;
}

static const struct vfs_fs_ops fat_ops = {
    .name = "fat32",
    .open = fat_open,
    .read = fat_read,
    .write = fat_write,
    .readdir = fat_readdir,
    .close = fat_close,
    .stat = fat_stat,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
};

// ---- Mount ------------------------------------------------------------------

static bool is_fat32_bpb(const uint8_t *s) {
    if (rd16(s + 510) != 0xaa55) return false;
    if (rd16(s + 11) != SECTOR) return false;
    uint8_t spc = s[13];
    if (spc == 0 || (spc & (spc - 1))) return false;
    if (s[16] == 0 || rd16(s + 14) == 0) return false;
    return rd16(s + 17) == 0 && rd16(s + 22) == 0 && rd32(s + 36) != 0;
}

int fat32_mount(struct blockdev *bd, const char *mount_path, char *info, size_t info_len) {
    if (bd->sector_size != SECTOR) return VFS_EINVAL;
    uint8_t *s = kmalloc(SECTOR);
    if (!s) return VFS_ENOMEM;
    uint64_t part = 0;
    if (blockdev_read(bd, 0, 1, s)) { kfree(s); return VFS_EIO; }
    if (!is_fat32_bpb(s)) {
        if (rd16(s + 510) != 0xaa55) { kfree(s); return VFS_EINVAL; }
        for (int i = 0; i < 4; i++) {
            const uint8_t *pe = s + 446 + i * 16;
            if (pe[4] == 0x0b || pe[4] == 0x0c) { part = rd32(pe + 8); break; }
        }
        if (!part || blockdev_read(bd, part, 1, s) || !is_fat32_bpb(s)) { kfree(s); return VFS_EINVAL; }
    }
    struct fat32 *fs = kcalloc(1, sizeof *fs);
    if (!fs) { kfree(s); return VFS_ENOMEM; }
    fs->bd = bd;
    fs->part_lba = part;
    fs->spc = s[13];
    fs->cluster_bytes = fs->spc * SECTOR;
    uint32_t reserved = rd16(s + 14);
    fs->nfats = s[16];
    fs->fat_size = rd32(s + 36);
    fs->root_cluster = rd32(s + 44);
    fs->fsinfo_sector = rd16(s + 48);
    if (fs->fsinfo_sector == 0xffff) fs->fsinfo_sector = 0;
    uint64_t total = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    fs->fat_lba = part + reserved;
    fs->data_lba = fs->fat_lba + (uint64_t)fs->nfats * fs->fat_size;
    uint64_t meta = reserved + (uint64_t)fs->nfats * fs->fat_size;
    uint64_t clusters = total > meta ? (total - meta) / fs->spc : 0;
    uint64_t fat_entries = (uint64_t)fs->fat_size * SECTOR / 4;
    if (clusters + 2 > fat_entries) clusters = fat_entries - 2;
    fs->cluster_count = (uint32_t)clusters;
    memcpy(fs->label, s + 71, 11);
    fs->label[11] = 0;
    for (int i = 10; i >= 0 && fs->label[i] == ' '; i--) fs->label[i] = 0;
    kfree(s);

    if (fs->cluster_count == 0 || part + total > bd->sectors || !valid_cluster(fs, fs->root_cluster)) {
        kfree(fs);
        return VFS_EINVAL;
    }
    fs->alloc_hint = 2;
    if (fs->fsinfo_sector) {
        uint8_t fsi[SECTOR];
        if (!blockdev_read(bd, part + fs->fsinfo_sector, 1, fsi) &&
            rd32(fsi) == 0x41615252 && rd32(fsi + 484) == 0x61417272) {
            uint32_t hint = rd32(fsi + 492);
            if (valid_cluster(fs, hint)) fs->alloc_hint = hint;
        } else {
            fs->fsinfo_sector = 0;
        }
    }
    fs->clbuf = kmalloc(fs->cluster_bytes);
    fs->zero = kcalloc(1, fs->cluster_bytes);
    if (!fs->clbuf || !fs->zero) {
        kfree(fs->clbuf); kfree(fs->zero); kfree(fs);
        return VFS_ENOMEM;
    }
    int r = vfs_mount(mount_path, &fat_ops, fs, false);
    if (r) {
        kfree(fs->clbuf); kfree(fs->zero); kfree(fs);
        return r;
    }
    ksnprintf(info, info_len, "%s label='%s' part_lba=%lu clusters=%u cluster_size=%u",
              bd->name, fs->label, (unsigned long)part, fs->cluster_count, fs->cluster_bytes);
    return 0;
}
