#ifndef WFP_PCSX2_HOOKS_H
#define WFP_PCSX2_HOOKS_H

/* Standalone C99/C++ hook core. See hooks.md for installation and CPU contracts. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PCSX2_HOOK_MAX_INSTRUCTIONS 16
#define PCSX2_HOOK_CODE_WORDS 240

typedef enum pcsx2_hook_status {
    PCSX2_HOOK_OK, PCSX2_HOOK_INVALID_ARGUMENT, PCSX2_HOOK_UNALIGNED,
    PCSX2_HOOK_DELAY_SLOT, PCSX2_HOOK_UNSUPPORTED_INSTRUCTION,
    PCSX2_HOOK_OUT_OF_RANGE, PCSX2_HOOK_BUFFER_TOO_SMALL,
    PCSX2_HOOK_ALLOCATION_FAILED, PCSX2_HOOK_READ_FAILED,
    PCSX2_HOOK_WRITE_FAILED, PCSX2_HOOK_CONFLICT, PCSX2_HOOK_INVALID_STATE,
    PCSX2_HOOK_BACKEND_UNAVAILABLE
} pcsx2_hook_status;

typedef struct pcsx2_hook_buffer {
    uint32_t address;
    uint32_t* words;
    size_t capacity; /* Bytes, including alignment padding only if usable. */
    void* allocation;
} pcsx2_hook_buffer;

typedef struct pcsx2_hook_backend {
    void* user;
    int (*read)(void*, uint32_t, void*, size_t);
    /* A failed write must leave the requested range untouched. */
    int (*write)(void*, uint32_t, const void*, size_t);
    int (*allocate)(void*, size_t, uint32_t near_address, pcsx2_hook_buffer*);
    void (*release)(void*, pcsx2_hook_buffer*);
    /* Cannot fail. Must publish generated code AND invalidate translated code. */
    void (*flush)(void*, uint32_t, size_t);
    uint32_t state_service; /* Optional ABI-1 host stub; zero means unavailable. */
} pcsx2_hook_backend;

typedef struct pcsx2_hook_register {
    uint32_t word[4];
} pcsx2_hook_register;

/* The generated frame aligns this context to 16 bytes. Values are raw bits. */
typedef struct pcsx2_hook_context {
    pcsx2_hook_register gpr[32];
    pcsx2_hook_register hi, lo;
    uint32_t sa, fcr31;
    uint32_t fpr[32];
    uint32_t acc, accflag; /* Valid only with SAVE_ACC; raw bits, including overflow. */
} pcsx2_hook_context;

typedef void (*pcsx2_mid_callback)(pcsx2_hook_context*, void*);
enum {
    PCSX2_HOOK_SAVE_FPU = 1, PCSX2_HOOK_RAW_CALLBACK = 2,
    PCSX2_HOOK_SAVE_ACC = 4, PCSX2_HOOK_SAVE_VU0 = 8, PCSX2_HOOK_REPLACE_INSTRUCTIONS = 16
};
/* SAVE_VU0 appends this opaque, read-only state after the base context. It
 * borrows an idle VU0 macro state; VU1/VIF and micro/data memories are excluded. */
typedef struct pcsx2_hook_vu0_state { uint32_t opaque[512]; } pcsx2_hook_vu0_state;
static inline const pcsx2_hook_vu0_state* pcsx2_hook_vu0(const pcsx2_hook_context* context)
{ return (const pcsx2_hook_vu0_state*)((const unsigned char*)context + sizeof(*context)); }

typedef struct pcsx2_hook {
    pcsx2_hook_backend backend;
    pcsx2_hook_buffer code;
    uint32_t address, trampoline;
    uint32_t original[PCSX2_HOOK_MAX_INSTRUCTIONS];
    uint32_t patch[PCSX2_HOOK_MAX_INSTRUCTIONS];
    size_t count, code_size;
    unsigned enabled;
} pcsx2_hook;

/* Internal helpers are prefixed too: this header can coexist with injector.h. */
static inline unsigned pcsx2_hook_op(uint32_t w) { return w >> 26; }
static inline uint32_t pcsx2_hook_i(unsigned op, unsigned rs, unsigned rt, int imm)
{ return (op << 26) | (rs << 21) | (rt << 16) | ((uint32_t)imm & 0xffff); }

/* Architectural delay slots, including link branches; traps have none. */
static inline int pcsx2_hook_has_delay_slot(uint32_t w)
{
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    if (op == 2 || op == 3) return 1;
    if (op == 0) return (w & 63) == 8 || (w & 63) == 9;
    if (op == 1) return rt <= 3 || (rt >= 16 && rt <= 19);
    if ((op >= 4 && op <= 7) || (op >= 20 && op <= 23)) return 1;
    return op >= 16 && op <= 18 && rs == 8;
}

/* 0 = ordinary, 1 = branch, 2 = J, 3 = JR, 4 = JAL, 5 = JALR,
 * 6 = linking REGIMM branch, -1 = cannot relocate safely. */
static inline int pcsx2_hook_control(uint32_t w)
{
    unsigned op = pcsx2_hook_op(w), rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    if (op == 2) return 2;
    if (op == 3) return 4;
    if (op == 0) {
        unsigned fn = w & 63;
        if (fn == 8) return 3;
        if (fn == 9) return 5;
        if (fn == 12 || fn == 13 || (fn >= 48 && fn <= 54)) return -1;
    }
    if (op == 1) {
        if (rt <= 3) return 1;
        if (rt >= 16 && rt <= 19) return 6;
        return (rt == 24 || rt == 25) ? 0 : -1; /* EE MTSAB/MTSAH; reject traps. */
    }
    if ((op >= 4 && op <= 7) || (op >= 20 && op <= 23)) return 1;
    if (op >= 16 && op <= 18) {
        if (rs == 8) return rt <= 3 ? 1 : -1;
        if (op == 16 && rs >= 16) return -1; /* ERET, TLB, privilege state. */
    }
    return 0;
}

static inline int pcsx2_hook_jump_fits(uint32_t pc, uint32_t destination)
{ return !(destination & 3) && (((pc + 4) ^ destination) & 0xf0000000u) == 0; }
static inline uint32_t pcsx2_hook_jump(uint32_t destination)
{ return 0x08000000u | ((destination >> 2) & 0x03ffffffu); }

static inline uint32_t pcsx2_hook_target(uint32_t pc, uint32_t word)
{
    if (pcsx2_hook_op(word) == 2 || pcsx2_hook_op(word) == 3)
        return ((pc + 4) & 0xf0000000u) | ((word & 0x03ffffffu) << 2);
    return pc + 4 + (uint32_t)((int32_t)(int16_t)word * 4);
}

static inline int pcsx2_hook_branch_fits(uint32_t pc, uint32_t target)
{
    int64_t delta = (int64_t)target - ((int64_t)pc + 4);
    return !(target & 3) && delta >= -131072 && delta <= 131068;
}

/* Pure relocation: no allocations, reads, writes, or patching of live memory.
 * Linking instructions keep their delay slot. When the call pair ends the window,
 * the link register receives the original return address (the window end), so the
 * callee returns straight to game code exactly as before. A call earlier in the
 * window links back into the trampoline, which then runs the rest of the window;
 * such code must stay alive while that callee can still return. */
static inline pcsx2_hook_status pcsx2_hook_relocate(
    const uint32_t* source, size_t count, uint32_t source_pc,
    uint32_t* output, size_t capacity, uint32_t output_pc, size_t* written)
{
    size_t map[PCSX2_HOOK_MAX_INSTRUCTIONS], sizes[PCSX2_HOOK_MAX_INSTRUCTIONS];
    size_t lead[PCSX2_HOOK_MAX_INSTRUCTIONS];
    size_t i, used = 0;
    uint32_t end;
    if (!source || !output || !written || !count || count > PCSX2_HOOK_MAX_INSTRUCTIONS)
        return PCSX2_HOOK_INVALID_ARGUMENT;
    *written = 0;
    if ((source_pc | output_pc) & 3) return PCSX2_HOOK_UNALIGNED;
    if (source_pc > UINT32_MAX - count * 4) return PCSX2_HOOK_OUT_OF_RANGE;
    end = source_pc + (uint32_t)count * 4;
    for (i = 0; i < count;) {
        int control = pcsx2_hook_control(source[i]);
        uint32_t target = pcsx2_hook_target(source_pc + (uint32_t)i * 4, source[i]);
        unsigned rs = (source[i] >> 21) & 31, rd = (source[i] >> 11) & 31;
        int last = i + 2 == count;
        if (control < 0) return PCSX2_HOOK_UNSUPPORTED_INSTRUCTION;
        map[i] = used;
        sizes[i] = 1;
        lead[i] = 0;
        if (control) {
            if (i + 1 == count) return PCSX2_HOOK_DELAY_SLOT;
            if (pcsx2_hook_control(source[i + 1]) != 0) return PCSX2_HOOK_DELAY_SLOT;
            /* The architecture leaves these link forms unpredictable. */
            if ((control == 5 && rd && rd == rs) || (control == 6 && rs == 31))
                return PCSX2_HOOK_UNSUPPORTED_INSTRUCTION;
            if ((control == 4 && last) || (control == 5 && last && rd) || control == 6) lead[i] = 2;
            sizes[i] = 2;
            if ((control == 1 || control == 6) && !(target >= source_pc && target < end) &&
                !pcsx2_hook_branch_fits(output_pc + (uint32_t)(used + lead[i]) * 4, target))
                sizes[i] = 6;
            sizes[i] += lead[i];
            map[i + 1] = used + lead[i] + 1;
            sizes[i + 1] = 0;
            used += sizes[i];
            i += 2;
        } else { ++used; ++i; }
    }
    if (used + 2 > capacity) return PCSX2_HOOK_BUFFER_TOO_SMALL;
    if (output_pc > UINT32_MAX - (used + 2) * 4) return PCSX2_HOOK_OUT_OF_RANGE;
    /* Validate every jump before writing any output. */
    for (i = 0; i < count; ++i) {
        int control;
        uint32_t pc, target;
        if (!sizes[i]) continue;
        control = pcsx2_hook_control(source[i]);
        pc = output_pc + (uint32_t)(map[i] + lead[i]) * 4;
        target = pcsx2_hook_target(source_pc + (uint32_t)i * 4, source[i]);
        if (target >= source_pc && target < end)
            target = output_pc + (uint32_t)map[(target - source_pc) / 4] * 4;
        if (((control == 2 || control == 4) && !pcsx2_hook_jump_fits(pc, target)) ||
            (sizes[i] - lead[i] == 6 && (!pcsx2_hook_jump_fits(pc + 8, pc + 24) ||
                                         !pcsx2_hook_jump_fits(pc + 16, target))))
            return PCSX2_HOOK_OUT_OF_RANGE;
        if ((control == 1 || control == 6) && sizes[i] - lead[i] == 2 &&
            !pcsx2_hook_branch_fits(pc, target))
            return PCSX2_HOOK_OUT_OF_RANGE;
    }
    if (!pcsx2_hook_jump_fits(output_pc + (uint32_t)used * 4, end))
        return PCSX2_HOOK_OUT_OF_RANGE;
    for (i = 0; i < count;) {
        int control = pcsx2_hook_control(source[i]);
        uint32_t word = source[i];
        uint32_t target = pcsx2_hook_target(source_pc + (uint32_t)i * 4, word);
        size_t n = map[i], k = map[i] + lead[i];
        uint32_t pc = output_pc + (uint32_t)k * 4;
        if (target >= source_pc && target < end)
            target = output_pc + (uint32_t)map[(target - source_pc) / 4] * 4;
        if (lead[i]) {
            /* Link register = original return point (window end) or next relocated word. */
            unsigned link = control == 5 ? (word >> 11) & 31 : 31;
            uint32_t back = i + 2 == count ? end : output_pc + (uint32_t)(n + sizes[i]) * 4;
            output[n] = pcsx2_hook_i(15, 0, link, (int)(back >> 16));
            output[n + 1] = pcsx2_hook_i(13, link, link, (int)(back & 0xffff));
        }
        output[k] = word;
        if (control == 2) output[k] = pcsx2_hook_jump(target);
        if (control == 4) output[k] = lead[i] ? pcsx2_hook_jump(target) : 0x0c000000u | ((target >> 2) & 0x03ffffffu);
        if (control == 5 && lead[i]) output[k] = (word & (31u << 21)) | 8; /* jr rs */
        if (control == 6) word = (word & ~(31u << 16)) | ((((word >> 16) & 31) - 16) << 16);
        if (control == 1 || control == 6) {
            int64_t offset = ((int64_t)target - ((int64_t)pc + 4)) / 4;
            output[k] = (word & 0xffff0000u) | (sizes[i] - lead[i] == 6 ? 3u : (uint16_t)offset);
        }
        if (control) {
            output[k + 1] = source[i + 1];
            if (sizes[i] - lead[i] == 6) {
                output[k + 2] = pcsx2_hook_jump(pc + 24); output[k + 3] = 0;
                output[k + 4] = pcsx2_hook_jump(target); output[k + 5] = 0;
            }
            i += 2;
        } else ++i;
    }
    output[used] = pcsx2_hook_jump(end); output[used + 1] = 0;
    *written = used + 2;
    return PCSX2_HOOK_OK;
}

/* Shared preparation captures a complete instruction/delay-slot window. */
static inline pcsx2_hook_status pcsx2_hook_prepare_ex(
    pcsx2_hook* hook, const pcsx2_hook_backend* backend, uint32_t address, size_t count, size_t prefix_words, int replace)
{
    uint32_t previous;
    size_t i;
    size_t code_bytes;
    if (!hook || !backend || !backend->read || !backend->write || !backend->allocate ||
        !backend->release || !backend->flush || hook->code.words ||
        count < 2 || count > PCSX2_HOOK_MAX_INSTRUCTIONS || address < 4 ||
        address > UINT32_MAX - PCSX2_HOOK_MAX_INSTRUCTIONS * 4)
        return PCSX2_HOOK_INVALID_ARGUMENT;
    if (address & 3) return PCSX2_HOOK_UNALIGNED;
    if (!backend->read(backend->user, address - 4, &previous, 4)) return PCSX2_HOOK_READ_FAILED;
    if (pcsx2_hook_has_delay_slot(previous)) return PCSX2_HOOK_DELAY_SLOT;
    memset(hook, 0, sizeof(*hook));
    hook->backend = *backend; hook->address = address; hook->count = count;
    if (!backend->read(backend->user, address, hook->original, count * 4))
        return PCSX2_HOOK_READ_FAILED;
    for (i = 0; i < hook->count; ++i) {
        int control = pcsx2_hook_control(hook->original[i]);
        int call = pcsx2_hook_op(hook->original[i]) == 3 ||
            (pcsx2_hook_op(hook->original[i]) == 0 && (hook->original[i] & 63) == 9);
        if (control < 0 && !(replace && call)) return PCSX2_HOOK_UNSUPPORTED_INSTRUCTION;
        if (pcsx2_hook_has_delay_slot(hook->original[i])) {
            if (i + 1 == hook->count) {
                if (hook->count == PCSX2_HOOK_MAX_INSTRUCTIONS) return PCSX2_HOOK_DELAY_SLOT;
                if (!backend->read(backend->user, address + (uint32_t)hook->count * 4,
                                   &hook->original[hook->count], 4)) return PCSX2_HOOK_READ_FAILED;
                ++hook->count;
            }
            if (pcsx2_hook_control(hook->original[++i]) != 0) return PCSX2_HOOK_DELAY_SLOT;
        }
    }
    /* A linking far branch relocates to eight words per instruction pair. */
    code_bytes = (prefix_words + hook->count * 4 + 2) * 4;
    code_bytes = (code_bytes + 15) & ~(size_t)15;
    if (!backend->allocate(backend->user, code_bytes, address, &hook->code))
        return PCSX2_HOOK_ALLOCATION_FAILED;
    if (!hook->code.words || ((uintptr_t)hook->code.words & 15) || (hook->code.address & 15) ||
        hook->code.capacity < code_bytes) {
        backend->release(backend->user, &hook->code);
        memset(&hook->code, 0, sizeof(hook->code));
        return PCSX2_HOOK_ALLOCATION_FAILED;
    }
    return PCSX2_HOOK_OK;
}

static inline pcsx2_hook_status pcsx2_hook_prepare(
    pcsx2_hook* hook, const pcsx2_hook_backend* backend, uint32_t address, size_t count, size_t prefix_words)
{ return pcsx2_hook_prepare_ex(hook, backend, address, count, prefix_words, 0); }

static inline void pcsx2_hook_discard(pcsx2_hook* hook)
{
    if (hook->code.words) hook->backend.release(hook->backend.user, &hook->code);
    memset(hook, 0, sizeof(*hook));
}

static inline pcsx2_hook_status pcsx2_hook_enable(pcsx2_hook* hook)
{
    uint32_t current[PCSX2_HOOK_MAX_INSTRUCTIONS];
    if (!hook || !hook->code.words) return PCSX2_HOOK_INVALID_STATE;
    if (!hook->backend.read(hook->backend.user, hook->address, current, hook->count * 4))
        return PCSX2_HOOK_READ_FAILED;
    if (memcmp(current, hook->enabled ? hook->patch : hook->original, hook->count * 4))
        return PCSX2_HOOK_CONFLICT;
    if (hook->enabled) return PCSX2_HOOK_OK;
    hook->backend.flush(hook->backend.user, hook->code.address, hook->code_size);
    if (!hook->backend.write(hook->backend.user, hook->address, hook->patch, hook->count * 4))
        return PCSX2_HOOK_WRITE_FAILED;
    hook->backend.flush(hook->backend.user, hook->address, hook->count * 4);
    hook->enabled = 1;
    return PCSX2_HOOK_OK;
}

static inline pcsx2_hook_status pcsx2_hook_disable(pcsx2_hook* hook)
{
    uint32_t current[PCSX2_HOOK_MAX_INSTRUCTIONS];
    if (!hook || !hook->code.words) return PCSX2_HOOK_INVALID_STATE;
    if (!hook->backend.read(hook->backend.user, hook->address, current, hook->count * 4))
        return PCSX2_HOOK_READ_FAILED;
    if (memcmp(current, hook->enabled ? hook->patch : hook->original, hook->count * 4))
        return PCSX2_HOOK_CONFLICT;
    if (!hook->enabled) return PCSX2_HOOK_OK;
    if (!hook->backend.write(hook->backend.user, hook->address, hook->original, hook->count * 4))
        return PCSX2_HOOK_WRITE_FAILED;
    hook->backend.flush(hook->backend.user, hook->address, hook->count * 4);
    hook->enabled = 0;
    return PCSX2_HOOK_OK;
}

static inline pcsx2_hook_status pcsx2_hook_destroy(pcsx2_hook* hook)
{
    pcsx2_hook_status status;
    if (!hook) return PCSX2_HOOK_INVALID_ARGUMENT;
    if (!hook->code.words) { memset(hook, 0, sizeof(*hook)); return PCSX2_HOOK_OK; }
    status = pcsx2_hook_disable(hook);
    if (status == PCSX2_HOOK_OK) pcsx2_hook_discard(hook);
    return status; /* Keep referenced code alive on a conflict/write failure. */
}

static inline pcsx2_hook_status pcsx2_hook_finish(pcsx2_hook* hook, size_t prefix, uint32_t destination)
{
    size_t written = 0;
    pcsx2_hook_status status;
    if (!pcsx2_hook_jump_fits(hook->address, destination)) {
        pcsx2_hook_discard(hook); return PCSX2_HOOK_OUT_OF_RANGE;
    }
    hook->trampoline = hook->code.address + (uint32_t)prefix * 4;
    status = pcsx2_hook_relocate(hook->original, hook->count, hook->address,
        hook->code.words + prefix, hook->code.capacity / 4 - prefix, hook->trampoline, &written);
    if (status != PCSX2_HOOK_OK) { pcsx2_hook_discard(hook); return status; }
    hook->code_size = (prefix + written) * 4;
    memset(hook->patch, 0, sizeof(hook->patch));
    hook->patch[0] = pcsx2_hook_jump(destination);
    return PCSX2_HOOK_OK;
}

/* Creates disabled. The replacement uses the normal EE function ABI. */
static inline pcsx2_hook_status pcsx2_hook_create_inline(pcsx2_hook* hook,
    const pcsx2_hook_backend* backend, uint32_t address, uint32_t replacement, size_t count)
{
    pcsx2_hook_status status;
    if (!replacement) return PCSX2_HOOK_INVALID_ARGUMENT;
    if (replacement & 3) return PCSX2_HOOK_UNALIGNED;
    status = pcsx2_hook_prepare(hook, backend, address, count, 0);
    if (status != PCSX2_HOOK_OK) return status;
    if (replacement >= address && replacement < address + hook->count * 4) {
        pcsx2_hook_discard(hook); return PCSX2_HOOK_INVALID_ARGUMENT;
    }
    return pcsx2_hook_finish(hook, 0, replacement);
}

static inline void pcsx2_hook_emit_li(uint32_t* words, size_t* n, unsigned rt, uint32_t value)
{
    words[(*n)++] = pcsx2_hook_i(15, 0, rt, (int)(value >> 16));
    words[(*n)++] = pcsx2_hook_i(13, rt, rt, (int)value);
}

/* State stubs return a boolean. A broken negotiated contract must trap before
 * the callback/continuation can consume unrestored state. */
static inline void pcsx2_hook_emit_state(uint32_t* w, size_t* n,
    uint32_t service, unsigned flags, int restore)
{
    pcsx2_hook_emit_li(w, n, 25, service);
    w[(*n)++] = pcsx2_hook_i(25, 29, 4, 64 + 680);
    w[(*n)++] = pcsx2_hook_i(9, 0, 5, (int)((flags & (PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0)) >> 2));
    w[(*n)++] = pcsx2_hook_i(9, 0, 6, restore);
    w[(*n)++] = (25 << 21) | (31 << 11) | 9; w[(*n)++] = 0;
    w[(*n)++] = pcsx2_hook_i(5, 2, 0, 2); w[(*n)++] = 0;
    w[(*n)++] = 0x0000000d; /* break */
}

/* Callback runs BEFORE the relocated instructions. No allocations per call. */
static inline pcsx2_hook_status pcsx2_hook_create_mid(pcsx2_hook* hook,
    const pcsx2_hook_backend* backend, uint32_t address, uint32_t callback,
    uint32_t user, uint32_t module_gp, unsigned flags, size_t count)
{
    /* 64-byte outgoing argument area; context begins at offset 64. */
    const int frame = flags & PCSX2_HOOK_SAVE_VU0 ? 2800 : 752, ctx = 64;
    uint32_t* w;
    size_t n = 0;
    unsigned r;
    pcsx2_hook_status status;
    if (!callback || (flags & ~(PCSX2_HOOK_SAVE_FPU | PCSX2_HOOK_RAW_CALLBACK | PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0 | PCSX2_HOOK_REPLACE_INSTRUCTIONS))) return PCSX2_HOOK_INVALID_ARGUMENT;
    if (callback & 3) return PCSX2_HOOK_UNALIGNED;
    if ((flags & (PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0)) &&
        (!backend || !backend->state_service || (backend->state_service & 3)))
        return PCSX2_HOOK_BACKEND_UNAVAILABLE;
    status = pcsx2_hook_prepare_ex(hook, backend, address, count,
                              ((flags & PCSX2_HOOK_REPLACE_INSTRUCTIONS) ? 2 : 0) +
                              (flags & PCSX2_HOOK_SAVE_FPU ? 155 : 87) +
                              (flags & (PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0) ?
                                  20 + (flags & PCSX2_HOOK_RAW_CALLBACK ? 8 : 0) : 0),
                              (flags & PCSX2_HOOK_REPLACE_INSTRUCTIONS) != 0);
    if (status != PCSX2_HOOK_OK) return status;
    if (callback >= address && callback < address + hook->count * 4) {
        pcsx2_hook_discard(hook); return PCSX2_HOOK_INVALID_ARGUMENT;
    }
    w = hook->code.words;
    w[n++] = pcsx2_hook_i(25, 29, 29, -frame); /* daddiu preserves upper GPR lanes */
    for (r = 0; r < 32; ++r) w[n++] = pcsx2_hook_i(31, 29, r, ctx + (int)r * 16); /* sq */
    w[n++] = pcsx2_hook_i(25, 29, 2, frame);
    w[n++] = pcsx2_hook_i(63, 29, 2, ctx + 29 * 16); /* original sp low 64 */
    w[n++] = 0x70000209u | (2 << 11); /* pmfhi v0 */
    w[n++] = pcsx2_hook_i(31, 29, 2, ctx + 512);
    w[n++] = 0x70000249u | (2 << 11); /* pmflo v0 */
    w[n++] = pcsx2_hook_i(31, 29, 2, ctx + 528);
    w[n++] = (2 << 11) | 0x28; /* mfsa v0 */
    w[n++] = pcsx2_hook_i(43, 29, 2, ctx + 544);
    if (flags & PCSX2_HOOK_SAVE_FPU) {
        w[n++] = 0x44400000u | (2 << 16) | (31 << 11); /* cfc1 v0,fcr31 */
        w[n++] = pcsx2_hook_i(43, 29, 2, ctx + 548);
        for (r = 0; r < 32; ++r) w[n++] = pcsx2_hook_i(57, 29, r, ctx + 552 + (int)r * 4);
    }
    if (flags & (PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0)) {
        pcsx2_hook_emit_state(w, &n, backend->state_service, flags, 0);
        if (flags & PCSX2_HOOK_RAW_CALLBACK)
            for (r = 4; r < 12; ++r) w[n++] = pcsx2_hook_i(30, 29, r, ctx + (int)r * 16);
    }
    /* Zero keeps the incoming GP: suitable for -G0 callbacks that call game code. */
    if (module_gp) pcsx2_hook_emit_li(w, &n, 28, module_gp);
    pcsx2_hook_emit_li(w, &n, 25, callback);
    if (!(flags & PCSX2_HOOK_RAW_CALLBACK)) {
        w[n++] = pcsx2_hook_i(25, 29, 4, ctx); /* a0=context */
        pcsx2_hook_emit_li(w, &n, 5, user);
    }
    w[n++] = (25 << 21) | (31 << 11) | 9; w[n++] = 0; /* jalr ra,t9 */
    if (flags & (PCSX2_HOOK_SAVE_ACC | PCSX2_HOOK_SAVE_VU0))
        pcsx2_hook_emit_state(w, &n, backend->state_service, flags, 1);
    if (flags & PCSX2_HOOK_SAVE_FPU) {
        for (r = 0; r < 32; ++r) w[n++] = pcsx2_hook_i(49, 29, r, ctx + 552 + (int)r * 4);
        w[n++] = pcsx2_hook_i(35, 29, 2, ctx + 548);
        w[n++] = 0x44c00000u | (2 << 16) | (31 << 11); /* ctc1 v0,fcr31 */
    }
    w[n++] = pcsx2_hook_i(30, 29, 2, ctx + 512); /* lq v0 */
    w[n++] = 0x70000229u | (2 << 21); /* pmthi v0 */
    w[n++] = pcsx2_hook_i(30, 29, 2, ctx + 528);
    w[n++] = 0x70000269u | (2 << 21); /* pmtlo v0 */
    w[n++] = pcsx2_hook_i(35, 29, 2, ctx + 544);
    w[n++] = (2 << 21) | 0x29; /* mtsa v0 */
    for (r = 1; r < 32; ++r)
        if (r != 29) w[n++] = pcsx2_hook_i(30, 29, r, ctx + (int)r * 16);
    w[n++] = pcsx2_hook_i(30, 29, 29, ctx + 29 * 16);
    /* Fall through with every restored register intact. No scratch jump. */
    if (flags & PCSX2_HOOK_REPLACE_INSTRUCTIONS) {
        uint32_t resume = address + (uint32_t)hook->count * 4;
        if (!pcsx2_hook_jump_fits(hook->address, hook->code.address) ||
            !pcsx2_hook_jump_fits(hook->code.address + (uint32_t)n * 4, resume)) {
            pcsx2_hook_discard(hook); return PCSX2_HOOK_OUT_OF_RANGE;
        }
        w[n++] = pcsx2_hook_jump(resume); w[n++] = 0;
        hook->trampoline = 0; /* Replaced instructions have no callable original. */
        hook->code_size = n * 4;
        memset(hook->patch, 0, sizeof(hook->patch));
        hook->patch[0] = pcsx2_hook_jump(hook->code.address);
        return PCSX2_HOOK_OK;
    }
    return pcsx2_hook_finish(hook, n, hook->code.address);
}

#endif
