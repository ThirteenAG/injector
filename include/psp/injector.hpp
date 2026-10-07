#ifndef PSP_EMULATOR_INJECTOR_HPP
#define PSP_EMULATOR_INJECTOR_HPP
#ifdef EMULATOR_INJECTOR_PLATFORM
#error Include only one platform frontend per translation unit
#endif
#define EMULATOR_INJECTOR_PLATFORM 1
#include "hooks.hpp"
#include "patches.h"
#include "memory.h"
#include "startup.hpp"
#include "game_abi.hpp"
#include <utility>
#include <type_traits>

#if defined(__GNUC__) && !defined(NDEBUG)
#define INJECTOR_LOG_FAILURES 1
extern "C" {
#include "log.h"
// Weak: a module that does not link log.c simply skips the diagnostic.
extern struct logger_t logger __attribute__((weak));
}
#endif

namespace injector {
// The first failed operation since Initialize/SetBackend: the game site being
// patched or hooked (0 when unknown) and the reason.
struct HookFailure { uint32_t address; psp_hook_status status; };
inline const char* StatusName(psp_hook_status status) {
    switch (status) {
    case PSP_HOOK_OK: return "ok";
    case PSP_HOOK_INVALID_ARGUMENT: return "invalid argument";
    case PSP_HOOK_UNALIGNED: return "unaligned address";
    case PSP_HOOK_DELAY_SLOT: return "site or window splits a delay slot";
    case PSP_HOOK_UNSUPPORTED_INSTRUCTION: return "window holds a non-relocatable instruction";
    case PSP_HOOK_OUT_OF_RANGE: return "address or jump target out of range";
    case PSP_HOOK_BUFFER_TOO_SMALL: return "buffer too small";
    case PSP_HOOK_ALLOCATION_FAILED: return "allocation failed";
    case PSP_HOOK_READ_FAILED: return "read failed";
    case PSP_HOOK_WRITE_FAILED: return "write failed";
    case PSP_HOOK_CONFLICT: return "conflicting patch or unexpected original code";
    case PSP_HOOK_INVALID_STATE: return "invalid state";
    case PSP_HOOK_BACKEND_UNAVAILABLE: return "backend unavailable";
    }
    return "unknown";
}
}

// Configure once at module startup. The backend's user and storage must remain
// alive for the session. These trivial globals need no CRT initialization.
namespace injector {
namespace detail {
struct batch_state {
    psp_patch* patches;
    size_t capacity, count;
    psp_hook_status status;
};
// Optional guest runtime access retains the existing direct-data and explicit
// instruction-patch paths. It is configured only by runtime.hpp.
template<class T> struct runtime_scalar {
    T (*read)(uintptr_t) = nullptr;
    void (*write)(uintptr_t, T) = nullptr;
};
struct runtime_access {
    void (*read)(uintptr_t, void*, size_t);
    void (*write)(uintptr_t, const void*, size_t);
    void (*fill)(uintptr_t, uint8_t, size_t);
    uintptr_t (*call)(uintptr_t, uintptr_t);
    uintptr_t (*jump)(uintptr_t, uintptr_t);
    uintptr_t (*target)(uintptr_t);
    void (*nop)(uintptr_t, size_t);
    psp_hook_status (*status)();
    runtime_scalar<uint8_t> u8;
    runtime_scalar<uint16_t> u16;
    runtime_scalar<uint32_t> u32;
    runtime_scalar<uint64_t> u64;
    runtime_scalar<float> f32;
    runtime_scalar<double> f64;

};
inline runtime_access runtime{};
template<class T> inline runtime_scalar<T>* scalar_access() {
    if constexpr (std::is_same_v<T, uint8_t>) return &runtime.u8;
    else if constexpr (std::is_same_v<T, uint16_t>) return &runtime.u16;
    else if constexpr (std::is_same_v<T, uint32_t>) return &runtime.u32;
    else if constexpr (std::is_same_v<T, uint64_t>) return &runtime.u64;
    else if constexpr (std::is_same_v<T, float>) return &runtime.f32;
    else if constexpr (std::is_same_v<T, double>) return &runtime.f64;
    else return nullptr;
}

inline batch_state* batch = nullptr;
inline void (*error_handler)(psp_hook_status) = nullptr;
inline psp_hook_status last_status = PSP_HOOK_BACKEND_UNAVAILABLE;
inline uint32_t current_site = 0;
inline HookFailure first_failure{0, PSP_HOOK_OK};
inline void note_site(uintptr_t value) { current_site = value <= UINT32_MAX ? static_cast<uint32_t>(value) : 0; }
inline void note_failure(psp_hook_status status) {
    if (status == PSP_HOOK_OK || first_failure.status != PSP_HOOK_OK) return;
    first_failure = {current_site, status};
#ifdef INJECTOR_LOG_FAILURES
    if (&logger && logger.WriteF)
        logger.WriteF("injector: %s (%u) at %08X\n", StatusName(status), unsigned(status), unsigned(current_site));
#endif
}
inline psp_hook_status record(psp_hook_status status) {
    auto previous = last_status;
    last_status = status;
    note_failure(status);
    if (batch && batch->status == PSP_HOOK_OK) batch->status = status;
    if (startup.active && status != PSP_HOOK_OK && startup.error == PSP_HOOK_OK) startup.error = status;
    if (!startup.active && status != PSP_HOOK_OK && status != previous && error_handler) error_handler(status);
    return status;
}
template<class T> uintptr_t address(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uintptr_t>(value);
}
// Resolves a patched/hooked game site and remembers it for failure reports.
template<class T> uintptr_t noted(T value) { auto result = address(value); note_site(result); return result; }
inline bool fits(uintptr_t value) { return value <= UINT32_MAX; }
inline psp::GameCallbacks game_callbacks;
template<class T> uintptr_t callback_address(T target) {
    const uintptr_t value=address(target);
#ifdef PSP_GAME_ABI_COMPAT
    if constexpr (std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>) {
        if (!fits(value)) { record(PSP_HOOK_OUT_OF_RANGE);return 0; }
        uint32_t entry=0;
        record(game_callbacks.bind(backend,target,entry));
        return entry;
    }
#endif
    return value;
}
}

inline psp_hook_status SetBackend(const psp_hook_backend& backend,
    void (*on_error)(psp_hook_status) = nullptr) {
    detail::error_handler = on_error;
    detail::first_failure = {0, PSP_HOOK_OK}; detail::current_site = 0;
    if (detail::batch || detail::startup.active) return detail::record(PSP_HOOK_INVALID_STATE);
    if (!backend.read || !backend.write || !backend.flush)
        return detail::record(PSP_HOOK_BACKEND_UNAVAILABLE);
    detail::runtime = {};
    detail::backend = backend;
    return detail::record(PSP_HOOK_OK);
}
inline psp_hook_status Initialize(const psp_hook_backend& backend,
    void (*on_error)(psp_hook_status) = nullptr) {
    auto status = SetBackend(backend, on_error);
    if (status != PSP_HOOK_OK) return status;
    if (!backend.allocate || !backend.release) return detail::record(PSP_HOOK_BACKEND_UNAVAILABLE);
    ++detail::startup.generation;
    detail::startup.next_id = 0; detail::startup.error = PSP_HOOK_OK; detail::startup.failed_address = 0;
    detail::startup.active = true;
    return PSP_HOOK_OK;
}
inline psp_hook_status FlushCaches() {
    if (!detail::startup.active) return detail::last_status;
    auto status = detail::finish_startup();
    if (detail::startup.failed_address) detail::note_site(detail::startup.failed_address);
    // Startup suppressed the handler for queued failures, and the same status may
    // already be the last one recorded. Report the rejected session exactly once.
    detail::last_status = PSP_HOOK_OK;
    return detail::record(status);
}

inline psp_hook_status LastError() { return detail::last_status; }
// First failure (site and reason) since the last Initialize/SetBackend.
inline HookFailure FirstFailure() { return detail::first_failure; }

class memory_pointer {
    uintptr_t value_ = 0;
    uint32_t generation_ = 0, entry_ = 0;
    struct pointer {
        uintptr_t value;
        template<class T, std::enable_if_t<std::is_pointer_v<T>, int> = 0>
        operator T() const { return reinterpret_cast<T>(value); }
    };
public:
    memory_pointer() = default;
    memory_pointer(uintptr_t value, uint32_t generation = 0, uint32_t entry = 0)
        : value_(value), generation_(generation), entry_(entry) {}
    template<class T> memory_pointer(T* value) : value_(reinterpret_cast<uintptr_t>(value)) {}
    memory_pointer operator+(ptrdiff_t offset) const { return memory_pointer(value_ + offset); }
    memory_pointer operator-(ptrdiff_t offset) const { return memory_pointer(value_ - offset); }
    pointer get() const { detail::observe_original(generation_, entry_); return {value_}; }
    template<class T> T* get(ptrdiff_t offset = 0) const { detail::observe_original(generation_, entry_); return reinterpret_cast<T*>(value_ + offset); }
    template<class T> T* get_raw() const { return reinterpret_cast<T*>(value_); }
    uintptr_t as_int() const { return value_; }
    explicit operator bool() const { return value_ != 0; }
};

namespace detail { inline uintptr_t address(memory_pointer value) { return value.as_int(); } }

template<class Function> struct hook_back {
    using function_type = std::conditional_t<std::is_pointer_v<Function>, Function, Function*>;
    static_assert(std::is_function_v<std::remove_pointer_t<function_type>>, "hook_back requires a function signature");
    function_type fun = nullptr;
    explicit operator bool() const { return fun != nullptr; }
    template<class... Args> decltype(auto) operator()(Args&&... args) const {
        return fun(std::forward<Args>(args)...);
    }
};

// Permanent instruction patches have PC injector semantics: ignoring the return
// value does not undo the patch. A batch is optional; it prepares every patch
// before publishing, so hook_back targets can be assigned before callbacks run.
// Its exact, compile-time capacity lives on the startup stack, not on a heap.
template<size_t Capacity> class patch_batch {
    static_assert(Capacity > 0, "batch must have at least one patch");
    psp_patch patches_[Capacity]{};
    detail::batch_state state_{patches_, Capacity, 0, PSP_HOOK_OK};
    psp_hook_status rollback_ = PSP_HOOK_OK;
    bool committed_ = false;
public:
    patch_batch() {
        if (detail::batch) state_.status = detail::record(PSP_HOOK_INVALID_STATE);
        else detail::batch = &state_;
    }
    patch_batch(const patch_batch&) = delete;
    patch_batch& operator=(const patch_batch&) = delete;
    ~patch_batch() {
        if (detail::batch == &state_) detail::batch = nullptr;
        // A successful commit transfers permanent ownership to the module.
        // Failed publication rolls back in commit(), while records still exist.
    }
    bool require(bool condition) {
        if (!condition && state_.status == PSP_HOOK_OK)
            state_.status = detail::record(PSP_HOOK_CONFLICT);
        return condition;
    }
    psp_hook_status rollback_error() const { return rollback_; }
    psp_hook_status commit() {
        if (committed_ || detail::batch != &state_) return detail::record(PSP_HOOK_INVALID_STATE);
        if (state_.status != PSP_HOOK_OK) return detail::record(state_.status);
        for (size_t i = 0; i < state_.count; ++i) {
            auto status = psp_patch_enable(&patches_[i]);
            if (status != PSP_HOOK_OK) {
                while (i) {
                    auto restored = psp_patch_disable(&patches_[--i]);
                    if (rollback_ == PSP_HOOK_OK) rollback_ = restored;
                }
                state_.status = status;
                return detail::record(status);
            }
        }
        committed_ = true;
        detail::batch = nullptr;
        return detail::record(PSP_HOOK_OK);
    }
};

template<class Address, class Target>
inline memory_pointer MakeCALL(Address address, Target target, bool = true) {
    auto site = detail::noted(address), destination = detail::callback_address(target);
    if (!detail::fits(site) || !detail::fits(destination)) {
        detail::record(PSP_HOOK_OUT_OF_RANGE); return {};
    }
    if (detail::runtime.call && !detail::startup.active && !detail::batch) {
        auto original = detail::runtime.call(site, destination);
        detail::record(detail::runtime.status());
        return memory_pointer(original);
    }
    if (detail::batch && detail::batch->status != PSP_HOOK_OK) return {};
    if (detail::startup.active && detail::startup.error != PSP_HOOK_OK) return {};
    if (detail::startup.active && detail::queued_overlap(static_cast<uint32_t>(site), 8)) {
        detail::record(PSP_HOOK_CONFLICT); return {};
    }
    auto pending = detail::startup.active ? detail::new_entry(static_cast<uint32_t>(site)) : nullptr;
    if (detail::startup.active && !pending) { detail::record(PSP_HOOK_ALLOCATION_FAILED); return {}; }
    psp_patch local{};
    auto patch = pending ? &pending->value.patch : &local;
    if (detail::batch) {
        if (detail::batch->count == detail::batch->capacity) {
            detail::record(PSP_HOOK_BUFFER_TOO_SMALL); return {};
        }
        patch = &detail::batch->patches[detail::batch->count];
    }
    auto status = psp_patch_create_call(patch, &detail::backend,
        static_cast<uint32_t>(site), static_cast<uint32_t>(destination));
    if (status == PSP_HOOK_OK) {
        if (detail::batch) ++detail::batch->count;
        else if (!pending) status = psp_patch_enable(patch);
        if (pending) pending->callback = static_cast<uint32_t>(destination);
    }
    if (pending && status != PSP_HOOK_OK) pending->kind = detail::entry_kind::cancelled;
    detail::record(status);
    return status == PSP_HOOK_OK ? memory_pointer(psp_patch_original_target(patch),
        pending ? detail::startup.generation : 0, pending ? pending->id : 0) : memory_pointer{};
}

namespace detail {
template<class Create> inline memory_pointer publish(Create create) {
    if (batch && batch->status != PSP_HOOK_OK) return {};
    if (startup.active && startup.error != PSP_HOOK_OK) return {};
    // Address is only known after the checked create helper runs.
    psp_patch local{};
    auto patch = &local;
    if (batch) {
        if (batch->count == batch->capacity) { record(PSP_HOOK_BUFFER_TOO_SMALL); return {}; }
        patch = &batch->patches[batch->count];
    }
    auto status = create(patch);
    uint32_t previous = 0;
    if (status == PSP_HOOK_OK) {
        previous = psp_patch_original_target(patch);
        if (batch) ++batch->count;
        else if (startup.active) {
            if (queued_overlap(patch->address, patch->count * 4)) status = PSP_HOOK_CONFLICT;
            else {
                auto pending = new_entry(patch->address);
                if (!pending) status = PSP_HOOK_ALLOCATION_FAILED;
                else pending->value.patch = *patch;
            }
        } else status = psp_patch_enable(patch);
    }
    record(status);
    return status == PSP_HOOK_OK ? memory_pointer(previous) : memory_pointer{};
}
template<class Address> bool site(Address address, uint32_t& result) {
    auto value = detail::noted(address);
    if (!fits(value)) { record(PSP_HOOK_OUT_OF_RANGE); return false; }
    result = static_cast<uint32_t>(value); return true;
}
// Destination operands do not replace the remembered site.
template<class Target> bool target_site(Target target, uint32_t& result) {
    auto previous = current_site;
    auto ok = site(target, result);
    current_site = previous;
    return ok;
}
}

using reg_pack = psp::Registers;
using memory_pointer_raw = memory_pointer;
using memory_pointer_tr = memory_pointer;

template<class Address> inline psp_hook_status ReadMemoryRaw(Address address, void* out, size_t bytes, bool = false) {
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    if (detail::runtime.read && !detail::startup.active) {
        detail::runtime.read(site, out, bytes);
        return detail::record(PSP_HOOK_OK);
    }
    return detail::record(detail::startup.active ? detail::queued_read(site, out, bytes)
                                                 : psp_memory_read(&detail::backend, site, out, bytes));
}
template<class T, class Address> inline psp_hook_status ReadObject(Address address, T& out, bool vp = false) {
    static_assert(std::is_trivially_copyable_v<T>, "read only trivially copyable guest values");
    static_assert(!std::is_pointer_v<T> || sizeof(T) == 4, "guest pointers are 32-bit");
    return ReadMemoryRaw(address, &out, sizeof(out), vp);
}
template<class T, class Address> inline T ReadMemory(Address address, bool vp = false) {
    auto* access = detail::scalar_access<T>();
    if (access && access->read && !detail::startup.active && !detail::batch) {
        uint32_t site;
        if (!detail::site(address, site)) return T{};
        if (!(site & (alignof(T) - 1))) {
            auto value = access->read(site);
            detail::record(PSP_HOOK_OK); return value;
        }
    }
    T value{}; ReadObject(address, value, vp); return value;
}
template<class Address> inline psp_hook_status WriteMemoryRaw(Address address, const void* input, size_t bytes, bool = false) {
    if (detail::batch) return detail::record(PSP_HOOK_INVALID_STATE);
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    if (detail::runtime.write && !detail::startup.active) {
        detail::runtime.write(site, input, bytes);
        return detail::record(PSP_HOOK_OK);
    }
    return detail::record(detail::startup.active ? detail::queue_memory(site, input, bytes)
                                                 : psp_memory_write(&detail::backend, site, input, bytes));
}
template<class T, class Address> inline psp_hook_status WriteObject(Address address, const T& value, bool vp = false) {
    static_assert(std::is_trivially_copyable_v<T>, "write only trivially copyable guest values");
    static_assert(!std::is_pointer_v<T> || sizeof(T) == 4, "guest pointers are 32-bit");
    return WriteMemoryRaw(address, &value, sizeof(value), vp);
}
template<class T, class Address> inline psp_hook_status WriteMemory(Address address, T value, bool vp = false) {
    auto* access = detail::scalar_access<T>();
    if (access && access->write && !detail::startup.active && !detail::batch) {
        uint32_t site;
        if (!detail::site(address, site)) return LastError();
        if (!(site & (alignof(T) - 1))) {
            access->write(site, value); return detail::record(PSP_HOOK_OK);
        }
    }
    return WriteObject(address, value, vp);
}
template<class Address> inline psp_hook_status MemoryFill(Address address, uint8_t value, size_t bytes, bool = true) {
    if (detail::batch) return detail::record(PSP_HOOK_INVALID_STATE);
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    if (detail::runtime.fill && !detail::startup.active) {
        detail::runtime.fill(site, value, bytes);
        return detail::record(PSP_HOOK_OK);
    }
    if (!detail::startup.active) return detail::record(psp_memory_fill(&detail::backend, site, value, bytes));
    if (bytes > UINT32_MAX || site > UINT32_MAX - bytes) return detail::record(PSP_HOOK_INVALID_ARGUMENT);
    unsigned char fill[64]; memset(fill, value, sizeof(fill));
    while (bytes) {
        size_t count = bytes < sizeof(fill) ? bytes : sizeof(fill);
        auto status = detail::queue_memory(site, fill, count);
        if (status != PSP_HOOK_OK) return detail::record(status);
        site += static_cast<uint32_t>(count); bytes -= count;
    }
    return detail::record(PSP_HOOK_OK);
}
template<class Address> inline memory_pointer GetBranchDestination(Address address, bool = true) {
    uint32_t site, word;
    if (!detail::site(address, site)) return {};
    if (site & 3) { detail::record(PSP_HOOK_UNALIGNED); return {}; }
    if (detail::runtime.target && !detail::startup.active) {
        auto original = detail::runtime.target(site);
        detail::record(detail::runtime.status());
        return memory_pointer(original);
    }
    auto status = psp_memory_read(&detail::backend, site, &word, 4);
    if (status == PSP_HOOK_OK && psp_hook_op(word) != 2 && psp_hook_op(word) != 3 &&
        psp_hook_control(word) != 1 && psp_hook_control(word) != 6) status = PSP_HOOK_UNSUPPORTED_INSTRUCTION;
    detail::record(status);
    return status == PSP_HOOK_OK ? memory_pointer(psp_hook_target(site, word)) : memory_pointer{};
}
template<class Address, class Target> inline memory_pointer MakeJMP(Address address, Target target, bool = true) {
    uint32_t site, destination;
    if (!detail::site(address, site) || !detail::target_site(detail::callback_address(target), destination)) return {};
    if (detail::runtime.jump && !detail::startup.active && !detail::batch) {
        auto original = detail::runtime.jump(site, destination);
        detail::record(detail::runtime.status());
        return memory_pointer(original);
    }
    return detail::publish([&](psp_patch* patch) {
        return psp_patch_create_jump(patch, &detail::backend, site, destination);
    });
}
template<class Address> inline psp_hook_status MakeNOP(Address address, size_t bytes = 4, bool = true) {
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    if (detail::runtime.nop && !detail::startup.active && !detail::batch) {
        detail::runtime.nop(site, bytes);
        return detail::record(detail::runtime.status());
    }
    if ((site | bytes) & 3) return detail::record(PSP_HOOK_UNALIGNED);
    if (!detail::batch && bytes > PSP_HOOK_MAX_INSTRUCTIONS * 4)
        return MemoryFill(site, 0, bytes);
    detail::publish([&](psp_patch* patch) {
        return psp_patch_create_nop(patch, &detail::backend, site, bytes);
    });
    return LastError();
}
template<class Address, class End> inline psp_hook_status MakeRangedNOP(Address address, End end, bool vp = true) {
    auto begin = detail::noted(address), limit = detail::address(end);
    if (limit <= begin || !detail::fits(begin) || !detail::fits(limit)) return detail::record(PSP_HOOK_INVALID_ARGUMENT);
    return MakeNOP(begin, limit - begin, vp);
}
template<class Address> inline psp_hook_status MakeRET(Address address, uint16_t pop = 0, bool = true) {
    if (pop) return detail::record(PSP_HOOK_INVALID_ARGUMENT); // MIPS callees do not pop stack arguments.
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    detail::publish([&](psp_patch* patch) {
        return psp_patch_create_return(patch, &detail::backend, site);
    });
    return LastError();
}
template<class Address, class Target> inline psp_hook_status SetBranchDestination(Address address, Target target) {
    uint32_t site, destination;
    if (!detail::site(address, site) || !detail::target_site(target, destination)) return LastError();
    detail::publish([&](psp_patch* patch) {
        return psp_patch_create_branch(patch, &detail::backend, site, destination);
    });
    return LastError();
}
template<class Address> inline psp_hook_status WriteConstant(Address address, uint32_t value) {
    uint32_t site;
    if (!detail::site(address, site)) return LastError();
    detail::publish([&](psp_patch* patch) {
        return psp_patch_create_constant(patch, &detail::backend, site, value);
    });
    return LastError();
}

template<class Signature> struct cstd;
template<class Return, class... Args> struct cstd<Return(Args...)> {
    template<class Address> static Return call(Address address, Args... args) {
        return reinterpret_cast<Return (*)(Args...)>(detail::address(address))(std::forward<Args>(args)...);
    }
};

// Scoped instruction patches keep their saved words inline. reset() reports a
// conflict rather than overwriting another plugin's subsequent patch.
class scoped_patch {
protected:
    psp_patch patch_{};
    psp_hook_status status_ = PSP_HOOK_OK;
    template<class Create> memory_pointer install(Create create) {
        if (detail::batch) { status_ = detail::record(PSP_HOOK_INVALID_STATE); return {}; }
        status_ = reset();
        if (status_ != PSP_HOOK_OK) return {};
        status_ = create(&patch_);
        if (status_ == PSP_HOOK_OK) status_ = psp_patch_enable(&patch_);
        detail::record(status_);
        return status_ == PSP_HOOK_OK ? memory_pointer(psp_patch_original_target(&patch_)) : memory_pointer{};
    }
public:
    scoped_patch() = default;
    scoped_patch(const scoped_patch&) = delete;
    scoped_patch& operator=(const scoped_patch&) = delete;
    scoped_patch(scoped_patch&& other) noexcept : patch_(other.patch_), status_(other.status_) { other.patch_ = {}; }
    ~scoped_patch() { psp_patch_destroy(&patch_); }
    psp_hook_status reset() { return status_ = psp_patch_destroy(&patch_); }
    psp_hook_status error() const { return status_; }
    bool enabled() const { return patch_.enabled != 0; }
};
class scoped_call : public scoped_patch {
public:
    scoped_call() = default;
    template<class Address, class Target> scoped_call(Address address, Target target, bool vp = true) { make_call(address, target, vp); }
    template<class Address, class Target> memory_pointer make_call(Address address, Target target, bool = true) {
        uint32_t site, destination;
        if (!detail::site(address, site) || !detail::target_site(detail::callback_address(target), destination)) { status_ = LastError(); return {}; }
        return install([&](psp_patch* patch) { return psp_patch_create_call(patch, &detail::backend, site, destination); });
    }
};
class scoped_jmp : public scoped_patch {
public:
    scoped_jmp() = default;
    template<class Address, class Target> scoped_jmp(Address address, Target target, bool vp = true) { make_jmp(address, target, vp); }
    template<class Address, class Target> memory_pointer make_jmp(Address address, Target target, bool = true) {
        uint32_t site, destination;
        if (!detail::site(address, site) || !detail::target_site(detail::callback_address(target), destination)) { status_ = LastError(); return {}; }
        return install([&](psp_patch* patch) { return psp_patch_create_jump(patch, &detail::backend, site, destination); });
    }
};
template<size_t Capacity = 16> class scoped_write {
    static_assert(Capacity > 0, "saved data needs storage");
    psp_hook_backend backend_{};
    unsigned char original_[Capacity]{}, replacement_[Capacity]{};
    uint32_t address_ = 0;
    size_t bytes_ = 0;
    psp_hook_status error_ = PSP_HOOK_OK;
public:
    scoped_write() = default;
    scoped_write(const scoped_write&) = delete;
    scoped_write& operator=(const scoped_write&) = delete;
    ~scoped_write() { reset(); }
    psp_hook_status error() const { return error_; }
    psp_hook_status reset() {
        if (!bytes_) return error_ = PSP_HOOK_OK;
        unsigned char current[Capacity];
        error_ = psp_memory_read(&backend_, address_, current, bytes_);
        if (error_ == PSP_HOOK_OK && memcmp(current, replacement_, bytes_)) error_ = PSP_HOOK_CONFLICT;
        if (error_ == PSP_HOOK_OK) {
            size_t restored = 0;
            error_ = psp_memory_write_ex(&backend_, address_, original_, bytes_, &restored);
            address_ += static_cast<uint32_t>(restored); bytes_ -= restored;
            memmove(original_, original_ + restored, bytes_);
            memmove(replacement_, replacement_ + restored, bytes_);
        }
        return error_;
    }
    template<class Address> psp_hook_status write(Address address, const void* input, size_t bytes, bool = false) {
        if (detail::batch) return error_ = detail::record(PSP_HOOK_INVALID_STATE);
        if (reset() != PSP_HOOK_OK) return detail::record(error_);
        uint32_t site;
        if (!input || !bytes || bytes > Capacity) return error_ = detail::record(PSP_HOOK_INVALID_ARGUMENT);
        if (!detail::site(address, site)) return error_ = LastError();
        backend_ = detail::backend; address_ = site;
        error_ = psp_memory_read(&backend_, site, original_, bytes);
        if (error_ != PSP_HOOK_OK) return detail::record(error_);
        memcpy(replacement_, input, bytes);
        size_t published = 0;
        error_ = psp_memory_write_ex(&backend_, site, replacement_, bytes, &published);
        bytes_ = published; // Retain even a partial write for conflict-checked recovery.
        return detail::record(error_);
    }
    template<class T, class Address> psp_hook_status write(Address address, T value, bool vp = false) {
        static_assert(std::is_trivially_copyable_v<T>, "save only trivially copyable guest values");
        static_assert(!std::is_pointer_v<T> || sizeof(T) == 4, "guest pointers are 32-bit");
        return write(address, &value, sizeof(value), vp);
    }
};

// Startup convenience: group repeated sites without hand-written validation or
// rollback. InstallCalls computes the exact record count from these arrays.
template<class Function, size_t Count> class call_sites {
    uintptr_t addresses_[Count];
    Function replacement_;
    hook_back<Function>* back_;
    Function saved_ = nullptr;
    bool shared_original_;
public:
    static constexpr size_t count = Count;
    call_sites(const uintptr_t (&addresses)[Count], Function replacement,
               hook_back<Function>* back, bool shared_original)
        : replacement_(replacement), back_(back), shared_original_(shared_original) {
        for (size_t i = 0; i < Count; ++i) addresses_[i] = addresses[i];
        if (back_) saved_ = back_->fun;
    }
    template<class Batch> void prepare(Batch& batch) {
        uintptr_t first = 0, last = 0;
        for (size_t i = 0; i < Count; ++i) {
            last = MakeCALL(addresses_[i], replacement_).as_int();
            if (!i) first = last;
            else if (shared_original_) batch.require(first == last);
        }
        if (back_) back_->fun = reinterpret_cast<Function>(last);
    }
    void restore_binding() { if (back_) back_->fun = saved_; }
};
template<size_t Count, class Function>
inline auto CallSites(const uintptr_t (&addresses)[Count], Function replacement,
                      hook_back<Function>& back, bool shared_original = true) {
    return call_sites<Function, Count>(addresses, replacement, &back, shared_original);
}
template<size_t Count, class Function>
inline auto CallSites(const uintptr_t (&addresses)[Count], Function replacement) {
    return call_sites<Function, Count>(addresses, replacement, nullptr, false);
}
template<class... Groups> inline bool InstallCalls(Groups... groups) {
    static_assert(sizeof...(Groups) > 0, "provide call sites");
    patch_batch<(Groups::count + ...)> batch;
    (groups.prepare(batch), ...);
    auto status = batch.commit();
    if (status != PSP_HOOK_OK) (groups.restore_binding(), ...);
    if (batch.rollback_error() != PSP_HOOK_OK) {
        detail::record(batch.rollback_error());
        // A partially restored call can still reference this module. Fail closed
        // rather than returning a failure which permits the loader to unload it.
#if defined(_MSC_VER)
        __debugbreak();
#else
        __builtin_trap();
#endif
    }
    return status == PSP_HOOK_OK;
}
}
#include "safetymips.hpp"
#endif
