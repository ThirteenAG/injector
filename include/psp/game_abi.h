#ifndef INJECTOR_PSP_GAME_ABI_H
#define INJECTOR_PSP_GAME_ABI_H
#include "hooks.h"

/* Native games and SDK EABI code disagree on callee-saved floating registers.
 * This callback boundary preserves their union. Integer/float arguments already
 * use independent compact register banks; only overflow stack words are copied.
 * The guest build must treat f21/f23/f25/f27 as caller-saved and reserve f28/f30
 * so a direct call into either ABI cannot invalidate a live compiler temporary. */
static inline size_t psp_game_callback_words(unsigned stack_words)
{ return stack_words<=64 ? 40u+2u*stack_words : 0; }
static inline psp_hook_status psp_game_emit_callback(uint32_t* words,size_t capacity,
    uint32_t address,uint32_t target,unsigned stack_words,size_t* written)
{
    unsigned i,spill,frame;size_t count=0;
    if (!words || !written || !address || !target || (address&3) || (target&3) || stack_words>64)
        return PSP_HOOK_INVALID_ARGUMENT;
    if (capacity<psp_game_callback_words(stack_words)) return PSP_HOOK_BUFFER_TOO_SMALL;
    if (!psp_hook_jump_fits(address,target)) return PSP_HOOK_OUT_OF_RANGE;
    spill=(stack_words*4u+15u)&~15u;frame=spill+64u;
#define PSP_GAME_WORD(word) words[count++]=(word)
    PSP_GAME_WORD(0x27BD0000u | ((0u-frame)&0xFFFFu));
    PSP_GAME_WORD(0xAFBF0000u | (spill+48u));
    PSP_GAME_WORD(0xAFBC0000u | (spill+52u));
    PSP_GAME_WORD(0x4458F800u); /* cfc1 t8,fcsr */
    PSP_GAME_WORD(0xAFB80000u | (spill+56u));
    for (i=0;i<12;++i) PSP_GAME_WORD(0xE7A00000u | ((20u+i)<<16) | (spill+i*4u));
    for (i=0;i<stack_words;++i) {
        PSP_GAME_WORD(0x8FB80000u | (frame+i*4u));
        PSP_GAME_WORD(0xAFB80000u | (i*4u));
    }
    PSP_GAME_WORD(0x0C000000u | ((target>>2)&0x03FFFFFFu));
    PSP_GAME_WORD(0);
    for (i=0;i<12;++i) PSP_GAME_WORD(0xC7A00000u | ((20u+i)<<16) | (spill+i*4u));
    PSP_GAME_WORD(0x8FBC0000u | (spill+52u));
    PSP_GAME_WORD(0x8FB80000u | (spill+56u));
    PSP_GAME_WORD(0x44D8F800u); /* ctc1 t8,fcsr */
    PSP_GAME_WORD(0x8FBF0000u | (spill+48u));
    PSP_GAME_WORD(0x03E00008u);
    PSP_GAME_WORD(0x27BD0000u | frame);
#undef PSP_GAME_WORD
    *written=count;return PSP_HOOK_OK;
}
#endif
