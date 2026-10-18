/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Generic call capture (runtime/call_capture.h).

   Each wrapped target gets a stub, `movabs r11, thunk; jmp rt_call_capture_thunk`; r11 is scratch
   in the System V convention and carries no argument. The shared thunk saves the argument
   registers, lets rt_call_capture_enter decide whether this call is recorded, takes the caller's
   return address off the stack (onto a per-thread shadow stack) and calls the target, so the
   target sees its stack arguments exactly where the caller put them. When the target returns,
   rt_call_capture_leave writes the record and hands back the caller's return address. */
#define _GNU_SOURCE
#include "runtime/call_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "The call capture thunk requires an x86-64 host"
#endif

enum { FIRST = 50, EVERY = 1000, MAX_RECORDS = 200 };
enum { DEPTH = 1024, SAMPLED = 32, STUB_SIZE = 32, STUB_PAGE = 1 << 16 };

typedef struct {
    char *name;
    char *path;   /* the run's record file for this name */
    void *target;
    atomic_uint calls;
    atomic_int recorded;
} thunk;

/* The thunk's save areas; the offsets are the assembly's. */
typedef struct {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, unused;
    unsigned char ymm[8][32];
} entry_registers;

typedef struct {
    uint64_t rax, rdx;
    unsigned char ymm[2][32];
} return_registers;

typedef struct {
    const thunk *t;
    uint64_t from;  /* the caller's return address */
    int sample;     /* slot in samples[], or -1 when this call is not recorded */
    unsigned call;
    int number;
} frame;

typedef struct {
    unsigned depth, sampled;
    frame frames[DEPTH];
    entry_registers samples[SAMPLED];
} shadow_stack;

void rt_call_capture_thunk(void) __attribute__((visibility("hidden")));
void *rt_call_capture_enter(thunk *t, uint64_t from, const entry_registers *in) __attribute__((visibility("hidden")));
uint64_t rt_call_capture_leave(const return_registers *out) __attribute__((visibility("hidden")));

__asm__(".text\n"
        ".globl rt_call_capture_thunk\n"
        ".hidden rt_call_capture_thunk\n"
        ".type rt_call_capture_thunk, @function\n"
        "rt_call_capture_thunk:\n"
        "  push %rbp\n"
        "  mov %rsp, %rbp\n"
        "  and $-16, %rsp\n"       /* the helpers get an aligned stack whatever the caller's */
        "  sub $320, %rsp\n"
        "  mov %rdi, 0(%rsp)\n  mov %rsi, 8(%rsp)\n  mov %rdx, 16(%rsp)\n  mov %rcx, 24(%rsp)\n"
        "  mov %r8, 32(%rsp)\n  mov %r9, 40(%rsp)\n  mov %rax, 48(%rsp)\n"
        "  vmovdqu %ymm0, 64(%rsp)\n  vmovdqu %ymm1, 96(%rsp)\n  vmovdqu %ymm2, 128(%rsp)\n"
        "  vmovdqu %ymm3, 160(%rsp)\n  vmovdqu %ymm4, 192(%rsp)\n  vmovdqu %ymm5, 224(%rsp)\n"
        "  vmovdqu %ymm6, 256(%rsp)\n  vmovdqu %ymm7, 288(%rsp)\n"
        "  mov %r11, %rdi\n"
        "  mov 8(%rbp), %rsi\n"
        "  mov %rsp, %rdx\n"
        "  call rt_call_capture_enter\n"
        "  mov %rax, %r11\n"
        "  mov 0(%rsp), %rdi\n  mov 8(%rsp), %rsi\n  mov 16(%rsp), %rdx\n  mov 24(%rsp), %rcx\n"
        "  mov 32(%rsp), %r8\n  mov 40(%rsp), %r9\n  mov 48(%rsp), %rax\n"
        "  vmovdqu 64(%rsp), %ymm0\n  vmovdqu 96(%rsp), %ymm1\n  vmovdqu 128(%rsp), %ymm2\n"
        "  vmovdqu 160(%rsp), %ymm3\n  vmovdqu 192(%rsp), %ymm4\n  vmovdqu 224(%rsp), %ymm5\n"
        "  vmovdqu 256(%rsp), %ymm6\n  vmovdqu 288(%rsp), %ymm7\n"
        "  leave\n"
        "  add $8, %rsp\n"          /* the return address is on the shadow stack now */
        "  call *%r11\n"
        "  sub $8, %rsp\n"          /* the return address goes back into its own slot */
        "  push %rbp\n"
        "  mov %rsp, %rbp\n"
        "  and $-16, %rsp\n"
        "  sub $80, %rsp\n"
        "  mov %rax, 0(%rsp)\n  mov %rdx, 8(%rsp)\n"
        "  vmovdqu %ymm0, 16(%rsp)\n  vmovdqu %ymm1, 48(%rsp)\n"
        "  mov %rsp, %rdi\n"
        "  call rt_call_capture_leave\n"
        "  mov %rax, 8(%rbp)\n"
        "  mov 0(%rsp), %rax\n  mov 8(%rsp), %rdx\n"
        "  vmovdqu 16(%rsp), %ymm0\n  vmovdqu 48(%rsp), %ymm1\n"
        "  leave\n"
        "  ret\n"
        ".size rt_call_capture_thunk, .-rt_call_capture_thunk\n");

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t shadow_key;
static __thread shadow_stack *shadow;
static pthread_mutex_t wrap_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *stubs;
static size_t stubs_used = STUB_PAGE;
static char run_id[64];
static atomic_int warned;

static void warn_once(const char *message, const char *path)
{
    if (!atomic_exchange(&warned, 1))
        fprintf(stderr, "call capture: %s%s%s\n", message, path ? ": " : "", path ? path : "");
}

static void free_shadow(void *s) { free(s); }

/* BB_CAPTURE_RUN when it is a safe file name component, else start time and process id. */
static void initialize(void)
{
    pthread_key_create(&shadow_key, free_shadow);
    const char *env = getenv("BB_CAPTURE_RUN");
    int safe = env && *env && strcmp(env, ".") && strcmp(env, "..") && strlen(env) < sizeof run_id;
    for (const char *p = env; safe && *p; p++)
        safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
               *p == '-' || *p == '.';
    if (safe) {
        snprintf(run_id, sizeof run_id, "%s", env);
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm))
        memset(&tm, 0, sizeof tm);
    snprintf(run_id, sizeof run_id, "%04d%02d%02d-%02d%02d%02d-%d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)getpid());
}

int rt_call_capture_enabled(void)
{
    const char *dir = getenv("BB_CALL_CAPTURE_DIR");
    return dir && *dir;
}

static int wanted(const char *name)
{
    const char *only = getenv("BB_CALL_CAPTURE_ONLY");
    if (!only || !*only)
        return 1;
    const size_t len = strlen(name);
    for (const char *p = only; *p;) {
        const size_t n = strcspn(p, ",");
        if (n == len && !strncmp(p, name, n))
            return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

/* mkdir -p */
static int make_dirs(char *path)
{
    struct stat st;
    for (char *p = path + 1;; p++) {
        if (*p != '/' && *p)
            continue;
        const char c = *p;
        *p = 0;
        const int failed = mkdir(path, 0700) && (errno != EEXIST || stat(path, &st) || !S_ISDIR(st.st_mode));
        *p = c;
        if (failed)
            return -1;
        if (!c)
            return 0;
    }
}

/* The record file of `name`, its directory created: names may hold any character, the directory
   only [A-Za-z0-9_.-] (others become '_'). */
static char *record_path(const char *name)
{
    const char *dir = getenv("BB_CALL_CAPTURE_DIR");
    const size_t size = strlen(dir) + strlen(name) + strlen(run_id) + 16;
    char *path = malloc(size);
    if (!path)
        return NULL;
    int n = snprintf(path, size, "%s/", dir);
    for (const char *p = name; *p; p++) {
        const char c = *p;
        const int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                         c == '-' || (c == '.' && p != name);
        path[n++] = safe ? c : '_';
    }
    path[n] = 0;
    if (make_dirs(path)) {
        warn_once("cannot create the record directory; calls are not recorded", path);
        free(path);
        return NULL;
    }
    snprintf(path + n, size - n, "/%s.calls.jsonl", run_id);
    return path;
}

void *rt_call_capture_wrap(const char *name, void *target)
{
    if (!target || !name || !*name || !rt_call_capture_enabled() || !wanted(name))
        return target;
    if (!__builtin_cpu_supports("avx")) {
        warn_once("the host has no AVX; calls are not recorded", NULL);
        return target;
    }
    pthread_once(&once, initialize);
    thunk *t = calloc(1, sizeof *t);
    if (!t || !(t->name = strdup(name)) || !(t->path = record_path(name))) {
        if (t)
            free(t->name);
        free(t);
        return target;
    }
    t->target = target;
    pthread_mutex_lock(&wrap_lock);
    if (stubs_used + STUB_SIZE > STUB_PAGE) {
        void *page = mmap(NULL, STUB_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            pthread_mutex_unlock(&wrap_lock);
            warn_once("cannot map thunk memory; calls are not recorded", NULL);
            free(t->path);
            free(t->name);
            free(t);
            return target;
        }
        stubs = page;
        stubs_used = 0;
    }
    unsigned char *stub = stubs + stubs_used;
    stubs_used += STUB_SIZE;
    pthread_mutex_unlock(&wrap_lock);
    const uint64_t data = (uint64_t)(uintptr_t)t, code = (uint64_t)(uintptr_t)rt_call_capture_thunk;
    unsigned char *p = stub;
    *p++ = 0x49, *p++ = 0xbb, memcpy(p, &data, 8), p += 8;                       /* movabs r11, thunk */
    *p++ = 0xff, *p++ = 0x25, memset(p, 0, 4), p += 4, memcpy(p, &code, 8);      /* jmp [rip]; .quad */
    __builtin___clear_cache((char *)stub, (char *)stub + STUB_SIZE);
    return stub;
}

void *rt_call_capture_enter(thunk *t, uint64_t from, const entry_registers *in)
{
    const int saved_errno = errno;
    shadow_stack *s = shadow;
    if (!s) {
        s = shadow = calloc(1, sizeof *s);
        if (!s) {
            fprintf(stderr, "call capture: %s: no memory for the shadow stack\n", t->name);
            abort();
        }
        pthread_setspecific(shadow_key, s);
    }
    if (s->depth == DEPTH) {
        fprintf(stderr, "call capture: %s: calls nested deeper than %d\n", t->name, DEPTH);
        abort();
    }
    frame *f = &s->frames[s->depth++];
    f->t = t;
    f->from = from;
    f->sample = -1;
    const unsigned call = atomic_fetch_add(&t->calls, 1);
    if ((call < FIRST || call % EVERY == 0) && s->sampled < SAMPLED && atomic_load(&t->recorded) < MAX_RECORDS) {
        const int number = atomic_fetch_add(&t->recorded, 1);
        if (number < MAX_RECORDS) {
            f->sample = (int)s->sampled++;
            f->call = call;
            f->number = number;
            s->samples[f->sample] = *in;
        }
    }
    errno = saved_errno;
    return t->target;
}

static int hex(char *out, size_t size, const unsigned char *bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    if (size < 2 * n + 1)
        return -1;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = 0;
    return 0;
}

static void write_record(const frame *f, const entry_registers *in, const return_registers *out)
{
    char line[4096], name[512], vector[65];
    size_t n = 0;
    for (const char *p = f->t->name; *p && n + 8 < sizeof name; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
            name[n++] = '\\', name[n++] = (char)c;
        else if (c < 0x20)
            n += (size_t)snprintf(name + n, sizeof name - n, "\\u%04x", c);
        else
            name[n++] = (char)c;
    }
    name[n] = 0;
    int len = snprintf(line, sizeof line,
                       "{\"name\": \"%s\", \"run_id\": \"%s\", \"call\": %u, \"record\": %d, \"from\": \"0x%llx\", "
                       "\"args\": {\"rdi\": \"0x%llx\", \"rsi\": \"0x%llx\", \"rdx\": \"0x%llx\", \"rcx\": \"0x%llx\", "
                       "\"r8\": \"0x%llx\", \"r9\": \"0x%llx\", \"al\": \"0x%llx\"",
                       name, run_id, f->call, f->number, (unsigned long long)f->from, (unsigned long long)in->rdi,
                       (unsigned long long)in->rsi, (unsigned long long)in->rdx, (unsigned long long)in->rcx,
                       (unsigned long long)in->r8, (unsigned long long)in->r9, (unsigned long long)(in->rax & 0xff));
    for (int i = 0; i < 8; i++) {
        hex(vector, sizeof vector, in->ymm[i], 32);
        len += snprintf(line + len, sizeof line - len, ", \"ymm%d\": \"%s\"", i, vector);
    }
    len += snprintf(line + len, sizeof line - len, "}, \"returns\": {\"rax\": \"0x%llx\", \"rdx\": \"0x%llx\"",
                    (unsigned long long)out->rax, (unsigned long long)out->rdx);
    for (int i = 0; i < 2; i++) {
        hex(vector, sizeof vector, out->ymm[i], 32);
        len += snprintf(line + len, sizeof line - len, ", \"ymm%d\": \"%s\"", i, vector);
    }
    len += snprintf(line + len, sizeof line - len, "}}\n");
    if (len <= 0 || len >= (int)sizeof line) {
        warn_once("record too long", f->t->path);
        return;
    }
    /* One write per record, appended: records of concurrent calls do not interleave. */
    const int fd = open(f->t->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, line, (size_t)len) != len)
        warn_once("cannot write a record", f->t->path);
    if (fd >= 0)
        close(fd);
}

uint64_t rt_call_capture_leave(const return_registers *out)
{
    const int saved_errno = errno;
    shadow_stack *s = shadow;
    if (!s || !s->depth) {
        fputs("call capture: a call returned that was never entered\n", stderr);
        abort();
    }
    const frame *f = &s->frames[--s->depth];
    if (f->sample >= 0) {
        write_record(f, &s->samples[f->sample], out);
        s->sampled--;
    }
    errno = saved_errno;
    return f->from;
}
