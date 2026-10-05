#ifndef WFP_PSP_PATCHES_H
#define WFP_PSP_PATCHES_H
#include "hooks.h"

/* An instruction patch owns its saved words, not a code allocation. Zero-init
 * before creation; backend.user must outlive the patch. Install while quiescent. */
typedef struct psp_patch {
    psp_hook_backend backend;
    uint32_t address;
    uint32_t original[PSP_HOOK_MAX_INSTRUCTIONS];
    uint32_t replacement[PSP_HOOK_MAX_INSTRUCTIONS];
    size_t count;
    unsigned enabled;
} psp_patch;

static inline psp_hook_status psp_patch_create(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, const uint32_t* words, size_t count)
{
    if (!patch || !backend || !backend->read || !backend->write || !backend->flush ||
        !words || !count || count > PSP_HOOK_MAX_INSTRUCTIONS ||
        address > UINT32_MAX - count * 4) return PSP_HOOK_INVALID_ARGUMENT;
    if (patch->count) return PSP_HOOK_INVALID_STATE;
    if (address & 3) return PSP_HOOK_UNALIGNED;
    if (!backend->read(backend->user, address, patch->original, count * 4))
        return PSP_HOOK_READ_FAILED;
    memcpy(patch->replacement, words, count * 4);
    patch->backend = *backend; patch->address = address; patch->count = count; patch->enabled = 0;
    return PSP_HOOK_OK;
}

static inline psp_hook_status psp_patch_set_enabled(psp_patch* patch, unsigned enabled)
{
    uint32_t current[PSP_HOOK_MAX_INSTRUCTIONS];
    if (!patch || !patch->count) return PSP_HOOK_INVALID_STATE;
    if (!patch->backend.read(patch->backend.user, patch->address, current, patch->count * 4))
        return PSP_HOOK_READ_FAILED;
    if (memcmp(current, patch->enabled ? patch->replacement : patch->original, patch->count * 4))
        return PSP_HOOK_CONFLICT;
    enabled = enabled != 0;
    if (patch->enabled == enabled) return PSP_HOOK_OK;
    if (!patch->backend.write(patch->backend.user, patch->address,
        enabled ? patch->replacement : patch->original, patch->count * 4)) return PSP_HOOK_WRITE_FAILED;
    patch->backend.flush(patch->backend.user, patch->address, patch->count * 4);
    patch->enabled = enabled;
    return PSP_HOOK_OK;
}
static inline psp_hook_status psp_patch_enable(psp_patch* patch)
{ return psp_patch_set_enabled(patch, 1); }
static inline psp_hook_status psp_patch_disable(psp_patch* patch)
{ return psp_patch_set_enabled(patch, 0); }
static inline psp_hook_status psp_patch_destroy(psp_patch* patch)
{
    psp_hook_status status;
    if (!patch) return PSP_HOOK_INVALID_ARGUMENT;
    if (!patch->count) { memset(patch, 0, sizeof(*patch)); return PSP_HOOK_OK; }
    status = psp_patch_disable(patch);
    if (status == PSP_HOOK_OK) memset(patch, 0, sizeof(*patch));
    return status;
}

/* A specialised decoder must not publish a replacement based on stale words
 * if the backend observes a change between decoding and capturing ownership. */
static inline psp_hook_status psp_patch_create_checked(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, const uint32_t* original,
    const uint32_t* replacement, size_t count)
{
    psp_hook_status status = psp_patch_create(patch, backend, address, replacement, count);
    if (status == PSP_HOOK_OK && memcmp(patch->original, original, count * 4)) {
        memset(patch, 0, sizeof(*patch));
        return PSP_HOOK_CONFLICT;
    }
    return status;
}

/* Dedicated call-site replacement: keep and compare the original delay slot.
 * Call the returned target normally; it is a function, not a prologue trampoline. */
static inline psp_hook_status psp_patch_create_call(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, uint32_t target)
{
    uint32_t words[2], original[2], previous;
    if (!patch || !backend || !backend->read || address < 4 || address > UINT32_MAX - 8 || !target)
        return PSP_HOOK_INVALID_ARGUMENT;
    if (patch->count) return PSP_HOOK_INVALID_STATE;
    if ((address | target) & 3) return PSP_HOOK_UNALIGNED;
    if (!psp_hook_jump_fits(address, target)) return PSP_HOOK_OUT_OF_RANGE;
    if (!backend->read(backend->user, address - 4, &previous, 4) ||
        !backend->read(backend->user, address, words, sizeof(words))) return PSP_HOOK_READ_FAILED;
    if (psp_hook_has_delay_slot(previous) || psp_hook_control(words[1]) != 0)
        return PSP_HOOK_DELAY_SLOT;
    if (psp_hook_op(words[0]) != 3) return PSP_HOOK_UNSUPPORTED_INSTRUCTION;
    if (target >= address && target < address + 8) return PSP_HOOK_INVALID_ARGUMENT;
    memcpy(original, words, sizeof(original));
    words[0] = 0x0c000000u | ((target >> 2) & 0x03ffffffu);
    return psp_patch_create_checked(patch, backend, address, original, words, 2);
}
static inline uint32_t psp_patch_original_target(const psp_patch* patch)
{
    if (!patch || !patch->count || (psp_hook_op(patch->original[0]) != 2 &&
        psp_hook_op(patch->original[0]) != 3)) return 0;
    return psp_hook_target(patch->address, patch->original[0]);
}

/* Adjacent LUI + ORI/ADDIU constant, including signed-low carry for ADDIU.
 * Both words are published together. No trampoline or scratch register is used. */
static inline psp_hook_status psp_patch_create_constant(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, uint32_t value)
{
    uint32_t words[2], original[2];
    unsigned reg, op;
    if (!patch || !backend || !backend->read || address > UINT32_MAX - 8) return PSP_HOOK_INVALID_ARGUMENT;
    if (patch->count) return PSP_HOOK_INVALID_STATE;
    if (address & 3) return PSP_HOOK_UNALIGNED;
    if (!backend->read(backend->user, address, words, sizeof(words))) return PSP_HOOK_READ_FAILED;
    reg = (words[0] >> 16) & 31; op = psp_hook_op(words[1]);
    if (psp_hook_op(words[0]) != 15 || ((words[0] >> 21) & 31) || !reg ||
        (op != 13 && op != 9) || ((words[1] >> 21) & 31) != reg || ((words[1] >> 16) & 31) != reg)
        return PSP_HOOK_UNSUPPORTED_INSTRUCTION;
    memcpy(original, words, sizeof(original));
    words[0] = (words[0] & 0xffff0000u) | ((value + (op == 9 ? 0x8000u : 0u)) >> 16);
    words[1] = (words[1] & 0xffff0000u) | (value & 0xffff);
    return psp_patch_create_checked(patch, backend, address, original, words, 2);
}

/* A jump or return replaces two complete instructions, using a NOP delay slot.
 * For function detours which retain the original instructions use an inline hook.
 * Starting in an existing delay slot or splitting nested control flow is rejected. */
static inline psp_hook_status psp_patch_create_transfer(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, uint32_t first)
{
    uint32_t previous, original[2], words[2] = {first, 0};
    if (!patch || !backend || !backend->read || address < 4 || address > UINT32_MAX - 8)
        return PSP_HOOK_INVALID_ARGUMENT;
    if (patch->count) return PSP_HOOK_INVALID_STATE;
    if (address & 3) return PSP_HOOK_UNALIGNED;
    if (!backend->read(backend->user, address - 4, &previous, 4) ||
        !backend->read(backend->user, address, original, sizeof(original))) return PSP_HOOK_READ_FAILED;
    if (psp_hook_has_delay_slot(previous) || psp_hook_control(original[1]) != 0)
        return PSP_HOOK_DELAY_SLOT;
    return psp_patch_create_checked(patch, backend, address, original, words, 2);
}
static inline psp_hook_status psp_patch_create_jump(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, uint32_t target)
{
    if (!target || (target >= address && (uint64_t)target < (uint64_t)address + 8)) return PSP_HOOK_INVALID_ARGUMENT;
    if ((address | target) & 3) return PSP_HOOK_UNALIGNED;
    if (!psp_hook_jump_fits(address, target)) return PSP_HOOK_OUT_OF_RANGE;
    return psp_patch_create_transfer(patch, backend, address, psp_hook_jump(target));
}
static inline psp_hook_status psp_patch_create_return(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address)
{ return psp_patch_create_transfer(patch, backend, address, 0x03e00008u); }
static inline psp_hook_status psp_patch_create_nop(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, size_t bytes)
{
    uint32_t words[PSP_HOOK_MAX_INSTRUCTIONS] = {0};
    if (!bytes || (bytes & 3)) return PSP_HOOK_UNALIGNED;
    if (bytes > sizeof(words)) return PSP_HOOK_BUFFER_TOO_SMALL;
    return psp_patch_create(patch, backend, address, words, bytes / 4);
}
static inline psp_hook_status psp_patch_create_branch(psp_patch* patch,
    const psp_hook_backend* backend, uint32_t address, uint32_t destination)
{
    uint32_t previous, original[2], words[2];
    unsigned op;
    if (!patch || !backend || !backend->read || address < 4 || address > UINT32_MAX - 8)
        return PSP_HOOK_INVALID_ARGUMENT;
    if (patch->count) return PSP_HOOK_INVALID_STATE;
    if ((address | destination) & 3) return PSP_HOOK_UNALIGNED;
    if (!backend->read(backend->user, address - 4, &previous, 4) ||
        !backend->read(backend->user, address, original, sizeof(original))) return PSP_HOOK_READ_FAILED;
    if (psp_hook_has_delay_slot(previous) || psp_hook_control(original[1]) != 0)
        return PSP_HOOK_DELAY_SLOT;
    memcpy(words, original, sizeof(words)); op = psp_hook_op(words[0]);
    if (op == 2 || op == 3) {
        if (!psp_hook_jump_fits(address, destination)) return PSP_HOOK_OUT_OF_RANGE;
        words[0] = (words[0] & 0xfc000000u) | ((destination >> 2) & 0x03ffffffu);
    } else if (psp_hook_control(words[0]) == 1) {
        if (!psp_hook_branch_fits(address, destination)) return PSP_HOOK_OUT_OF_RANGE;
        words[0] = (words[0] & 0xffff0000u) |
            (uint16_t)(((int64_t)destination - ((int64_t)address + 4)) / 4);
    } else return PSP_HOOK_UNSUPPORTED_INSTRUCTION;
    if (destination >= address && destination < address + 8) return PSP_HOOK_INVALID_ARGUMENT;
    return psp_patch_create_checked(patch, backend, address, original, words, 2);
}

#endif
