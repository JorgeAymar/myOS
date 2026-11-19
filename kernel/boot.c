// Limine requests and accessors. Owner: core.
#include "boot.h"
#include "panic.h"
#include "string.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void boot_check(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("limine base revision not supported");
    if (!fb_req.response || fb_req.response->framebuffer_count < 1)
        panic("no framebuffer");
    if (!memmap_req.response) panic("no memory map");
    if (!hhdm_req.response) panic("no HHDM");
}

struct limine_framebuffer *boot_framebuffer(void) { return fb_req.response->framebuffers[0]; }
struct limine_memmap_response *boot_memmap(void) { return memmap_req.response; }
uint64_t boot_hhdm_offset(void) { return hhdm_req.response->offset; }

const char *boot_cmdline(void) {
    if (!cmdline_req.response || !cmdline_req.response->cmdline) return "";
    return cmdline_req.response->cmdline;
}

bool boot_is_test(void) { return strcmp(boot_cmdline(), "test") == 0; }
