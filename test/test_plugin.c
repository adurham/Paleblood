/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime/call_capture.h"
#include "runtime/guest.h"
#include "runtime/hooks.h"
#include "runtime/host.h"

typedef struct {
    uint64_t offset;
    uint64_t size;
    const void *target;
} installed_hook;

static installed_hook installed[4];
static unsigned installed_count;
static int install_error;
static unsigned probe_count;
static unsigned char *probe_image;
static uint64_t probe_size;
static int probe_error;

static int replacement_one_calls;
static void replacement_one(void) { replacement_one_calls++; }
static void replacement_two(void) {}

const rt_hook_entry game_hooks[] = {
    {0x00401000u, 8u, "replacement_one", replacement_one},
    {0x00402000u, 12u, "replacement_two", replacement_two},
};
const unsigned game_hook_count = sizeof game_hooks / sizeof game_hooks[0];

int rt_probes_install(unsigned char *image, uint64_t image_size)
{
    probe_count++;
    probe_image = image;
    probe_size = image_size;
    return probe_error;
}

static int fake_install_hook(uint64_t offset, uint64_t size, const void *target)
{
    if (installed_count >= sizeof installed / sizeof installed[0])
        return -1;
    installed[installed_count++] = (installed_hook){offset, size, target};
    return install_error;
}

static int check(int condition, const char *what)
{
    if (condition)
        return 0;
    fprintf(stderr, "test_plugin: %s\n", what);
    return 1;
}

static void reset_observations(void)
{
    installed_count = 0;
    install_error = 0;
    probe_count = 0;
    probe_image = NULL;
    probe_size = 0;
    probe_error = 0;
}

int main(void)
{
    unsigned char image[64] = {0};
    rt_host host = {RT_HOST_API_VERSION, image, sizeof image, fake_install_hook};
    int failed = 0;

    reset_observations();
    failed |= check(bbgame_init(NULL) != 0, "null host must be rejected");
    failed |= check(installed_count == 0 && probe_count == 0,
                    "null host must not install hooks or probes");

    reset_observations();
    host.version++;
    failed |= check(bbgame_init(&host) != 0, "unsupported host version must be rejected");
    failed |= check(installed_count == 0 && probe_count == 0,
                    "unsupported host must not install hooks or probes");
    host.version = RT_HOST_API_VERSION;

    /* BB_CALL_CAPTURE_DIR is unset: every hook below gets the replacement itself, as before call
       capture existed. */
    reset_observations();
    failed |= check(unsetenv("BB_CALL_CAPTURE_DIR") == 0, "call capture must start off");
    failed |= check(bbgame_init(&host) == 0, "compatible host must initialize");
    failed |= check(installed_count == 2, "compatible host must receive both hooks");
    failed |= check(installed[0].offset == 0x1000 && installed[0].size == 8 &&
                    installed[0].target == (const void *)replacement_one,
                    "first hook must use image-relative offset, size, and replacement");
    failed |= check(installed[1].offset == 0x2000 && installed[1].size == 12 &&
                    installed[1].target == (const void *)replacement_two,
                    "second hook must use image-relative offset, size, and replacement");
    failed |= check(rt_image == image, "guest image must be retained for replacements");
    failed |= check(probe_count == 1 && probe_image == image && probe_size == sizeof image,
                    "probe installation must receive the host image and size after hooks");
    failed |= check(rt_hook_count() == 2 && rt_hook_lookup(0x00401000u) == replacement_one &&
                    rt_hook_lookup(0x00402000u) == replacement_two,
                    "registered replacements must remain available by guest address");

    reset_observations();
    failed |= check(setenv("BB_HOOK_SKIP", "replacement_two", 1) == 0,
                    "hook-name skip setting must be configurable");
    failed |= check(bbgame_init(&host) == 0, "host must initialize with one hook skipped by name");
    failed |= check(installed_count == 1 && installed[0].offset == 0x1000 && probe_count == 1,
                    "name skip must omit only the selected hook and continue initialization");

    reset_observations();
    failed |= check(setenv("BB_HOOK_SKIP", "0x00401000", 1) == 0,
                    "hook-address skip setting must be configurable");
    failed |= check(bbgame_init(&host) == 0, "host must initialize with one hook skipped by address");
    failed |= check(installed_count == 1 && installed[0].offset == 0x2000 && probe_count == 1,
                    "address skip must omit only the selected hook and continue initialization");
    failed |= check(unsetenv("BB_HOOK_SKIP") == 0, "hook skip setting must be removed after the test");

    reset_observations();
    install_error = 1;
    failed |= check(bbgame_init(&host) != 0, "hook installation failure must abort initialization");
    failed |= check(installed_count == 1 && probe_count == 0,
                    "hook failure must stop before later hooks and probes");

    reset_observations();
    probe_error = 1;
    failed |= check(bbgame_init(&host) != 0, "probe installation failure must be returned");
    failed |= check(installed_count == 2 && probe_count == 1,
                    "probe failure must occur after hooks are installed");

    /* With call capture on, a wanted hook gets a recording thunk that still runs its replacement;
       the others keep the replacement itself (runtime/call_capture.h). In a build without call
       capture every hook keeps its replacement and nothing is written. */
    reset_observations();
    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pb-plugin-capture-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    failed |= check(mkdtemp(dir) != NULL, "a temporary capture directory must be created");
    setenv("BB_CALL_CAPTURE_DIR", dir, 1);
    setenv("BB_CALL_CAPTURE_ONLY", "replacement_one", 1);
    setenv("BB_CAPTURE_RUN", "plugin-test", 1);
    failed |= check(bbgame_init(&host) == 0, "host must initialize with call capture on");
#if BB_ENABLE_CALL_CAPTURE
    failed |= check(installed_count == 2 && installed[0].target != (const void *)replacement_one &&
                    installed[1].target == (const void *)replacement_two,
                    "only the wanted hook must go through a recording thunk");
    if (installed_count == 2 && installed[0].target) {
        ((void (*)(void))installed[0].target)();
        failed |= check(replacement_one_calls == 1, "the thunk must run the replacement");
    }
    char path[600];
    snprintf(path, sizeof path, "%s/replacement_one/plugin-test.calls.jsonl", dir);
    FILE *record = fopen(path, "r");
    char line[4096] = "";
    failed |= check(record && fgets(line, sizeof line, record) &&
                    strstr(line, "\"name\": \"replacement_one\""),
                    "the call must be recorded under the hook's name");
    if (record)
        fclose(record);
    unlink(path);
    snprintf(path, sizeof path, "%s/replacement_one", dir);
    rmdir(path);
#else
    failed |= check(installed_count == 2 && installed[0].target == (const void *)replacement_one &&
                    installed[1].target == (const void *)replacement_two,
                    "a build without call capture must install the replacements themselves");
#endif
    failed |= check(rmdir(dir) == 0, "the capture directory must hold nothing else");
    unsetenv("BB_CALL_CAPTURE_DIR");
    unsetenv("BB_CALL_CAPTURE_ONLY");
    unsetenv("BB_CAPTURE_RUN");

    return failed ? 1 : 0;
}
