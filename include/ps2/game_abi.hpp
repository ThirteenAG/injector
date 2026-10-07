#ifndef INJECTOR_PS2_GAME_ABI_HPP
#define INJECTOR_PS2_GAME_ABI_HPP
#include "safetymips.hpp"
#include "game_abi.h"
#include <tuple>

namespace pcsx2 {
namespace game_abi_detail {
template<class T> constexpr bool argument = [] {
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) return sizeof(T) <= 8;
    else if constexpr (std::is_pointer_v<T>) return sizeof(T) == 4;
    else return std::is_same_v<T, float>;
}();

template<class... Args> struct Layout {
    static_assert(sizeof...(Args) <= 24 && (argument<Args> && ...),
                  "EE bridge accepts scalar values, 32-bit pointers and single floats");
    static constexpr unsigned count = sizeof...(Args);
    template<size_t... I> static constexpr uint32_t mask(std::index_sequence<I...>) {
        return ((uint32_t(std::is_same_v<std::tuple_element_t<I, std::tuple<Args...>>, float>) << I) | ... | 0u);
    }
    static constexpr uint32_t floats = mask(std::index_sequence_for<Args...>{});
    static constexpr unsigned fp = (unsigned(std::is_same_v<Args, float>) + ... + 0u);
    static_assert(fp <= 8 && count - fp <= 16, "EE bridge supports eight float and sixteen integer arguments");
    static constexpr bool identical = fp == 0 || (count <= 8 && fp == count);
};

class Bridge {
    pcsx2_hook_backend backend_{};
    pcsx2_hook_buffer code_{};
public:
    Bridge() = default;
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&& other) noexcept : backend_(other.backend_), code_(other.code_) { other.code_ = {}; }
    Bridge& operator=(Bridge&& other) noexcept {
        if (this != &other) { reset(); backend_ = other.backend_; code_ = other.code_; other.code_ = {}; }
        return *this;
    }
    ~Bridge() { reset(); }
    void reset() {
        if (code_.allocation) backend_.release(backend_.user, &code_);
        code_ = {};
    }
    void release() { code_ = {}; } // Retain referenced storage on a hook conflict.
    uint32_t address() const { return code_.address; }
    HookStatus bind(const pcsx2_hook_backend& backend, uint32_t target,
                    unsigned count, uint32_t floats, pcsx2_game_abi_direction direction) {
        reset();
        uint32_t words[128]; size_t n = 0;
        auto status = pcsx2_game_abi_emit(words, 128, target, count, floats, direction, &n);
        if (status != PCSX2_HOOK_OK) return status;
        if (!backend.allocate || !backend.release || !backend.flush) return PCSX2_HOOK_BACKEND_UNAVAILABLE;
        pcsx2_hook_buffer code{};
        if (!backend.allocate(backend.user, n * 4, target, &code)) return PCSX2_HOOK_ALLOCATION_FAILED;
        if (!code.words || code.capacity < n * 4 || (code.address & 3) || !code.address) {
            backend.release(backend.user, &code); return PCSX2_HOOK_ALLOCATION_FAILED;
        }
        memcpy(code.words, words, n * 4);
        backend.flush(backend.user, code.address, n * 4);
        backend_ = backend; code_ = code;
        return PCSX2_HOOK_OK;
    }
};
}

// Bind once at startup; the call thereafter is just a short register shuffle
// and a tail jump. Bridge storage belongs to the plugin's bounded allocator.
template<class Signature> class GameFunction;
template<class Return, class... Args> class GameFunction<Return(Args...)> {
    game_abi_detail::Bridge bridge_;
    uint32_t target_ = 0;
    using Layout = game_abi_detail::Layout<Args...>;
public:
    GameFunction() = default;
    GameFunction(GameFunction&& other) noexcept : bridge_(std::move(other.bridge_)), target_(other.target_) { other.target_ = 0; }
    GameFunction& operator=(GameFunction&& other) noexcept {
        if (this != &other) { bridge_ = std::move(other.bridge_); target_ = other.target_; other.target_ = 0; }
        return *this;
    }
    template<class Address> HookStatus bind(Address address) {
        static_assert(std::is_void_v<Return> || game_abi_detail::argument<Return>, "EE aggregate returns require an explicit bridge");
        auto target = injector::detail::address(address);
        reset();
        auto status = PCSX2_HOOK_OUT_OF_RANGE;
        if (injector::detail::fits(target)) {
            if constexpr (Layout::identical) {
                status = target && !(target & 3) ? PCSX2_HOOK_OK : PCSX2_HOOK_INVALID_ARGUMENT;
                if (status == PCSX2_HOOK_OK) target_ = static_cast<uint32_t>(target);
            } else {
                status = bridge_.bind(injector::detail::backend, static_cast<uint32_t>(target), Layout::count,
                                      Layout::floats, PCSX2_GAME_ABI_CALL);
                if (status == PCSX2_HOOK_OK) target_ = bridge_.address();
            }
        }
        return injector::detail::record(status);
    }
    Return operator()(Args... args) const {
        if (!target_) {
#if defined(_MSC_VER)
            __debugbreak();
#else
            __builtin_trap();
#endif
        }
        return reinterpret_cast<Return (*)(Args...)>(static_cast<uintptr_t>(target_))(args...);
    }
    void reset() { bridge_.reset(); target_ = 0; }
    void release() { bridge_.release(); target_ = 0; }
    explicit operator bool() const { return target_ != 0; }
};

// Keep this owner alive for as long as game call sites reference address().
// Like GameFunction, it allocates only at bind time, from the module reservation.
template<class Signature> class GameCallback;
template<class Return, class... Args> class GameCallback<Return(Args...)> {
    game_abi_detail::Bridge bridge_;
    uint32_t target_ = 0;
    using Layout = game_abi_detail::Layout<Args...>;
public:
    HookStatus bind(Return (*callback)(Args...)) {
        static_assert(std::is_void_v<Return> || game_abi_detail::argument<Return>, "EE aggregate returns require an explicit bridge");
        reset();
        auto target = reinterpret_cast<uintptr_t>(callback);
        auto status = PCSX2_HOOK_OUT_OF_RANGE;
        if (injector::detail::fits(target)) {
            status = bridge_.bind(injector::detail::backend, uint32_t(target), Layout::count,
                                  Layout::floats, PCSX2_GAME_ABI_CALLBACK);
            if (status == PCSX2_HOOK_OK) target_ = bridge_.address();
        }
        return injector::detail::record(status);
    }
    void reset() { bridge_.reset(); target_ = 0; }
    void release() { bridge_.release(); target_ = 0; }
    uint32_t address() const { return target_; }
    explicit operator bool() const { return target_ != 0; }
};
}

namespace safetymips {
template<class Signature> class GameInline;
template<class Return, class... Args> class GameInline<Return(Args...)> {
    // Destruction restores the hook before releasing either ABI bridge.
    pcsx2::GameFunction<Return(Args...)> original_;
    pcsx2::game_abi_detail::Bridge entry_;
    InlineHook hook_;
    Status error_ = PCSX2_HOOK_OK;
    using Layout = pcsx2::game_abi_detail::Layout<Args...>;
public:
    GameInline() = default;
    GameInline(const GameInline&) = delete;
    GameInline& operator=(const GameInline&) = delete;
    GameInline(GameInline&& other) noexcept
        : original_(std::move(other.original_)), entry_(std::move(other.entry_)),
          hook_(std::move(other.hook_)), error_(other.error_) {}
    GameInline& operator=(GameInline&& other) {
        if (this != &other && reset() == PCSX2_HOOK_OK) {
            original_ = std::move(other.original_); entry_ = std::move(other.entry_);
            hook_ = std::move(other.hook_); error_ = other.error_;
        }
        return *this;
    }
    ~GameInline() {
        if (reset() != PCSX2_HOOK_OK) {
            original_.release(); entry_.release(); hook_.release();
        }
    }
    Status reset() {
        auto status = hook_.reset();
        if (status == PCSX2_HOOK_OK) { original_.reset(); entry_.reset(); }
        return error_ = status;
    }
    Status error() const { return error_ == PCSX2_HOOK_OK ? hook_.error() : error_; }
    explicit operator bool() const { return error() == PCSX2_HOOK_OK && bool(hook_); }
    Return call(Args... args) const { return original_(args...); }
    template<class Address> Status create(Address address, Return (*callback)(Args...), Options options = {}) {
        auto status = reset();
        auto site = injector::detail::noted(address);
        auto target = reinterpret_cast<uintptr_t>(callback);
        if (status == PCSX2_HOOK_OK && (!injector::detail::fits(site) || !injector::detail::fits(target)))
            status = PCSX2_HOOK_OUT_OF_RANGE;
        auto entry = static_cast<uint32_t>(target);
        if (status == PCSX2_HOOK_OK) {
            status = entry_.bind(injector::detail::backend, entry,
                                 Layout::count, Layout::floats, PCSX2_GAME_ABI_CALLBACK);
            entry = entry_.address();
        }
        pcsx2::Hook prepared;
        if (status == PCSX2_HOOK_OK)
            status = prepared.create_inline(injector::detail::backend, static_cast<uint32_t>(site),
                                            entry, options.instructions);
        if (status == PCSX2_HOOK_OK) status = original_.bind(prepared.trampoline());
        if (status == PCSX2_HOOK_OK) {
            hook_ = detail::finish(std::move(prepared), status);
            status = hook_.error();
        }
        if (status != PCSX2_HOOK_OK) { original_.reset(); entry_.reset(); }
        return error_ = injector::detail::record(status);
    }
};
template<class Address, class Return, class... Args>
inline GameInline<Return(Args...)> create_inline_game(Address address,
    Return (*callback)(Args...), Options options = {}) {
    GameInline<Return(Args...)> hook;
    hook.create(address, callback, options);
    return hook;
}
}
#endif
