// Minimal freestanding shim so stb_image.h can be compiled in the kernel.
#pragma once
#include <stddef.h>
#define abs(x) __builtin_abs(x)
