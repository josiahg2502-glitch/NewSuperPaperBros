/* Example New Super Paper Bros. code mod: coins count double, and a line in the log every 10 seconds.
 * Build (Windows, llvm-mingw):  x86_64-w64-mingw32-clang -shared -O2 -I<repo>/wiicompiled/runtime/include example_mod.c -o example_mod.dll
 * Build (Linux):                cc -shared -fPIC -O2 -I<repo>/wiicompiled/runtime/include example_mod.c -o example_mod.so
 * Install: <mods folder>/Example/code/example_mod.dll (Options > Mods > Open the mods folder). */
#include "spm_mod_api.h"

#include <stdio.h>

static const SpmModApi* api;
static uint32_t coin_calls, mario_calls, frames;

/* marioMain(): only counted, to show a hook on a function the game calls every frame. */
static int on_mario_main(SpmCpu* cpu, void* user) {
    (void)cpu;
    (void)user;
    ++mario_calls;
    return 0;
}

/* pouchAddCoin(amount): double the amount, then let the game's own function run. */
static int on_add_coin(SpmCpu* cpu, void* user) {
    (void)user;
    api->gpr(cpu)[3] *= 2;
    ++coin_calls;
    return 0;
}

static void on_frame(SpmCpu* cpu, void* user) {
    (void)cpu;
    (void)user;
    if (++frames % 600 == 0) {
        char text[128];
        snprintf(text, sizeof text, "example mod: %u frames, %u coin pickups doubled, marioMain ran %u times", frames,
                 coin_calls, mario_calls);
        api->log(text);
    }
}

SPM_MOD_EXPORT int spm_mod_init(const SpmModApi* a) {
    api = a;
    if (a->version < SPM_MOD_API_VERSION) return 0;
    const uint32_t add_coin = a->find_function("pouchAddCoin");
    if (!add_coin || !a->hook(add_coin, on_add_coin, 0)) return 0;
    a->hook(a->find_function("marioMain"), on_mario_main, 0);
    a->on_frame(on_frame, 0);
    a->log("example mod: loaded");
    return 1;
}
