#pragma once
// Cross-app entry points (kernel/apps). Keep this the only coupling between apps.
#include <stddef.h>

// Opens the Editor window on the given absolute path (loads the file).
// Defined in kernel/apps/editor.c; called by Files.
void editor_open(const char *path);

// Shared clipboard (defined in kernel/apps/editor.c). Capped at 64 KiB.
// clipboard_set copies the bytes; clipboard_get returns a NUL-terminated
// string valid until the next clipboard_set (never NULL; *n = length).
void clipboard_set(const char *s, size_t n);
const char *clipboard_get(size_t *n);
