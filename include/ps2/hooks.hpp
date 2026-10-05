#ifndef WFP_PCSX2_HOOKS_HPP
#define WFP_PCSX2_HOOKS_HPP
#include "hooks.h"
#include "registers.hpp"
#include <type_traits>
#include <utility>

namespace pcsx2 {
using HookContext = pcsx2_hook_context;
using HookBackend = pcsx2_hook_backend;
using HookStatus = pcsx2_hook_status;

/* Compile-time callback selection: no heap storage or indirect C++ dispatch. */
template<auto Callback>
inline void mid_callback(pcsx2_hook_context* context, void* user) {
    static_assert(Callback != nullptr, "mid callback must not be null");
    Registers regs(*context);
    if constexpr (std::is_invocable_v<decltype(Callback), Registers&, void*>) {
        static_assert(std::is_same_v<decltype(Callback(regs, user)), void>, "mid callback must return void");
        Callback(regs, user);
    } else {
        static_assert(std::is_invocable_v<decltype(Callback), Registers&>, "callback accepts Registers& and optional void*");
        static_assert(std::is_same_v<decltype(Callback(regs)), void>, "mid callback must return void");
        (void)user;
        Callback(regs);
    }
}

/* Explicit destruction reports conflicts. A destructor never frees live code. */
class Hook {
    pcsx2_hook value_{};
public:
    Hook() = default;
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&& other) noexcept : value_(other.value_) { other.value_ = {}; }
    Hook& operator=(Hook&&) = delete; // Use reset() to handle restoration failures explicitly.
    [[nodiscard]] HookStatus replace(Hook&& other) {
        if (this == &other) return PCSX2_HOOK_OK;
        auto status = reset();
        if (status != PCSX2_HOOK_OK) return status;
        value_ = other.value_; other.value_ = {};
        return PCSX2_HOOK_OK;
    }
    ~Hook() { pcsx2_hook_destroy(&value_); }
    explicit operator bool() const noexcept { return value_.code.words != nullptr; }
    bool enabled() const noexcept { return value_.enabled != 0; }
    pcsx2_hook take() noexcept { auto result = value_; value_ = {}; return result; }
    void release() noexcept { value_ = {}; } // Permanent module-owned code; no restore/free.
    uint32_t trampoline() const noexcept { return value_.trampoline; }
    size_t code_size() const noexcept { return value_.code_size; }
    HookStatus enable() { return pcsx2_hook_enable(&value_); }
    HookStatus disable() { return pcsx2_hook_disable(&value_); }
    HookStatus reset() { return pcsx2_hook_destroy(&value_); }
    template<class Function> Function original() const {
        return reinterpret_cast<Function>(static_cast<uintptr_t>(trampoline()));
    }
    HookStatus create_inline(const HookBackend& backend, uint32_t address,
                             uint32_t replacement, size_t count = 2) {
        return pcsx2_hook_create_inline(&value_, &backend, address, replacement, count);
    }
    template<class Function, std::enable_if_t<std::is_pointer_v<Function> &&
             std::is_function_v<std::remove_pointer_t<Function>>, int> = 0>
    HookStatus create_inline(const HookBackend& backend, uint32_t address,
                             Function replacement, size_t count = 2) {
        static_assert(std::is_pointer_v<Function> &&
                      std::is_function_v<std::remove_pointer_t<Function>>, "replacement must be a function pointer");
        if (reinterpret_cast<uintptr_t>(replacement) > UINT32_MAX) return PCSX2_HOOK_OUT_OF_RANGE;
        return create_inline(backend, address,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement)), count);
    }
    template<auto Callback>
    HookStatus create_mid(const HookBackend& backend, uint32_t address,
                          void* user = nullptr, uint32_t module_gp = 0,
                          unsigned flags = PCSX2_HOOK_SAVE_FPU, size_t count = 2) {
        // A typed callback needs a context, so reject the raw ABI before allocating.
        if (flags & PCSX2_HOOK_RAW_CALLBACK) return PCSX2_HOOK_INVALID_ARGUMENT;
        if (reinterpret_cast<uintptr_t>(&mid_callback<Callback>) > UINT32_MAX ||
            reinterpret_cast<uintptr_t>(user) > UINT32_MAX) return PCSX2_HOOK_OUT_OF_RANGE;
        return create_mid(backend, address,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&mid_callback<Callback>)),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user)), module_gp, flags, count);
    }
    HookStatus create_mid(const HookBackend& backend, uint32_t address, uint32_t callback,
                          uint32_t user, uint32_t module_gp,
                          unsigned flags = PCSX2_HOOK_SAVE_FPU, size_t count = 2) {
        return pcsx2_hook_create_mid(&value_, &backend, address, callback, user, module_gp, flags, count);
    }
};
}
#endif
