#include "core_internal.h"

namespace dsr::detail {
namespace {
unsigned index_for_mode(std::uint32_t mode) {
    switch (mode & kCpsrModeMask) {
    case kModeUsr:
    case kModeSys: return 0;
    case kModeFiq: return 1;
    case kModeIrq: return 2;
    case kModeSvc: return 3;
    case kModeAbt: return 4;
    case kModeUnd: return 5;
    default: return 0;
    }
}

bool valid_mode(std::uint32_t mode) {
    switch (mode & kCpsrModeMask) {
    case kModeUsr: case kModeFiq: case kModeIrq: case kModeSvc:
    case kModeAbt: case kModeUnd: case kModeSys: return true;
    default: return false;
    }
}

std::uint32_t flags(const Cpu& c) {
    return ((c.n & 1u) << 31) | ((c.z & 1u) << 30) |
           ((c.cf & 1u) << 29) | ((c.v & 1u) << 28) | ((c.q & 1u) << 27);
}

void set_flags(Cpu& c, std::uint32_t value, std::uint32_t mask) {
    if (mask & (1u << 31)) c.n = (value >> 31) & 1;
    if (mask & (1u << 30)) c.z = (value >> 30) & 1;
    if (mask & (1u << 29)) c.cf = (value >> 29) & 1;
    if (mask & (1u << 28)) c.v = (value >> 28) & 1;
    if (mask & (1u << 27)) c.q = (value >> 27) & 1;
}

std::uint32_t fields_mask(std::uint32_t field_mask) {
    std::uint32_t mask = 0;
    if (field_mask & 1u) mask |= 0x000000ff;
    if (field_mask & 2u) mask |= 0x0000ff00;
    if (field_mask & 4u) mask |= 0x00ff0000;
    if (field_mask & 8u) mask |= 0xff000000;
    return mask;
}
} // namespace

Bank CpuBanks::bank_for(std::uint32_t mode) {
    switch (mode & kCpsrModeMask) {
    case kModeFiq: return Bank::Fiq;
    case kModeIrq: return Bank::Irq;
    case kModeSvc: return Bank::Svc;
    case kModeAbt: return Bank::Abt;
    case kModeUnd: return Bank::Und;
    default: return Bank::UsrSys;
    }
}

void CpuBanks::switch_mode(Cpu& c, std::uint32_t new_mode) {
    const std::uint32_t old_mode = c.cpsr_ctrl & kCpsrModeMask;
    new_mode &= kCpsrModeMask;
    if (new_mode == old_mode || !valid_mode(new_mode)) return;
    auto& old_high = old_mode == kModeFiq ? fiq_r8_r12 : user_r8_r12;
    for (unsigned i = 0; i < 5; ++i) old_high[i] = c.r[8 + i];
    const unsigned old_bank = index_for_mode(old_mode);
    sp_lr[old_bank][0] = c.r[13];
    sp_lr[old_bank][1] = c.r[14];

    const auto& new_high = new_mode == kModeFiq ? fiq_r8_r12 : user_r8_r12;
    for (unsigned i = 0; i < 5; ++i) c.r[8 + i] = new_high[i];
    const unsigned new_bank = index_for_mode(new_mode);
    c.r[13] = sp_lr[new_bank][0];
    c.r[14] = sp_lr[new_bank][1];
    c.cpsr_ctrl = (c.cpsr_ctrl & ~kCpsrModeMask) | new_mode;
}

std::uint32_t CpuBanks::read_user(const Cpu& c, unsigned reg) const {
    if (reg >= 16) return 0;
    const std::uint32_t mode = c.cpsr_ctrl & kCpsrModeMask;
    if (reg >= 8 && reg <= 12 && mode == kModeFiq) return user_r8_r12[reg - 8];
    if ((reg == 13 || reg == 14) && bank_for(mode) != Bank::UsrSys)
        return sp_lr[0][reg - 13];
    return c.r[reg];
}

void CpuBanks::write_user(Cpu& c, unsigned reg, std::uint32_t value) {
    if (reg >= 16) return;
    const std::uint32_t mode = c.cpsr_ctrl & kCpsrModeMask;
    if (reg >= 8 && reg <= 12 && mode == kModeFiq) user_r8_r12[reg - 8] = value;
    else if ((reg == 13 || reg == 14) && bank_for(mode) != Bank::UsrSys)
        sp_lr[0][reg - 13] = value;
    else c.r[reg] = value;
}

std::uint32_t CpuBanks::cpsr(const Cpu& c) const {
    return (c.cpsr_ctrl & 0xffu) | flags(c);
}

void CpuBanks::write_cpsr(Cpu& c, std::uint32_t value, std::uint32_t field_mask) {
    const std::uint32_t mask = fields_mask(field_mask);
    set_flags(c, value, mask);
    const std::uint32_t mode = c.cpsr_ctrl & kCpsrModeMask;
    const bool privileged = mode != kModeUsr;
    if (privileged && (mask & 0xffu)) {
        const std::uint32_t new_mode = value & kCpsrModeMask;
        if (valid_mode(new_mode)) switch_mode(c, new_mode);
        c.cpsr_ctrl = (c.cpsr_ctrl & ~0xe0u) | (value & 0xe0u);
    }
}

std::uint32_t CpuBanks::read_spsr(const Cpu& c) const {
    const unsigned index = index_for_mode(c.cpsr_ctrl);
    if (index == 0) return 0;
    return spsr[index];
}

void CpuBanks::write_spsr(const Cpu& c, std::uint32_t value, std::uint32_t field_mask) {
    const unsigned index = index_for_mode(c.cpsr_ctrl);
    if (index == 0) return;
    const std::uint32_t mask = fields_mask(field_mask);
    spsr[index] = (spsr[index] & ~mask) | (value & mask);
}

void CpuBanks::exception_return(Cpu& c) {
    const unsigned index = index_for_mode(c.cpsr_ctrl);
    if (index == 0) return;
    const std::uint32_t value = spsr[index];
    write_cpsr(c, value, 0xf);
}

} // namespace dsr::detail

namespace dsr {

std::uint32_t dsr_mrs_cpsr(Cpu* c) {
    auto& m = detail::MachineAccess::impl(c);
    return m.runtime(c->id).banks.cpsr(*c);
}

void dsr_msr_cpsr(Cpu* c, std::uint32_t value, std::uint32_t field_mask) {
    auto& m = detail::MachineAccess::impl(c);
    m.runtime(c->id).banks.write_cpsr(*c, value, field_mask);
}

std::uint32_t dsr_mrs_spsr(Cpu* c) {
    auto& m = detail::MachineAccess::impl(c);
    return m.runtime(c->id).banks.read_spsr(*c);
}

void dsr_msr_spsr(Cpu* c, std::uint32_t value, std::uint32_t field_mask) {
    auto& m = detail::MachineAccess::impl(c);
    m.runtime(c->id).banks.write_spsr(*c, value, field_mask);
}

void dsr_exception_return(Cpu* c) {
    auto& m = detail::MachineAccess::impl(c);
    m.runtime(c->id).banks.exception_return(*c);
}

std::uint32_t dsr_user_reg_read(Cpu* c, unsigned reg) {
    auto& m = detail::MachineAccess::impl(c);
    return m.runtime(c->id).banks.read_user(*c, reg);
}

void dsr_user_reg_write(Cpu* c, unsigned reg, std::uint32_t value) {
    auto& m = detail::MachineAccess::impl(c);
    m.runtime(c->id).banks.write_user(*c, reg, value);
}

} // namespace dsr
