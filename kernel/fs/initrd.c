// Read-only initrd filesystem over a USTAR archive loaded by Limine as a module.
// Owner: fs agent.
#include "fs_internal.h"
#include "limine.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0 };

#define INITRD_MAX_ENTRIES 256

struct tar_entry {
    char name[VFS_PATH_MAX];      // relative path, no leading "./" or trailing '/'
    uint32_t type;
    const uint8_t *data;
    uint64_t size;
};

static struct {
    struct tar_entry *entries;
    int count;
    int files;
} initrd;

static uint64_t octal(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (uint64_t)(s[i] - '0');
    }
    return v;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static struct limine_file *find_module(void) {
    struct limine_module_response *resp = module_req.response;
    if (!resp) return NULL;
    for (uint64_t i = 0; i < resp->module_count; i++) {
        struct limine_file *m = resp->modules[i];
        if (m->path && ends_with(m->path, "initrd.tar")) return m;
    }
    return resp->module_count ? resp->modules[0] : NULL;
}

static int parse(const uint8_t *base, uint64_t size) {
    initrd.entries = kcalloc(INITRD_MAX_ENTRIES, sizeof(struct tar_entry));
    if (!initrd.entries) return VFS_ENOMEM;
    uint64_t off = 0;
    while (off + 512 <= size) {
        const char *h = (const char *)(base + off);
        if (h[0] == 0) break;                                   // end-of-archive block
        if (memcmp(h + 257, "ustar", 5) != 0) return VFS_EIO;
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        uint64_t data_off = off + 512;
        if (data_off + fsize > size) return VFS_EIO;
        off = data_off + ((fsize + 511) & ~511ull);

        if (type != '0' && type != 0 && type != '5') continue;  // skip links, pax, ...
        char full[VFS_PATH_MAX];
        size_t n = 0;
        if (h[345]) {                                           // ustar prefix field
            for (size_t i = 0; i < 155 && h[345 + i] && n < VFS_PATH_MAX - 2; i++) full[n++] = h[345 + i];
            full[n++] = '/';
        }
        for (size_t i = 0; i < 100 && h[i] && n < VFS_PATH_MAX - 1; i++) full[n++] = h[i];
        full[n] = 0;
        const char *p = full;
        while (p[0] == '.' && p[1] == '/') p += 2;
        while (*p == '/') p++;
        size_t len = strlen(p);
        while (len && p[len - 1] == '/') len--;
        if (len == 0 || (len == 1 && p[0] == '.')) continue;   // the archive root itself
        if (initrd.count >= INITRD_MAX_ENTRIES) return VFS_ENOMEM;
        struct tar_entry *e = &initrd.entries[initrd.count++];
        memcpy(e->name, p, len);
        e->name[len] = 0;
        e->type = type == '5' ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        e->data = base + data_off;
        e->size = e->type == VFS_TYPE_FILE ? fsize : 0;
        if (e->type == VFS_TYPE_FILE) initrd.files++;
    }
    return 0;
}

// Looks up rel. Returns the entry, or NULL; *is_dir set for implicit dirs/root.
static struct tar_entry *lookup(const char *rel, bool *implicit_dir) {
    *implicit_dir = false;
    if (rel[0] == 0) { *implicit_dir = true; return NULL; }
    size_t rl = strlen(rel);
    for (int i = 0; i < initrd.count; i++) {
        struct tar_entry *e = &initrd.entries[i];
        if (strcmp(e->name, rel) == 0) return e;
        if (strncmp(e->name, rel, rl) == 0 && e->name[rl] == '/') *implicit_dir = true;
    }
    return NULL;
}

static int tar_open(void *fs, const char *rel, int flags, struct vfs_file *f) {
    (void)fs; (void)flags;
    bool dir;
    struct tar_entry *e = lookup(rel, &dir);
    if (!e && !dir) return VFS_ENOENT;
    f->priv = e;
    f->type = e ? e->type : VFS_TYPE_DIR;
    f->size = e ? e->size : 0;
    return 0;
}

static int64_t tar_read(struct vfs_file *f, uint64_t off, void *buf, size_t n) {
    struct tar_entry *e = f->priv;
    if (off >= e->size) return 0;
    if (n > e->size - off) n = (size_t)(e->size - off);
    memcpy(buf, e->data + off, n);
    return (int64_t)n;
}

static int tar_readdir(struct vfs_file *f, struct vfs_dirent *out) {
    // Directory prefix relative to the mount: f->path minus the mount path is
    // not available here, but the initrd is always mounted at "/".
    const char *dir = f->path + 1;
    size_t dl = strlen(dir);
    while (f->dir_pos < (uint64_t)initrd.count) {
        struct tar_entry *e = &initrd.entries[f->dir_pos++];
        const char *name;
        if (dl == 0) name = e->name;
        else if (strncmp(e->name, dir, dl) == 0 && e->name[dl] == '/') name = e->name + dl + 1;
        else continue;
        if (!*name || strchr(name, '/')) continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, name, VFS_NAME_MAX - 1);
        out->type = e->type;
        out->size = e->size;
        return 1;
    }
    return 0;
}

static int tar_stat(void *fs, const char *rel, struct vfs_stat *st) {
    (void)fs;
    bool dir;
    struct tar_entry *e = lookup(rel, &dir);
    if (!e && !dir) return VFS_ENOENT;
    st->type = e ? e->type : VFS_TYPE_DIR;
    st->size = e ? e->size : 0;
    return 0;
}

static const struct vfs_fs_ops tar_ops = {
    .name = "initrd",
    .open = tar_open,
    .read = tar_read,
    .readdir = tar_readdir,
    .stat = tar_stat,
};

int initrd_mount(const char *mount_path) {
    if (strcmp(mount_path, "/") != 0) return VFS_EINVAL;   // see tar_readdir
    struct limine_file *m = find_module();
    if (!m) {
        // Still mount an empty root so "/disk" etc. resolve.
        initrd.count = 0;
        initrd.files = 0;
        int r = vfs_mount(mount_path, &tar_ops, &initrd, true);
        return r ? r : VFS_ENOENT;
    }
    int r = parse((const uint8_t *)m->address, m->size);
    if (r) return r;
    r = vfs_mount(mount_path, &tar_ops, &initrd, true);
    return r ? r : initrd.count;
}

int initrd_file_count(void) { return initrd.files; }
