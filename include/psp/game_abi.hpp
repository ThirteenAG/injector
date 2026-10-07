#ifndef INJECTOR_PSP_GAME_ABI_HPP
#define INJECTOR_PSP_GAME_ABI_HPP
#include "game_abi.h"
#include <type_traits>

namespace psp {
template<class Function> struct GameStack;
template<class Return,class... Args> struct GameStack<Return(*)(Args...)> {
    static unsigned words() {
        unsigned integers=0,floats=0,stack=0;
        [[maybe_unused]] const auto add=[&](auto type) {
            using T=typename decltype(type)::type;
            static_assert(std::is_scalar_v<T>,"pass game aggregates by pointer");
            constexpr unsigned width=!std::is_pointer_v<T> && sizeof(T)>4 ? 2 : 1;
            unsigned& bank=std::is_floating_point_v<T> ? floats : integers;
            bank=(bank+width-1)&~(width-1);
            if (bank+width>8) { stack=(stack+width-1)&~(width-1);stack+=width; }
            bank+=width;
        };
        (add(Type<Args>{}),...);
        return stack;
    }
private:
    template<class T> struct Type { using type=T; };
};
template<class Return,class... Args> struct GameStack<Return(*)(Args...) noexcept> : GameStack<Return(*)(Args...)> {};
// A cache belongs to one module backend and survives until module unload.
// Creation occurs only while installing hooks; callbacks allocate nothing.
struct GameCallbacks {
    struct Entry { uint32_t target,address;unsigned stack;psp_hook_buffer buffer; };
    Entry entries[128]{};
    unsigned count=0;
    psp_hook_status bind(const psp_hook_backend& backend,uint32_t target,unsigned stack,uint32_t& result) {
        for (unsigned i=0;i<count;++i) if (entries[i].target==target && entries[i].stack==stack) {
            result=entries[i].address;return PSP_HOOK_OK;
        }
        const size_t capacity=psp_game_callback_words(stack);
        if (!capacity || !target || !backend.allocate || !backend.release || !backend.flush)
            return PSP_HOOK_INVALID_ARGUMENT;
        if (count==128) return PSP_HOOK_ALLOCATION_FAILED;
        psp_hook_buffer buffer{};
        if (!backend.allocate(backend.user,capacity*4,target,&buffer)) return PSP_HOOK_ALLOCATION_FAILED;
        size_t words=0;
        auto status=psp_game_emit_callback(buffer.words,buffer.capacity/4,buffer.address,target,stack,&words);
        if (status!=PSP_HOOK_OK) { backend.release(backend.user,&buffer);return status; }
        backend.flush(backend.user,buffer.address,words*4);
        entries[count++]={target,buffer.address,stack,buffer};
        result=buffer.address;return PSP_HOOK_OK;
    }
    template<class Function> psp_hook_status bind(const psp_hook_backend& backend,Function target,uint32_t& result) {
        static_assert(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>);
        return bind(backend,uint32_t(reinterpret_cast<uintptr_t>(target)),GameStack<Function>::words(),result);
    }
};
}
#endif
