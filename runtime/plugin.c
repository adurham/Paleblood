/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/call_capture.h"
#include "runtime/guest.h"
#include "runtime/host.h"
#include "runtime/hooks.h"
#include "runtime/probe.h"

unsigned char *rt_image;

extern const rt_hook_entry game_hooks[];
extern const unsigned game_hook_count;

/* BB_HOOK_SKIP: comma-separated addresses or names of hooks to leave out (bisecting a problem
 * between replaced functions). */
static int skipped(const rt_hook_entry *h)
{
    const char *env = getenv("BB_HOOK_SKIP");
    char addr[16];
    if (!env || !*env)
        return 0;
    snprintf(addr, sizeof addr, "0x%08x", h->address);
    for (const char *p = env; *p;) {
        size_t n = strcspn(p, ",");
        if ((n == strlen(addr) && !strncmp(p, addr, n)) || (n == strlen(h->name) && !strncmp(p, h->name, n)))
            return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

int bbgame_init(const rt_host *host)
{
    if (!host || host->version != RT_HOST_API_VERSION) {
        fprintf(stderr, "bbgame: loader interface version %u, expected %u\n",
                host ? host->version : 0u, RT_HOST_API_VERSION);
        return 1;
    }
    rt_image = host->image;
    if (rt_hooks_install(game_hooks, game_hook_count))
        return 1;
    unsigned installed = 0, recorded = 0;
    for (unsigned i = 0; i < game_hook_count; i++) {
        const rt_hook_entry *h = &game_hooks[i];
        if (skipped(h)) {
            fprintf(stderr, "bbgame: %s (0x%08x) skipped (BB_HOOK_SKIP)\n", h->name, h->address);
            continue;
        }
        installed++;
        /* With BB_CALL_CAPTURE_DIR set the hook goes through a recording thunk; otherwise this is
           the replacement itself (runtime/call_capture.h). */
        const void *target = rt_call_capture_wrap(h->name, (void *)h->fn);
        recorded += target != (const void *)h->fn;
        if (h->address < RT_EBOOT_BASE || host->install_hook(h->address - RT_EBOOT_BASE, h->size, target)) {
            fprintf(stderr, "bbgame: cannot hook %s at 0x%08x (%u bytes)\n", h->name, h->address, h->size);
            return 1;
        }
    }
    fprintf(stderr, "bbgame: %u hook(s) installed\n", installed);
    if (recorded)
        fprintf(stderr, "bbgame: calls to %u hook(s) are recorded (BB_CALL_CAPTURE_DIR)\n", recorded);
    return rt_probes_install(host->image, host->image_size);
}
