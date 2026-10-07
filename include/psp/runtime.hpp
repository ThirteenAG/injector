#ifndef PSP_EMULATOR_RUNTIME_HPP
#define PSP_EMULATOR_RUNTIME_HPP
#include "injector.hpp"
#include "hooks_guest.h"
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include "memalloc.h"
#include <stdarg.h>
namespace mips_asm {
extern "C" {
#include "mips.h"
}
}

// The audited C instruction emitter remains the shared implementation for small
// explicit stubs and constant association. Keep its object out of plugin scope.
namespace injector { namespace detail {
using mips_asm::RegisterID;
extern "C" {
#include "injector.h"
}
}}
#undef MakeInlineWrapper
#undef MakeInlineWrapperWithNOP
#undef HIWORD
#undef LOWORD

namespace injector {
inline psp_hook_status InitializeRuntime(void (*on_error)(psp_hook_status) = nullptr)
{
    static psp_hook_guest guest{};
    psp_hook_backend backend{};
    auto status = detail::psp_injector_backend(&backend, &guest);
    if (status != PSP_HOOK_OK) return detail::record(status);
    status = SetBackend(backend, on_error);
    if (status != PSP_HOOK_OK) return status;
    // Explicit stubs and overlapping constant patches depend on publication
    // order. This mode publishes immediately, like the original C plugin API.
    detail::runtime = {
        detail::injector.ReadMemoryRaw,
        [](uintptr_t at, const void* value, size_t bytes) {
            detail::injector.WriteMemoryRaw(at, const_cast<void*>(value), bytes);
        },
        detail::injector.MemoryFill,
        detail::injector.MakeJAL,
        detail::injector.MakeJMPwNOP,
        detail::injector.GetBranchDestination,
        detail::injector.MakeNOPWithSize,
        detail::psp_injector_last_patch_status
    };
    // Aligned scalar writes retain the original single typed store path.
    // Raw/unaligned objects continue through memcpy and remain allocation-free.
    detail::runtime.u8 = {detail::injector.ReadMemory8, detail::injector.WriteMemory8};
    detail::runtime.u16 = {detail::injector.ReadMemory16, detail::injector.WriteMemory16};
    detail::runtime.u32 = {detail::injector.ReadMemory32, detail::injector.WriteMemory32};
    detail::runtime.u64 = {detail::injector.ReadMemory64, detail::injector.WriteMemory64};
    detail::runtime.f32 = {detail::injector.ReadMemoryFloat, detail::injector.WriteMemoryFloat};
    detail::runtime.f64 = {detail::injector.ReadMemoryDouble, detail::injector.WriteMemoryDouble};
    return PSP_HOOK_OK;
}

inline psp_hook_status InitializeCheckedRuntime(void (*on_error)(psp_hook_status) = nullptr)
{
    auto status = InitializeRuntime(on_error);
    if (status != PSP_HOOK_OK) return status;
    auto backend = detail::backend;
    return Initialize(backend, on_error);
}

template<class T> inline uint32_t WordBits(T value)
{
    if constexpr (std::is_same_v<T, float>) {
        uint32_t bits; memcpy(&bits, &value, sizeof(bits)); return bits;
    } else if constexpr (std::is_pointer_v<T>) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
    } else return static_cast<uint32_t>(value);
}
template<class T> inline uint16_t HighWord(T value) { return static_cast<uint16_t>(WordBits(value) >> 16); }
template<class T> inline uint16_t LowWord(T value) { return static_cast<uint16_t>(WordBits(value)); }

template<class Address, class... Words> inline memory_pointer MakeInline(Address address, Words... words)
{
    static_assert(sizeof...(Words) > 0, "provide replacement instructions");
    auto result = detail::injector.MakeInline(sizeof...(Words), detail::noted(address), static_cast<uint32_t>(words)...);
    detail::record(detail::psp_injector_last_patch_status());
    return memory_pointer(result);
}
template<class Address, class... Words> inline memory_pointer MakeInlineWithNOP(Address address, Words... words)
{
    static_assert(sizeof...(Words) > 0, "provide replacement instructions");
    auto result = detail::MakeInlineWithNOP(sizeof...(Words), detail::noted(address), static_cast<uint32_t>(words)...);
    detail::record(detail::psp_injector_last_patch_status());
    return memory_pointer(result);
}
template<class Address> inline psp_hook_status WriteInstr(Address address, uint32_t word)
{
    detail::injector.WriteInstr(detail::noted(address), word);
    return detail::record(detail::psp_injector_last_patch_status());
}
template<class Address> inline psp_hook_status MakeInlineLUIORI(Address address, float value)
{
    detail::injector.MakeInlineLUIORI(detail::noted(address), value);
    return detail::record(detail::psp_injector_last_patch_status());
}
template<class Address> inline psp_hook_status MakeInlineLI(Address address, int32_t value)
{
    detail::injector.MakeInlineLI(detail::noted(address), value);
    return detail::record(detail::psp_injector_last_patch_status());
}
template<class Address, class Target> inline memory_pointer MakeTrampoline(Address address, Target target)
{
    auto result = detail::injector.MakeTrampoline(detail::noted(address), detail::callback_address(target));
    detail::record(detail::psp_injector_last_patch_status());
    return memory_pointer(result);
}
inline memory_pointer MakeCallStub(size_t count) { return memory_pointer(detail::MakeCallStub(count)); }
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void* GetGP() { return detail::injector.GetGP(); }
inline void* SetGP(void* gp) { return detail::injector.SetGP(gp); }
inline void SetGameBaseAddress(uintptr_t address, size_t bytes) { detail::injector.SetGameBaseAddress(address, bytes); }
inline void SetModuleBaseAddress(uintptr_t address, size_t bytes) { detail::injector.SetModuleBaseAddress(address, bytes); }
}
#endif
