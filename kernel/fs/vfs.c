// VFS core: mount table, path normalization/resolution and dispatch to drivers.
// Owner: fs agent. All global VFS state lives in `vfs` below (add a lock here).
#include "vfs.h"
#include "mm.h"
#include "string.h"

#define VFS_MAX_MOUNTS 8

struct vfs_mount {
    char path[VFS_PATH_MAX];      // normalized, "/" or "/a/b"
    size_t len;
    const struct vfs_fs_ops *ops;
    void *fs;
    bool readonly;
    bool used;
};

static struct {
    struct vfs_mount mounts[VFS_MAX_MOUNTS];
    int count;
} vfs;

void vfs_init(void) {
    memset(&vfs, 0, sizeof vfs);
}

// Normalizes an absolute path into out ("/" or "/a/b"). Resolves "." and "..".
static int normalize(const char *in, char *out) {
    if (!in || in[0] != '/') return VFS_EINVAL;
    size_t o = 0;
    out[o++] = '/';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;   // strip last component
            if (o > 1) o--;                            // and its separator
            continue;
        }
        if (n >= VFS_NAME_MAX) return VFS_ENAMETOOLONG;
        if (o > 1) {
            if (o + 1 >= VFS_PATH_MAX) return VFS_ENAMETOOLONG;
            out[o++] = '/';
        }
        if (o + n >= VFS_PATH_MAX) return VFS_ENAMETOOLONG;
        memcpy(out + o, s, n);
        o += n;
    }
    out[o] = 0;
    return 0;
}

// Finds the mount with the longest matching prefix; *rel points into norm.
static struct vfs_mount *resolve(const char *norm, const char **rel) {
    struct vfs_mount *best = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_mount *m = &vfs.mounts[i];
        if (!m->used) continue;
        bool match;
        if (m->len == 1) match = true;
        else match = strncmp(norm, m->path, m->len) == 0 && (norm[m->len] == 0 || norm[m->len] == '/');
        if (match && (!best || m->len > best->len)) best = m;
    }
    if (!best) return NULL;
    const char *r = norm + (best->len == 1 ? 0 : best->len);
    while (*r == '/') r++;
    *rel = r;
    return best;
}

int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *fs, bool readonly) {
    char norm[VFS_PATH_MAX];
    int r = normalize(path, norm);
    if (r) return r;
    struct vfs_mount *slot = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs.mounts[i].used && strcmp(vfs.mounts[i].path, norm) == 0) return VFS_EBUSY;
        if (!vfs.mounts[i].used && !slot) slot = &vfs.mounts[i];
    }
    if (!slot) return VFS_ENOMEM;
    strcpy(slot->path, norm);
    slot->len = strlen(norm);
    slot->ops = ops;
    slot->fs = fs;
    slot->readonly = readonly;
    slot->used = true;
    vfs.count++;
    return 0;
}

int vfs_mount_count(void) { return vfs.count; }

static int join_path(char *out, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + nl + 2 > VFS_PATH_MAX) return VFS_ENAMETOOLONG;
    memcpy(out, dir, dl);
    if (dl > 1) out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return 0;
}

static int is_mountpoint(const char *norm) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (vfs.mounts[i].used && strcmp(vfs.mounts[i].path, norm) == 0) return 1;
    return 0;
}

int vfs_open(const char *path, int flags, struct vfs_file **out) {
    *out = NULL;
    struct vfs_file *f = kcalloc(1, sizeof *f);
    if (!f) return VFS_ENOMEM;
    int r = normalize(path, f->path);
    if (r) { kfree(f); return r; }
    const char *rel;
    struct vfs_mount *m = resolve(f->path, &rel);
    if (!m) { kfree(f); return VFS_ENOENT; }
    if (!(flags & (VFS_O_READ | VFS_O_WRITE))) flags |= VFS_O_READ;
    if (m->readonly && (flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC | VFS_O_APPEND))) {
        // Opening an existing file read-write on a RO mount is still an error.
        kfree(f);
        return VFS_EROFS;
    }
    f->mnt = m;
    f->fs = m->fs;
    f->flags = flags;
    r = m->ops->open(m->fs, rel, flags, f);
    if (r) { kfree(f); return r; }
    if (f->type == VFS_TYPE_DIR && (flags & VFS_O_WRITE)) {
        if (m->ops->close) m->ops->close(f);
        kfree(f);
        return VFS_EISDIR;
    }
    *out = f;
    return 0;
}

int vfs_close(struct vfs_file *f) {
    if (!f) return VFS_EINVAL;
    if (f->mnt->ops->close) f->mnt->ops->close(f);
    kfree(f);
    return 0;
}

int64_t vfs_read(struct vfs_file *f, void *buf, size_t n) {
    if (!f) return VFS_EINVAL;
    if (f->type != VFS_TYPE_FILE) return VFS_EISDIR;
    if (!(f->flags & VFS_O_READ)) return VFS_EINVAL;
    if (f->pos >= f->size || n == 0) return 0;
    if (n > f->size - f->pos) n = (size_t)(f->size - f->pos);
    int64_t r = f->mnt->ops->read(f, f->pos, buf, n);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

int64_t vfs_write(struct vfs_file *f, const void *buf, size_t n) {
    if (!f) return VFS_EINVAL;
    if (f->type != VFS_TYPE_FILE) return VFS_EISDIR;
    if (!(f->flags & VFS_O_WRITE)) return VFS_EINVAL;
    if (f->mnt->readonly || !f->mnt->ops->write) return VFS_EROFS;
    if (n == 0) return 0;
    if (f->flags & VFS_O_APPEND) f->pos = f->size;
    int64_t r = f->mnt->ops->write(f, f->pos, buf, n);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

int64_t vfs_seek(struct vfs_file *f, int64_t off, int whence) {
    if (!f) return VFS_EINVAL;
    int64_t base;
    switch (whence) {
    case VFS_SEEK_SET: base = 0; break;
    case VFS_SEEK_CUR: base = (int64_t)f->pos; break;
    case VFS_SEEK_END: base = (int64_t)f->size; break;
    default: return VFS_EINVAL;
    }
    if (base + off < 0) return VFS_EINVAL;
    f->pos = (uint64_t)(base + off);
    return (int64_t)f->pos;
}

int64_t vfs_tell(struct vfs_file *f) { return f ? (int64_t)f->pos : VFS_EINVAL; }
uint64_t vfs_file_size(struct vfs_file *f) { return f ? f->size : 0; }
uint32_t vfs_file_type(struct vfs_file *f) { return f ? f->type : 0; }

int vfs_readdir(struct vfs_file *dir, struct vfs_dirent *out) {
    if (!dir) return VFS_EINVAL;
    if (dir->type != VFS_TYPE_DIR) return VFS_ENOTDIR;
    if (dir->mnt_cursor == 0) {
        for (;;) {
            int r = dir->mnt->ops->readdir(dir, out);
            if (r <= 0) {
                if (r < 0) return r;
                break;
            }
            // Hide fs entries shadowed by a mount point (listed below instead).
            char child[VFS_PATH_MAX];
            if (join_path(child, dir->path, out->name) == 0 && is_mountpoint(child)) continue;
            return 1;
        }
        dir->mnt_cursor = 1;
    }
    // Then synthesize entries for mount points that are direct children.
    size_t dl = strlen(dir->path);
    while (dir->mnt_cursor - 1 < VFS_MAX_MOUNTS) {
        struct vfs_mount *m = &vfs.mounts[dir->mnt_cursor - 1];
        dir->mnt_cursor++;
        if (!m->used || m->len == 1) continue;
        const char *name;
        if (dl == 1) name = m->path + 1;
        else if (strncmp(m->path, dir->path, dl) == 0 && m->path[dl] == '/') name = m->path + dl + 1;
        else continue;
        if (strchr(name, '/')) continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, name, VFS_NAME_MAX - 1);
        out->type = VFS_TYPE_DIR;
        return 1;
    }
    return 0;
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    char norm[VFS_PATH_MAX];
    int r = normalize(path, norm);
    if (r) return r;
    const char *rel;
    struct vfs_mount *m = resolve(norm, &rel);
    if (!m) return VFS_ENOENT;
    memset(st, 0, sizeof *st);
    r = m->ops->stat(m->fs, rel, st);
    if (!r) st->readonly = m->readonly;
    return r;
}

static int modify_op(const char *path, struct vfs_mount **mo, const char **relo, char *norm) {
    int r = normalize(path, norm);
    if (r) return r;
    struct vfs_mount *m = resolve(norm, relo);
    if (!m) return VFS_ENOENT;
    if (m->readonly) return VFS_EROFS;
    if (**relo == 0 || is_mountpoint(norm)) return VFS_EBUSY;
    *mo = m;
    return 0;
}

int vfs_mkdir(const char *path) {
    char norm[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = modify_op(path, &m, &rel, norm);
    if (r == VFS_EBUSY) return VFS_EEXIST;
    if (r) return r;
    if (!m->ops->mkdir) return VFS_EROFS;
    return m->ops->mkdir(m->fs, rel);
}

int vfs_create(const char *path) {
    struct vfs_file *f;
    int r = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL, &f);
    if (r) return r;
    return vfs_close(f);
}

int vfs_unlink(const char *path) {
    char norm[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = modify_op(path, &m, &rel, norm);
    if (r) return r;
    if (!m->ops->unlink) return VFS_EROFS;
    return m->ops->unlink(m->fs, rel);
}

int64_t vfs_read_all(const char *path, void **out_buf) {
    *out_buf = NULL;
    struct vfs_file *f;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r) return r;
    if (f->type != VFS_TYPE_FILE) { vfs_close(f); return VFS_EISDIR; }
    uint64_t size = f->size;
    char *buf = kmalloc(size + 1);
    if (!buf) { vfs_close(f); return VFS_ENOMEM; }
    uint64_t got = 0;
    while (got < size) {
        int64_t n = vfs_read(f, buf + got, (size_t)(size - got));
        if (n <= 0) {
            kfree(buf);
            vfs_close(f);
            return n < 0 ? n : VFS_EIO;
        }
        got += (uint64_t)n;
    }
    buf[size] = 0;
    vfs_close(f);
    *out_buf = buf;
    return (int64_t)size;
}

const char *vfs_strerror(int err) {
    switch (err) {
    case VFS_OK: return "ok";
    case VFS_ENOENT: return "no such file or directory";
    case VFS_EIO: return "I/O error";
    case VFS_ENOMEM: return "out of memory";
    case VFS_EBUSY: return "busy";
    case VFS_EEXIST: return "already exists";
    case VFS_ENOTDIR: return "not a directory";
    case VFS_EISDIR: return "is a directory";
    case VFS_EINVAL: return "invalid argument";
    case VFS_EMFILE: return "too many open files";
    case VFS_ENOSPC: return "no space left on device";
    case VFS_EROFS: return "read-only file system";
    case VFS_ENAMETOOLONG: return "name too long";
    case VFS_ENOTEMPTY: return "directory not empty";
    case VFS_ENOSYS: return "not supported";
    default: return "unknown error";
    }
}
