#ifndef WFP_PSP_HOOKS_GUEST_H
#define WFP_PSP_HOOKS_GUEST_H
#include "hooks.h"
#include "guest_memory.h"
#include <psputils.h>
#include <pspintrman.h>

typedef struct psp_hook_guest {
    uint32_t storage_begin, storage_end;
    void* (*allocate)(size_t);
    void (*release)(void*);
} psp_hook_guest;

/* Cached mapped RAM, including CLEO's kernel-module partition below user RAM.
 * No scratchpad, uncached/kernel aliases or MMIO. Code allocation is separately
 * restricted to the caller's explicitly supplied module-owned storage. */
static inline int psp_hook_guest_range(uint32_t address, size_t size)
{ return psp_guest_ram_range(address, size); }
static inline int psp_hook_guest_read(void* user, uint32_t address, void* output, size_t size)
{
    (void)user;
    if (!output || !psp_hook_guest_range(address, size)) return 0;
    /* PPSSPP stores JIT entry markers in guest instruction memory. Invalidate
     * before decoding/comparing so it restores the original MIPS words. This
     * is an installation-only operation, never part of a hook callback. */
    sceKernelIcacheInvalidateRange((const void*)(uintptr_t)address, (unsigned int)size);
    memcpy(output, (const void*)(uintptr_t)address, size);
    return 1;
}
static inline int psp_hook_guest_write(void* user, uint32_t address, const void* input, size_t size)
{
    (void)user;
    if (!input || !psp_hook_guest_range(address, size)) return 0;
    unsigned int interrupts = sceKernelCpuSuspendIntr();
    /* Restore intact PPSSPP markers before replacing a translated entry. */
    sceKernelIcacheInvalidateRange((const void*)(uintptr_t)address, (unsigned int)size);
    memmove((void*)(uintptr_t)address, input, size);
    sceKernelDcacheWritebackRange((const void*)(uintptr_t)address, (unsigned int)size);
    sceKernelIcacheInvalidateRange((const void*)(uintptr_t)address, (unsigned int)size);
    sceKernelCpuResumeIntr(interrupts);
    return 1;
}
static inline int psp_hook_guest_allocate(void* user, size_t bytes, uint32_t near_address, psp_hook_buffer* out)
{
    psp_hook_guest* guest = (psp_hook_guest*)user;
    void* allocation;
    uintptr_t address;
    if (!bytes || bytes > guest->storage_end - guest->storage_begin) return 0;
    allocation = guest->allocate(bytes);
    if (!allocation) return 0;
    address = (uintptr_t)allocation;
    if (address < guest->storage_begin || address > guest->storage_end - bytes || (address & 15) ||
        !psp_hook_jump_fits(near_address, (uint32_t)address)) {
        guest->release(allocation);
        return 0;
    }
    out->address = (uint32_t)address; out->words = (uint32_t*)allocation;
    out->capacity = bytes; out->allocation = allocation;
    return 1;
}
static inline void psp_hook_guest_release(void* user, psp_hook_buffer* buffer)
{ ((psp_hook_guest*)user)->release(buffer->allocation); }
static inline void psp_hook_guest_flush(void* user, uint32_t address, size_t bytes)
{
    (void)user;
    sceKernelDcacheWritebackRange((const void*)(uintptr_t)address, (unsigned int)bytes);
    sceKernelIcacheInvalidateRange((const void*)(uintptr_t)address, (unsigned int)bytes);
}
static inline psp_hook_status psp_hook_guest_backend(psp_hook_backend* backend,
    psp_hook_guest* guest, uint32_t storage_begin, size_t storage_size,
    void* (*allocate)(size_t), void (*release)(void*))
{
    if (!backend || !guest || !allocate || !release || (storage_begin & 15) || storage_size < 32 ||
        !psp_hook_guest_range(storage_begin, storage_size)) return PSP_HOOK_INVALID_ARGUMENT;
    guest->storage_begin = storage_begin; guest->storage_end = storage_begin + (uint32_t)storage_size;
    guest->allocate = allocate; guest->release = release;
    backend->user = guest; backend->read = psp_hook_guest_read; backend->write = psp_hook_guest_write;
    backend->allocate = psp_hook_guest_allocate; backend->release = psp_hook_guest_release;
    backend->flush = psp_hook_guest_flush;
    return PSP_HOOK_OK;
}
#endif
