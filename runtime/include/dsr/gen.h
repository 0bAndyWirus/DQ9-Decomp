// Inline arithmetic helpers shared by generated ARM/Thumb code.
#pragma once

#include <bit>
#include <cstdint>
#include <limits>

#include "dsr/cpu.h"
#include "dsr/mem.h"
#include "dsr/rt.h"

namespace dsr {

struct AluResult {
    std::uint32_t value;
    std::uint32_t carry;
    std::uint32_t overflow;
};

inline bool condition_passed(const Cpu* c, unsigned cond) {
    const bool n = c->n != 0;
    const bool z = c->z != 0;
    const bool carry = c->cf != 0;
    const bool v = c->v != 0;
    switch (cond & 0xFu) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return carry;
    case 0x3: return !carry;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return carry && !z;
    case 0x9: return !carry || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return true;
    default: return false;
    }
}

inline AluResult add_with_carry(std::uint32_t a, std::uint32_t b, std::uint32_t carry_in) {
    const std::uint64_t wide = static_cast<std::uint64_t>(a) + b + (carry_in & 1u);
    const std::uint32_t result = static_cast<std::uint32_t>(wide);
    const std::uint32_t overflow = static_cast<std::uint32_t>((~(a ^ b) & (a ^ result)) >> 31);
    return {result, static_cast<std::uint32_t>(wide >> 32), overflow};
}

// carry_in follows ARM subtraction convention: 1 means no borrow.
inline AluResult sub_with_carry(std::uint32_t a, std::uint32_t b, std::uint32_t carry_in) {
    const std::uint32_t not_b = ~b;
    const std::uint64_t wide = static_cast<std::uint64_t>(a) + not_b + (carry_in & 1u);
    const std::uint32_t result = static_cast<std::uint32_t>(wide);
    const std::uint32_t overflow = static_cast<std::uint32_t>(((a ^ b) & (a ^ result)) >> 31);
    return {result, static_cast<std::uint32_t>(wide >> 32), overflow};
}

inline void set_nz(Cpu* c, std::uint32_t value) {
    c->n = value >> 31;
    c->z = static_cast<std::uint32_t>(value == 0);
}

// ARM shift type encodings: LSL=0, LSR=1, ASR=2, ROR=3.
// by_register selects the register-shift rules (amount is then low 8 bits).
inline std::uint32_t shift_carry(std::uint32_t value, unsigned type, unsigned amount,
                                 std::uint32_t carry_in, bool by_register,
                                 std::uint32_t* carry_out) {
    std::uint32_t carry = carry_in & 1u;
    if (by_register) amount &= 0xFFu;
    if (by_register && amount == 0) {
        *carry_out = carry;
        return value;
    }

    switch (type & 3u) {
    case 0: // LSL
        if (amount == 0) break;
        if (amount < 32) {
            carry = (value >> (32 - amount)) & 1u;
            value <<= amount;
        } else if (amount == 32) {
            carry = value & 1u;
            value = 0;
        } else {
            carry = 0;
            value = 0;
        }
        break;
    case 1: // LSR
        if (amount == 0) {
            if (by_register) break;
            amount = 32;
        }
        if (amount < 32) {
            carry = (value >> (amount - 1)) & 1u;
            value >>= amount;
        } else if (amount == 32) {
            carry = value >> 31;
            value = 0;
        } else {
            carry = 0;
            value = 0;
        }
        break;
    case 2: // ASR
        if (amount == 0) {
            if (by_register) break;
            amount = 32;
        }
        if (amount < 32) {
            carry = (value >> (amount - 1)) & 1u;
            value = static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> amount);
        } else {
            carry = value >> 31;
            value = carry ? 0xFFFFFFFFu : 0;
        }
        break;
    default: // ROR / immediate RRX
        if (amount == 0) {
            if (by_register) break;
            carry = value & 1u;
            value = (value >> 1) | ((carry_in & 1u) << 31);
        } else {
            const unsigned rotate = amount & 31u;
            if (rotate == 0) {
                carry = value >> 31;
            } else {
                value = std::rotr(value, static_cast<int>(rotate));
                carry = value >> 31;
            }
        }
        break;
    }
    *carry_out = carry;
    return value;
}
inline std::uint32_t shift_value(std::uint32_t value, unsigned type, unsigned amount,
                                 std::uint32_t carry_in, bool by_register) {
    std::uint32_t carry_out = carry_in & 1u;
    return shift_carry(value, type, amount, carry_in, by_register, &carry_out);
}

inline std::uint32_t clz32(std::uint32_t value) {
    return static_cast<std::uint32_t>(std::countl_zero(value));
}

inline std::int32_t signed_bits(std::uint32_t value) {
    return std::bit_cast<std::int32_t>(value);
}

inline std::uint32_t saturate_signed(Cpu* c, std::int64_t value) {
    constexpr std::int64_t low = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t high = std::numeric_limits<std::int32_t>::max();
    if (value < low) {
        c->q = 1;
        return 0x80000000u;
    }
    if (value > high) {
        c->q = 1;
        return 0x7FFFFFFFu;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

inline std::uint32_t qadd(Cpu* c, std::uint32_t a, std::uint32_t b) {
    return saturate_signed(c, static_cast<std::int64_t>(signed_bits(a)) + signed_bits(b));
}
inline std::uint32_t qsub(Cpu* c, std::uint32_t a, std::uint32_t b) {
    return saturate_signed(c, static_cast<std::int64_t>(signed_bits(a)) - signed_bits(b));
}
inline std::uint32_t qdadd(Cpu* c, std::uint32_t a, std::uint32_t b) {
    const std::uint32_t doubled = saturate_signed(c, static_cast<std::int64_t>(signed_bits(b)) * 2);
    return saturate_signed(c, static_cast<std::int64_t>(signed_bits(a)) + signed_bits(doubled));
}
inline std::uint32_t qdsub(Cpu* c, std::uint32_t a, std::uint32_t b) {
    const std::uint32_t doubled = saturate_signed(c, static_cast<std::int64_t>(signed_bits(b)) * 2);
    return saturate_signed(c, static_cast<std::int64_t>(signed_bits(a)) - signed_bits(doubled));
}

inline std::uint64_t umull(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint64_t>(a) * b;
}
inline std::uint64_t smull(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_bits(a)) * signed_bits(b));
}

inline std::int32_t halfword(std::uint32_t value, bool high) {
    const std::uint16_t part = static_cast<std::uint16_t>(value >> (high ? 16 : 0));
    return std::bit_cast<std::int16_t>(part);
}
inline std::int32_t smulxy(std::uint32_t a, std::uint32_t b, bool x_high, bool y_high) {
    return halfword(a, x_high) * halfword(b, y_high);
}
inline std::uint32_t smla_xy(Cpu* c, std::uint32_t a, std::uint32_t b,
                             std::uint32_t accumulate, bool x_high, bool y_high) {
    const std::int64_t result = static_cast<std::int64_t>(signed_bits(accumulate)) +
                                static_cast<std::int64_t>(halfword(a, x_high)) * halfword(b, y_high);
    if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max()) c->q = 1;
    return static_cast<std::uint32_t>(result);
}
inline std::int32_t smulwy(std::uint32_t a, std::uint32_t b, bool y_high) {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(signed_bits(a)) * halfword(b, y_high)) >> 16);
}
inline std::uint32_t smlawy(Cpu* c, std::uint32_t a, std::uint32_t b,
                            std::uint32_t accumulate, bool y_high) {
    const std::int64_t product = static_cast<std::int64_t>(signed_bits(a)) * halfword(b, y_high);
    const std::int64_t result = static_cast<std::int64_t>(signed_bits(accumulate)) + (product >> 16);
    if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max()) c->q = 1;
    return static_cast<std::uint32_t>(result);
}
inline std::uint64_t smlalxy(std::uint64_t accumulate, std::uint32_t a, std::uint32_t b,
                             bool x_high, bool y_high) {
    const std::uint64_t product_bits = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(halfword(a, x_high)) * halfword(b, y_high));
    return accumulate + product_bits;
}

} // namespace dsr
