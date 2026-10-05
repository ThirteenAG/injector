#ifndef WFP_PCSX2_PATCHES_HPP
#define WFP_PCSX2_PATCHES_HPP
#include "patches.h"
#include <utility>
#include <type_traits>

namespace pcsx2 {
class Patch {
    pcsx2_patch value_{};
public:
    Patch() = default;
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;
    Patch(Patch&& other) noexcept : value_(other.value_) { other.value_ = {}; }
    Patch& operator=(Patch&&) = delete;
    ~Patch() { pcsx2_patch_destroy(&value_); }
    explicit operator bool() const noexcept { return value_.count != 0; }
    bool enabled() const noexcept { return value_.enabled != 0; }
    uint32_t original_target() const noexcept { return pcsx2_patch_original_target(&value_); }
    pcsx2_hook_status enable() { return pcsx2_patch_enable(&value_); }
    pcsx2_hook_status disable() { return pcsx2_patch_disable(&value_); }
    pcsx2_hook_status reset() { return pcsx2_patch_destroy(&value_); }
    pcsx2_hook_status create(const pcsx2_hook_backend& backend, uint32_t address,
                            const uint32_t* words, size_t count) {
        return pcsx2_patch_create(&value_, &backend, address, words, count);
    }
    pcsx2_hook_status create_call(const pcsx2_hook_backend& backend, uint32_t address, uint32_t target) {
        return pcsx2_patch_create_call(&value_, &backend, address, target);
    }
    template<class Function> Function original() const noexcept {
        static_assert(std::is_pointer_v<Function> &&
                      std::is_function_v<std::remove_pointer_t<Function>>, "original must be a function pointer");
        return reinterpret_cast<Function>(static_cast<uintptr_t>(original_target()));
    }
    template<class Function, std::enable_if_t<std::is_pointer_v<Function> &&
             std::is_function_v<std::remove_pointer_t<Function>>, int> = 0>
    pcsx2_hook_status create_call(const pcsx2_hook_backend& backend, uint32_t address, Function target) {
        static_assert(std::is_pointer_v<Function> &&
                      std::is_function_v<std::remove_pointer_t<Function>>, "target must be a function pointer");
        if (reinterpret_cast<uintptr_t>(target) > UINT32_MAX) return PCSX2_HOOK_OUT_OF_RANGE;
        return create_call(backend, address,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)));
    }
    pcsx2_hook_status create_constant(const pcsx2_hook_backend& backend, uint32_t address, uint32_t value) {
        return pcsx2_patch_create_constant(&value_, &backend, address, value);
    }
};
}
#endif
