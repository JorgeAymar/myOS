# Storage / filesystems (phase 10). Owner: fs agent.
#  - build/initrd.tar : USTAR archive of initrd/, copied to /boot/initrd.tar in the
#                       ISO and loaded by Limine as a module (limine*.conf module_path)
#  - build/disk.img   : 64 MiB FAT32 disk (needs mtools: brew install mtools),
#                       attached as SATA disk on the q35 AHCI controller (port 0)
#  - make test boots on a fresh copy (build/disk-test.img), so tests are repeatable.

# This file is included before the Makefile's `all` rule: keep `make` = ISO.
.DEFAULT_GOAL := all

INITRD       := $(BUILD)/initrd.tar
FS_DISK      := $(BUILD)/disk.img
FS_TEST_DISK := $(BUILD)/disk-test.img
INITRD_SRCS  := $(shell find initrd 2>/dev/null)

ISO_EXTRA_FILES += $(INITRD)
TEST_DEPS       += $(FS_DISK) fs-test-disk

fs_drive = -boot order=d -drive file=$(1),format=raw,if=none,id=fsdisk0 -device ide-hd,drive=fsdisk0,bus=ide.0

test: QEMU_EXTRA += $(call fs_drive,$(FS_TEST_DISK))
run run-uefi screenshot debug: QEMU_EXTRA += $(call fs_drive,$(FS_DISK))
run run-uefi screenshot debug: $(FS_DISK)

$(INITRD): $(INITRD_SRCS) mk/fs.mk
	@mkdir -p $(dir $@)
	cd initrd && COPYFILE_DISABLE=1 tar --format=ustar -cf ../$@.tmp .
	mv $@.tmp $@

$(FS_DISK): tools/mkdisk.sh
	@mkdir -p $(dir $@)
	tools/mkdisk.sh $@ 64

.PHONY: fs-test-disk disk
fs-test-disk: $(FS_DISK)
	cp $(FS_DISK) $(FS_TEST_DISK)

# Recreate the disk image from scratch (discards anything written to it).
disk:
	rm -f $(FS_DISK)
	tools/mkdisk.sh $(FS_DISK) 64
