// fs_init: mounts the initrd at "/" and FAT32 (first SATA disk) at "/disk",
// then runs boot self-tests. Owner: fs agent.
#include "fs_internal.h"
#include "subsystems.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"
#include "panic.h"
#include "boot.h"

#define SELFTEST_INITRD_FILE "/docs/selftest.txt"
#define SELFTEST_INITRD_TEXT "myOS initrd self-test v1\n"
#define SELFTEST_HELLO_FILE  "/disk/HELLO.TXT"
#define SELFTEST_HELLO_TEXT  "Hello from the myOS FAT32 disk!\n"
#define SELFTEST_LFN_FILE    "/disk/docs/A long file name.txt"
#define SELFTEST_LFN_TEXT    "Long file names work.\n"
#define SELFTEST_DIR         "/disk/selftest"
#define SELFTEST_BIG         "/disk/selftest/Big Data File.bin"
#define SELFTEST_SMALL       "/disk/selftest/small.txt"

static void check(int r, const char *what) {
    if (r < 0) panic("fs selftest: %s: %s (%d)", what, vfs_strerror(r), r);
}

static void expect_file(const char *path, const char *text) {
    void *buf;
    int64_t n = vfs_read_all(path, &buf);
    check((int)n, path);
    size_t len = strlen(text);
    if ((size_t)n != len || memcmp(buf, text, len) != 0)
        panic("fs selftest: %s content mismatch (%ld bytes)", path, (long)n);
    kfree(buf);
}

// Lists a directory to serial; returns the entry count. Sets *found if `name` is present.
static int list_dir(const char *path, const char *name, bool *found) {
    struct vfs_file *d;
    check(vfs_open(path, VFS_O_READ, &d), path);
    struct vfs_dirent ent;
    int count = 0, r;
    if (found) *found = false;
    kprintf("[fs] ls %s:", path);
    while ((r = vfs_readdir(d, &ent)) > 0) {
        kprintf(" %s%s", ent.name, ent.type == VFS_TYPE_DIR ? "/" : "");
        if (found && name && strcmp(ent.name, name) == 0) *found = true;
        count++;
    }
    kprintf("\n");
    check(r, "readdir");
    vfs_close(d);
    return count;
}

// Removes a directory tree (used to clean up after an interrupted self-test).
static void remove_tree(const char *path, int depth) {
    struct vfs_stat st;
    if (depth > 8 || vfs_stat(path, &st) != 0) return;
    if (st.type == VFS_TYPE_DIR) {
        struct vfs_file *d;
        struct vfs_dirent *ent = kmalloc(sizeof *ent);
        char *child = kmalloc(VFS_PATH_MAX);
        if (!ent || !child) { kfree(ent); kfree(child); return; }
        while (vfs_open(path, VFS_O_READ, &d) == 0) {
            int r = vfs_readdir(d, ent);
            vfs_close(d);
            if (r <= 0) break;
            ksnprintf(child, VFS_PATH_MAX, "%s/%s", path, ent->name);
            remove_tree(child, depth + 1);
            if (vfs_stat(child, &st) == 0) break;      // could not remove: give up
        }
        kfree(ent);
        kfree(child);
    }
    vfs_unlink(path);
}

static uint8_t pattern(uint64_t i) { return (uint8_t)(i * 7 + (i >> 8) * 13 + 0x5a); }

static void initrd_selftest(void) {
    expect_file(SELFTEST_INITRD_FILE, SELFTEST_INITRD_TEXT);
    struct vfs_stat st;
    check(vfs_stat("/docs", &st), "stat /docs");
    if (st.type != VFS_TYPE_DIR) panic("fs selftest: /docs is not a directory");
    check(vfs_stat("/docs/../README.txt", &st), "stat /README.txt");
    if (st.type != VFS_TYPE_FILE || !st.readonly) panic("fs selftest: bad stat for /README.txt");
    if (vfs_create("/new.txt") != VFS_EROFS) panic("fs selftest: initrd accepted a write");
    if (vfs_stat("/nope", &st) != VFS_ENOENT) panic("fs selftest: missing file found");
}

static void fat32_selftest(void) {
    bool found;
    int n = list_dir("/", "disk", &found);
    if (!found) panic("fs selftest: mount point /disk not listed in /");
    n = list_dir("/disk", "HELLO.TXT", &found);
    if (!found) panic("fs selftest: HELLO.TXT not listed in /disk");
    expect_file(SELFTEST_HELLO_FILE, SELFTEST_HELLO_TEXT);
    expect_file("/disk/hello.txt", SELFTEST_HELLO_TEXT);          // case-insensitive lookup
    expect_file(SELFTEST_LFN_FILE, SELFTEST_LFN_TEXT);

    // Leftovers from an interrupted earlier run.
    remove_tree(SELFTEST_DIR, 0);

    check(vfs_mkdir(SELFTEST_DIR), "mkdir");
    if (vfs_mkdir(SELFTEST_DIR) != VFS_EEXIST) panic("fs selftest: mkdir twice succeeded");

    // Write > 1 cluster of patterned data in odd-sized chunks.
    struct vfs_file *f;
    check(vfs_open(SELFTEST_BIG, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f), "create big");
    const uint64_t total = 3 * 4096 + 1234;      // > 1 cluster for any cluster size <= 4 KiB
    uint8_t *buf = kmalloc(total);
    if (!buf) panic("fs selftest: out of memory");
    for (uint64_t i = 0; i < total; i++) buf[i] = pattern(i);
    for (uint64_t off = 0; off < total;) {
        size_t chunk = total - off < 1000 ? (size_t)(total - off) : 1000;
        int64_t w = vfs_write(f, buf + off, chunk);
        check((int)w, "write");
        if ((size_t)w != chunk) panic("fs selftest: short write");
        off += chunk;
    }
    vfs_close(f);

    struct vfs_stat st;
    check(vfs_stat(SELFTEST_BIG, &st), "stat big");
    if (st.size != total || st.type != VFS_TYPE_FILE) panic("fs selftest: big file size %lu", (unsigned long)st.size);

    check(vfs_open(SELFTEST_BIG, VFS_O_READ, &f), "reopen big");
    memset(buf, 0, total);
    uint64_t got = 0;
    for (;;) {
        int64_t r = vfs_read(f, buf + got, 777);
        check((int)r, "read big");
        if (r == 0) break;
        got += (uint64_t)r;
    }
    if (got != total) panic("fs selftest: read back %lu of %lu bytes", (unsigned long)got, (unsigned long)total);
    for (uint64_t i = 0; i < total; i++)
        if (buf[i] != pattern(i)) panic("fs selftest: data mismatch at byte %lu", (unsigned long)i);
    // Random access in the middle, across a cluster boundary.
    uint8_t mid[64];
    check((int)vfs_seek(f, 4096 - 20, VFS_SEEK_SET), "seek");
    if (vfs_read(f, mid, sizeof mid) != (int64_t)sizeof mid) panic("fs selftest: seek read");
    for (unsigned i = 0; i < sizeof mid; i++)
        if (mid[i] != pattern(4096 - 20 + i)) panic("fs selftest: seek data mismatch");
    vfs_close(f);
    kfree(buf);

    // Overwrite in place + append.
    check(vfs_open(SELFTEST_BIG, VFS_O_RDWR | VFS_O_APPEND, &f), "append big");
    check((int)vfs_write(f, "TAIL", 4), "append");
    if (vfs_file_size(f) != total + 4) panic("fs selftest: append size");
    vfs_close(f);

    check(vfs_create(SELFTEST_SMALL), "create small");
    if (vfs_create(SELFTEST_SMALL) != VFS_EEXIST) panic("fs selftest: create twice succeeded");
    bool big_found, small_found;
    n = list_dir(SELFTEST_DIR, "Big Data File.bin", &big_found);
    list_dir(SELFTEST_DIR, "small.txt", &small_found);
    if (n != 2 || !big_found || !small_found) panic("fs selftest: readdir of %s wrong (%d entries)", SELFTEST_DIR, n);
    if (vfs_unlink(SELFTEST_DIR) != VFS_ENOTEMPTY) panic("fs selftest: removed non-empty dir");

    check(vfs_unlink(SELFTEST_BIG), "unlink big");
    check(vfs_unlink(SELFTEST_SMALL), "unlink small");
    if (list_dir(SELFTEST_DIR, NULL, NULL) != 0) panic("fs selftest: dir not empty after unlink");
    check(vfs_unlink(SELFTEST_DIR), "rmdir");
    if (vfs_stat(SELFTEST_DIR, &st) != VFS_ENOENT) panic("fs selftest: dir still exists");
    kprintf("[OK] fat32 selftest: read, write %lu bytes, verify, mkdir, readdir, unlink\n",
            (unsigned long)total);
}


// vfs_path_join and the string helpers added for the apps (no disk needed).
static void helpers_selftest(void) {
    char out[VFS_PATH_MAX];
    check(vfs_path_join("/a/b", "../c", out), "path_join ..");
    if (strcmp(out, "/a/c")) panic("fs selftest: path_join .. gave %s", out);
    check(vfs_path_join("/a", "/x//y/./z", out), "path_join abs");
    if (strcmp(out, "/x/y/z")) panic("fs selftest: path_join abs gave %s", out);
    char longrel[VFS_PATH_MAX * 2 + 8];
    memset(longrel, 'a', sizeof longrel - 1); longrel[sizeof longrel - 1] = 0;
    if (vfs_path_join("/", longrel, out) >= 0) panic("fs selftest: path_join accepted an overlong path");
    if (vfs_path_join("/", NULL, out) != VFS_EINVAL) panic("fs selftest: path_join accepted NULL");

    const char *h = "hello world";
    if (strstr(h, "") != h || strstr(h, "wor") != h + 6 || strstr(h, "xyz")) panic("fs selftest: strstr");
    if (strrchr(h, 0) != h + 11 || strrchr(h, 'o') != h + 7 || strrchr(h, 'z')) panic("fs selftest: strrchr");
    if (atoi("  -12x") != -12 || atoi("42") != 42 || atoi("") != 0) panic("fs selftest: atoi");
    if (strcasecmp("AbC", "aBc") != 0 || strcasecmp("a", "b") >= 0) panic("fs selftest: strcasecmp");
    kprintf("[OK] vfs path_join + string helpers\n");
}

static void put_file(const char *path, const char *text) {
    struct vfs_file *f;
    check(vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f), path);
    check((int)vfs_write(f, text, strlen(text)), "write");
    vfs_close(f);
}

// cp/mv guard rails: same-file, directory target, rename semantics.
static void vfs_ops_selftest(void) {
    const char *dir = "/disk/vops", *a = "/disk/vops/a.txt", *b = "/disk/vops/b.txt", *c = "/disk/vops/c.txt";
    struct vfs_stat st;
    check(vfs_mkdir(dir), "mkdir vops");
    put_file(a, "alpha");

    if (vfs_copy(a, a) != VFS_EINVAL) panic("fs selftest: copy onto itself not refused");
    if (vfs_copy(a, "/disk/vops/./a.txt") != VFS_EINVAL) panic("fs selftest: copy onto itself (alias) not refused");
    expect_file(a, "alpha");                                  // still intact
    if (vfs_rename(a, a) != VFS_EINVAL) panic("fs selftest: rename onto itself not refused");
    expect_file(a, "alpha");
    if (vfs_copy(a, dir) != VFS_EISDIR) panic("fs selftest: copy onto a directory not refused");

    check(vfs_copy(a, b), "copy a b");
    expect_file(b, "alpha");
    put_file(b, "beta");                                      // copy must overwrite an existing target
    check(vfs_copy(a, b), "copy over existing");
    expect_file(b, "alpha");

    check(vfs_rename(b, c), "rename b c");
    if (vfs_stat(b, &st) != VFS_ENOENT) panic("fs selftest: rename left the source");
    expect_file(c, "alpha");
    if (vfs_rename("/disk/vops/missing", "/disk/vops/d.txt") >= 0) panic("fs selftest: rename of a missing file succeeded");
    if (vfs_stat("/disk/vops/d.txt", &st) != VFS_ENOENT) panic("fs selftest: failed rename created its target");

    check(vfs_unlink(a), "unlink a");
    check(vfs_unlink(c), "unlink c");
    check(vfs_unlink(dir), "rmdir vops");
    kprintf("[OK] vfs copy/rename guards\n");
}

void fs_init(void) {
    vfs_init();
    int r = initrd_mount("/");
    if (r < 0) {
        kprintf("[WARN] fs: initrd not available (%s), empty root mounted\n", vfs_strerror(r));
    } else {
        initrd_selftest();
        helpers_selftest();
        list_dir("/", NULL, NULL);
        kprintf("[OK] fs initrd files=%d entries=%d\n", initrd_file_count(), r);
    }

    if (ahci_init() == 0) {
        kprintf("[WARN] fs: no SATA disk found, /disk not mounted\n");
        return;
    }
    char info[128];
    r = fat32_mount(blockdev_get(0), "/disk", info, sizeof info);
    if (r) {
        kprintf("[WARN] fs: no FAT32 volume on %s (%s), /disk not mounted\n",
                blockdev_get(0)->name, vfs_strerror(r));
        return;
    }
    kprintf("[OK] fat32 mounted /disk %s\n", info);
    // The write/verify test modifies the disk: only in test mode (make test uses a fresh copy).
    if (boot_is_test()) { fat32_selftest(); vfs_ops_selftest(); }
}
