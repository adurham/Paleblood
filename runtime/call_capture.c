/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Generic call capture (runtime/call_capture.h).

   Each wrapped target gets a stub, `movabs r11, thunk; jmp rt_call_capture_thunk`; r11 is scratch
   in the System V convention and carries no argument. The shared thunk saves the argument
   registers and lets rt_call_capture_enter open a frame for the call: the caller's return address,
   rbx and r12, and the argument registers when this call is recorded. It then takes the return
   address off the stack and calls the target with rbx pointing at the frame and r12 holding the
   frame's serial, so the target sees its stack arguments exactly where the caller put them, and
   the frame is found again from those two callee-saved registers however the stack was left in
   between. When the target returns, rt_call_capture_leave writes the record and closes the frame.

   Non-local exits. The thunk's unwind information gives the caller's return address, rbx and r12
   from the frame, so an unwinder (a C++ exception) passes through it to the caller. A frame left
   that way, or by longjmp or the thread's exit, is closed when the next wrapped call on the thread
   is entered at the same stack slot (the earlier call there can no longer be running), or when the
   thread exits; its record, if it was sampled, says it did not return. A target that returns
   twice (setjmp) cannot be wrapped: its second return finds its frame closed and stops the
   process with a message, rather than returning to the wrong place. */
#define _GNU_SOURCE
#include "runtime/call_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
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
/* MAX_FRAMES: wrapped calls in progress (or left and not yet closed) on one thread; past it, calls
   run through the stub unrecorded. */
enum { MAX_FRAMES = 1 << 20, FRAME_CHUNK = 256, STUB_SIZE = 32, STUB_PAGE = 1 << 16 };

typedef struct {
    char *name;
    char *path;   /* the run's record file for this name */
    void *target;
    atomic_uint calls;
    atomic_int recorded;
    atomic_int warned_unreturned;
} thunk;

/* The thunk's save areas; the offsets are the assembly's. */
typedef struct {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rbx, r12, unused;
    unsigned char ymm[8][32];
} entry_registers;

typedef struct {
    uint64_t rax, rdx;
    unsigned char ymm[2][32];
    uint64_t from, rbx, r12, unused;   /* the caller's, handed back by rt_call_capture_leave */
    unsigned char x87[108];            /* fnsave: control, status and tag words, then st0..st7 */
    unsigned char unused_x87[4];
} return_registers;

/* One wrapped call. The first five fields are read by the thunk and its unwind information. */
typedef struct frame {
    uint64_t from;            /* the caller's return address */
    uint64_t rbx, r12;        /* the caller's, while the target runs with the frame in them */
    uint64_t serial;
    uint64_t cfa;             /* the canonical frame address the thunk reports while the target runs */
    uint64_t *slot;           /* where the caller's return address was */
    const thunk *t;
    entry_registers *in;      /* the entry registers when this call is recorded, else NULL */
    struct frame *next;       /* the older live frame at the same slot (a tail call), or the next free one */
    unsigned call;
    int number;
    int live;
} frame;

_Static_assert(offsetof(entry_registers, rbx) == 56 && offsetof(entry_registers, ymm) == 80 &&
                   sizeof(entry_registers) == 336,
               "entry save area");
_Static_assert(offsetof(return_registers, from) == 80 && offsetof(return_registers, x87) == 112 &&
                   sizeof(return_registers) == 224,
               "return save area");
_Static_assert(offsetof(frame, rbx) == 8 && offsetof(frame, r12) == 16 && offsetof(frame, serial) == 24 &&
                   offsetof(frame, cfa) == 32,
               "frame fields the thunk reads");

typedef struct chunk {
    struct chunk *next;
    frame frames[FRAME_CHUNK];
} chunk;

/* A thread's open frames, by slot: an open-addressed table holding the newest frame at each slot. */
typedef struct {
    frame **table;
    size_t capacity, slots, open;
    frame *free_frames;
    chunk *chunks;
    uint64_t serial;
} thread_frames;

typedef struct {
    void *target;
    frame *f;   /* NULL: the thunk jumps to the target unrecorded */
} entered;

void rt_call_capture_thunk(void) __attribute__((visibility("hidden")));
extern const char rt_call_capture_returned[] __attribute__((visibility("hidden")));
entered rt_call_capture_enter(thunk *t, uint64_t *slot, const entry_registers *in)
    __attribute__((visibility("hidden")));
void rt_call_capture_leave(frame *f, uint64_t serial, return_registers *out) __attribute__((visibility("hidden")));

/* Unwind information, as DWARF expressions (DW_CFA_expression, register, length, DW_OP_breg3
   offset): while the target runs, the caller's return address (register 16) is at [rbx+0], its
   rbx (3) at [rbx+8] and its r12 (12) at [rbx+16]. The caller's rsp (7) is the thunk's own
   (DW_CFA_val_expression, DW_OP_breg7 0), and the frame address is the frame's `cfa`
   (DW_CFA_def_cfa_expression, DW_OP_breg3 32, DW_OP_deref): an unwinder tells frames apart by that
   address, and the caller's own rsp would make the thunk look like the caller. */
#define CFI_FRAME_RBX ".cfi_escape 0x10, 0x03, 0x02, 0x73, 0x08\n"
#define CFI_FRAME_R12 ".cfi_escape 0x10, 0x0c, 0x02, 0x73, 0x10\n"
#define CFI_FRAME_RETURN ".cfi_escape 0x10, 0x10, 0x02, 0x73, 0x00\n"
#define CFI_CALLER_RSP ".cfi_escape 0x16, 0x07, 0x02, 0x77, 0x00\n"
#define CFI_FRAME_CFA ".cfi_escape 0x0f, 0x03, 0x73, 0x20, 0x06\n"
#define RESTORE_ARGUMENTS                                                                          \
    "  mov 0(%rsp), %rdi\n  mov 8(%rsp), %rsi\n  mov 16(%rsp), %rdx\n  mov 24(%rsp), %rcx\n"        \
    "  mov 32(%rsp), %r8\n  mov 40(%rsp), %r9\n  mov 48(%rsp), %rax\n"                              \
    "  vmovdqu 80(%rsp), %ymm0\n  vmovdqu 112(%rsp), %ymm1\n  vmovdqu 144(%rsp), %ymm2\n"           \
    "  vmovdqu 176(%rsp), %ymm3\n  vmovdqu 208(%rsp), %ymm4\n  vmovdqu 240(%rsp), %ymm5\n"          \
    "  vmovdqu 272(%rsp), %ymm6\n  vmovdqu 304(%rsp), %ymm7\n"

__asm__(".text\n"
        ".globl rt_call_capture_thunk\n"
        ".hidden rt_call_capture_thunk\n"
        ".type rt_call_capture_thunk, @function\n"
        "rt_call_capture_thunk:\n"
        "  .cfi_startproc\n"
        "  push %rbp\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  .cfi_rel_offset %rbp, 0\n"
        "  mov %rsp, %rbp\n"
        "  .cfi_def_cfa_register %rbp\n"
        "  and $-16, %rsp\n"       /* the helpers get an aligned stack whatever the caller's */
        "  sub $336, %rsp\n"
        "  mov %rdi, 0(%rsp)\n  mov %rsi, 8(%rsp)\n  mov %rdx, 16(%rsp)\n  mov %rcx, 24(%rsp)\n"
        "  mov %r8, 32(%rsp)\n  mov %r9, 40(%rsp)\n  mov %rax, 48(%rsp)\n"
        "  mov %rbx, 56(%rsp)\n  mov %r12, 64(%rsp)\n"
        "  vmovdqu %ymm0, 80(%rsp)\n  vmovdqu %ymm1, 112(%rsp)\n  vmovdqu %ymm2, 144(%rsp)\n"
        "  vmovdqu %ymm3, 176(%rsp)\n  vmovdqu %ymm4, 208(%rsp)\n  vmovdqu %ymm5, 240(%rsp)\n"
        "  vmovdqu %ymm6, 272(%rsp)\n  vmovdqu %ymm7, 304(%rsp)\n"
        "  mov %r11, %rdi\n"
        "  lea 8(%rbp), %rsi\n"    /* the slot of the caller's return address */
        "  mov %rsp, %rdx\n"
        "  call rt_call_capture_enter\n"
        "  mov %rax, %r11\n"
        "  test %rdx, %rdx\n"
        "  .cfi_remember_state\n"
        "  jz 2f\n"
        "  mov %rdx, %rbx\n"
        CFI_FRAME_RBX
        "  mov 24(%rbx), %r12\n"
        CFI_FRAME_R12
        RESTORE_ARGUMENTS
        "  leave\n"
        "  .cfi_def_cfa %rsp, 8\n"
        "  .cfi_restore %rbp\n"
        "  add $8, %rsp\n"          /* the return address is in the frame now */
        CFI_FRAME_CFA
        CFI_CALLER_RSP
        CFI_FRAME_RETURN
        "  call *%r11\n"
        ".globl rt_call_capture_returned\n"
        ".hidden rt_call_capture_returned\n"
        "rt_call_capture_returned:\n"
        "  sub $8, %rsp\n"          /* the return address goes back into its own slot */
        "  .cfi_def_cfa %rsp, 8\n"
        "  .cfi_restore %rsp\n"
        "  push %rbp\n"
        "  .cfi_adjust_cfa_offset 8\n"
        "  .cfi_rel_offset %rbp, 0\n"
        "  mov %rsp, %rbp\n"
        "  .cfi_def_cfa_register %rbp\n"
        "  and $-16, %rsp\n"
        "  sub $224, %rsp\n"
        "  mov %rax, 0(%rsp)\n  mov %rdx, 8(%rsp)\n"
        "  vmovdqu %ymm0, 16(%rsp)\n  vmovdqu %ymm1, 48(%rsp)\n"
        "  fnsave 112(%rsp)\n"      /* st0, st1 (a long double result) saved; the x87 stack is empty for the call */
        "  mov %rbx, %rdi\n"
        "  mov %r12, %rsi\n"
        "  mov %rsp, %rdx\n"
        "  call rt_call_capture_leave\n"
        "  frstor 112(%rsp)\n"
        "  mov 80(%rsp), %rax\n"
        "  mov %rax, 8(%rbp)\n"
        "  .cfi_offset 16, -8\n"
        "  mov 88(%rsp), %rbx\n"
        "  .cfi_restore %rbx\n"
        "  mov 96(%rsp), %r12\n"
        "  .cfi_restore %r12\n"
        "  mov 0(%rsp), %rax\n  mov 8(%rsp), %rdx\n"
        "  vmovdqu 16(%rsp), %ymm0\n  vmovdqu 48(%rsp), %ymm1\n"
        "  leave\n"
        "  .cfi_def_cfa %rsp, 8\n"
        "  .cfi_restore %rbp\n"
        "  ret\n"
        "2:\n"                      /* no frame: the target runs as if called directly */
        "  .cfi_restore_state\n"
        RESTORE_ARGUMENTS
        "  leave\n"
        "  .cfi_def_cfa %rsp, 8\n"
        "  .cfi_restore %rbp\n"
        "  jmp *%r11\n"
        "  .cfi_endproc\n"
        ".size rt_call_capture_thunk, .-rt_call_capture_thunk\n");

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t frames_key;
static __thread thread_frames *frames;
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

static void thread_exit(void *p);

/* BB_CAPTURE_RUN when it is a safe file name component, else start time and process id. */
static void initialize(void)
{
    pthread_key_create(&frames_key, thread_exit);
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
        /* Writable and executable at once: a host that refuses such mappings gets no thunks. */
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

/* One line per recorded call; `out` is NULL for a call that did not return. */
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
    if (!out) {
        len += snprintf(line + len, sizeof line - len, "}, \"returns\": null}\n");
    } else {
        len += snprintf(line + len, sizeof line - len, "}, \"returns\": {\"rax\": \"0x%llx\", \"rdx\": \"0x%llx\"",
                        (unsigned long long)out->rax, (unsigned long long)out->rdx);
        for (int i = 0; i < 2; i++) {
            hex(vector, sizeof vector, out->ymm[i], 32);
            len += snprintf(line + len, sizeof line - len, ", \"ymm%d\": \"%s\"", i, vector);
        }
        /* st0 and st1 when they hold a value (a long double result): the fnsave image's status word
           gives the top of the x87 stack, its tag word which registers are empty (tag 3). */
        uint16_t status, tags;
        memcpy(&status, out->x87 + 4, 2);
        memcpy(&tags, out->x87 + 8, 2);
        const unsigned top = (status >> 11) & 7;
        for (unsigned i = 0; i < 2; i++) {
            if ((tags >> (2 * ((top + i) & 7)) & 3) == 3)
                continue;
            hex(vector, sizeof vector, out->x87 + 28 + 10 * i, 10);
            len += snprintf(line + len, sizeof line - len, ", \"st%u\": \"%s\"", i, vector);
        }
        len += snprintf(line + len, sizeof line - len, "}}\n");
    }
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

static size_t slot_index(const thread_frames *s, const uint64_t *slot)
{
    return (size_t)(((uint64_t)(uintptr_t)slot >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 17) & (s->capacity - 1);
}

/* The table index of `slot`, or of the empty entry where it would go. */
static size_t find_slot(const thread_frames *s, const uint64_t *slot)
{
    size_t i = slot_index(s, slot);
    while (s->table[i] && s->table[i]->slot != slot)
        i = (i + 1) & (s->capacity - 1);
    return i;
}

/* Empties table entry i, moving later entries of the same probe run back into the gap. */
static void remove_slot(thread_frames *s, size_t i)
{
    const size_t mask = s->capacity - 1;
    s->table[i] = NULL;
    s->slots--;
    for (size_t j = (i + 1) & mask; s->table[j]; j = (j + 1) & mask) {
        const size_t home = slot_index(s, s->table[j]->slot);
        if (((j - home) & mask) >= ((j - i) & mask)) {
            s->table[i] = s->table[j];
            s->table[j] = NULL;
            i = j;
        }
    }
}

static int grow_table(thread_frames *s)
{
    if (s->capacity && 2 * (s->slots + 1) <= s->capacity)
        return 0;
    const size_t capacity = s->capacity ? 2 * s->capacity : 64;
    frame **table = calloc(capacity, sizeof *table), **old = s->table;
    if (!table)
        return -1;
    const size_t old_capacity = s->capacity;
    s->table = table;
    s->capacity = capacity;
    for (size_t i = 0; i < old_capacity; i++)
        if (old[i])
            s->table[find_slot(s, old[i]->slot)] = old[i];
    free(old);
    return 0;
}

static void report_unreturned(frame *f)
{
    thunk *t = (thunk *)f->t;
    if (!atomic_exchange(&t->warned_unreturned, 1))
        fprintf(stderr,
                "call capture: %s: a call was left without returning (longjmp, an exception or the thread's "
                "exit); its record has no results\n",
                t->name);
    if (f->in)
        write_record(f, f->in, NULL);
}

static void close_frame(thread_frames *s, frame *f, int returned)
{
    const size_t i = find_slot(s, f->slot);
    if (s->table[i] == f) {
        if (!(s->table[i] = f->next))
            remove_slot(s, i);
    } else {
        for (frame *g = s->table[i]; g; g = g->next)
            if (g->next == f) {
                g->next = f->next;
                break;
            }
    }
    if (!returned)
        report_unreturned(f);
    free(f->in);
    f->in = NULL;
    f->live = 0;
    f->next = s->free_frames;
    s->free_frames = f;
    s->open--;
}

/* A call entered at `slot` means every earlier call whose return address was there has been left,
   except when the slot holds the thunk's own return: the target there tail-called this one. */
static void close_left(thread_frames *s, uint64_t *slot)
{
    if (!s->capacity || *slot == (uint64_t)(uintptr_t)rt_call_capture_returned)
        return;
    const size_t i = find_slot(s, slot);
    while (s->table[i] && s->table[i]->slot == slot)
        close_frame(s, s->table[i], 0);
}

static frame *open_frame(thread_frames *s, thunk *t, uint64_t *slot, const entry_registers *in)
{
    if (s->open >= MAX_FRAMES) {
        warn_once("too many wrapped calls in progress on one thread; further calls are not recorded", NULL);
        return NULL;
    }
    if (!s->free_frames) {
        chunk *c = malloc(sizeof *c);
        if (!c) {
            warn_once("no memory for a call frame; the call is not recorded", NULL);
            return NULL;
        }
        c->next = s->chunks;
        s->chunks = c;
        for (int i = FRAME_CHUNK - 1; i >= 0; i--) {
            c->frames[i].live = 0;
            c->frames[i].next = s->free_frames;
            s->free_frames = &c->frames[i];
        }
    }
    if (grow_table(s)) {
        warn_once("no memory for a call frame; the call is not recorded", NULL);
        return NULL;
    }
    frame *f = s->free_frames;
    s->free_frames = f->next;
    f->from = *slot;
    f->rbx = in->rbx;
    f->r12 = in->r12;
    f->serial = ++s->serial;
    f->slot = slot;
    /* Just above the caller's rsp, and distinct for each frame in a chain of tail calls. */
    const size_t i = find_slot(s, slot);
    unsigned chained = 0;
    for (const frame *g = s->table[i]; g; g = g->next)
        chained++;
    f->cfa = (uint64_t)(uintptr_t)slot + 9 + chained;
    f->t = t;
    f->in = NULL;
    f->live = 1;
    f->next = s->table[i];
    s->slots += !s->table[i];
    s->table[i] = f;
    s->open++;
    const unsigned call = atomic_fetch_add(&t->calls, 1);
    if ((call < FIRST || call % EVERY == 0) && atomic_load(&t->recorded) < MAX_RECORDS &&
        (f->in = malloc(sizeof *in))) {
        const int number = atomic_fetch_add(&t->recorded, 1);
        if (number < MAX_RECORDS) {
            *f->in = *in;
            f->call = call;
            f->number = number;
        } else {
            free(f->in);
            f->in = NULL;
        }
    }
    return f;
}

/* At a thread's exit: its open frames were left without returning. */
static void thread_exit(void *p)
{
    thread_frames *s = p;
    for (size_t i = 0; i < s->capacity; i++)
        for (frame *f = s->table[i]; f; f = f->next) {
            report_unreturned(f);
            free(f->in);
        }
    for (chunk *c = s->chunks, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(s->table);
    free(s);
    frames = NULL;   /* a wrapped call in a later destructor starts afresh */
}

entered rt_call_capture_enter(thunk *t, uint64_t *slot, const entry_registers *in)
{
    const int saved_errno = errno;
    entered e = {t->target, NULL};
    thread_frames *s = frames;
    if (!s && (s = calloc(1, sizeof *s))) {
        frames = s;
        pthread_setspecific(frames_key, s);
    }
    if (!s) {
        warn_once("no memory for call frames; calls are not recorded", NULL);
    } else {
        close_left(s, slot);
        e.f = open_frame(s, t, slot, in);
    }
    errno = saved_errno;
    return e;
}

void rt_call_capture_leave(frame *f, uint64_t serial, return_registers *out)
{
    const int saved_errno = errno;
    thread_frames *s = frames;
    if (!s || !f->live || f->serial != serial) {
        fputs("call capture: a wrapped call returned after its frame was closed: the target returns twice (as "
              "setjmp does) and cannot be wrapped, or did not preserve rbx and r12\n",
              stderr);
        abort();
    }
    out->from = f->from;
    out->rbx = f->rbx;
    out->r12 = f->r12;
    if (f->in)
        write_record(f, f->in, out);
    close_frame(s, f, 1);
    errno = saved_errno;
}
