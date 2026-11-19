# Ring 3 programs (user/*.c) built as static ELFs and embedded in the kernel
# by kernel/proc/user_blobs.asm (incbin).
.DEFAULT_GOAL := all

USER_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
               -fno-PIC -fno-pie -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
               -fno-asynchronous-unwind-tables -Iuser
USER_PROGS  := hello

$(BUILD)/user/%.o: user/%.c user/syscall.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o user/user.ld
	$(LD) -nostdlib -static -z noexecstack -z max-page-size=0x1000 -T user/user.ld $< -o $@

$(BUILD)/kernel/proc/user_blobs.asm.o: $(patsubst %,$(BUILD)/user/%.elf,$(USER_PROGS))
