#pragma once
// Init entry points of subsystems wired into kmain. Each owner defines the real
// function; kernel/lib/weak_stubs.c provides no-op fallbacks.
//   sched_init : kernel/proc/  (threads, scheduler, later userspace)
//   fs_init    : kernel/fs/    (VFS, initrd, block devices, FAT32)
//   net_init   : kernel/net/   (NIC driver + TCP/IP stack)
void sched_init(void);
void fs_init(void);
void net_init(void);
void html_selftest(void);  // kernel/apps/html_selftest.c (layout engine robustness)
void tls_selftest(void);   // kernel/lib/tls_selftest.c (BearSSL known-answer checks)
void img_selftest(void);   // kernel/gfx/img_selftest.c (stb_image decode + downscale checks)
void css_selftest(void);   // kernel/apps/css_selftest.c (CSS engine checks)
