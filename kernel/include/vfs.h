#pragma once
// Virtual file system. Owner: fs agent (kernel/fs/).
//
// Kernel API without file descriptors: open a path, get a struct vfs_file *.
// Paths are absolute ("/disk/docs/a.txt"); "." , ".." and repeated slashes are
// normalized. Mounts: "/" = initrd (read-only USTAR tar), "/disk" = FAT32 on
// the first AHCI SATA disk (if present).
//
// All functions return 0 / a non-negative count on success and a negative
// VFS_E* code on failure. Not thread-safe yet: every entry point goes through
// vfs.c, which is the single place to add a lock later.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VFS_PATH_MAX 256
#define VFS_NAME_MAX 256          // including the NUL terminator

#define VFS_OK            0
#define VFS_ENOENT       -2
#define VFS_EIO          -5
#define VFS_ENOMEM      -12
#define VFS_EBUSY       -16
#define VFS_EEXIST      -17
#define VFS_ENOTDIR     -20
#define VFS_EISDIR      -21
#define VFS_EINVAL      -22
#define VFS_EMFILE      -24
#define VFS_ENOSPC      -28
#define VFS_EROFS       -30
#define VFS_ENAMETOOLONG -36
#define VFS_ENOTEMPTY   -39
#define VFS_ENOSYS      -38

enum { VFS_TYPE_FILE = 1, VFS_TYPE_DIR = 2 };

// vfs_open flags
#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_RDWR   (VFS_O_READ | VFS_O_WRITE)
#define VFS_O_CREATE 0x04         // create the file if it does not exist
#define VFS_O_TRUNC  0x08         // truncate to 0 bytes (needs VFS_O_WRITE)
#define VFS_O_APPEND 0x10         // every write goes to the end of the file
#define VFS_O_EXCL   0x20         // with CREATE: fail with EEXIST if it exists

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

struct vfs_stat {
    uint32_t type;                // VFS_TYPE_*
    uint64_t size;                // bytes (0 for directories)
    bool readonly;                // true if on a read-only mount
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];      // entry name only (no path), never "." or ".."
    uint32_t type;                // VFS_TYPE_*
    uint64_t size;
};

struct vfs_file;

void vfs_init(void);              // resets the mount table (called by fs_init)

int vfs_open(const char *path, int flags, struct vfs_file **out);
int vfs_close(struct vfs_file *f);
int64_t vfs_read(struct vfs_file *f, void *buf, size_t n);         // bytes read, 0 at EOF
int64_t vfs_write(struct vfs_file *f, const void *buf, size_t n);  // bytes written
int64_t vfs_seek(struct vfs_file *f, int64_t off, int whence);     // new position
int64_t vfs_tell(struct vfs_file *f);
uint64_t vfs_file_size(struct vfs_file *f);
uint32_t vfs_file_type(struct vfs_file *f);
// Directory iteration on a handle opened with vfs_open(dir, VFS_O_READ).
// Returns 1 and fills *out for each entry, 0 at the end. Mount points under
// the directory (e.g. "disk" in "/") are listed too.
int vfs_readdir(struct vfs_file *dir, struct vfs_dirent *out);

int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_mkdir(const char *path);
int vfs_create(const char *path); // empty regular file; VFS_EEXIST if it exists
int vfs_unlink(const char *path); // removes a file or an empty directory

const char *vfs_strerror(int err);
// Reads a whole file into a fresh kmalloc buffer (NUL-terminated, caller frees).
// Returns the size or a negative error.
int64_t vfs_read_all(const char *path, void **out_buf);

// ---------------------------------------------------------------------------
// Filesystem driver interface. A driver fills a vfs_fs_ops and calls
// vfs_mount(). "rel" paths are normalized and relative to the mount root:
// "" is the root itself, components are separated by single '/'.
struct vfs_fs_ops {
    const char *name;
    // Opens rel. Must set f->type, f->size and f->priv. Handles CREATE/TRUNC/EXCL.
    int (*open)(void *fs, const char *rel, int flags, struct vfs_file *f);
    int64_t (*read)(struct vfs_file *f, uint64_t off, void *buf, size_t n);
    int64_t (*write)(struct vfs_file *f, uint64_t off, const void *buf, size_t n); // updates f->size
    int (*readdir)(struct vfs_file *f, struct vfs_dirent *out);  // cursor in f->dir_pos
    void (*close)(struct vfs_file *f);
    int (*stat)(void *fs, const char *rel, struct vfs_stat *st);
    int (*mkdir)(void *fs, const char *rel);                     // NULL = read-only fs
    int (*unlink)(void *fs, const char *rel);
};

struct vfs_mount;

struct vfs_file {
    struct vfs_mount *mnt;
    void *fs;                     // driver instance passed to vfs_mount
    void *priv;                   // driver per-file state
    uint32_t type;
    int flags;
    uint64_t pos;
    uint64_t size;
    uint64_t dir_pos;             // driver readdir cursor
    int mnt_cursor;               // vfs readdir cursor over child mount points
    char path[VFS_PATH_MAX];      // normalized absolute path
};

// Mounts a filesystem at an absolute path ("/" first). readonly makes the VFS
// reject writes with VFS_EROFS before reaching the driver.
int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *fs, bool readonly);
int vfs_mount_count(void);
