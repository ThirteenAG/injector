#include "injector.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

uint32_t parseCommand(uint32_t command, uint32_t from, uint32_t to)
{
    if (from > to || to > 31) return 0;
    return (command >> from) & (UINT32_MAX >> (31 - (to - from)));
}

void* GetGP()
{
    void* gp;
    asm volatile (
        "move %0, $gp\n"
        : "=r"(gp)
    );
    return gp;
}

void* SetGP(void* gp)
{
    void* oldgp;
    asm volatile (
        "move %0, $gp\n"
        "move $gp, %1\n"
        : "=r"(oldgp)
        : "r"(gp)
        );
    return oldgp;
}

void SetModuleBaseAddress(uintptr_t addr, size_t size)
{
    injector.module_addr = addr;
    injector.module_size = size;
}

void SetGameBaseAddress(uintptr_t addr, size_t size)
{
    injector.base_addr = addr;
    injector.base_size = size;
}

uintptr_t adjustAddress(uintptr_t addr)
{
    if (addr >= injector.base_addr && addr < injector.base_addr + injector.base_size)
        return addr;
    else if (addr >= injector.module_addr && addr < injector.module_addr + injector.module_size)
        return addr;
    else
    {
        uintptr_t m = MIN(injector.base_addr, injector.module_addr);
        if (addr >= m)
            return addr;
        else
            return addr + injector.base_addr;
    }
}

void WriteMemoryRaw(uintptr_t addr, void* value, size_t size)
{
    memcpy((void*)adjustAddress(addr), value, size);
}

void ReadMemoryRaw(uintptr_t addr, void* ret, size_t size)
{
    memcpy(ret, (const void*)adjustAddress(addr), size);
}

void MemoryFill(uintptr_t addr, uint8_t value, size_t size)
{
    memset((void*)adjustAddress(addr), value, size);
}

/* Legacy patches are permanent. Publish complete instruction windows through
 * the same checked backend as owned hooks; data writes below remain direct. */
static psp_hook_status s_last_patch_status;
psp_hook_status psp_injector_last_patch_status(void) { return s_last_patch_status; }
static unsigned s_patch_failures;
unsigned psp_injector_patch_failures(void) { return s_patch_failures; }
static void RecordPatchStatus(psp_hook_status status)
{
    s_last_patch_status = status;
    if (status != PSP_HOOK_OK) ++s_patch_failures;
}
static int ReadInstructionWords(uintptr_t address, uint32_t* words, size_t count)
{
    psp_hook_guest guest;
    psp_hook_backend backend;
    psp_hook_status status = psp_injector_backend(&backend, &guest);
    if (status == PSP_HOOK_OK && !backend.read(backend.user, (uint32_t)adjustAddress(address), words, count * 4))
        status = PSP_HOOK_READ_FAILED;
    RecordPatchStatus(status);
    return status == PSP_HOOK_OK;
}
static uint32_t ReadInstructionWord(uintptr_t address)
{
    uint32_t word = 0;
    ReadInstructionWords(address, &word, 1);
    return word;
}
static psp_hook_status PublishInstructions(uintptr_t address, const uint32_t* words,
    size_t count, const uint32_t* expected, uintptr_t code, size_t code_size)
{
    psp_hook_guest guest;
    psp_hook_backend backend;
    psp_patch patch = {0};
    psp_hook_status status = psp_injector_backend(&backend, &guest);
    if (status == PSP_HOOK_OK) {
        address = adjustAddress(address);
        status = expected ? psp_patch_create_checked(&patch, &backend, (uint32_t)address, expected, words, count) :
            psp_patch_create(&patch, &backend, (uint32_t)address, words, count);
    }
    if (status == PSP_HOOK_OK) {
        if (code_size) backend.flush(backend.user, (uint32_t)code, code_size);
        status = psp_patch_enable(&patch);
    }
    RecordPatchStatus(status);
    return status;
}
void WriteInstr(uintptr_t addr, uint32_t value)
{
    PublishInstructions(addr, &value, 1, NULL, 0, 0);
}

void WriteMemory8(uintptr_t addr, uint8_t value)
{
    *(uint8_t*)(adjustAddress(addr)) = value;
}

void WriteMemory16(uintptr_t addr, uint16_t value)
{
    *(uint16_t*)(adjustAddress(addr)) = value;
}

void WriteMemory32(uintptr_t addr, uint32_t value)
{
    *(uint32_t*)(adjustAddress(addr)) = value;
}

void WriteMemory64(uintptr_t addr, uint64_t value)
{
    *(uint64_t*)(adjustAddress(addr)) = value;
}

void WriteMemoryFloat(uintptr_t addr, float value)
{
    *(float*)(adjustAddress(addr)) = value;
}

void WriteMemoryDouble(uintptr_t addr, double value)
{
    *(double*)(adjustAddress(addr)) = value;
}

uint8_t ReadMemory8(uintptr_t addr)
{
    return *(uint8_t*)(adjustAddress(addr));
}

uint16_t ReadMemory16(uintptr_t addr)
{
    return *(uint16_t*)(adjustAddress(addr));
}

uint32_t ReadMemory32(uintptr_t addr)
{
    return *(uint32_t*)(adjustAddress(addr));
}

uint64_t ReadMemory64(uintptr_t addr)
{
    return *(uint64_t*)(adjustAddress(addr));
}

float ReadMemoryFloat(uintptr_t addr)
{
    return *(float*)(adjustAddress(addr));
}

double ReadMemoryDouble(uintptr_t addr)
{
    return *(double*)(adjustAddress(addr));
}

uintptr_t GetBranchDestination(uintptr_t at)
{
    static const uint32_t instr_len = 4;

    uint8_t J = parseCommand(j(0x123456), 26, 31);
    uint8_t JAL = parseCommand(jal(0x123456), 26, 31);

    uint32_t bytes = ReadInstructionWord(adjustAddress(at));
    uint8_t instr = parseCommand(bytes, 26, 31);

    if (instr == J || instr == JAL)
        return ((adjustAddress(at) + instr_len) & 0xf0000000u) | (parseCommand(bytes, 0, 25) * instr_len);
    return 0;
}

static uintptr_t MakeJump(uintptr_t at, uintptr_t dest, uint32_t opcode, int clear_delay)
{
    uint32_t words[2];
    uintptr_t original;
    at = adjustAddress(at); dest = adjustAddress(dest);
    if ((at | dest) & 3) { RecordPatchStatus(PSP_HOOK_UNALIGNED); return 0; }
    if (!psp_hook_guest_range((uint32_t)at, clear_delay ? 8 : 4)) {
        RecordPatchStatus(PSP_HOOK_OUT_OF_RANGE); return 0;
    }
    if (!psp_hook_jump_fits((uint32_t)at, (uint32_t)dest)) {
        RecordPatchStatus(PSP_HOOK_OUT_OF_RANGE); return 0;
    }
    original = GetBranchDestination(at);
    words[0] = opcode | (((uint32_t)dest >> 2) & 0x03ffffffu); words[1] = 0;
    if (PublishInstructions(at, words, clear_delay ? 2 : 1, NULL, 0, 0) != PSP_HOOK_OK) return 0;
    return original;
}
uintptr_t MakeJMP(uintptr_t at, uintptr_t dest) { return MakeJump(at, dest, 0x08000000u, 0); }
uintptr_t MakeJMPwNOP(uintptr_t at, uintptr_t dest) { return MakeJump(at, dest, 0x08000000u, 1); }
uintptr_t MakeJAL(uintptr_t at, uintptr_t dest) { return MakeJump(at, dest, 0x0c000000u, 0); }
void MakeNOP(uintptr_t at) { WriteInstr(at, 0); }
void MakeNOPWithSize(uintptr_t at, size_t size)
{
    const uint32_t zeros[16] = {0};
    at = adjustAddress(at);
    if (!size) { RecordPatchStatus(PSP_HOOK_OK); return; }
    if ((at | size) & 3 || !psp_hook_guest_range((uint32_t)at, size)) {
        RecordPatchStatus(PSP_HOOK_INVALID_ARGUMENT); return;
    }
    while (size) {
        size_t bytes = size < sizeof(zeros) ? size : sizeof(zeros);
        if (PublishInstructions(at, zeros, bytes / 4, NULL, 0, 0) != PSP_HOOK_OK) return;
        at += bytes; size -= bytes;
    }
}
void MakeRangedNOP(uintptr_t at, uintptr_t until)
{
    at = adjustAddress(at); until = adjustAddress(until);
    if (until < at) { RecordPatchStatus(PSP_HOOK_INVALID_ARGUMENT); return; }
    MakeNOPWithSize(at, until - at);
}

static uintptr_t MakeInlineImpl(size_t instrCount, uintptr_t at, int discardDelay, va_list ap)
{
    uint32_t original[3], patch[3] = {0}, previous;
    uint32_t* words;
    uintptr_t functor;
    size_t i, count = 2, tail = 2, written = 0;
    psp_hook_status status;
    at = adjustAddress(at);
    if (instrCount > SIZE_MAX / sizeof(uint32_t) - 8 || at < 4 || (at & 3)) {
        RecordPatchStatus(PSP_HOOK_INVALID_ARGUMENT); return 0;
    }
    if (!ReadInstructionWords(at - 4, &previous, 1) || !ReadInstructionWords(at, original, 2)) return 0;
    if (psp_hook_has_delay_slot(previous)) {
        RecordPatchStatus(PSP_HOOK_DELAY_SLOT); return 0;
    }
    if (!discardDelay && psp_hook_control(original[1]) != 0) {
        if (psp_hook_control(original[1]) < 0) {
            RecordPatchStatus(PSP_HOOK_UNSUPPORTED_INSTRUCTION); return 0;
        }
        if (!ReadInstructionWords(at + 8, original + 2, 1)) return 0;
        if (psp_hook_control(original[2]) != 0) {
            RecordPatchStatus(PSP_HOOK_DELAY_SLOT); return 0;
        }
        count = 3; tail = 10; /* A far linking branch needs eight words plus return. */
    }
    functor = MakeCallStub(instrCount + tail);
    if (!functor) { RecordPatchStatus(PSP_HOOK_ALLOCATION_FAILED); return 0; }
    status = PSP_HOOK_OK;
    if (!psp_hook_jump_fits((uint32_t)at, (uint32_t)functor) ||
        !psp_hook_jump_fits((uint32_t)(functor + instrCount * 4), (uint32_t)(at + count * 4)))
        status = PSP_HOOK_OUT_OF_RANGE;
    /* Supplied instructions are an explicit replacement. Validate their delay
     * slots without relocating their deliberately encoded branch destinations. */
    words = (uint32_t*)functor;
    for (i = 0; i < instrCount; ++i) words[i] = va_arg(ap, uint32_t);
    for (i = 0; i < instrCount; ++i) {
        if (psp_hook_has_delay_slot(words[i])) {
            if (++i == instrCount || psp_hook_control(words[i]) != 0) {
                status = PSP_HOOK_DELAY_SLOT; break;
            }
        }
    }
    if (status == PSP_HOOK_OK && count == 3)
        status = psp_hook_relocate(original + 1, 2, (uint32_t)at + 4,
            words + instrCount, tail, (uint32_t)(functor + instrCount * 4), &written);
    else if (status == PSP_HOOK_OK) {
        words[instrCount] = psp_hook_jump((uint32_t)at + 8);
        words[instrCount + 1] = discardDelay ? 0 : original[1];
        written = 2;
    }
    if (status != PSP_HOOK_OK) {
        FreeMemBlock((void*)functor); RecordPatchStatus(status); return 0;
    }
    patch[0] = psp_hook_jump((uint32_t)functor);
    if (PublishInstructions(at, patch, count, original, functor, (instrCount + written) * 4) != PSP_HOOK_OK) {
        FreeMemBlock((void*)functor); return 0;
    }
    return functor;
}

uintptr_t MakeInline(size_t instrCount, uintptr_t at, ...)
{
    va_list ap;
    va_start(ap, at);
    uintptr_t result = MakeInlineImpl(instrCount, at, 0, ap);
    va_end(ap);
    return result;
}

uintptr_t MakeInlineWithNOP(size_t instrCount, uintptr_t at, ...)
{
    va_list ap;
    va_start(ap, at);
    uintptr_t result = MakeInlineImpl(instrCount, at, 1, ap);
    va_end(ap);
    return result;
}

uintptr_t MakeCallStub(uintptr_t numInstr)
{
    if (numInstr > SIZE_MAX / sizeof(uint32_t)) return 0;
    return (uintptr_t)AllocMemBlock(numInstr * sizeof(uint32_t));
}

/* A permanent legacy constant patch can move the next LUI into its stub.
 * Keep that logical instruction editable for adjacent HUD constant patches.
 * Metadata and code belong to this plugin; lookup happens only at installation. */
typedef struct ConstantSuffix {
    uintptr_t source, emitted;
    struct ConstantSuffix* next;
} ConstantSuffix;
static ConstantSuffix* s_constant_suffixes;
static uintptr_t ResolveConstantInstruction(uintptr_t at)
{
    ConstantSuffix* suffix;
    at = adjustAddress(at);
    for (suffix = s_constant_suffixes; suffix; suffix = suffix->next)
        if (suffix->source == at) return suffix->emitted;
    return at;
}

static void MakeConstantStub(uintptr_t at, RegisterID reg, uint32_t value)
{
    if (!reg || reg > 31) { RecordPatchStatus(PSP_HOOK_INVALID_ARGUMENT); return; }
    uint32_t original[2], previous;
    at = ResolveConstantInstruction(at);
    if (at < 4 || (at & 3)) { RecordPatchStatus(PSP_HOOK_INVALID_ARGUMENT); return; }
    if (!ReadInstructionWords(at, original, 2)) return;
    if ((original[1] >> 26) == 15 && ((original[1] >> 16) & 31)) {
        ConstantSuffix* suffix;
        uint32_t* code;
        uint32_t patch[2];
        unsigned next_reg = (original[1] >> 16) & 31;
        if (!ReadInstructionWords(at - 4, &previous, 1)) return;
        if (psp_hook_has_delay_slot(previous)) { RecordPatchStatus(PSP_HOOK_DELAY_SLOT); return; }
        suffix = (ConstantSuffix*)AllocMemBlock(sizeof(*suffix));
        if (!suffix) { RecordPatchStatus(PSP_HOOK_ALLOCATION_FAILED); return; }
        code = (uint32_t*)MakeCallStub(6);
        if (!code) { FreeMemBlock(suffix); RecordPatchStatus(PSP_HOOK_ALLOCATION_FAILED); return; }
        if (!psp_hook_jump_fits((uint32_t)at, (uint32_t)code)) {
            FreeMemBlock(code); FreeMemBlock(suffix); RecordPatchStatus(PSP_HOOK_OUT_OF_RANGE); return;
        }
        code[0] = lui(reg, (uint16_t)(value >> 16));
        code[1] = ori(reg, reg, (uint16_t)value);
        code[2] = original[1];
        code[3] = ori(next_reg, next_reg, 0); /* Identity; reserves its editable low half. */
        code[4] = psp_hook_jump((uint32_t)at + 8); code[5] = 0;
        patch[0] = psp_hook_jump((uint32_t)code); patch[1] = 0;
        if (PublishInstructions(at, patch, 2, original, (uintptr_t)code, 24) != PSP_HOOK_OK) {
            FreeMemBlock(code); FreeMemBlock(suffix); return;
        }
        suffix->source = at + 4; suffix->emitted = (uintptr_t)(code + 2);
        suffix->next = s_constant_suffixes; s_constant_suffixes = suffix;
        return;
    }
    /* The replaced instruction constructs a full constant. Execute its suffix
     * exactly once; branches and their delay slots use the checked relocator. */
    (void)MakeInline(2, at, lui(reg, (uint16_t)(value >> 16)), ori(reg, reg, (uint16_t)value));
}

void MakeLUIORI(uintptr_t at, RegisterID reg, float imm)
{
    uint32_t value;
    memcpy(&value, &imm, sizeof(value));
    MakeConstantStub(at, reg, value);
}
void MakeLI(uintptr_t at, RegisterID reg, int32_t imm)
{
    MakeConstantStub(at, reg, (uint32_t)imm);
}

uint32_t isDelaySlotNearby(uintptr_t at)
{
    uint32_t previous, next;
    if (at < 4 || !ReadInstructionWords(at - 4, &previous, 1) ||
        !ReadInstructionWords(at + 4, &next, 1)) return 1;
    return psp_hook_has_delay_slot(previous) || psp_hook_has_delay_slot(next);
}

static void WriteInstructionImmediate(uintptr_t at, uint16_t value)
{
    uint32_t word;
    if (!ReadInstructionWords(at, &word, 1)) return;
    WriteInstr(at, (word & 0xffff0000u) | value);
}

void MakeInlineLUIORI(uintptr_t at, float imm)
{
    static const uint32_t instr_len = 4;
    at = ResolveConstantInstruction(at);
    uint32_t value;
    memcpy(&value, &imm, sizeof(value));

    uint8_t LUI = parseCommand(lui(0, 0), 26, 31);
    uint8_t ORI = parseCommand(ori(0, 0, 0), 26, 31);

    uint32_t bytes;
    if (!ReadInstructionWords(at, &bytes, 1)) return;
    uint8_t instr = parseCommand(bytes, 26, 31);
    uint8_t reg_lui = parseCommand(bytes, 16, 20);
    if (instr != LUI || !reg_lui) { RecordPatchStatus(PSP_HOOK_UNSUPPORTED_INSTRUCTION); return; }

    if (instr == LUI)
    {
        for (uintptr_t i = at + instr_len; i <= (at + instr_len + (5 * instr_len)); i += instr_len)
        {
            if (!ReadInstructionWords(i, &bytes, 1)) return;
            instr = parseCommand(bytes, 26, 31);
            uint8_t reg_ori = parseCommand(bytes, 16, 20);

            /* Do not search past consumption/redefinition of this constant
             * or into a different control-flow path. */
            if (psp_hook_control(bytes) != 0 || (instr == LUI && reg_ori == reg_lui) ||
                (instr >= 16 && instr <= 18 && parseCommand(bytes, 21, 25) <= 6 && reg_ori == reg_lui))
                break;

            if (instr == ORI && reg_lui == reg_ori && parseCommand(bytes, 21, 25) == reg_lui)
            {
                if (i == at + instr_len && reg_lui) {
                    uint32_t original[2], replacement[2];
                    if (!ReadInstructionWords(at, original, 2)) return;
                    replacement[0] = (original[0] & 0xffff0000u) | (value >> 16);
                    replacement[1] = (original[1] & 0xffff0000u) | (value & 0xffff);
                    (void)PublishInstructions(at, replacement, 2, original, 0, 0);
                    return; /* Adjacent pair: no trampoline or allocation. */
                }
                if (isDelaySlotNearby(i))
                {
                    WriteInstructionImmediate(at, (uint16_t)(value >> 16));
                    WriteInstructionImmediate(i, (uint16_t)value);
                    return;
                }
                else
                    return MakeLUIORI(i, reg_ori, imm);
            }
        }

        if (isDelaySlotNearby(at))
            WriteInstructionImmediate(at, (uint16_t)(value >> 16));
        else
            return MakeLUIORI(at, reg_lui, imm);
    }
}

void MakeInlineLI(uintptr_t at, int32_t imm)
{
    uint8_t ORI = parseCommand(ori(0, 0, 0), 26, 31);
    uint8_t ZERO = parseCommand(ori(0, 0, 0), 21, 25);

    uint32_t bytes;
    if (!ReadInstructionWords(at, &bytes, 1)) return;
    uint8_t instr = parseCommand(bytes, 26, 31);
    uint8_t reg_ori = parseCommand(bytes, 16, 20);
    uint8_t reg_zero = parseCommand(bytes, 21, 25);

    if (instr == ORI && reg_zero == ZERO)
    {
        if (isDelaySlotNearby(at))
            return WriteInstructionImmediate(at, (uint16_t)imm);
        else
            return MakeLI(at, reg_ori, imm);
    }
}

psp_hook_status psp_injector_backend(psp_hook_backend* backend, psp_hook_guest* guest)
{
    return psp_hook_guest_backend(backend, guest, (uint32_t)psp_mem_storage_begin(),
        psp_mem_storage_size(), AllocMemBlock, FreeMemBlock);
}

uintptr_t MakeTrampoline(uintptr_t at, uintptr_t wrapper_func)
{
    static psp_hook_guest guest;
    psp_hook_backend backend;
    psp_hook* hook;
    psp_hook_status status = psp_injector_backend(&backend, &guest);
    if (status != PSP_HOOK_OK) { RecordPatchStatus(status); return 0; }
    hook = (psp_hook*)AllocMemBlock(sizeof(*hook));
    if (!hook) { RecordPatchStatus(PSP_HOOK_ALLOCATION_FAILED); return 0; }
    memset(hook, 0, sizeof(*hook));
    status = psp_hook_create_mid(hook, &backend, (uint32_t)adjustAddress(at), (uint32_t)wrapper_func,
        0, 0, PSP_HOOK_SAVE_FPU | PSP_HOOK_SAVE_VFPU | PSP_HOOK_RAW_CALLBACK, 2);
    if (status == PSP_HOOK_OK) status = psp_hook_enable(hook);
    RecordPatchStatus(status);
    if (status != PSP_HOOK_OK) {
        psp_hook_destroy(hook);
        FreeMemBlock(hook);
        return 0;
    }
    return hook->trampoline;
}

struct injector_t injector =
{
    .base_addr = 0,
    .base_size = 0,
    .module_addr = 0,
    .module_size = 0,
    .FreeMemBlock = FreeMemBlock,
    .GetGP = GetGP,
    .SetGP = SetGP,
    .SetModuleBaseAddress = SetModuleBaseAddress,
    .SetGameBaseAddress = SetGameBaseAddress,
    .WriteMemoryRaw = WriteMemoryRaw,
    .ReadMemoryRaw = ReadMemoryRaw,
    .MemoryFill = MemoryFill,
    .WriteInstr = WriteInstr,
    .WriteMemory8 = WriteMemory8,
    .WriteMemory16 = WriteMemory16,
    .WriteMemory32 = WriteMemory32,
    .WriteMemory64 = WriteMemory64,
    .WriteMemoryFloat = WriteMemoryFloat,
    .WriteMemoryDouble = WriteMemoryDouble,
    .ReadMemory8 = ReadMemory8,
    .ReadMemory16 = ReadMemory16,
    .ReadMemory32 = ReadMemory32,
    .ReadMemory64 = ReadMemory64,
    .ReadMemoryFloat = ReadMemoryFloat,
    .ReadMemoryDouble = ReadMemoryDouble,
    .GetBranchDestination = GetBranchDestination,
    .MakeJMP = MakeJMP,
    .MakeJMPwNOP = MakeJMPwNOP,
    .MakeJAL = MakeJAL,
    .MakeNOP = MakeNOP,
    .MakeNOPWithSize = MakeNOPWithSize,
    .MakeRangedNOP = MakeRangedNOP,
    .MakeInline = MakeInline,
    .MakeInlineLUIORI = MakeInlineLUIORI,
    .MakeInlineLI = MakeInlineLI,
    .MakeTrampoline = MakeTrampoline
};
