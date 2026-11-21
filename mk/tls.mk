# BearSSL (phase 13): vendored in third_party/bearssl, built as a static archive
# with the kernel's freestanding flags but WITHOUT -Werror/-Wall (third-party code).
# Linking the archive pulls in only the members that are referenced.
BEARSSL_DIR := third_party/bearssl
# Exclude x86 SIMD/AES-NI/PCLMUL, POWER8 and OS-RNG implementations (kernel has no SSE),
# plus 'big'/'small' duplicates are kept (archive: unused members cost nothing).
BEARSSL_EXCL := ghash_pclmul aes_x86ni aes_x86ni_cbcdec aes_x86ni_cbcenc aes_x86ni_ctr \
                aes_x86ni_ctrcbc chacha20_sse2 ghash_pwr8 aes_pwr8 aes_pwr8_cbcdec \
                aes_pwr8_cbcenc aes_pwr8_ctr aes_pwr8_ctrcbc sysrng
BEARSSL_SRCS := $(filter-out $(addprefix %/,$(addsuffix .c,$(BEARSSL_EXCL))), \
                $(shell find $(BEARSSL_DIR)/src -name '*.c'))
BEARSSL_OBJS := $(patsubst %,$(BUILD)/%.o,$(BEARSSL_SRCS))
BEARSSL_LIB  := $(BUILD)/libbearssl.a
BEARSSL_CFLAGS := $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w -Os \
    -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src \
    -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0 \
    -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0 -DBR_SSE2=0 -DBR_AES_X86NI=0 -DBR_POWER8=0

$(BEARSSL_OBJS): $(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	rm -f $@
	x86_64-elf-ar rcs $@ $^

KERNEL_LIBS += $(BEARSSL_LIB)
$(KERNEL): $(BEARSSL_LIB)
-include $(BEARSSL_OBJS:.o=.d)

# kernel/ code that includes BearSSL headers
$(BUILD)/kernel/%.c.o: CFLAGS += -I$(BEARSSL_DIR)/inc
