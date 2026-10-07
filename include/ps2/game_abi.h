#ifndef INJECTOR_PS2_GAME_ABI_H
#define INJECTOR_PS2_GAME_ABI_H
#include "hooks.h"

/* The SDK is N32, but SGI-built EE games use independent argument banks: integer arguments
 * occupy a0..a7 independently of float arguments in f12..f19. Keep the SDK
 * ABI for libc and module services. Bridge explicitly at the game boundary.
 * Supports up to sixteen integer/pointer and eight single-float arguments.
 * Integer overflow uses the game's 8-byte stack slots (verified against VCS
 * native corona calls). This is not the GCC -mabi=eabi 4-byte stack layout.
 * No variadics, aggregates, doubles or float-bank overflow are accepted.
 * Scalar returns have the same register convention.
 */
typedef enum pcsx2_game_abi_direction {
    PCSX2_GAME_ABI_CALL = 0, /* SDK caller -> game */
    PCSX2_GAME_ABI_CALLBACK = 1 /* game caller -> SDK callback */
} pcsx2_game_abi_direction;

static inline pcsx2_hook_status pcsx2_game_abi_emit(uint32_t* words,
    size_t capacity, uint32_t target, unsigned count, uint32_t floats,
    pcsx2_game_abi_direction direction, size_t* written)
{
    uint32_t code[128];
    unsigned integer_index[24], float_index[24], integers = 0, fp = 0;
    size_t n = 0;
    unsigned i, frame = 0, saved = 0, r;
    if (!words || !written || !target || (target & 3) || count > 24 ||
        (floats >> count) || (direction != PCSX2_GAME_ABI_CALL &&
                              direction != PCSX2_GAME_ABI_CALLBACK))
        return PCSX2_HOOK_INVALID_ARGUMENT;
    for (i = 0; i < count; ++i) {
        integer_index[i] = integers;
        float_index[i] = fp;
        if (floats & (1u << i)) ++fp; else ++integers;
    }
    if (integers > 16 || fp > 8) return PCSX2_HOOK_INVALID_ARGUMENT;
    if (direction == PCSX2_GAME_ABI_CALLBACK) {
        saved = count > 8 ? (count - 8) * 8 : 0;
        saved = (saved + 15) & ~15u;
        frame = saved + 224;
        code[n++] = pcsx2_hook_i(25, 29, 29, -(int)frame);
        code[n++] = pcsx2_hook_i(63, 29, 31, (int)frame - 8);
        /* SGI EE code keeps 128-bit values in s0..s7 and fp. N32 only
         * guarantees their lower 64-bit halves across a compiled callback.
         * Preserve the game contract even when argument banks are identical.
         * GP and the complete callee FPU bank also survive SDK library calls. */
        for (r = 16; r <= 23; ++r) code[n++] = pcsx2_hook_i(31, 29, r, (int)saved + (int)(r - 16) * 16);
        code[n++] = pcsx2_hook_i(31, 29, 28, (int)saved + 128);
        code[n++] = pcsx2_hook_i(31, 29, 30, (int)saved + 144);
        for (r = 20; r < 32; ++r) code[n++] = pcsx2_hook_i(57, 29, r, (int)saved + 160 + (int)(r - 20) * 4);
        code[n++] = (24 << 11) | 0x28; /* mfsa t8 */
        code[n++] = pcsx2_hook_i(43, 29, 24, (int)saved + 208);
        code[n++] = 0x44400000u | (24 << 16) | (31 << 11);
        code[n++] = pcsx2_hook_i(43, 29, 24, (int)saved + 212);
    }
    /* Compact CALL registers in ascending order; expand CALLBACK registers in
     * descending order. This preserves every source without temporary spills.
     * N32 arguments past slot eight live in 8-byte slots at the caller's sp.
     */
    for (i = 0; i < count; ++i) {
        unsigned index = direction == PCSX2_GAME_ABI_CALL ? i : count - i - 1;
        int is_float = (floats & (1u << index)) != 0;
        unsigned game = is_float ? 12 + float_index[index] : 4 + integer_index[index];
        unsigned sdk = (is_float ? 12 : 4) + index;
        if (!is_float && integer_index[index] >= 8) {
            unsigned sdk_offset = (index - 8) * 8;
            unsigned game_offset = (integer_index[index] - 8) * 8;
            if (direction == PCSX2_GAME_ABI_CALL) {
                if (sdk_offset == game_offset) continue;
                code[n++] = pcsx2_hook_i(55, 29, 24, (int)sdk_offset);
                code[n++] = pcsx2_hook_i(63, 29, 24, (int)game_offset);
            } else {
                code[n++] = pcsx2_hook_i(55, 29, 24, (int)(frame + game_offset));
                code[n++] = pcsx2_hook_i(63, 29, 24, (int)sdk_offset);
            }
            continue;
        }
        if (index >= 8) {
            unsigned op = direction == PCSX2_GAME_ABI_CALL
                ? (is_float ? 49 : 55) : (is_float ? 57 : 63);
            code[n++] = pcsx2_hook_i(op, 29, game, (int)(index - 8) * 8);
        } else if (sdk != game) {
            unsigned src = direction == PCSX2_GAME_ABI_CALL ? sdk : game;
            unsigned dst = direction == PCSX2_GAME_ABI_CALL ? game : sdk;
            code[n++] = is_float ? (0x46000006u | (src << 11) | (dst << 6))
                                : ((src << 21) | (dst << 11) | 0x2du); /* daddu dst,src,zero */
        }
    }
    pcsx2_hook_emit_li(code, &n, 25, target);
    code[n++] = frame ? 0x0320f809u : 0x03200008u; /* jalr/jr t9 */
    code[n++] = 0;
    if (frame) {
        for (r = 20; r < 32; ++r) code[n++] = pcsx2_hook_i(49, 29, r, (int)saved + 160 + (int)(r - 20) * 4);
        code[n++] = pcsx2_hook_i(35, 29, 24, (int)saved + 208);
        code[n++] = (24 << 21) | 0x29; /* mtsa t8 */
        code[n++] = pcsx2_hook_i(35, 29, 24, (int)saved + 212);
        code[n++] = 0x44c00000u | (24 << 16) | (31 << 11);
        for (r = 16; r <= 23; ++r) code[n++] = pcsx2_hook_i(30, 29, r, (int)saved + (int)(r - 16) * 16);
        code[n++] = pcsx2_hook_i(30, 29, 28, (int)saved + 128);
        code[n++] = pcsx2_hook_i(30, 29, 30, (int)saved + 144);
        code[n++] = pcsx2_hook_i(55, 29, 31, (int)frame - 8);
        code[n++] = 0x03e00008u;
        code[n++] = pcsx2_hook_i(25, 29, 29, (int)frame);
    }
    if (capacity < n) return PCSX2_HOOK_BUFFER_TOO_SMALL;
    memcpy(words, code, n * sizeof(*words));
    *written = n;
    return PCSX2_HOOK_OK;
}
#endif
