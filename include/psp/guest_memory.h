#ifndef WFP_PSP_GUEST_MEMORY_H
#define WFP_PSP_GUEST_MEMORY_H
#include <stddef.h>
#include <stdint.h>

/* Cached physical RAM. The default fits a 32 MiB PSP; PPSSPP plugins must
 * explicitly match the RAM configured in plugin.ini (93 MiB: 0x0dd00000).
 * This is an access bound, not permission to allocate game or kernel memory. */
#ifndef PSP_HOOK_RAM_END
#define PSP_HOOK_RAM_END 0x0a000000u
#endif
#if PSP_HOOK_RAM_END <= 0x08000000u || PSP_HOOK_RAM_END > 0x10000000u
#error PSP_HOOK_RAM_END must bound cached PSP RAM below 0x10000000
#endif
static inline int psp_guest_ram_range(uintptr_t address, size_t size)
{
    return address >= 0x08000000u && size &&
        size <= PSP_HOOK_RAM_END - 0x08000000u && address <= PSP_HOOK_RAM_END - size;
}
#endif
