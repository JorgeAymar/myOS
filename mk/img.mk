# stb_image (third_party/stb): vendored single header, built with relaxed warnings.
STB_DIR := third_party/stb
STB_OBJ := $(BUILD)/$(STB_DIR)/stb_impl.c.o
STB_CFLAGS := $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w -Os -I$(STB_DIR)/shim -I$(STB_DIR)

$(STB_OBJ): $(STB_DIR)/stb_impl.c $(STB_DIR)/stb_image.h
	@mkdir -p $(dir $@)
	$(CC) $(STB_CFLAGS) -c $< -o $@

KERNEL_LIBS += $(STB_OBJ)
$(KERNEL): $(STB_OBJ)
-include $(STB_OBJ:.o=.d)
