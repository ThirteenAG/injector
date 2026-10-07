#ifndef PCSX2_EMULATOR_SAFETYMIPS_HPP
#define PCSX2_EMULATOR_SAFETYMIPS_HPP
#include "injector.hpp"
#include <new>

using SafetyMipsContext = pcsx2::Registers;
namespace safetymips {
using Context = SafetyMipsContext;
using Status = pcsx2::HookStatus;
struct Options {
    size_t instructions = 2;
    unsigned preserve = PCSX2_HOOK_SAVE_FPU;
    uint32_t module_gp = 0;
    bool execute_original = true;
};

// Owned inline/mid hooks restore on destruction. Keep the returned handle alive.
class Hook {
    pcsx2_hook hook_{};
    Status error_ = PCSX2_HOOK_OK;
    injector::detail::startup_entry* pending_ = nullptr;
    void rebind() { injector::detail::rebind_hook(pending_, &hook_, &pending_, &error_); }
public:
    Hook() = default;
    Hook(pcsx2::Hook&& hook, Status status) : hook_(hook.take()), error_(status) {
        if (status == PCSX2_HOOK_OK && injector::detail::startup.active) {
            if (injector::detail::queued_overlap(hook_.address, hook_.count * 4)) error_ = PCSX2_HOOK_CONFLICT;
            else {
                pending_ = injector::detail::queue_hook(&hook_, &pending_, &error_);
                if (!pending_) error_ = PCSX2_HOOK_ALLOCATION_FAILED;
            }
            injector::detail::record(error_);
        }
    }
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&& other) noexcept : hook_(other.hook_), error_(other.error_), pending_(other.pending_) {
        other.hook_ = {}; other.pending_ = nullptr; rebind();
    }
    ~Hook() { reset(); }
    [[nodiscard]] Status try_assign(Hook&& other) {
        if (this == &other) return error_;
        auto status = reset();
        if (status != PCSX2_HOOK_OK) return status;
        hook_ = other.hook_; error_ = other.error_; pending_ = other.pending_;
        other.hook_ = {}; other.pending_ = nullptr; rebind();
        return error_;
    }
    Hook& operator=(Hook&& other) { (void)try_assign(std::move(other)); return *this; }
    explicit operator bool() const { return error_ == PCSX2_HOOK_OK && hook_.code.words != nullptr; }
    Status error() const { return error_; }
    bool enabled() const { return hook_.enabled != 0; }
    void release() {
        if (pending_) injector::detail::release_hook(pending_, hook_);
        pending_ = nullptr; hook_ = {};
    }
    uint32_t trampoline() const { return hook_.trampoline; }
    size_t code_size() const { return hook_.code_size; }
    Status enable() {
        if (pending_) return error_ = PCSX2_HOOK_INVALID_STATE;
        return error_ = pcsx2_hook_enable(&hook_);
    }
    Status disable() {
        if (pending_) { injector::detail::cancel_hook(pending_); pending_ = nullptr; return error_ = PCSX2_HOOK_OK; }
        return error_ = pcsx2_hook_disable(&hook_);
    }
    Status reset() {
        if (pending_) { injector::detail::cancel_hook(pending_); pending_ = nullptr; }
        return error_ = pcsx2_hook_destroy(&hook_);
    }
    template<class Function> Function original() const {
        return reinterpret_cast<Function>(static_cast<uintptr_t>(hook_.trampoline));
    }
    template<class Return = void, class... Args> Return call(Args... args) const {
        return original<Return (*)(Args...)>()(args...);
    }
};
using InlineHook = Hook;
using MidHook = Hook;

namespace detail {
inline Hook finish(pcsx2::Hook&& hook, Status status) {
    if (status == PCSX2_HOOK_OK && !injector::detail::startup.active) status = hook.enable();
    injector::detail::record(status);
    return Hook(std::move(hook), status);
}
// Only stateless, noncapturing callbacks are accepted. Storage is one trivial
// byte per lambda type in the module; the SDK can inline the lambda body into
// this thunk. No std::function, heap, dispatch table or callback-time allocation.
template<class Callback> struct lambda_callback {
    alignas(Callback) inline static unsigned char storage[sizeof(Callback)]{};
    static void invoke(pcsx2_hook_context* context, void*) {
        Context regs(*context);
        (*reinterpret_cast<Callback*>(storage))(regs);
    }
};
}

template<class Address, class Function>
inline InlineHook create_inline(Address address, Function replacement, Options options = {}) {
    pcsx2::Hook hook;
    auto site = injector::detail::noted(address);
    auto status = injector::detail::fits(site)
        ? hook.create_inline(injector::detail::backend, static_cast<uint32_t>(site), replacement, options.instructions)
        : PCSX2_HOOK_OUT_OF_RANGE;
    return detail::finish(std::move(hook), status);
}

template<class Address, class Callback>
inline MidHook create_mid(Address address, Callback callback, Options options = {}) {
    static_assert(std::is_empty_v<Callback> && std::is_convertible_v<Callback, void (*)(Context&)>,
        "use a noncapturing lambda taking SafetyMipsContext&; named functions use create_mid<&function>()");
    pcsx2::Hook hook;
    auto site = injector::detail::noted(address);
    auto entry = reinterpret_cast<uintptr_t>(&detail::lambda_callback<Callback>::invoke);
    auto status = PCSX2_HOOK_OUT_OF_RANGE;
    if (options.preserve & PCSX2_HOOK_RAW_CALLBACK) status = PCSX2_HOOK_INVALID_ARGUMENT;
    else if (injector::detail::fits(site) && injector::detail::fits(entry)) {
        new (detail::lambda_callback<Callback>::storage) Callback(callback);
        status = hook.create_mid(injector::detail::backend, static_cast<uint32_t>(site),
            static_cast<uint32_t>(entry), 0, options.module_gp, options.preserve | (options.execute_original ? 0u : unsigned(PCSX2_HOOK_REPLACE_INSTRUCTIONS)), options.instructions);
    }
    return detail::finish(std::move(hook), status);
}

template<auto Callback, class Address>
inline MidHook create_mid(Address address, void* user = nullptr, Options options = {}) {
    pcsx2::Hook hook;
    auto site = injector::detail::noted(address);
    auto status = injector::detail::fits(site)
        ? hook.template create_mid<Callback>(injector::detail::backend, static_cast<uint32_t>(site),
            user, options.module_gp, options.preserve | (options.execute_original ? 0u : unsigned(PCSX2_HOOK_REPLACE_INSTRUCTIONS)), options.instructions)
        : PCSX2_HOOK_OUT_OF_RANGE;
    return detail::finish(std::move(hook), status);
}
}
using SafetyMipsInline = safetymips::InlineHook;
using SafetyMipsMid = safetymips::MidHook;

namespace injector {
// Windows MakeInline functors replace displaced instructions. An owning mid hook
// is preferable when removal is needed; this legacy-shaped helper deliberately
// keeps its generated code in the module's bounded reservation until reset/unload.
template<class Functor, class Address, class End>
inline safetymips::Status MakeInline(Address address, End end) {
    static_assert(std::is_empty_v<Functor> && std::is_default_constructible_v<Functor>,
                  "MakeInline requires a stateless default-constructible functor");
    static_assert(std::is_invocable_r_v<void, Functor, reg_pack&>, "functor accepts injector::reg_pack&");
    auto begin = detail::noted(address), limit = detail::address(end);
    if (!detail::fits(begin) || !detail::fits(limit) || limit <= begin || ((limit - begin) & 3))
        return detail::record(PCSX2_HOOK_INVALID_ARGUMENT);
    safetymips::Options options{};
    options.instructions = (limit - begin) / 4;
    options.execute_original = false;
    auto hook = safetymips::create_mid(begin, [](SafetyMipsContext& regs) { Functor{}(regs); }, options);
    auto status = hook.error();
    if (hook) hook.release();
    return status;
}
template<class Functor, class Address> inline safetymips::Status MakeInline(Address address) {
    auto site = detail::noted(address);
    return MakeInline<Functor>(site, site + 8);
}
}


namespace injector {
namespace detail {
struct once_state {
    pcsx2_hook hook;
    pcsx2_hook_buffer storage;
    startup_entry* pending;
    pcsx2_hook_status error;
};
inline void destroy_once(void* user) {
    auto state = static_cast<once_state*>(user);
    auto storage = state->storage;
    auto owner_backend = state->hook.backend;
    if (pcsx2_hook_destroy(&state->hook) != PCSX2_HOOK_OK) return; // Retain live code and its callback state.
    owner_backend.release(owner_backend.user, &storage);
}
template<class Functor> void once_callback(pcsx2_hook_context* context, void* user) {
    auto state = static_cast<once_state*>(user);
    // Restore the original site before invoking user code. Recursive game calls
    // now execute the original instructions, and later frames pay no hook cost.
    auto status = pcsx2_hook_disable(&state->hook);
    if (status != PCSX2_HOOK_OK) {
        record(status);
#if defined(_MSC_VER)
        __debugbreak();
#else
        __builtin_trap();
#endif
        return;
    }
    reg_pack regs(*context);
    Functor{}(regs);
    // The current generated frame still executes here; retain its code/state
    // until module reset rather than freeing a live trampoline in the callback.
}
}
template<class Functor, class Address, class End>
inline safetymips::Status MakeInlineOnce(Address address, End end) {
    static_assert(std::is_empty_v<Functor> && std::is_default_constructible_v<Functor> &&
                  std::is_invocable_r_v<void, Functor, reg_pack&>, "use a stateless reg_pack functor");
    auto begin = detail::noted(address), limit = detail::address(end);
    auto callback = reinterpret_cast<uintptr_t>(&detail::once_callback<Functor>);
    if (!detail::fits(begin) || !detail::fits(limit) || !detail::fits(callback))
        return detail::record(PCSX2_HOOK_OUT_OF_RANGE);
    if (limit <= begin || ((limit - begin) & 3)) return detail::record(PCSX2_HOOK_INVALID_ARGUMENT);
    if (detail::startup.active && detail::startup.error != PCSX2_HOOK_OK) return detail::startup.error;
    if (detail::startup.active && detail::queued_overlap(static_cast<uint32_t>(begin), limit - begin))
        return detail::record(PCSX2_HOOK_CONFLICT);
    pcsx2_hook_buffer buffer{};
    auto& backend = detail::backend;
    if (!backend.allocate || !backend.release ||
        !backend.allocate(backend.user, sizeof(detail::once_state), static_cast<uint32_t>(begin), &buffer))
        return detail::record(PCSX2_HOOK_ALLOCATION_FAILED);
    if (!buffer.words || buffer.capacity < sizeof(detail::once_state) ||
        !detail::fits(reinterpret_cast<uintptr_t>(buffer.words))) {
        backend.release(backend.user, &buffer); return detail::record(PCSX2_HOOK_OUT_OF_RANGE);
    }
    auto state = new (buffer.words) detail::once_state{};
    state->storage = buffer;
    safetymips::Options options{};
    state->error = pcsx2_hook_create_mid(&state->hook, &backend, static_cast<uint32_t>(begin),
        static_cast<uint32_t>(callback), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(state)),
        options.module_gp, options.preserve | PCSX2_HOOK_REPLACE_INSTRUCTIONS, (limit - begin) / 4);
    if (state->error != PCSX2_HOOK_OK) {
        auto error = state->error; backend.release(backend.user, &buffer); return detail::record(error);
    }
    if (detail::startup.active) {
        state->pending = detail::queue_hook(&state->hook, &state->pending, &state->error);
        if (!state->pending) {
            detail::destroy_once(state); return detail::record(PCSX2_HOOK_ALLOCATION_FAILED);
        }
        state->pending->on_failure = detail::destroy_once;
        state->pending->cleanup_user = state;
    } else {
        auto status = pcsx2_hook_enable(&state->hook);
        if (status != PCSX2_HOOK_OK) { detail::destroy_once(state); return detail::record(status); }
    }
    return detail::record(PCSX2_HOOK_OK);
}
template<class Functor, class Address> inline safetymips::Status MakeInlineOnce(Address address) {
    auto site = detail::noted(address);
    return MakeInlineOnce<Functor>(site, site + 8);
}
}

#endif
