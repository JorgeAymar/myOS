// Block device registry. Owner: fs agent.
#include "blockdev.h"

static struct {
    struct blockdev *devs[BLOCKDEV_MAX];
    int count;
} bdreg;

int blockdev_register(struct blockdev *bd) {
    if (bdreg.count >= BLOCKDEV_MAX) return -1;
    bdreg.devs[bdreg.count] = bd;
    return bdreg.count++;
}

int blockdev_count(void) { return bdreg.count; }

struct blockdev *blockdev_get(int i) {
    return (i >= 0 && i < bdreg.count) ? bdreg.devs[i] : 0;
}
