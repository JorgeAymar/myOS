myOS initial ramdisk
====================

This read-only tree is packed into build/initrd.tar (USTAR) at build time,
loaded by Limine as a boot module and mounted by the kernel at "/".

  /README.txt         this file
  /motd.txt           message of the day
  /docs/              documentation
  /disk/              FAT32 volume on the first SATA disk (read-write)
