#ifndef WFP_PCSX2_HOOKS_GUEST_H
#define WFP_PCSX2_HOOKS_GUEST_H
#include "hooks.h"

/* Guest integration is separate from the portable emitter. All storage is owned
 * by this module; the supplied allocator must be its private, bounded allocator. */
typedef struct pcsx2_hook_guest {
    uint32_t storage_begin, storage_end, service, write_service;
    void* (*allocate)(size_t);
    void (*release)(void*);
} pcsx2_hook_guest;

static inline int pcsx2_hook_guest_range(uint32_t address, size_t size)
{ return size && size <= 0x08000000u && address <= 0x08000000u - size; }

static inline int pcsx2_hook_guest_read(void* user, uint32_t address, void* output, size_t size)
{
    (void)user;
    if (!output || !pcsx2_hook_guest_range(address, size)) return 0;
    memcpy(output, (const void*)(uintptr_t)address, size);
    return 1;
}
static inline int pcsx2_hook_guest_write(void* user, uint32_t address, const void* input, size_t size)
{
    pcsx2_hook_guest* guest = (pcsx2_hook_guest*)user;
    int (*write)(uint32_t, uint32_t, uint32_t) =
        (int (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)guest->write_service;
    if (!input || ((address | size) & 3) || !pcsx2_hook_guest_range(address, size) ||
        !pcsx2_hook_guest_range((uint32_t)(uintptr_t)input, size)) return 0;
    return write(address, (uint32_t)(uintptr_t)input, (uint32_t)size);
}
static inline int pcsx2_hook_guest_allocate(void* user, size_t bytes, uint32_t near_address, pcsx2_hook_buffer* out)
{
    pcsx2_hook_guest* guest = (pcsx2_hook_guest*)user;
    void* allocation;
    uintptr_t address;
    if (!bytes || bytes > guest->storage_end - guest->storage_begin) return 0;
    allocation = guest->allocate(bytes);
    if (!allocation) return 0;
    address = (uintptr_t)allocation;
    if (address < guest->storage_begin || address > guest->storage_end - bytes || (address & 15) ||
        !pcsx2_hook_jump_fits(near_address, (uint32_t)address)) {
        guest->release(allocation);
        return 0;
    }
    out->address = (uint32_t)address; out->words = (uint32_t*)allocation;
    out->capacity = bytes; out->allocation = allocation;
    return 1;
}
static inline void pcsx2_hook_guest_release(void* user, pcsx2_hook_buffer* buffer)
{ ((pcsx2_hook_guest*)user)->release(buffer->allocation); }
static inline void pcsx2_hook_guest_flush(void* user, uint32_t address, size_t bytes)
{
    pcsx2_hook_guest* guest = (pcsx2_hook_guest*)user;
    int (*service)(uint32_t, uint32_t) = (int (*)(uint32_t, uint32_t))(uintptr_t)guest->service;
    /* Read/write and code allocation have validated these ranges already. A
     * rejected negotiated service indicates a broken host/guest contract. */
    if (!service(address, (uint32_t)bytes)) {
#if defined(_MSC_VER)
        __debugbreak();
#else
        __builtin_trap();
#endif
    }
}

/* raw_context is the existing 32-byte module context. Its last two formerly
 * reserved words negotiate a service pointer and ABI/capability bits. Old hosts
 * leave them zero, so they fail cleanly before any hook is installed. */
static inline pcsx2_hook_status pcsx2_hook_guest_backend(pcsx2_hook_backend* backend,
    pcsx2_hook_guest* guest, const void* raw_context, void* (*allocate)(size_t), void (*release)(void*))
{
    uint32_t context[8];
    if (!backend || !guest || !raw_context || !allocate || !release) return PCSX2_HOOK_INVALID_ARGUMENT;
    memcpy(context, raw_context, sizeof(context));
    if (context[0] != sizeof(context) || context[1] != 1 || context[6] != 0x02000020u ||
        (context[7] >> 16) != 1 || (context[7] & 3) != 3) return PCSX2_HOOK_BACKEND_UNAVAILABLE;
    if (context[4] < 0x02001000u || context[4] >= context[5] || context[5] > 0x08000000u ||
        ((context[4] | context[5]) & 15)) return PCSX2_HOOK_INVALID_ARGUMENT;
    guest->storage_begin = context[4]; guest->storage_end = context[5]; guest->service = context[6];
    guest->write_service = context[6] + 32;
    guest->allocate = allocate; guest->release = release;
    backend->user = guest; backend->read = pcsx2_hook_guest_read; backend->write = pcsx2_hook_guest_write;
    backend->allocate = pcsx2_hook_guest_allocate; backend->release = pcsx2_hook_guest_release;
    backend->flush = pcsx2_hook_guest_flush;
    backend->state_service = context[7] & 4 ? context[6] + 64 : 0;
    return PCSX2_HOOK_OK;
}
#endif
