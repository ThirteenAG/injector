#ifndef INJECTOR_PSP_REGISTERS_HPP
#define INJECTOR_PSP_REGISTERS_HPP
#include "hooks.h"

namespace psp {
/* Bit casting (or memcpy on older compilers) avoids float/integer aliasing. Assignment between proxies copies bits. */
class FloatRegister {
    uint32_t& bits_;
public:
    explicit FloatRegister(uint32_t& bits) noexcept : bits_(bits) {}
    FloatRegister(const FloatRegister&) = default;
    operator float() const noexcept {
#if defined(__GNUC__) && __GNUC__ >= 11
        return __builtin_bit_cast(float, bits_);
#else
        float value;
        memcpy(&value, &bits_, sizeof(value));
        return value;
#endif
    }
    FloatRegister& operator=(float value) noexcept {
#if defined(__GNUC__) && __GNUC__ >= 11
        bits_ = __builtin_bit_cast(uint32_t, value);
#else
        memcpy(&bits_, &value, sizeof(value));
#endif
        return *this;
    }
    FloatRegister& operator=(const FloatRegister& value) noexcept {
        bits_ = value.bits_;
        return *this;
    }
    FloatRegister& operator+=(float value) noexcept { return *this = static_cast<float>(*this) + value; }
    FloatRegister& operator-=(float value) noexcept { return *this = static_cast<float>(*this) - value; }
    FloatRegister& operator*=(float value) noexcept { return *this = static_cast<float>(*this) * value; }
    FloatRegister& operator/=(float value) noexcept { return *this = static_cast<float>(*this) / value; }
    uint32_t& bits() noexcept { return bits_; }
    const uint32_t& bits() const noexcept { return bits_; }
};

/* A borrowed callback view, never an owning copy of CPU state. Named GPRs
 * refer to low 32-bit words; raw() exposes all saved lanes and special state.
 * FPU fields require SAVE_FPU. This view cannot outlive its callback. */
class Registers {
    psp_hook_context& context_;
public:
    const uint32_t& zero;
    uint32_t& at;
    uint32_t& v0;
    uint32_t& v1;
    uint32_t& a0;
    uint32_t& a1;
    uint32_t& a2;
    uint32_t& a3;
    uint32_t& t0;
    uint32_t& t1;
    uint32_t& t2;
    uint32_t& t3;
    uint32_t& t4;
    uint32_t& t5;
    uint32_t& t6;
    uint32_t& t7;
    uint32_t& s0;
    uint32_t& s1;
    uint32_t& s2;
    uint32_t& s3;
    uint32_t& s4;
    uint32_t& s5;
    uint32_t& s6;
    uint32_t& s7;
    uint32_t& t8;
    uint32_t& t9;
    uint32_t& k0;
    uint32_t& k1;
    uint32_t& gp;
    uint32_t& sp;
    uint32_t& s8;
    uint32_t& ra;
    uint32_t& fp;
    FloatRegister f0;
    FloatRegister f1;
    FloatRegister f2;
    FloatRegister f3;
    FloatRegister f4;
    FloatRegister f5;
    FloatRegister f6;
    FloatRegister f7;
    FloatRegister f8;
    FloatRegister f9;
    FloatRegister f10;
    FloatRegister f11;
    FloatRegister f12;
    FloatRegister f13;
    FloatRegister f14;
    FloatRegister f15;
    FloatRegister f16;
    FloatRegister f17;
    FloatRegister f18;
    FloatRegister f19;
    FloatRegister f20;
    FloatRegister f21;
    FloatRegister f22;
    FloatRegister f23;
    FloatRegister f24;
    FloatRegister f25;
    FloatRegister f26;
    FloatRegister f27;
    FloatRegister f28;
    FloatRegister f29;
    FloatRegister f30;
    FloatRegister f31;
    uint32_t& fcr31;
    explicit Registers(psp_hook_context& context) noexcept
        : context_(context),
          zero(context.gpr[0].word[0]),
          at(context.gpr[1].word[0]),
          v0(context.gpr[2].word[0]),
          v1(context.gpr[3].word[0]),
          a0(context.gpr[4].word[0]),
          a1(context.gpr[5].word[0]),
          a2(context.gpr[6].word[0]),
          a3(context.gpr[7].word[0]),
          t0(context.gpr[8].word[0]),
          t1(context.gpr[9].word[0]),
          t2(context.gpr[10].word[0]),
          t3(context.gpr[11].word[0]),
          t4(context.gpr[12].word[0]),
          t5(context.gpr[13].word[0]),
          t6(context.gpr[14].word[0]),
          t7(context.gpr[15].word[0]),
          s0(context.gpr[16].word[0]),
          s1(context.gpr[17].word[0]),
          s2(context.gpr[18].word[0]),
          s3(context.gpr[19].word[0]),
          s4(context.gpr[20].word[0]),
          s5(context.gpr[21].word[0]),
          s6(context.gpr[22].word[0]),
          s7(context.gpr[23].word[0]),
          t8(context.gpr[24].word[0]),
          t9(context.gpr[25].word[0]),
          k0(context.gpr[26].word[0]),
          k1(context.gpr[27].word[0]),
          gp(context.gpr[28].word[0]),
          sp(context.gpr[29].word[0]),
          s8(context.gpr[30].word[0]),
          ra(context.gpr[31].word[0]),
          fp(context.gpr[30].word[0]),
          f0(context.fpr[0]),
          f1(context.fpr[1]),
          f2(context.fpr[2]),
          f3(context.fpr[3]),
          f4(context.fpr[4]),
          f5(context.fpr[5]),
          f6(context.fpr[6]),
          f7(context.fpr[7]),
          f8(context.fpr[8]),
          f9(context.fpr[9]),
          f10(context.fpr[10]),
          f11(context.fpr[11]),
          f12(context.fpr[12]),
          f13(context.fpr[13]),
          f14(context.fpr[14]),
          f15(context.fpr[15]),
          f16(context.fpr[16]),
          f17(context.fpr[17]),
          f18(context.fpr[18]),
          f19(context.fpr[19]),
          f20(context.fpr[20]),
          f21(context.fpr[21]),
          f22(context.fpr[22]),
          f23(context.fpr[23]),
          f24(context.fpr[24]),
          f25(context.fpr[25]),
          f26(context.fpr[26]),
          f27(context.fpr[27]),
          f28(context.fpr[28]),
          f29(context.fpr[29]),
          f30(context.fpr[30]),
          f31(context.fpr[31]),
          fcr31(context.fcr31) {}
    Registers(const Registers&) = delete;
    Registers& operator=(const Registers&) = delete;
    psp_hook_context& raw() noexcept { return context_; }
    const psp_hook_context& raw() const noexcept { return context_; }
};
}
#endif
