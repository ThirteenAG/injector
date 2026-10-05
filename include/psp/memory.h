#ifndef PSP_EMULATOR_MEMORY_H
#define PSP_EMULATOR_MEMORY_H
#include "hooks.h"

static inline psp_hook_status psp_memory_read(const psp_hook_backend* backend,
    uint32_t address, void* out, size_t bytes)
{
    if (!bytes) return PSP_HOOK_OK;
    if (!backend || !backend->read || !out || bytes > UINT32_MAX || address > UINT32_MAX - bytes)
        return PSP_HOOK_INVALID_ARGUMENT;
    return backend->read(backend->user, address, out, bytes) ? PSP_HOOK_OK : PSP_HOOK_READ_FAILED;
}

/* Installation-time writes. Publish aligned words even for byte/halfword data,
 * preserving neighboring bytes: the PS2 host service accepts whole words only.
 * Chunk storage is bounded on the stack, and every published chunk is flushed.
 * Multi-chunk writes can fail partially; callers install while quiescent. Input
 * must not overlap the destination. No guest/game heap or allocator is involved. */
static inline psp_hook_status psp_memory_write_ex(const psp_hook_backend* backend,
    uint32_t address, const void* input, size_t bytes, size_t* completed)
{
    const unsigned char* source = (const unsigned char*)input;
    uint32_t words[16];
    if (completed) *completed = 0;
    if (!bytes) return PSP_HOOK_OK;
    if (!backend || !backend->read || !backend->write || !backend->flush || !input ||
        bytes > UINT32_MAX - 3u || address > UINT32_MAX - (bytes + 3u)) return PSP_HOOK_INVALID_ARGUMENT;
    while (bytes) {
        uint32_t base = address & ~3u;
        size_t offset = address & 3u, used = bytes < sizeof(words) - offset ? bytes : sizeof(words) - offset;
        size_t span = (offset + used + 3u) & ~(size_t)3;
        if (offset || span != used) {
            if (!backend->read(backend->user, base, words, span)) return PSP_HOOK_READ_FAILED;
        }
        memcpy((unsigned char*)words + offset, source, used);
        if (!backend->write(backend->user, base, words, span)) return PSP_HOOK_WRITE_FAILED;
        backend->flush(backend->user, base, span);
        if (completed) *completed += used;
        source += used; address += (uint32_t)used; bytes -= used;
    }
    return PSP_HOOK_OK;
}
static inline psp_hook_status psp_memory_write(const psp_hook_backend* backend,
    uint32_t address, const void* input, size_t bytes)
{ return psp_memory_write_ex(backend, address, input, bytes, NULL); }
static inline psp_hook_status psp_memory_fill(const psp_hook_backend* backend,
    uint32_t address, unsigned char value, size_t bytes)
{
    unsigned char fill[64];
    if (bytes > UINT32_MAX || address > UINT32_MAX - bytes) return PSP_HOOK_INVALID_ARGUMENT;
    memset(fill, value, sizeof(fill));
    while (bytes) {
        size_t used = bytes < sizeof(fill) ? bytes : sizeof(fill);
        psp_hook_status status = psp_memory_write(backend, address, fill, used);
        if (status != PSP_HOOK_OK) return status;
        address += (uint32_t)used; bytes -= used;
    }
    return PSP_HOOK_OK;
}
#endif
