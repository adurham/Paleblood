/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The runtime's system library implementations, called the way the guest calls them: through the
   RT_SYSLIB registry, by library and symbol. */
#define _GNU_SOURCE
#include "runtime/call_capture.h"
#include "runtime/process.h"
#include "runtime/syslib.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static int unresolved_test_entry(void) { return -1; }
RT_SYSLIB_UNRESOLVED("test", testUnresolvedEntry, unresolved_test_entry);

static int failures;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void *fn(const char *library, const char *symbol)
{
    for (const rt_syslib_entry *e = rt_syslib_begin(); e && e != rt_syslib_end(); e++)
        if (!strcmp(e->library, library) && !strcmp(e->symbol, symbol)) return e->fn;
    fprintf(stderr, "no %s %s\n", library, symbol);
    failures++;
    return NULL;
}

static int is_implemented(const char *library, const char *symbol)
{
    for (const rt_syslib_entry *e = rt_syslib_begin(); e && e != rt_syslib_end(); e++)
        if (!strcmp(e->library, library) && !strcmp(e->symbol, symbol))
            return e->capability == RT_SYSLIB_CAPABILITY_IMPLEMENTED;
    return 0;
}

typedef int (*f1)(void **);
typedef int (*f2i)(void **, int);
typedef int (*f_state)(int *);
typedef int (*f_check)(void);
typedef int (*f_register)(void *, void *, int *);
typedef int (*f_unregister)(int);
typedef int (*f_info)(int, void *);
typedef int (*f_nat)(void *);
typedef int (*finit)(void **, void **, const char *);
typedef int (*f_equal)(void *, void *);
typedef int (*f_attr_get)(void *, void **);
typedef uint32_t (*f_u32_u32)(uint32_t);
typedef uint16_t (*f_u16_u16)(uint16_t);
typedef int (*f_inet_pton)(int, const char *, void *);
typedef int (*f_affinity)(void **, uint64_t *);
typedef int (*f_initattr)(void **);
typedef int (*f_destroyattr)(void **);
typedef int (*f_attrget)(void *, void **);
typedef uint64_t (*f_size)(void);
typedef int (*f_dmem_alloc)(int64_t, int64_t, uint64_t, uint64_t, int, uint64_t *);
typedef int (*f_dmem_map)(void **, uint64_t, int, int, uint64_t, uint64_t);
typedef int (*f_dmem_release)(int64_t, uint64_t);

static void *net_inet_pton_entry;
static int net_pton_unknown_family(void)
{
    unsigned char address[4];
    return ((f_inet_pton)net_inet_pton_entry)(0x7f, "192.0.2.1", address);
}
static int net_pton_null_source(void)
{
    unsigned char address[4];
    return ((f_inet_pton)net_inet_pton_entry)(2, NULL, address);
}
static int net_pton_null_destination(void)
{
    return ((f_inet_pton)net_inet_pton_entry)(2, "192.0.2.1", NULL);
}
static void net_pton_unknown_family_call(void) { (void)net_pton_unknown_family(); }
static void net_pton_null_source_call(void) { (void)net_pton_null_source(); }
static void net_pton_null_destination_call(void) { (void)net_pton_null_destination(); }

/* libSceNetCtl, the offline group (issue #41). rt_fatal aborts the process (runtime/process.c), so a
   fatal path can only be proved in a child: fork, call the entry the way the guest does, and check
   the signal it died on. netctl_shared is MAP_SHARED and poisoned, so a child that wrote into the
   buffer it was handed would still be visible here even though the child never returns. */
static void *netctl_get_state_entry, *netctl_check_entry, *netctl_register_entry, *netctl_unregister_entry;
static void *netctl_get_info_entry, *netctl_get_nat_info_entry;
static char *netctl_shared;
static int netctl_callbacks_run;

static void netctl_must_not_run(void) { netctl_callbacks_run++; }

static int netctl_state(int *state) { return ((f_state)netctl_get_state_entry)(state); }
static int netctl_check(void) { return ((f_check)netctl_check_entry)(); }
static int netctl_register(void *callback, void *arg, int *id)
{
    return ((f_register)netctl_register_entry)(callback, arg, id);
}
static int netctl_unregister(int id) { return ((f_unregister)netctl_unregister_entry)(id); }
static int netctl_info(int module, void *info) { return ((f_info)netctl_get_info_entry)(module, info); }
static int netctl_nat(void *nat_info) { return ((f_nat)netctl_get_nat_info_entry)(nat_info); }

static int died_in_fatal(void (*call)(void))
{
    const pid_t child = fork();
    if (child == 0) {
        call();
        _exit(1); /* the call returned, so the answer was not the fatal being proved */
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* The paths whose PS4 answer is not established. The ids are file-scope so a child can unregister
   one the parent registered before forking. */
static int netctl_id_a, netctl_id_b;
static void state_into_null(void) { netctl_state(NULL); }
static void register_null_callback(void) { netctl_register(NULL, NULL, &netctl_id_a); }
static void register_null_id(void) { netctl_register((void *)netctl_must_not_run, NULL, NULL); }
static void unregister_id_a(void) { netctl_unregister(netctl_id_a); }
static void unregister_unknown_id(void) { netctl_unregister(0x7fff); }
static void info_of_module_one(void) { netctl_info(1, netctl_shared); }
/* A size-prefixed buffer of the shape the guest's own sites build (16 at offset 0), on the child's
   stack: the fatal must not write through it, and netctl_shared stays the untouched witness. */
static void nat_info_with_size(void) { int nat[4] = {16, 0, 0, 0}; netctl_nat(nat); }

/* Thread creation will read an attribute's detach state through rt_thread_attr_detach_state. A
   state the guest did not set is one the system supplies, which is not established, so the read
   must stop the process; each such case is read in a child. detach_reader_attr is the variable the
   child reads. */
static int detach_state_of(void **attr) { return rt_thread_attr_detach_state(attr, "test"); }
static void *detach_reader_attr;
static void read_detach_state(void) { (void)detach_state_of(&detach_reader_attr); }
static void read_detach_state_without_variable(void) { (void)detach_state_of(NULL); }
#if BB_ENABLE_CALL_CAPTURE
#if __has_include("call_capture_commit.h")
#include "call_capture_commit.h"   /* RT_CALL_CAPTURE_COMMIT, the build's stamp */
#endif

/* Call capture (runtime/call_capture.h): one target per argument class the thunk has to forward
   untouched - stack arguments, vector registers, a variadic call's count in al, a nested wrapped
   call. */
typedef int64_t (*f_eight)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef double (*f_mixed)(double, int64_t, float);
typedef double (*f_variadic)(int, ...);
static __attribute__((noinline)) int64_t eight_integers(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                                                        int64_t f, int64_t g, int64_t h)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
static __attribute__((noinline)) double mixed(double x, int64_t n, float y) { return x * (double)n + y; }
static __attribute__((noinline)) double sum_doubles(int count, ...)
{
    va_list list;
    va_start(list, count);
    double sum = 0;
    for (int i = 0; i < count; i++) sum += va_arg(list, double);
    va_end(list);
    return sum;
}
static f_size nested_inner;
static __attribute__((noinline)) uint64_t nested_outer(void) { return nested_inner() + 1; }

/* Non-local exits through a wrapped frame: longjmp to an unwrapped caller, longjmp from an inner
   wrapped call into an outer one, a thread that exits inside a wrapped call, and a function that
   returns twice. */
static jmp_buf jump_back;
static int64_t (*jumper_call)(int64_t);
static __attribute__((noinline)) int64_t jumper(int64_t n) { longjmp(jump_back, (int)n); }
static __attribute__((noinline)) int64_t jump_out_of_inner(int64_t n)
{
    if (!setjmp(jump_back)) jumper_call(n);   /* jumper never returns */
    return 5;
}
static f_size thread_exit_call;
static __attribute__((noinline)) uint64_t exit_thread(void) { pthread_exit(NULL); }
static void *exits_inside_a_call(void *unused) { (void)unused; thread_exit_call(); return NULL; }

static int (*setjmp_call)(jmp_buf);
static jmp_buf twice;
static void returns_twice(void)
{
    if (!setjmp_call(twice)) longjmp(twice, 1);
}

/* Recursion through a wrapped name, deeper than the old fixed shadow stack (1024). */
static int64_t (*recurse_call)(int64_t);
static __attribute__((noinline)) int64_t recurse(int64_t n) { return n ? recurse_call(n - 1) + 1 : 0; }

/* Calls from several threads at once: each returns its own result; the records stay whole lines
   and each record number is taken once. */
enum { CAPTURE_THREADS = 8, CALLS_PER_THREAD = 20000 };
static f_eight threaded_call;
static void *call_many_times(void *arg)
{
    const int64_t base = (int64_t)(intptr_t)arg;
    for (int i = 0; i < CALLS_PER_THREAD; i++)
        if (threaded_call(base, i, 0, 0, 0, 0, 0, 0) != base + 2 * i) return (void *)1;
    return NULL;
}

/* A wrapped call from a thread-specific destructor that runs after call capture's own. */
static pthread_key_t late_key;
static uint64_t late_result;
static void late_destructor(void *unused) { (void)unused; late_result = nested_inner(); }
static void *registers_late_destructor(void *unused)
{
    (void)unused;
    CHECK(nested_inner() == UINT64_C(0x180000000));   /* opens this thread's call frames first */
    pthread_setspecific(late_key, (void *)1);
    return NULL;
}

/* x87 results: a long double comes back in st0, a complex long double in st0 and st1. */
static __attribute__((noinline)) long double twice_long(long double x) { return x * 2; }
static __attribute__((noinline)) long double _Complex long_pair(long double re, long double im) { return re + im * 1.0iL; }
static unsigned x87_tags(void)
{
    unsigned char environment[28];
    __asm__ volatile("fnstenv %0\n fldenv %0" : "=m"(environment));
    return (unsigned)environment[8] | (unsigned)environment[9] << 8;
}

/* A tail call between two wrapped functions: tail_call jumps to tail_target (a thunk) with the
   first thunk's return address still on the stack. */
__attribute__((visibility("hidden"))) void *tail_target;
uint64_t tail_call(void);
/* pushsection/popsection: the compiler does not see a bare ".text" here, and would place what
   follows in .text (gcc -O0 put capture_dir there, which then faulted on its first write). */
__asm__(".pushsection .text\n.globl tail_call\n.hidden tail_call\ntail_call:\n  jmp *tail_target(%rip)\n"
        ".popsection\n");

/* A wrapped call that returns on a different thread from the one it entered on: a coroutine that
   yields inside the call and is resumed on another thread. */
static ucontext_t coroutine, coroutine_caller;
static char coroutine_stack[1 << 16];
static int64_t (*migrating_call)(int64_t);
static __attribute__((noinline)) int64_t yields_once(int64_t n)
{
    swapcontext(&coroutine, &coroutine_caller);
    return n + 1;
}
static void coroutine_body(void) { migrating_call(41); }
/* Runs the coroutine on this thread up to its yield, inside the wrapped call. */
static void start_coroutine(void)
{
    migrating_call = (int64_t (*)(int64_t))rt_call_capture_wrap("migrating", (void *)yields_once);
    getcontext(&coroutine);
    coroutine.uc_stack.ss_sp = coroutine_stack;
    coroutine.uc_stack.ss_size = sizeof coroutine_stack;
    coroutine.uc_link = &coroutine_caller;
    makecontext(&coroutine, coroutine_body, 0);
    swapcontext(&coroutine_caller, &coroutine);
}
static void *start_coroutine_on_thread(void *unused) { (void)unused; start_coroutine(); return NULL; }
static void *resume_coroutine_on_thread(void *unused)
{
    (void)unused;
    swapcontext(&coroutine_caller, &coroutine);
    return NULL;
}
/* Entered on this thread, returns on another while this one waits for it. */
static void return_on_another_thread(void)
{
    start_coroutine();
    pthread_t thread;
    if (!pthread_create(&thread, NULL, resume_coroutine_on_thread, NULL)) pthread_join(thread, NULL);
}
/* Entered on a thread that then exits, returns on this one. */
static void return_after_entering_thread_exited(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, start_coroutine_on_thread, NULL)) pthread_join(thread, NULL);
    swapcontext(&coroutine_caller, &coroutine);
}

/* Runs `call` in a child with its stderr captured: 1 when the child stopped with SIGABRT having
   said `needle`. */
static int aborts_saying(void (*call)(void), const char *needle)
{
    int out[2];
    if (pipe(out)) return 0;
    fflush(stdout);
    fflush(stderr);
    const pid_t child = fork();
    if (child == 0) {
        close(out[0]);
        dup2(out[1], 2);
        call();
        _exit(1);
    }
    close(out[1]);
    static char said[1 << 16];
    size_t n = 0;
    for (ssize_t got; n < sizeof said - 1 && (got = read(out[0], said + n, sizeof said - 1 - n)) > 0;) n += (size_t)got;
    said[n] = 0;
    close(out[0]);
    int status = 0;
    const int stopped = child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
                        WTERMSIG(status) == SIGABRT;
    if (!stopped || !strstr(said, needle))
        fprintf(stderr, "expected an abort saying \"%s\"; it said:\n%s", needle, said);
    return stopped && strstr(said, needle);
}

/* The stamp every record starts with: the record format and the commit the capture code was built
   from (40 hex digits, "-dirty" when runtime/ had changes), or "unknown" outside a checkout. */
static int stamped(const char *text)
{
    static const char start[] = "{\"record_format\": 1, \"capture_commit\": \"";
    if (!text || strncmp(text, start, sizeof start - 1)) return 0;
    const char *commit = text + sizeof start - 1;
    if (!strncmp(commit, "unknown\"", 8)) return 1;
    size_t n = 0;
    while ((commit[n] >= '0' && commit[n] <= '9') || (commit[n] >= 'a' && commit[n] <= 'f')) n++;
    return n == 40 && (commit[n] == '"' || !strncmp(commit + n, "-dirty\"", 7));
}

/* The permissions of the mapping holding `address` ("r-xp"), from /proc/self/maps; whether any
   mapping of the process is writable and executable at once. */
static int mapping_of(const void *address, char perms[5], int *any_writable_executable)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], p[5];
    unsigned long long low, high;
    int found = 0;
    *any_writable_executable = 0;
    while (fgets(line, sizeof line, maps))
        if (sscanf(line, "%llx-%llx %4s", &low, &high, p) == 3) {
            *any_writable_executable |= p[1] == 'w' && p[2] == 'x';
            if ((uintptr_t)address >= low && (uintptr_t)address < high) memcpy(perms, p, 5), found = 1;
        }
    fclose(maps);
    return found;
}

int call_capture_exceptions(void);                         /* test_call_capture_unwind.cpp */
void throw_through_frame_without_unwind_info(void);        /* test_call_capture_unwind.cpp */

static char *capture_dir;

/* The record file of one name in this test's run, read whole (NULL when there is none). */
static char *records_of(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s/syslib-test.calls.jsonl", capture_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char text[1 << 20];
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    return text;
}

static int lines(const char *text)
{
    int n = 0;
    for (; text && *text; text++) n += *text == '\n';
    return n;
}

static int occurrences(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; p && (p = strstr(p, needle)); p++) n++;
    return n;
}

static void remove_records(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s/syslib-test.calls.jsonl", capture_dir, name);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s", capture_dir, name);
    rmdir(path);
}

/* Runs one call capture case in a child process: it passes when the child's checks all passed. */
static int passes_in_child(void (*body)(void))
{
    fflush(stdout);
    fflush(stderr);
    const pid_t child = fork();
    if (child == 0) {
        failures = 0;
        body();
        fflush(stdout);
        _exit(failures != 0);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* sceKernelGetDirectMemorySize's entry: a target with a known result for the cases below. */
static f_size capture_target;

/* A tail call from one wrapped function into another: both return, both are recorded. */
static void capture_tail_calls(void)
{
    tail_target = rt_call_capture_wrap("tail_target", (void *)capture_target);
    const f_size tail = (f_size)rt_call_capture_wrap("tail_call", (void *)tail_call);
    CHECK(tail() == UINT64_C(0x180000000) && tail() == UINT64_C(0x180000000));
    CHECK(lines(records_of("tail_call")) == 2 && lines(records_of("tail_target")) == 2);
}

/* longjmp out of a wrapped call to an unwrapped caller, more often than the old shadow stack had
   frames: every call lands where it should, and each left call is noted when the next one enters
   at its slot - the sampled ones as records without results. */
static void capture_longjmp_to_unwrapped_caller(void)
{
    jumper_call = (int64_t (*)(int64_t))rt_call_capture_wrap("jumper", (void *)jumper);
    int landed = 0;
    for (volatile int i = 0; i < 1100; i++)
        if (setjmp(jump_back) == 7 || !jumper_call(7)) landed++;
    CHECK(landed == 1100);
    const char *text = records_of("jumper");
    CHECK(lines(text) == 51 && occurrences(text, "\"returns\": null}") == 51);
    const f_eight eight = (f_eight)rt_call_capture_wrap("eight_after_jumps", (void *)eight_integers);
    CHECK(eight(1, 2, 3, 4, 5, 6, 7, 8) == eight_integers(1, 2, 3, 4, 5, 6, 7, 8));
}

/* longjmp from an inner wrapped call to a setjmp inside an outer wrapped call, which then returns:
   the outer call goes back to its own caller with its own result. */
static void capture_longjmp_into_outer_call(void)
{
    jumper_call = (int64_t (*)(int64_t))rt_call_capture_wrap("jumper", (void *)jumper);
    int64_t (*outer)(int64_t) = (int64_t (*)(int64_t))rt_call_capture_wrap("jump_out_of_inner",
                                                                         (void *)jump_out_of_inner);
    for (int i = 0; i < 3; i++) CHECK(outer(3) == 5);
    const char *text = records_of("jump_out_of_inner");
    CHECK(lines(text) == 3 && occurrences(text, "\"returns\": {\"rax\": \"0x5\"") == 3);
}

/* A thread that exits inside a wrapped call: the call is recorded at the thread's exit. */
static void capture_thread_exit_inside_a_call(void)
{
    thread_exit_call = (f_size)rt_call_capture_wrap("exit_thread", (void *)exit_thread);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, exits_inside_a_call, NULL) == 0 && pthread_join(thread, NULL) == 0);
    const char *text = records_of("exit_thread");
    CHECK(lines(text) == 1 && occurrences(text, "\"returns\": null}") == 1);
}

/* A wrapped call from a destructor that runs after the thread's call frames were freed: it gets
   fresh frames, not the freed ones. */
static void capture_call_from_late_destructor(void)
{
    nested_inner = (f_size)rt_call_capture_wrap("late_call", (void *)capture_target);
    pthread_t thread;
    CHECK(pthread_key_create(&late_key, late_destructor) == 0);
    CHECK(pthread_create(&thread, NULL, registers_late_destructor, NULL) == 0 && pthread_join(thread, NULL) == 0);
    CHECK(late_result == UINT64_C(0x180000000));
    CHECK(lines(records_of("late_call")) == 2);
}

/* A function that returns twice cannot be wrapped: its second return stops the process with a
   message instead of returning to a stale address. */
static void capture_returns_twice_is_fatal(void)
{
    setjmp_call = (int (*)(jmp_buf))rt_call_capture_wrap("setjmp_like", (void *)_setjmp);
    CHECK(died_in_fatal(returns_twice));
}

/* Recursion through a wrapped name 5000 deep: the first 50 calls and every 1000th recorded. */
static void capture_recursion_past_old_limit(void)
{
    recurse_call = (int64_t (*)(int64_t))rt_call_capture_wrap("recurse", (void *)recurse);
    CHECK(recurse_call(4999) == 4999);
    const char *text = records_of("recurse");
    CHECK(lines(text) == 54 && occurrences(text, "\"returns\": {") == 54);
    CHECK(text && strstr(text, "\"call\": 4000, \"record\": 53") && strstr(text, "\"rax\": \"0x3e7\""));
}

/* 160000 calls from 8 threads: each gets its own result; 200 records, each number taken once. */
static void capture_threads(void)
{
    threaded_call = (f_eight)rt_call_capture_wrap("threaded", (void *)eight_integers);
    pthread_t threads[CAPTURE_THREADS];
    for (intptr_t i = 0; i < CAPTURE_THREADS; i++)
        CHECK(pthread_create(&threads[i], NULL, call_many_times, (void *)(i << 32)) == 0);
    for (int i = 0; i < CAPTURE_THREADS; i++) {
        void *wrong = (void *)1;
        CHECK(pthread_join(threads[i], &wrong) == 0 && wrong == NULL);
    }
    const char *text = records_of("threaded");
    CHECK(lines(text) == 200);
    for (int i = 0; i < 200; i++) {
        char expected[32];
        snprintf(expected, sizeof expected, "\"record\": %d,", i);
        CHECK(occurrences(text, expected) == 1);
    }
}

/* x87 results are forwarded and recorded as st0 (and st1), and the x87 stack is left as the target
   left it: values for the caller to pop, or empty. */
static void capture_x87_results(void)
{
    long double (*twice_call)(long double) = (long double (*)(long double))rt_call_capture_wrap("twice_long",
                                                                                                (void *)twice_long);
    CHECK(twice_call(1.5L) == 3.0L && x87_tags() == 0xffff);
    const char *text = records_of("twice_long");
    CHECK(text && strstr(text, "\"st0\": \"00000000000000c00040\"") && !strstr(text, "\"st1\""));  /* 3.0L */
    long double _Complex (*pair_call)(long double, long double) =
        (long double _Complex (*)(long double, long double))rt_call_capture_wrap("long_pair", (void *)long_pair);
    const long double _Complex pair = pair_call(1.0L, 2.0L);
    CHECK(__real__ pair == 1.0L && __imag__ pair == 2.0L && x87_tags() == 0xffff);
    text = records_of("long_pair");
    CHECK(text && strstr(text, "\"st0\": \"0000000000000080ff3f\"") &&   /* 1.0L */
          strstr(text, "\"st1\": \"00000000000000800040\""));             /* 2.0L */
    const f_size integer_call = (f_size)rt_call_capture_wrap("no_x87", (void *)capture_target);
    CHECK(integer_call() == UINT64_C(0x180000000) && x87_tags() == 0xffff);
    text = records_of("no_x87");
    CHECK(text && !strstr(text, "\"st0\""));
}

/* A record that cannot be written: the call is unaffected and the run goes on. */
static void capture_unwritable_record(void)
{
    char blocked[600];
    snprintf(blocked, sizeof blocked, "%s/unwritable", capture_dir);
    CHECK(mkdir(blocked, 0700) == 0);
    snprintf(blocked, sizeof blocked, "%s/unwritable/syslib-test.calls.jsonl", capture_dir);
    CHECK(mkdir(blocked, 0700) == 0);   /* a directory where the record file would go */
    const f_size unwritable = (f_size)rt_call_capture_wrap("unwritable", (void *)capture_target);
    errno = 4321;
    CHECK(unwritable != capture_target && unwritable() == UINT64_C(0x180000000) && errno == 4321);
    CHECK(rmdir(blocked) == 0);
}

/* C++ exceptions through one and two wrapped frames and a tail call (test_call_capture_unwind.cpp). */
static void capture_exceptions(void)
{
    CHECK(call_capture_exceptions() == 0);
    CHECK(occurrences(records_of("thrower"), "\"returns\": null}") >= 2);
}

/* A call that returns on a different thread stops the process naming it, before any bookkeeping is
   touched: with the entering thread still running and after it exited. Its return is never
   recorded (the second case's thread notes the call as left at its exit, as for any thread that
   exits inside a call). */
static void capture_return_on_another_thread_is_fatal(void)
{
    static const char said[] = "call capture: migrating: a wrapped call returned on a different thread";
    CHECK(aborts_saying(return_on_another_thread, said));
    CHECK(records_of("migrating") == NULL);
    CHECK(aborts_saying(return_after_entering_thread_exited, said));
    const char *text = records_of("migrating");
    CHECK(lines(text) == 1 && occurrences(text, "\"returns\": null}") == 1);
}

/* A C++ exception that meets a frame its unwinder has no unwind information for - what a wrapped
   frame is to a guest unwinder - stops in std::terminate; it is never skipped past to a catch. */
static void capture_unwind_without_information_terminates(void)
{
    CHECK(aborts_saying(throw_through_frame_without_unwind_info, "terminate called after throwing"));
}

/* The stubs are W^X: the stub a call goes through is executable and not writable, and the process
   has no mapping that is both. */
static void capture_stubs_are_not_writable(void)
{
    const f_size stub = (f_size)rt_call_capture_wrap("w_xor_x", (void *)capture_target);
    char perms[5] = "";
    int writable_executable = 1;
    CHECK(stub != capture_target && mapping_of((const void *)stub, perms, &writable_executable));
    CHECK(!strcmp(perms, "r-xp") && !writable_executable);
    CHECK(stub() == UINT64_C(0x180000000));
    /* stubs of later wraps come from the same or a new region, all of them alike */
    for (int i = 0; i < 5000; i++) CHECK(rt_call_capture_wrap("w_xor_x_more", (void *)capture_target) != NULL);
    const f_size last = (f_size)rt_call_capture_wrap("w_xor_x_last", (void *)capture_target);
    CHECK(mapping_of((const void *)last, perms, &writable_executable) && !strcmp(perms, "r-xp") &&
          !writable_executable && last() == UINT64_C(0x180000000));
}

static void call_capture_tests(f_size direct_memory_size, f_affinity getaffinity, f_initattr attr_init_call,
                               f_destroyattr attr_destroy_call)
{
    /* Off, the default: wrapping hands back the target itself, so every call is exactly today's. */
    unsetenv("BB_CALL_CAPTURE_DIR");
    unsetenv("BB_CALL_CAPTURE_ONLY");
    CHECK(!rt_call_capture_enabled());
    CHECK(rt_call_capture_wrap("libkernel.sceKernelGetDirectMemorySize", (void *)direct_memory_size) ==
          (void *)direct_memory_size);
    CHECK(rt_call_capture_wrap("eight", (void *)eight_integers) == (void *)eight_integers);

    /* On: a thunk that returns what the target returns and records the call by name. */
    const char *tmp = getenv("TMPDIR");
    static char dir[512];
    snprintf(dir, sizeof dir, "%s/pb-call-capture-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    capture_dir = mkdtemp(dir);
    CHECK(capture_dir != NULL);
    if (!capture_dir) return;
    setenv("BB_CALL_CAPTURE_DIR", capture_dir, 1);
    setenv("BB_CAPTURE_RUN", "syslib-test", 1);
    CHECK(rt_call_capture_enabled());

    const f_size size_call = (f_size)rt_call_capture_wrap("libkernel.sceKernelGetDirectMemorySize",
                                                          (void *)direct_memory_size);
    CHECK(size_call != direct_memory_size);
    errno = 1234;
    CHECK(size_call() == UINT64_C(0x180000000));
    CHECK(errno == 1234);   /* writing the record does not leak into the caller's errno */
    const char *text = records_of("libkernel.sceKernelGetDirectMemorySize");
    CHECK(text && lines(text) == 1);
    CHECK(text && strstr(text, "\"name\": \"libkernel.sceKernelGetDirectMemorySize\""));
    CHECK(text && strstr(text, "\"run_id\": \"syslib-test\", \"call\": 0, \"record\": 0"));
    CHECK(stamped(text));   /* every record names the capture code that made it */
#ifdef RT_CALL_CAPTURE_COMMIT
    CHECK(text && strstr(text, "\"capture_commit\": \"" RT_CALL_CAPTURE_COMMIT "\""));
#endif
    CHECK(text && strstr(text, "\"returns\": {\"rax\": \"0x180000000\""));

    /* A pointer argument: the target writes through it as before; the record holds its value. */
    const f_affinity affinity_call = (f_affinity)rt_call_capture_wrap("libkernel.scePthreadAttrGetaffinity",
                                                                      (void *)getaffinity);
    void *attr = NULL;
    uint64_t mask = 0;
    char expected[64];
    CHECK(attr_init_call(&attr) == 0);
    CHECK(affinity_call(&attr, &mask) == 0 && mask == UINT64_C(0x3f));
    CHECK(affinity_call(&attr, NULL) == (int)0x80020016);
    CHECK(attr_destroy_call(&attr) == 0);
    text = records_of("libkernel.scePthreadAttrGetaffinity");
    snprintf(expected, sizeof expected, "\"rsi\": \"0x%llx\"", (unsigned long long)(uintptr_t)&mask);
    CHECK(text && lines(text) == 2 && strstr(text, expected));
    CHECK(text && strstr(text, "\"rsi\": \"0x0\"") && strstr(text, "\"rax\": \"0x80020016\""));

    /* Stack arguments arrive where the caller put them. */
    const f_eight eight = (f_eight)rt_call_capture_wrap("eight", (void *)eight_integers);
    CHECK(eight(1, 2, 3, 4, 5, 6, 7, 8) == eight_integers(1, 2, 3, 4, 5, 6, 7, 8));
    CHECK(eight(-1, 0, 0, 0, 0, 0, 0, 1000) == 7999);
    text = records_of("eight");
    CHECK(text && strstr(text, "\"r9\": \"0x6\"") && strstr(text, "\"rax\": \"0xcc\""));

    /* Vector arguments and a vector return: 1.5 is 0x3ff8000000000000, recorded in memory order. */
    const f_mixed mixed_call = (f_mixed)rt_call_capture_wrap("mixed", (void *)mixed);
    CHECK(mixed_call(1.5, 4, 0.25f) == 6.25);
    text = records_of("mixed");
    CHECK(text && strstr(text, "\"ymm0\": \"000000000000f83f") && strstr(text, "\"rdi\": \"0x4\""));
    CHECK(text && strstr(text, "\"ymm1\": \"0000803e"));                   /* 0.25f */
    CHECK(text && strstr(text, "\"returns\": {\"rax\": ") && strstr(text, "\"ymm0\": \"0000000000001940")); /* 6.25 */

    /* A variadic call keeps al, the count of vector registers it uses. */
    const f_variadic variadic = (f_variadic)rt_call_capture_wrap("sum_doubles", (void *)sum_doubles);
    CHECK(variadic(3, 1.0, 2.0, 4.0) == 7.0);
    text = records_of("sum_doubles");
    CHECK(text && strstr(text, "\"al\": \"0x3\""));

    /* A wrapped call inside a wrapped call: each returns to its own caller, each is recorded. */
    nested_inner = (f_size)rt_call_capture_wrap("nested_inner", (void *)direct_memory_size);
    const f_size outer = (f_size)rt_call_capture_wrap("nested_outer", (void *)nested_outer);
    CHECK(outer() == UINT64_C(0x180000001));
    CHECK(lines(records_of("nested_inner")) == 1 && lines(records_of("nested_outer")) == 1);

    /* Sampling, per name: the first 50 calls, then every 1000th. */
    for (int i = 0; i < 1199; i++) CHECK(eight(i, 0, 0, 0, 0, 0, 0, 0) == i);
    CHECK(lines(records_of("eight")) == 51);

    /* Each of these runs in a child, so a crash in one is reported as that case's failure. */
    capture_target = direct_memory_size;
    CHECK(passes_in_child(capture_tail_calls));
    CHECK(passes_in_child(capture_longjmp_to_unwrapped_caller));
    CHECK(passes_in_child(capture_longjmp_into_outer_call));
    CHECK(passes_in_child(capture_thread_exit_inside_a_call));
    CHECK(passes_in_child(capture_call_from_late_destructor));
    CHECK(passes_in_child(capture_returns_twice_is_fatal));
    CHECK(passes_in_child(capture_recursion_past_old_limit));
    CHECK(passes_in_child(capture_threads));
    CHECK(passes_in_child(capture_x87_results));
    CHECK(passes_in_child(capture_unwritable_record));
    CHECK(passes_in_child(capture_exceptions));
    CHECK(passes_in_child(capture_return_on_another_thread_is_fatal));
    CHECK(passes_in_child(capture_unwind_without_information_terminates));
    CHECK(passes_in_child(capture_stubs_are_not_writable));

    /* BB_CALL_CAPTURE_ONLY leaves every other name unwrapped. */
    setenv("BB_CALL_CAPTURE_ONLY", "mixed,other", 1);
    CHECK(rt_call_capture_wrap("eight_again", (void *)eight_integers) == (void *)eight_integers);
    CHECK(rt_call_capture_wrap("other", (void *)eight_integers) != (void *)eight_integers);
    unsetenv("BB_CALL_CAPTURE_ONLY");
    unsetenv("BB_CALL_CAPTURE_DIR");
    unsetenv("BB_CAPTURE_RUN");

    static const char *const names[] = {"libkernel.sceKernelGetDirectMemorySize", "libkernel.scePthreadAttrGetaffinity",
                                        "eight", "mixed", "sum_doubles", "nested_inner", "nested_outer", "other",
                                        "tail_target", "tail_call", "jumper", "eight_after_jumps",
                                        "jump_out_of_inner", "exit_thread", "late_call", "setjmp_like", "recurse",
                                        "threaded", "twice_long", "long_pair", "no_x87", "unwritable", "thrower",
                                        "rethrow_outer", "tail_to_thrower", "migrating", "w_xor_x", "w_xor_x_more",
                                        "w_xor_x_last"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) remove_records(names[i]);
    CHECK(rmdir(capture_dir) == 0);
}
#else
/* Built without call capture (BB_ENABLE_CALL_CAPTURE=OFF): nothing is wrapped and nothing written,
   even with BB_CALL_CAPTURE_DIR set. */
static void call_capture_tests(f_size direct_memory_size, f_affinity getaffinity, f_initattr attr_init_call,
                               f_destroyattr attr_destroy_call)
{
    (void)getaffinity, (void)attr_init_call, (void)attr_destroy_call;
    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pb-call-capture-off-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    setenv("BB_CALL_CAPTURE_DIR", dir, 1);
    CHECK(!rt_call_capture_enabled());
    CHECK(rt_call_capture_wrap("libkernel.sceKernelGetDirectMemorySize", (void *)direct_memory_size) ==
          (void *)direct_memory_size);
    CHECK(direct_memory_size() == UINT64_C(0x180000000));
    unsetenv("BB_CALL_CAPTURE_DIR");
    CHECK(rmdir(dir) == 0);   /* still empty */
}
#endif

int main(void)
{
    /* Unresolved entries are still callable diagnostics, but are not reported as implemented. */
    CHECK(((int (*)(void))fn("test", "testUnresolvedEntry"))() == -1);
    CHECK(!is_implemented("test", "testUnresolvedEntry"));

    const finit init = (finit)fn("libkernel", "scePthreadMutexInit");
    const f1 lock = (f1)fn("libkernel", "scePthreadMutexLock"), trylock = (f1)fn("libkernel", "scePthreadMutexTrylock");
    const f1 unlock = (f1)fn("libkernel", "scePthreadMutexUnlock"), destroy = (f1)fn("libkernel", "scePthreadMutexDestroy");
    const f1 ainit = (f1)fn("libkernel", "scePthreadMutexattrInit"), adestroy = (f1)fn("libkernel", "scePthreadMutexattrDestroy");
    const f2i settype = (f2i)fn("libkernel", "scePthreadMutexattrSettype");
    const f1 plock = (f1)fn("libScePosix", "pthread_mutex_lock");
    const f_u32_u32 htonl = (f_u32_u32)fn("libSceNet", "sceNetHtonl");
    const f_u16_u16 htons = (f_u16_u16)fn("libSceNet", "sceNetHtons");
    const f_u32_u32 ntohl = (f_u32_u32)fn("libSceNet", "sceNetNtohl");
    const f_u16_u16 ntohs = (f_u16_u16)fn("libSceNet", "sceNetNtohs");
    net_inet_pton_entry = fn("libSceNet", "sceNetInetPton");
    if (failures) return 1;

    /* default (error-checking): relocking is EDEADLK (FreeBSD 11), as an SCE code */
    void *m = NULL;
    CHECK(init(&m, NULL, "m") == 0 && m);
    CHECK(lock(&m) == 0);
    CHECK(lock(&m) == (int)0x8002000b);
    CHECK(plock(&m) == 11);                       /* the POSIX name returns the errno itself */
    CHECK(unlock(&m) == 0);
    CHECK(unlock(&m) == (int)0x80020001);         /* not the owner: EPERM */
    CHECK(destroy(&m) == 0 && m == (void *)2);
    CHECK(lock(&m) == (int)0x80020016);           /* destroyed: EINVAL */

    /* a static initializer (NULL) becomes a mutex on first lock */
    void *s = NULL;
    CHECK(unlock(&s) == (int)0x80020001);
    CHECK(lock(&s) == 0 && s && s != (void *)1 && s != (void *)2);
    CHECK(unlock(&s) == 0);
    CHECK(destroy(&s) == 0);

    /* recursive through an attribute */
    void *a = NULL, *r = NULL;
    CHECK(ainit(&a) == 0 && a);
    CHECK(settype(&a, 5) == (int)0x80020016);
    CHECK(settype(&a, 2) == 0);
    CHECK(init(&r, &a, NULL) == 0);
    CHECK(lock(&r) == 0 && lock(&r) == 0 && trylock(&r) == 0);
    CHECK(unlock(&r) == 0 && unlock(&r) == 0 && unlock(&r) == 0);
    CHECK(destroy(&r) == 0);
    CHECK(adestroy(&a) == 0 && a == NULL);

    /* the process parameter block comes from the loaded executable */
    static unsigned char block[0x40];
    rt_process.proc_param = block;
    CHECK(((void *(*)(void))fn("libkernel", "sceKernelGetProcParam"))() == block);

    /* a thread variable equals itself and nothing else: the same object pointer is the same thread.
       The runtime has a single guest thread so far (scePthreadCreate is not implemented), so the
       unequal cases compare it with a pointer that is not a thread: they exercise pointer inequality
       only, not two distinct valid thread handles. */
    static unsigned char main_stack[0x100];
    rt_thread_register_main(main_stack, sizeof main_stack);
    void *const self = ((void *(*)(void))fn("libkernel", "scePthreadSelf"))();
    const f_equal equal = (f_equal)fn("libkernel", "scePthreadEqual");
    unsigned char not_a_thread;
    CHECK(self && equal(self, self) != 0);
    CHECK(equal(self, &not_a_thread) == 0 && equal(&not_a_thread, self) == 0);
    CHECK(equal(self, NULL) == 0);

    /* A thread attribute takes one of the two detach states, joinable (0) or detached (1). Any
       other value, or a variable holding no attribute object, is EINVAL: an SCE code under the SCE
       name, the errno itself under the POSIX name. The state is read back the way thread creation
       will read it, through rt_thread_attr_detach_state. */
    const f1 thread_attr_init = (f1)fn("libkernel", "scePthreadAttrInit");
    const f1 thread_attr_destroy = (f1)fn("libkernel", "scePthreadAttrDestroy");
    const f_attr_get thread_attr_get = (f_attr_get)fn("libkernel", "scePthreadAttrGet");
    const f2i set_detach = (f2i)fn("libkernel", "scePthreadAttrSetdetachstate");
    const f2i posix_set_detach = (f2i)fn("libScePosix", "pthread_attr_setdetachstate");
    if (failures) return 1;
    void *thread_attr = NULL;
    CHECK(thread_attr_init(&thread_attr) == 0 && thread_attr);

    /* A fresh attribute has no state of its own, and a rejected value does not give it one:
       reading it stops instead of answering joinable. */
    CHECK(set_detach(&thread_attr, 2) == (int)0x80020016);
    CHECK(posix_set_detach(&thread_attr, -1) == 22);
    detach_reader_attr = thread_attr;
    CHECK(died_in_fatal(read_detach_state));

    /* The last state set is the one kept, and a rejected value leaves it alone. */
    CHECK(set_detach(&thread_attr, 1) == 0 && detach_state_of(&thread_attr) == 1);
    CHECK(set_detach(&thread_attr, 0) == 0 && detach_state_of(&thread_attr) == 0);
    CHECK(set_detach(&thread_attr, 1) == 0);
    CHECK(set_detach(&thread_attr, 2) == (int)0x80020016);
    CHECK(set_detach(&thread_attr, -1) == (int)0x80020016);
    CHECK(detach_state_of(&thread_attr) == 1);

    /* The POSIX name records on the same attribute object. */
    CHECK(posix_set_detach(&thread_attr, 0) == 0 && detach_state_of(&thread_attr) == 0);
    CHECK(posix_set_detach(&thread_attr, 2) == 22 && posix_set_detach(&thread_attr, -1) == 22);
    CHECK(detach_state_of(&thread_attr) == 0);
    CHECK(set_detach(NULL, 0) == (int)0x80020016 && posix_set_detach(NULL, 0) == 22);

    /* scePthreadAttrGet replaces the attribute with the running thread's, whose detach state the
       system supplies: the state set before is gone, and a state set afterwards is kept again. */
    CHECK(set_detach(&thread_attr, 1) == 0);
    CHECK(thread_attr_get(self, &thread_attr) == 0);
    detach_reader_attr = thread_attr;
    CHECK(died_in_fatal(read_detach_state));
    CHECK(set_detach(&thread_attr, 0) == 0 && detach_state_of(&thread_attr) == 0);

    CHECK(thread_attr_destroy(&thread_attr) == 0 && thread_attr == NULL);
    /* Destroyed, the variable holds no object: setting is EINVAL. In FreeBSD libthr, creating a
       thread from an empty variable, or from none, uses the default attributes, so reading stops
       for both. */
    CHECK(set_detach(&thread_attr, 0) == (int)0x80020016);
    CHECK(posix_set_detach(&thread_attr, 0) == 22);
    detach_reader_attr = thread_attr;
    CHECK(died_in_fatal(read_detach_state));
    CHECK(died_in_fatal(read_detach_state_without_variable));
    CHECK(is_implemented("libkernel", "scePthreadAttrSetdetachstate"));
    CHECK(is_implemented("libScePosix", "pthread_attr_setdetachstate"));

    /* sched_yield gives up the processor and succeeds with 0, every time. That the host scheduler
       actually ran another thread is not observable from here, so only the contract's result is
       checked. */
    const f_check yield = (f_check)fn("libkernel", "sched_yield");
    CHECK(yield() == 0 && yield() == 0);
    CHECK(is_implemented("libkernel", "sched_yield"));

    /* scePthreadYield's PS4 signature (no result, or an int) is not established, so the entry stops
       by name and is registered without counting as implemented. */
    void (*pthread_yield_entry)(void) = (void (*)(void))fn("libkernel", "scePthreadYield");
    CHECK(died_in_fatal(pthread_yield_entry));
    CHECK(!is_implemented("libkernel", "scePthreadYield"));

    /* scePthreadAttrGetaffinity(attr, mask) reads the CPUs an attribute admits. The value the game
       counts as libc.elf starts is the whole application CPU set: the six cores of the PS4's eight
       that are left to applications (see runtime/syslib/kernel_thread.c). */
    const f_affinity getaffinity = (f_affinity)fn("libkernel", "scePthreadAttrGetaffinity");
    const f_initattr attr_init_call = (f_initattr)fn("libkernel", "scePthreadAttrInit");
    const f_destroyattr attr_destroy_call = (f_destroyattr)fn("libkernel", "scePthreadAttrDestroy");
    const f_attrget attr_get_call = (f_attrget)fn("libkernel", "scePthreadAttrGet");
    CHECK(is_implemented("libkernel", "scePthreadAttrGetaffinity"));
    uint64_t mask = 0;

    /* an attribute taken from the running thread carries its CPU set */
    void *attrs = NULL;
    CHECK(attr_init_call(&attrs) == 0 && attrs);
    CHECK(attr_get_call(self, &attrs) == 0);
    CHECK(getaffinity(&attrs, &mask) == 0 && mask == UINT64_C(0x3f));
    CHECK(attr_destroy_call(&attrs) == 0 && attrs == NULL);

    /* a fresh attribute defaults to the same whole set */
    void *fresh = NULL;
    mask = 0;
    CHECK(attr_init_call(&fresh) == 0 && fresh);
    CHECK(getaffinity(&fresh, &mask) == 0 && mask == UINT64_C(0x3f));
    CHECK(attr_destroy_call(&fresh) == 0);

    /* a null attribute or mask is EINVAL, the value the neighbouring entries report */
    void *one = NULL;
    CHECK(attr_init_call(&one) == 0);
    CHECK(getaffinity(&one, NULL) == (int)0x80020016);
    CHECK(getaffinity(NULL, &mask) == (int)0x80020016);
    CHECK(attr_destroy_call(&one) == 0);

    /* sceKernelGetDirectMemorySize() takes nothing and returns the size of the console's direct
       memory region, the pool sceKernelAllocateDirectMemory allocates from. The documented region
       of the console (see runtime/syslib/kernel.c) bounds every allocation eboot makes with it; the
       largest observed one asks for 0x7400000 bytes. */
    const f_size direct_memory_size = (f_size)fn("libkernel", "sceKernelGetDirectMemorySize");
    CHECK(is_implemented("libkernel", "sceKernelGetDirectMemorySize"));
    CHECK(direct_memory_size() == UINT64_C(0x180000000));
    CHECK(direct_memory_size() > UINT64_C(0x7400000));

    /* Direct memory: sceKernelAllocateDirectMemory hands out a physical range of the region the
       previous call reports, sceKernelMapDirectMemory makes that range memory the guest can use,
       and sceKernelReleaseDirectMemory gives it back to the pool. eboot's request is 116 MiB at
       2 MiB alignment, searched from 0 to the region's end. The pool's own behaviour - reuse,
       alignment, fragmentation over a few hundred allocations - is tested in test_dmem_pool.c. */
    const f_dmem_alloc dm_alloc = (f_dmem_alloc)fn("libkernel", "sceKernelAllocateDirectMemory");
    const f_dmem_map dm_map = (f_dmem_map)fn("libkernel", "sceKernelMapDirectMemory");
    const f_dmem_release dm_release = (f_dmem_release)fn("libkernel", "sceKernelReleaseDirectMemory");
    CHECK(is_implemented("libkernel", "sceKernelAllocateDirectMemory"));
    CHECK(is_implemented("libkernel", "sceKernelMapDirectMemory"));
    CHECK(is_implemented("libkernel", "sceKernelReleaseDirectMemory"));
    if (failures) return 1;

    /* The executable's image is reserved at the bottom of the region, so the first allocatable
       address is the image's end, aligned, and never 0. */
    rt_process.image_size = 0x5d00000;
    const uint64_t image_end = (rt_process.image_size + 0x1fffff) & ~UINT64_C(0x1fffff);
    uint64_t phys = 0;
    CHECK(dm_alloc(0, (int64_t)direct_memory_size(), 0x7400000, 0x200000, 0, &phys) == 0);
    CHECK(phys == image_end);
    CHECK(phys % 0x200000 == 0 && phys + 0x7400000 <= direct_memory_size());

    /* The mapping is usable memory with the protection asked for (CPU read and write), and it
       starts zeroed; the last byte is inside the mapping, not past it. */
    void *addr = NULL;
    CHECK(dm_map(&addr, 0x7400000, 0x3, 0, phys, 0x200000) == 0 && addr);
    if (addr) {
        unsigned char *bytes = addr;
        bytes[0] = 0x5a;
        bytes[0x1000] = 0xa5;
        bytes[0x7400000 - 1] = 0x11;
        CHECK(bytes[0] == 0x5a && bytes[0x1000] == 0xa5 && bytes[0x7400000 - 1] == 0x11);
        CHECK(bytes[1] == 0);
        CHECK(munmap(addr, 0x7400000) == 0);

        /* MAP_FIXED maps at the address the caller names, which is how a title places the range
           where its own pointers expect it. */
        void *fixed = addr;
        CHECK(dm_map(&fixed, 0x7400000, 0x3, 0x10, phys, 0x200000) == 0);
        CHECK(fixed == addr);
        CHECK(munmap(fixed, 0x7400000) == 0);
    }

    /* A release returns the range, and only the range: a second release, a part of it, and a
       mapping of it are all refused once it is back in the pool. */
    CHECK(dm_release((int64_t)phys, 0x7400000) == 0);
    CHECK(dm_release((int64_t)phys, 0x7400000) == (int)0x80020016);
    CHECK(dm_release((int64_t)phys, 0x4000000) == (int)0x80020016);
    void *released = NULL;
    CHECK(dm_map(&released, 0x7400000, 0x3, 0, phys, 0x200000) == (int)0x80020016);

    /* The next request of the same size gets the released range back. */
    uint64_t again = 0;
    CHECK(dm_alloc(0, (int64_t)direct_memory_size(), 0x7400000, 0x200000, 0, &again) == 0);
    CHECK(again == phys);
    CHECK(dm_release((int64_t)again, 0x7400000) == 0);

    /* Arguments the region cannot satisfy, and a window no free range fits. */
    uint64_t rejected = 0;
    CHECK(dm_alloc(0, (int64_t)direct_memory_size(), 0, 0x1000, 0, &rejected) == (int)0x80020016);
    CHECK(dm_alloc(0, (int64_t)direct_memory_size(), 0x1000, 3, 0, &rejected) == (int)0x80020016);
    CHECK(dm_alloc(0, (int64_t)direct_memory_size(), 0x1000, 0x1000, 0, NULL) == (int)0x8002000e);
    CHECK(dm_alloc(0, 0x1000, 0x2000, 0x1000, 0, &rejected) == (int)0x8002000c);
    CHECK(dm_release(0, 0x1000) == (int)0x80020016);

    /* The PS4 host is little-endian AMD64; the network representation is big-endian. */
    CHECK(htonl(UINT32_C(0x12345678)) == UINT32_C(0x78563412));
    CHECK(htons(UINT16_C(0x1234)) == UINT16_C(0x3412));
    CHECK(ntohl(UINT32_C(0x78563412)) == UINT32_C(0x12345678));
    CHECK(ntohs(UINT16_C(0x3412)) == UINT16_C(0x1234));
    CHECK(htonl(0) == 0 && ntohl(0) == 0);
    CHECK(htons(UINT16_MAX) == UINT16_MAX && ntohs(UINT16_MAX) == UINT16_MAX);
    CHECK(ntohl(htonl(UINT32_C(0x01020304))) == UINT32_C(0x01020304));
    CHECK(ntohs(htons(UINT16_C(0x0102))) == UINT16_C(0x0102));

    /* The executable's observed use is AF_INET. Output is four network-order bytes; malformed
       text returns zero. Other address families and null-pointer behavior remain fail-loud. */
    unsigned char address[4] = {0};
    const f_inet_pton inet_pton_call = (f_inet_pton)net_inet_pton_entry;
    CHECK(is_implemented("libSceNet", "sceNetInetPton"));
    CHECK(inet_pton_call(2, "192.0.2.1", address) == 1);
    CHECK(address[0] == 192 && address[1] == 0 && address[2] == 2 && address[3] == 1);
    CHECK(inet_pton_call(2, "0.0.0.0", address) == 1);
    CHECK(address[0] == 0 && address[1] == 0 && address[2] == 0 && address[3] == 0);
    CHECK(inet_pton_call(2, "255.255.255.255", address) == 1);
    CHECK(address[0] == 255 && address[1] == 255 && address[2] == 255 && address[3] == 255);
    CHECK(inet_pton_call(2, "192.0.2.256", address) == 0);
    CHECK(inet_pton_call(2, "192.0.2", address) == 0);
    CHECK(died_in_fatal(net_pton_unknown_family_call));
    CHECK(died_in_fatal(net_pton_null_source_call));
    CHECK(died_in_fatal(net_pton_null_destination_call));

    /* libSceNetCtl: the offline answers (issue #41). */
    netctl_get_state_entry = fn("libSceNetCtl", "sceNetCtlGetState");
    netctl_check_entry = fn("libSceNetCtl", "sceNetCtlCheckCallback");
    netctl_register_entry = fn("libSceNetCtl", "sceNetCtlRegisterCallback");
    netctl_unregister_entry = fn("libSceNetCtl", "sceNetCtlUnregisterCallback");
    netctl_get_info_entry = fn("libSceNetCtl", "sceNetCtlGetInfo");
    netctl_get_nat_info_entry = fn("libSceNetCtl", "sceNetCtlGetNatInfo");
    CHECK(is_implemented("libSceNetCtl", "sceNetCtlGetState"));
    CHECK(is_implemented("libSceNetCtl", "sceNetCtlCheckCallback"));
    CHECK(is_implemented("libSceNetCtl", "sceNetCtlRegisterCallback"));
    CHECK(is_implemented("libSceNetCtl", "sceNetCtlUnregisterCallback"));
    CHECK(!is_implemented("libSceNetCtl", "sceNetCtlGetInfo"));
    CHECK(!is_implemented("libSceNetCtl", "sceNetCtlGetNatInfo"));
    if (failures) return 1;
    netctl_shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (netctl_shared == MAP_FAILED) { perror("mmap"); return 1; }
    char poisoned[64];
    memset(poisoned, 0xa5, sizeof poisoned);
    memset(netctl_shared, 0xa5, sizeof poisoned);

    int state = -1;
    CHECK(netctl_state(&state) == 0 && state == 0);   /* the disconnected state */
    CHECK(netctl_check() == 0);                       /* nothing is due yet */

    /* A registered callback is kept, not run: offline there is no state change to report. The id
       comes back through the caller's pointer, which is what the guest's own sites read back. */
    CHECK(netctl_register((void *)netctl_must_not_run, &state, &netctl_id_a) == 0);
    CHECK(netctl_id_a != 0 && netctl_id_a != -1);       /* written, and never the unwritten look */
    CHECK(netctl_check() == 0);
    CHECK(netctl_callbacks_run == 0);

    /* Keyed by id rather than by state: a second registration is its own registration, and each
       removes under its own id. */
    CHECK(netctl_register((void *)netctl_must_not_run, NULL, &netctl_id_b) == 0);
    CHECK(netctl_id_b != netctl_id_a);
    CHECK(netctl_unregister(netctl_id_a) == 0);
    CHECK(netctl_check() == 0 && netctl_callbacks_run == 0);

    /* Every answer the PS4 has not established stops the process instead of guessing, and the two
       structure calls stop before writing through a buffer whose layout is not established. */
    CHECK(died_in_fatal(unregister_id_a));             /* an id already removed */
    CHECK(died_in_fatal(unregister_unknown_id));       /* an id never handed back */
    CHECK(died_in_fatal(state_into_null));             /* no error value for a bad pointer */
    CHECK(died_in_fatal(register_null_callback));      /* no answer for a null callback */
    CHECK(died_in_fatal(register_null_id));            /* no answer for a null id slot */
    CHECK(died_in_fatal(info_of_module_one));
    CHECK(died_in_fatal(nat_info_with_size));
    CHECK(memcmp(netctl_shared, poisoned, sizeof poisoned) == 0);

    /* The store survived all of that, because every fatal above was in a child. */
    CHECK(netctl_unregister(netctl_id_b) == 0);
    CHECK(netctl_check() == 0 && netctl_callbacks_run == 0);

    /* the target page size has no public source, so both entries stop by name */
    void (*posix_getpagesize)(void) = (void (*)(void))fn("libScePosix", "getpagesize");
    void (*kernel_getpagesize)(void) = (void (*)(void))fn("libkernel", "getpagesize");
    CHECK(died_in_fatal(posix_getpagesize));
    CHECK(died_in_fatal(kernel_getpagesize));
    /* Registered so a boot that reaches the call names it, but not counted as implemented: the
       fatal alone does not prove the classification, since an absent entry would also never be
       reported as implemented. */
    CHECK(!is_implemented("libScePosix", "getpagesize"));
    CHECK(!is_implemented("libkernel", "getpagesize"));

    /* memcpy and memset (libSceLibcInternal): ISO C fixes the bytes written and the returned
       destination. The guard bytes past n prove nothing beyond n is touched. */
    void *(*libc_memcpy)(void *, const void *, size_t) =
        (void *(*)(void *, const void *, size_t))fn("libSceLibcInternal", "memcpy");
    void *(*libc_memset)(void *, int, size_t) = (void *(*)(void *, int, size_t))fn("libSceLibcInternal", "memset");
    const unsigned char source[5] = {1, 2, 3, 4, 5};
    unsigned char copy[6] = {0, 0, 0, 0, 0, 0xee};
    CHECK(libc_memcpy(copy, source, 5) == copy);
    CHECK(memcmp(copy, source, 5) == 0 && copy[5] == 0xee);
    CHECK(libc_memcpy(copy, (const unsigned char[1]){0x77}, 0) == copy && copy[0] == 1);
    unsigned char filled[4] = {0, 0, 0, 0xee};
    CHECK(libc_memset(filled, 0x1a5, 3) == filled);           /* c is converted to unsigned char */
    CHECK(filled[0] == 0xa5 && filled[1] == 0xa5 && filled[2] == 0xa5 && filled[3] == 0xee);
    CHECK(libc_memset(filled, 0, 0) == filled && filled[0] == 0xa5);
    CHECK(is_implemented("libSceLibcInternal", "memcpy") && is_implemented("libSceLibcInternal", "memset"));
    call_capture_tests(direct_memory_size, getaffinity, attr_init_call, attr_destroy_call);

    if (!failures) puts("syslib: ok");
    return failures != 0;
}
