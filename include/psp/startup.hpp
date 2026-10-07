#ifndef PSP_EMULATOR_STARTUP_HPP
#define PSP_EMULATOR_STARTUP_HPP
#include "patches.h"
#include "memory.h"
#include <new>

namespace injector { namespace detail {
enum class entry_kind { patch, hook, permanent_hook, cancelled };
struct startup_entry {
    psp_hook_buffer storage;
    startup_entry *next, *previous;
    union { psp_patch patch; psp_hook permanent; } value;
    psp_hook* owner;
    startup_entry** owner_token;
    psp_hook_status* owner_error;
    uint32_t callback, id;
    void (*on_failure)(void*);
    void* cleanup_user;
    bool original_used;
    entry_kind kind;
};
struct startup_state {
    startup_entry *first, *last;
    uint32_t generation, next_id;
    psp_hook_status error;
    uint32_t failed_address; /* Site whose validation/publication failed. */
    bool active;
};
inline startup_state startup{};
inline psp_hook_backend backend{};

inline startup_entry* new_entry(uint32_t address) {
    psp_hook_buffer buffer{};
    if (!backend.allocate || !backend.release ||
        !backend.allocate(backend.user, sizeof(startup_entry), address, &buffer)) return nullptr;
    if (!buffer.words || buffer.capacity < sizeof(startup_entry) ||
        (reinterpret_cast<uintptr_t>(buffer.words) & (alignof(startup_entry) - 1))) {
        backend.release(backend.user, &buffer); return nullptr;
    }
    auto entry = new (buffer.words) startup_entry{};
    entry->storage = buffer;
    entry->previous = startup.last;
    entry->id = ++startup.next_id;
    if (startup.last) startup.last->next = entry;
    else startup.first = entry;
    startup.last = entry;
    return entry;
}
inline void observe_original(uint32_t generation, uint32_t id) {
    if (!startup.active || startup.generation != generation) return;
    for (auto entry = startup.first; entry; entry = entry->next)
        if (entry->id == id && entry->kind == entry_kind::patch) { entry->original_used = true; return; }
}
inline startup_entry* queue_hook(psp_hook* hook, startup_entry** token, psp_hook_status* error) {
    auto entry = new_entry(hook->address);
    if (!entry) return nullptr;
    entry->kind = entry_kind::hook;
    entry->owner = hook; entry->owner_token = token; entry->owner_error = error;
    return entry;
}
inline void rebind_hook(startup_entry* entry, psp_hook* hook,
                        startup_entry** token, psp_hook_status* error) {
    if (entry) { entry->owner = hook; entry->owner_token = token; entry->owner_error = error; }
}
inline void cancel_hook(startup_entry* entry) {
    if (entry) {
        entry->kind = entry_kind::cancelled;
        entry->owner = nullptr; entry->owner_token = nullptr; entry->owner_error = nullptr;
    }
}
inline void release_hook(startup_entry* entry, const psp_hook& hook) {
    entry->value.permanent = hook;
    entry->owner = &entry->value.permanent;
    entry->owner_token = nullptr; entry->owner_error = nullptr;
    entry->kind = entry_kind::permanent_hook;
}

inline bool overlaps(uint32_t first, size_t bytes, uint32_t other, size_t length) {
    return uint64_t(first) < uint64_t(other) + length && uint64_t(other) < uint64_t(first) + bytes;
}
inline bool queued_overlap(uint32_t address, size_t bytes) {
    for (auto entry = startup.first; entry; entry = entry->next) {
        if (entry->kind == entry_kind::patch && overlaps(address, bytes, entry->value.patch.address, entry->value.patch.count * 4)) return true;
        if (entry->owner && overlaps(address, bytes, entry->owner->address, entry->owner->count * 4)) return true;
    }
    return false;
}

// Queue byte/halfword/word data patches as aligned windows. Writes to bytes in
// an already queued word coalesce; adjacent fields behave like ordinary writes.
// No changes reach the guest until FlushCaches validates the entire session.
inline psp_hook_status queue_memory(uint32_t address, const void* input, size_t bytes) {
    if (startup.error != PSP_HOOK_OK) return startup.error;
    if (!bytes) return PSP_HOOK_OK;
    if (!input || bytes > UINT32_MAX - 3u || address > UINT32_MAX - (bytes + 3u)) return PSP_HOOK_INVALID_ARGUMENT;
    auto source = static_cast<const unsigned char*>(input);
    while (bytes) {
        uint32_t base = address & ~3u;
        size_t offset = address & 3u, used = bytes < 4 - offset ? bytes : 4 - offset;
        startup_entry* existing = nullptr;
        for (auto entry = startup.first; entry; entry = entry->next) {
            if (entry->owner && overlaps(base, 4, entry->owner->address, entry->owner->count * 4))
                return PSP_HOOK_CONFLICT;
            if (entry->kind == entry_kind::patch && base >= entry->value.patch.address &&
                base < entry->value.patch.address + entry->value.patch.count * 4) { existing = entry; break; }
        }
        if (existing) {
            auto& patch = existing->value.patch;
            auto word = reinterpret_cast<unsigned char*>(patch.replacement) + base - patch.address;
            if (existing->callback && base == patch.address && memcmp(word + offset, source, used))
                return PSP_HOOK_CONFLICT; // Do not silently change a verified call's target.
            memcpy(word + offset, source, used);
        } else {
            // Combine the largest contiguous unowned portion into one record.
            size_t run = used;
            while (offset + run < 64 && run < bytes) {
                uint32_t next = base + static_cast<uint32_t>((offset + run + 3) & ~size_t(3));
                if (queued_overlap(next, 4)) break;
                size_t available = 64 - offset - run;
                size_t add = bytes - run < available ? bytes - run : available;
                if (add > 4) add = 4;
                run += add;
            }
            size_t span = (offset + run + 3) & ~size_t(3);
            uint32_t words[16];
            auto status = psp_memory_read(&backend, base, words, span);
            if (status != PSP_HOOK_OK) return status;
            memcpy(reinterpret_cast<unsigned char*>(words) + offset, source, run);
            auto entry = new_entry(base);
            if (!entry) return PSP_HOOK_ALLOCATION_FAILED;
            status = psp_patch_create(&entry->value.patch, &backend, base, words, span / 4);
            if (status != PSP_HOOK_OK) { entry->kind = entry_kind::cancelled; return status; }
            used = run;
        }
        address += static_cast<uint32_t>(used); source += used; bytes -= used;
    }
    return PSP_HOOK_OK;
}

inline psp_hook_status queued_read(uint32_t address, void* output, size_t bytes) {
    auto status = psp_memory_read(&backend, address, output, bytes);
    if (status != PSP_HOOK_OK || !bytes) return status;
    for (auto entry = startup.first; entry; entry = entry->next) {
        if (entry->kind != entry_kind::patch) continue;
        auto& patch = entry->value.patch;
        uint64_t begin = address > patch.address ? address : patch.address;
        uint64_t end = uint64_t(address) + bytes;
        if (end > uint64_t(patch.address) + patch.count * 4) end = uint64_t(patch.address) + patch.count * 4;
        if (begin < end) memcpy(static_cast<unsigned char*>(output) + size_t(begin - address),
            reinterpret_cast<const unsigned char*>(patch.replacement) + size_t(begin - patch.address), size_t(end - begin));
    }
    return PSP_HOOK_OK;
}

inline psp_hook_status validate_startup() {
    if (startup.error != PSP_HOOK_OK) return startup.error;
    // A requested original means this callback uses one shared hook_back callee.
    // Compare all call sites using it before any instruction is published.
    for (auto expected = startup.first; expected; expected = expected->next) {
        if (!expected->original_used || !expected->callback) continue;
        auto original = psp_patch_original_target(&expected->value.patch);
        for (auto entry = startup.first; entry; entry = entry->next)
            if (entry->kind == entry_kind::patch && entry->callback == expected->callback &&
                psp_patch_original_target(&entry->value.patch) != original) return PSP_HOOK_CONFLICT;
    }
    // Coalesced edits to a call's delay slot must still be ordinary instructions.
    for (auto entry = startup.first; entry; entry = entry->next)
        if (entry->kind == entry_kind::patch && entry->callback &&
            psp_hook_control(entry->value.patch.replacement[1]) != 0) return PSP_HOOK_DELAY_SLOT;
    return PSP_HOOK_OK;
}
inline psp_hook_status finish_startup() {
    auto status = validate_startup();
    startup_entry* last_published = nullptr;
    if (status == PSP_HOOK_OK) {
        for (auto entry = startup.first; entry; entry = entry->next) {
            if (entry->kind == entry_kind::cancelled) continue;
            status = entry->kind == entry_kind::patch ? psp_patch_enable(&entry->value.patch)
                                                      : psp_hook_enable(entry->owner);
            if (status != PSP_HOOK_OK) {
                startup.failed_address = entry->kind == entry_kind::patch ? entry->value.patch.address : entry->owner->address;
                break;
            }
            last_published = entry;
        }
    }
    if (status != PSP_HOOK_OK) {
        for (auto entry = last_published; entry; entry = entry->previous) {
            if (entry->kind == entry_kind::cancelled) continue;
            auto restored = entry->kind == entry_kind::patch ? psp_patch_disable(&entry->value.patch)
                                                            : psp_hook_disable(entry->owner);
            if (restored != PSP_HOOK_OK) {
                // Preserve all metadata/code. Returning failure here could let
                // a PSP loader unload a module still referenced by a live JAL.
#if defined(_MSC_VER)
                __debugbreak();
#else
                __builtin_trap();
#endif
                return restored;
            }
        }
    }
    for (auto entry = startup.first; entry;) {
        auto next = entry->next;
        if (entry->owner_error) *entry->owner_error = status;
        if (entry->owner_token) *entry->owner_token = nullptr;
        if (status != PSP_HOOK_OK && entry->kind == entry_kind::permanent_hook)
            psp_hook_destroy(&entry->value.permanent);
        if (status != PSP_HOOK_OK && entry->on_failure) entry->on_failure(entry->cleanup_user);
        auto storage = entry->storage;
        backend.release(backend.user, &storage);
        entry = next;
    }
    startup.first = startup.last = nullptr; startup.active = false;
    return status;
}
}}
#endif
