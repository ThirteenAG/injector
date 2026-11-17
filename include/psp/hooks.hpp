#ifndef WFP_PSP_HOOKS_HPP
#define WFP_PSP_HOOKS_HPP
#include "hooks.h"
#include "registers.hpp"
#include <type_traits>
#include <utility>

#ifdef PSP_CALLBACK_MODULE_GP
extern "C" char _gp[];
#endif

namespace psp {
using HookContext = psp_hook_context;
using HookBackend = psp_hook_backend;
using HookStatus = psp_hook_status;

// PSP_CALLBACK_MODULE_GP: callbacks run with the module's GP, as the C library addresses
// its data through it. Game code needs its own, so every call into the game goes through
// game_call.
inline uint32_t game_gp = 0; // GP of the game code that entered the module last.
#ifdef PSP_CALLBACK_MODULE_GP
inline uint32_t module_gp() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_gp)); }
inline void note_game_gp(const psp_hook_context& context) { game_gp = context.gpr[28].word[0]; }
[[gnu::noinline]] inline uint32_t enter_game() {
    uint32_t current;
    asm volatile("move %0, $gp" : "=r"(current));
    // A callback entered without an adapter already has the game's GP.
    if (current == module_gp() && game_gp) asm volatile("move $gp, %0" : : "r"(game_gp));
    return current;
}
[[gnu::noinline]] inline void leave_game(uint32_t restore) {
    // Code of another game module may have entered this one while the game ran.
    if (restore == module_gp()) asm volatile("move %0, $gp" : "=r"(game_gp));
    asm volatile("move $gp, %0" : : "r"(restore));
}
#else
inline uint32_t module_gp() { return 0; }
inline void note_game_gp(const psp_hook_context&) {}
inline uint32_t enter_game() { return 0; }
inline void leave_game(uint32_t) {}
#endif
template<class Return, class... Args, class... Values>
inline Return game_call(Return (*function)(Args...), Values&&... values) {
    const uint32_t restore = enter_game();
    if constexpr (std::is_void_v<Return>) {
        function(std::forward<Values>(values)...);
        leave_game(restore);
    } else {
        Return result = function(std::forward<Values>(values)...);
        leave_game(restore);
        return result;
    }
}

/* Compile-time callback selection: no heap storage or indirect C++ dispatch. */
template<auto Callback>
inline void mid_callback(psp_hook_context* context, void* user) {
    static_assert(Callback != nullptr, "mid callback must not be null");
    note_game_gp(*context);
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
    psp_hook value_{};
public:
    Hook() = default;
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&& other) noexcept : value_(other.value_) { other.value_ = {}; }
    Hook& operator=(Hook&&) = delete; // Use reset() to handle restoration failures explicitly.
    [[nodiscard]] HookStatus replace(Hook&& other) {
        if (this == &other) return PSP_HOOK_OK;
        auto status = reset();
        if (status != PSP_HOOK_OK) return status;
        value_ = other.value_; other.value_ = {};
        return PSP_HOOK_OK;
    }
    ~Hook() { psp_hook_destroy(&value_); }
    explicit operator bool() const noexcept { return value_.code.words != nullptr; }
    bool enabled() const noexcept { return value_.enabled != 0; }
    psp_hook take() noexcept { auto result = value_; value_ = {}; return result; }
    void release() noexcept { value_ = {}; } // Permanent module-owned code; no restore/free.
    uint32_t trampoline() const noexcept { return value_.trampoline; }
    size_t code_size() const noexcept { return value_.code_size; }
    HookStatus enable() { return psp_hook_enable(&value_); }
    HookStatus disable() { return psp_hook_disable(&value_); }
    HookStatus reset() { return psp_hook_destroy(&value_); }
    template<class Function> Function original() const {
        return reinterpret_cast<Function>(static_cast<uintptr_t>(trampoline()));
    }
    HookStatus create_inline(const HookBackend& backend, uint32_t address,
                             uint32_t replacement, size_t count = 2) {
        return psp_hook_create_inline(&value_, &backend, address, replacement, count);
    }
    template<class Function, std::enable_if_t<std::is_pointer_v<Function> &&
             std::is_function_v<std::remove_pointer_t<Function>>, int> = 0>
    HookStatus create_inline(const HookBackend& backend, uint32_t address,
                             Function replacement, size_t count = 2) {
        static_assert(std::is_pointer_v<Function> &&
                      std::is_function_v<std::remove_pointer_t<Function>>, "replacement must be a function pointer");
        if (reinterpret_cast<uintptr_t>(replacement) > UINT32_MAX) return PSP_HOOK_OUT_OF_RANGE;
        return create_inline(backend, address,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement)), count);
    }
    template<auto Callback>
    HookStatus create_mid(const HookBackend& backend, uint32_t address,
                          void* user = nullptr, uint32_t module_gp = 0,
                          unsigned flags = PSP_HOOK_SAVE_FPU | PSP_HOOK_SAVE_VFPU, size_t count = 2) {
        // A typed callback needs a context, so reject the raw ABI before allocating.
        if (flags & PSP_HOOK_RAW_CALLBACK) return PSP_HOOK_INVALID_ARGUMENT;
        if (reinterpret_cast<uintptr_t>(&mid_callback<Callback>) > UINT32_MAX ||
            reinterpret_cast<uintptr_t>(user) > UINT32_MAX) return PSP_HOOK_OUT_OF_RANGE;
        return create_mid(backend, address,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&mid_callback<Callback>)),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user)), module_gp, flags, count);
    }
    HookStatus create_mid(const HookBackend& backend, uint32_t address, uint32_t callback,
                          uint32_t user, uint32_t module_gp,
                          unsigned flags = PSP_HOOK_SAVE_FPU | PSP_HOOK_SAVE_VFPU, size_t count = 2) {
        return psp_hook_create_mid(&value_, &backend, address, callback, user, module_gp, flags, count);
    }
};
}
#endif
