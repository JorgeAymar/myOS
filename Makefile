# myOS build system
# Targets: all (ISO), run, run-uefi, test, screenshot, debug, clean, deps

CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
NASM    := nasm
QEMU    := qemu-system-x86_64
BUILD   := build
ISO     := $(BUILD)/myos.iso
ISO_TEST := $(BUILD)/myos-test.iso
KERNEL  := $(BUILD)/kernel.elf
LIMINE_DIR    := limine
LIMINE_BRANCH := v9.x-binary
OVMF    := $(shell brew --prefix qemu 2>/dev/null)/share/qemu/edk2-x86_64-code.fd

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
          -fno-PIC -fno-pie -m64 -march=x86-64 \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
          -mcmodel=kernel -Ikernel/include -MMD -MP
LDFLAGS := -nostdlib -static -z noexecstack -z max-page-size=0x1000 -T kernel/linker.ld
NASMFLAGS := -f elf64 -g

C_SRCS   := $(shell find kernel -name '*.c')
ASM_SRCS := $(shell find kernel -name '*.asm')
OBJS := $(patsubst %,$(BUILD)/%.o,$(C_SRCS) $(ASM_SRCS))

# Subsystems add devices/prereqs in their own mk/<name>.mk:
#   QEMU_EXTRA += -nic ...      TEST_DEPS += build/disk.img
#   ISO_EXTRA_FILES += build/initrd.tar   (copied to /boot/ in the ISO)
QEMU_EXTRA :=
TEST_DEPS  :=
ISO_EXTRA_FILES :=
-include mk/*.mk

# Interactive window: on macOS scale the guest screen when the window is resized.
ifeq ($(shell uname -s),Darwin)
QEMU_DISPLAY := -display cocoa,zoom-to-fit=on
else
QEMU_DISPLAY :=
endif

QEMU_BASE = -M q35 -m 256M -no-reboot -rtc base=localtime $(QEMU_EXTRA)
QEMU_TEST = $(QEMU_BASE) -display none -serial stdio \
             -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all run run-uefi test screenshot debug clean distclean deps qa

all: $(ISO)

deps: $(LIMINE_DIR)/limine

$(LIMINE_DIR)/limine:
	@test -d $(LIMINE_DIR) || git clone --depth=1 --branch=$(LIMINE_BRANCH) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR) CC=cc

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# make_iso(conf, output)
define make_iso
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	cp $(1) $(BUILD)/iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	$(foreach f,$(ISO_EXTRA_FILES),cp $(f) $(BUILD)/iso_root/boot/;) true
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(2) 2>/dev/null
	$(LIMINE_DIR)/limine bios-install $(2) 2>/dev/null
endef

$(ISO): $(KERNEL) limine.conf $(LIMINE_DIR)/limine $(ISO_EXTRA_FILES)
	$(call make_iso,limine.conf,$@)

$(ISO_TEST): $(KERNEL) limine-test.conf $(LIMINE_DIR)/limine $(ISO_EXTRA_FILES)
	$(call make_iso,limine-test.conf,$@)

# Interactive window (BIOS boot)
run: $(ISO) $(TEST_DEPS)
	$(QEMU) $(QEMU_BASE) $(QEMU_DISPLAY) -cdrom $(ISO) -serial stdio

# Interactive window (UEFI boot via OVMF)
run-uefi: $(ISO) $(TEST_DEPS)
	$(QEMU) $(QEMU_BASE) $(QEMU_DISPLAY) -drive if=pflash,unit=0,format=raw,file=$(OVMF),readonly=on \
		-cdrom $(ISO) -serial stdio

# Headless automated boot test: must print "[TEST] boot OK" and exit 33
test: $(ISO_TEST) $(TEST_DEPS)
	@QEMU="$(QEMU)" QEMU_ARGS="$(QEMU_TEST)" ISO="$(ISO_TEST)" OVMF="$(OVMF)" tests/boot_test.sh

# Boot headless and save a PNG of the screen to build/screenshot.png
screenshot: $(ISO) $(TEST_DEPS)
	@QEMU="$(QEMU)" QEMU_ARGS="$(QEMU_BASE)" ISO="$(ISO)" tests/screenshot.sh

# Interactive UI QA: drives the desktop with real PS/2 events over QMP,
# screenshots each step into build/qa/
qa: $(ISO) build/disk.img
	python3 -I tools/qa_ui.py

# Start paused with a GDB stub on :1234 and interrupt/reset logging
debug: $(ISO)
	$(QEMU) $(QEMU_BASE) -cdrom $(ISO) -serial stdio -s -S -d int,cpu_reset -no-shutdown

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(LIMINE_DIR)

-include $(OBJS:.o=.d)
