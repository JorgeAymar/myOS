#pragma once
// Init entry points of subsystems wired into kmain. Each owner defines the real
// function; kernel/lib/weak_stubs.c provides no-op fallbacks.
//   sched_init : kernel/proc/  (threads, scheduler, later userspace)
//   fs_init    : kernel/fs/    (VFS, initrd, block devices, FAT32)
//   net_init   : kernel/net/   (NIC driver + TCP/IP stack)
void sched_init(void);
void fs_init(void);
void net_init(void);
