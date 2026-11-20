#pragma once
// Cross-app entry points (kernel/apps). Keep this the only coupling between apps.

// Opens the Editor window on the given absolute path (loads the file).
// Defined in kernel/apps/editor.c; called by Files.
void editor_open(const char *path);
