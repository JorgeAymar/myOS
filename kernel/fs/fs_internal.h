#pragma once
// Private to kernel/fs/.
#include "vfs.h"
#include "blockdev.h"

// Initrd (USTAR tar handed over by Limine as a module). Parses the archive and
// mounts it read-only at mount_path. Returns the number of entries (files and
// directories) or a negative error (VFS_ENOENT if no module was loaded).
int initrd_mount(const char *mount_path);
int initrd_file_count(void);

// FAT32 on a block device (superfloppy or first FAT32 MBR partition).
// Returns 0 and fills the info string on success.
int fat32_mount(struct blockdev *bd, const char *mount_path, char *info, size_t info_len);
