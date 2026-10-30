/* New Super Paper Bros. code mods: the C interface a native mod (a .dll on Windows, a .so on Linux) is built against.
 *
 * Put the built library in <mods folder>/<your mod>/code/. At start the game loads it and calls its exported
 *     SPM_MOD_EXPORT int spm_mod_init(const SpmModApi* api);
 * Return nonzero when the mod set itself up. Hooks can only be added inside spm_mod_init.
 *
 * Addresses are the game's own (Super Paper Mario's memory, 0x80000000...). Use api->find_function("name") with a name
 * from the game's symbol map instead of a fixed address, so the mod works on both the USA and the European disc. */
#ifndef SPM_MOD_API_H
#define SPM_MOD_API_H

#include <stdint.h>

#ifdef _WIN32
#define SPM_MOD_EXPORT __declspec(dllexport)
#else
#define SPM_MOD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPM_MOD_API_VERSION 1

typedef struct SpmCpu SpmCpu; /* the game's registers for the call being hooked, or the frame callback */

/* Runs before the game function it hooks, with its arguments in r3..r10 / f1..f8. Return 1 to skip the game's
 * function (put its result in r3 or f1 first), 0 to let it run. */
typedef int (*SpmHookFn)(SpmCpu* cpu, void* user);
/* Runs once per game frame (60 times a second), on the game's own thread. */
typedef void (*SpmFrameFn)(SpmCpu* cpu, void* user);

typedef struct SpmModApi {
    uint32_t version;       /* SPM_MOD_API_VERSION */
    const char* mod_folder; /* this mod's folder, UTF-8 */
    int region;             /* 0 USA (R8PE01), 1 Europe (R8PP01) */

    void (*log)(const char* text);

    /* Game memory (big-endian values, converted for you). */
    uint8_t (*read8)(uint32_t address);
    uint16_t (*read16)(uint32_t address);
    uint32_t (*read32)(uint32_t address);
    float (*read_f32)(uint32_t address);
    void (*write8)(uint32_t address, uint8_t value);
    void (*write16)(uint32_t address, uint16_t value);
    void (*write32)(uint32_t address, uint32_t value);
    void (*write_f32)(uint32_t address, float value);

    /* Registers. */
    uint32_t* (*gpr)(SpmCpu* cpu); /* r0..r31, writable */
    double (*get_fpr)(SpmCpu* cpu, int index);
    void (*set_fpr)(SpmCpu* cpu, int index, double value);

    /* Call a game function with up to 8 integer arguments (r3..r10); returns r3. Only from a hook or frame callback. */
    uint32_t (*call)(SpmCpu* cpu, uint32_t function, const uint32_t* args, int count);
    /* Inside a hook on `function`: run the game's own function with the current registers (changed arguments
     * included), skipping hooks; returns r3. */
    uint32_t (*call_original)(SpmCpu* cpu, uint32_t function);

    /* Only during spm_mod_init. Return 0 when the address is not a game function. Very small functions (getters
     * such as marioGetPtr) are built into their callers by the recomp and never reach a hook: hook a caller. */
    int (*hook)(uint32_t function, SpmHookFn callback, void* user);
    void (*on_frame)(SpmFrameFn callback, void* user);

    /* A game function's address by its symbol map name (e.g. "marioGetPtr"), or 0. */
    uint32_t (*find_function)(const char* name);
} SpmModApi;

#ifdef __cplusplus
}
#endif

#endif /* SPM_MOD_API_H */
