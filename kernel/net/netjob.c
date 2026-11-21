// Asynchronous network jobs: run one blocking HTTP GET or ping on a worker
// kernel thread so the GUI thread never blocks. Only one job at a time.
// The worker holds net_lock while it drives the (non thread-safe) stack;
// the desktop uses net_poll_try() which skips when the worker holds the lock.
#include "net_internal.h"
#include "net.h"
#include "proc.h"
#include "mm.h"
#include "string.h"
#include "tls.h"

// The worker thread gets a 128 KiB stack (thread_create_sized): TLS handshakes
// (BearSSL x509/RSA/EC code) need far more than the default 16 KiB. Even so,
// big BearSSL contexts (br_ssl_client_context, x509 minimal context, I/O buffers,
// ~25-30 KB+) must NOT live on this stack: tls.c allocates them with kmalloc.
#define NETJOB_STACK_SIZE (128 * 1024)

static struct net_job job;
static mutex_t net_lock = MUTEX_INIT;

void net_poll_try(void) {
    if (!mutex_trylock(&net_lock)) return;
    net_poll();
    mutex_unlock(&net_lock);
}

static void worker(void *arg) {
    struct net_job *j = arg;
    mutex_lock(&net_lock);
    if (j->kind == NJ_HTTP) {
        j->result = j->https
            ? https_get_timeout(j->host, j->path, j->buf, j->buflen, j->timeout_ms)
            : http_get_timeout(j->host, j->path, j->buf, j->buflen, j->timeout_ms);
        j->length = j->result > 0 ? (size_t)j->result : 0;
    } else {
        uint32_t ip = 0;
        int r = 0;
        if (!ip_parse(j->host, &ip)) r = dns_resolve(j->host, &ip, j->timeout_ms);
        j->ip = ip;
        j->result = r < 0 ? r : net_ping(ip, j->timeout_ms);
        j->resolve_failed = r < 0;
    }
    mutex_unlock(&net_lock);
    __asm__ volatile ("" ::: "memory");
    j->state = NJ_DONE;
}

static bool start(const void *owner, enum net_job_kind kind, const char *host,
                  const char *path, size_t buflen, uint32_t timeout_ms, bool https) {
    if (job.state != NJ_IDLE) return false;
    memset(&job, 0, sizeof job);
    if (kind == NJ_HTTP) {
        job.buf = kmalloc(buflen);
        if (!job.buf) return false;
        job.buf[0] = 0;
        job.buflen = buflen;
    }
    job.owner = owner;
    job.kind = kind;
    job.timeout_ms = timeout_ms;
    job.https = https;
    strncpy(job.host, host, sizeof job.host - 1);
    if (path) strncpy(job.path, path, sizeof job.path - 1);
    job.state = NJ_RUNNING;
    if (thread_create_sized("netjob", worker, &job, NETJOB_STACK_SIZE) < 0) {
        kfree(job.buf);
        job.buf = NULL;
        job.state = NJ_IDLE;
        return false;
    }
    return true;
}

bool net_job_start_http(const void *owner, const char *host, const char *path,
                        size_t buflen, uint32_t timeout_ms) {
    return start(owner, NJ_HTTP, host, path, buflen, timeout_ms, false);
}

bool net_job_start_https(const void *owner, const char *host, const char *path,
                         size_t buflen, uint32_t timeout_ms) {
    return start(owner, NJ_HTTP, host, path, buflen, timeout_ms, true);
}

bool net_job_start_ping(const void *owner, const char *host, uint32_t timeout_ms) {
    return start(owner, NJ_PING, host, NULL, 0, timeout_ms, false);
}

bool net_job_busy(void) { return job.state != NJ_IDLE; }

struct net_job *net_job_done(const void *owner) {
    return (job.state == NJ_DONE && job.owner == owner && !job.cancelled) ? &job : NULL;
}

bool net_job_running(const void *owner) {
    return job.state == NJ_RUNNING && job.owner == owner && !job.cancelled;
}

void net_job_release(void) {
    if (job.state != NJ_DONE) return;
    kfree(job.buf);
    job.buf = NULL;
    job.state = NJ_IDLE;
}

void net_job_cancel(const void *owner) {
    if (job.state == NJ_IDLE || job.owner != owner) return;
    if (job.state == NJ_DONE) net_job_release();
    else job.cancelled = true;       // worker keeps its buffer; freed by net_job_poll()
}

void net_job_poll(void) {
    if (job.state == NJ_DONE && job.cancelled) net_job_release();
}
