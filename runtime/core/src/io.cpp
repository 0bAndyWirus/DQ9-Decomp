#include "core_internal.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace dsr {
namespace {
std::uint32_t width_mask(int width) {
    if (width == 8) return 0xffu;
    if (width == 16) return 0xffffu;
    return 0xffffffffu;
}

std::uint16_t merge16(std::uint16_t old_value, std::uint32_t value,
                      std::uint32_t addr, int width) {
    if (width == 8) {
        const unsigned shift = (addr & 1u) * 8u;
        const std::uint16_t mask = static_cast<std::uint16_t>(0xffu << shift);
        return static_cast<std::uint16_t>((old_value & ~mask) | ((value << shift) & mask));
    }
    if (width == 16) return static_cast<std::uint16_t>(value);
    return static_cast<std::uint16_t>((addr & 2u) ? (value >> 16) : value);
}

std::uint32_t extract(std::uint32_t value, std::uint32_t addr, int width) {
    if (width == 32) return value;
    return (value >> ((addr & 3u) * 8u)) & width_mask(width);
}

std::uint32_t insert(std::uint32_t old, std::uint32_t value, std::uint32_t addr, int width) {
    const unsigned shift = (addr & 3u) * 8u;
    const std::uint32_t mask = width_mask(width) << shift;
    return (old & ~mask) | ((value << shift) & mask);
}

std::uint32_t load_shadow(const std::array<std::uint8_t, 0x1000>& shadow, std::uint32_t offset) {
    return static_cast<std::uint32_t>(shadow[offset]) |
           (static_cast<std::uint32_t>(shadow[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(shadow[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(shadow[offset + 3]) << 24);
}

void save_shadow(std::array<std::uint8_t, 0x1000>& shadow, std::uint32_t offset,
                 std::uint32_t value, int width) {
    for (int i = 0; i < width / 8; ++i)
        shadow[offset + static_cast<unsigned>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::int64_t signed_division(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0) return numerator < 0 ? 1 : -1;
    if (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1)
        return std::numeric_limits<std::int64_t>::min();
    return numerator / denominator;
}

std::uint32_t integer_sqrt(std::uint64_t value) {
    std::uint64_t result = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::uint32_t>(result);
}

void recalculate_divider(Machine::Impl& m) {
    const unsigned mode = m.divcnt & 3u;
    std::int64_t numerator;
    std::int64_t denominator;
    if (mode == 0) {
        numerator = static_cast<std::int32_t>(m.div_numer);
        denominator = static_cast<std::int32_t>(m.div_denom);
    } else {
        numerator = static_cast<std::int64_t>(m.div_numer);
        denominator = (mode == 1 || mode == 3)
                          ? static_cast<std::int32_t>(m.div_denom)
                          : static_cast<std::int64_t>(m.div_denom);
    }
    const bool denominator_zero = denominator == 0;
    const bool div0_flag = m.div_denom == 0;
    const std::int64_t quotient = signed_division(numerator, denominator);
    const std::int64_t min_value = std::numeric_limits<std::int64_t>::min();
    const std::int64_t remainder = denominator_zero ? numerator :
        (numerator == min_value && denominator == -1 ? 0 : numerator % denominator);
    m.div_pending_result = static_cast<std::uint64_t>(quotient);
    m.div_pending_remainder = static_cast<std::uint64_t>(remainder);
    m.div_busy_ticks = mode == 0 ? 18u : 34u;
    m.divcnt = (m.divcnt & 3u) | (div0_flag ? 0x4000u : 0u) | 0x8000u;
}

void recalculate_sqrt(Machine::Impl& m) {
    const std::uint64_t parameter =
        (m.sqrtcnt & 1u) ? m.sqrt_param : (m.sqrt_param & 0xffffffffu);
    m.sqrt_pending_result = integer_sqrt(parameter);
    m.sqrt_busy_ticks = 13;
    m.sqrtcnt = (m.sqrtcnt & 1u) | 0x8000u;
}

std::uint32_t write_register_word(std::uint32_t old_value, std::uint32_t value,
                                  std::uint32_t addr, int width) {
    if (width == 8) {
        const unsigned shift = (addr & 3u) * 8u;
        const std::uint32_t mask = 0xffu << shift;
        return (old_value & ~mask) | ((value & 0xffu) << shift);
    }
    if (width == 16) {
        const unsigned shift = (addr & 2u) * 8u;
        const std::uint32_t mask = 0xffffu << shift;
        return (old_value & ~mask) | ((value & 0xffffu) << shift);
    }
    return value;
}

void write_register64(std::uint64_t& reg, std::uint32_t base,
                      std::uint32_t addr, std::uint32_t value, int width) {
    std::uint32_t word = addr >= base + 4 ? static_cast<std::uint32_t>(reg >> 32)
                                         : static_cast<std::uint32_t>(reg);
    word = write_register_word(word, value, addr, width);
    if (addr >= base + 4)
        reg = (reg & 0xffffffffu) | (static_cast<std::uint64_t>(word) << 32);
    else
        reg = (reg & 0xffffffff00000000ull) | word;
}

constexpr std::array<bool, 0x69> kBasebandWritable = {
    false, true, true, true, true, true, true, true, true, true, true, true, true, false, false, false,
    false, false, false, true, true, true, false, false, false, false, false, true, true, true, true, true,
    true, true, true, true, true, true, true, false, true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, false, true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, false,
    false, false, true, true, false, true, false, true, true
};

bool wifi_register_mapped(std::uint32_t offset) {
    switch (offset & 0x1ffeu) {
    case 0x158: case 0x15a: case 0x15c: case 0x15e: case 0x168:
    case 0x178: case 0x17c: case 0x17e: case 0x180: case 0x184:
        return true;
    default:
        return false;
    }
}

std::uint16_t wifi_register_read(const WifiState& wifi, std::uint32_t offset) {
    switch (offset & 0x1ffeu) {
    case 0x158: return wifi.bb_read;
    case 0x15a: case 0x15e: case 0x180: return 0;
    case 0x15c: return wifi.bb_read;
    case 0x168: return wifi.bb_power;
    case 0x178: case 0x17c: return wifi.rf_data2;
    case 0x17e: return wifi.rf_data1;
    case 0x184: return wifi.rf_control;
    default: return 0xffff;
    }
}

void wifi_write_register(WifiState& wifi, std::uint32_t offset,
                         std::uint16_t value) {
    switch (offset & 0x1ffeu) {
    case 0x15a:
        wifi.bb_write = value & 0xffu;
        break;
    case 0x158: {
        wifi.bb_control = value & 0xf0ffu;
        const unsigned index = wifi.bb_control & 0xffu;
        const unsigned direction = (wifi.bb_control >> 12) & 0xfu;
        if (direction == 5 && index < wifi.baseband.size() &&
            kBasebandWritable[index])
            wifi.baseband[index] = static_cast<std::uint8_t>(wifi.bb_write);
        else if (direction == 6)
            wifi.bb_read = index < wifi.baseband.size() ? wifi.baseband[index] : 0;
        break;
    }
    case 0x168:
        wifi.bb_power = value & 0x800fu;
        break;
    case 0x17e:
        wifi.rf_data1 = value;
        break;
    case 0x17c: {
        wifi.rf_data2 = value & 0xffu;
        const unsigned index = (wifi.rf_data2 >> 2) & 0x1fu;
        if ((wifi.rf_data2 & 0x80u) != 0) {
            const std::uint32_t reg = wifi.rf_registers[index];
            wifi.rf_data1 = static_cast<std::uint16_t>(reg);
            wifi.rf_data2 = (wifi.rf_data2 & 0xfcu) |
                            static_cast<std::uint16_t>((reg >> 16) & 3u);
        } else if (index <= 12) {
            wifi.rf_registers[index] = wifi.rf_data1 |
                                       (static_cast<std::uint32_t>(
                                            wifi.rf_data2 & 3u) << 16);
        } else if (index == 31) {
            wifi.reset_rf();
        }
        wifi.rf_control = 0;
        break;
    }
    case 0x184:
        wifi.rf_control = value;
        break;
    default:
        break;
    }
}
} // namespace

void Machine::Impl::log_unimplemented(CpuId id, std::uint32_t addr, int width) {
    const std::uint32_t key = (static_cast<std::uint32_t>(id) << 24) | (addr & 0x00ffffffu);
    if (!logged_unimplemented.insert(key).second) return;
    std::fprintf(stderr, "[dsr] unimplemented %s IO access width %d at 0x%08x\n",
                 id == CpuId::Arm9 ? "ARM9" : "ARM7", width, addr);
}

bool Machine::Impl::can_deliver_irq(unsigned cpu_index) const {
    const CpuRuntime& rt = cpus[cpu_index];
    const IrqState& state = irq[cpu_index];
    return state.ime != 0 && (state.ie & state.flags) != 0 &&
           (rt.cpu.cpsr_ctrl & kCpsrI) == 0 && !rt.stopped;
}

void Machine::Impl::set_irq(unsigned cpu_index, unsigned bit) {
    if (bit >= 32) return;
    IrqState& state = irq[cpu_index];
    state.flags |= 1u << bit;
    CpuRuntime& rt = cpus[cpu_index];
    // Halt ends on any enabled request (IE & IF), independent of IME/CPSR.I;
    // the guest handler acknowledges IF itself.
    if ((state.ie & state.flags) != 0 && rt.halted && !rt.fiber_done) {
        rt.halted = false;
        rt.stopped = false;
    }
    if (can_deliver_irq(cpu_index)) rt.cpu.budget = 0;
}

void Machine::Impl::handle_key_irq() {
    const std::uint16_t pressed = static_cast<std::uint16_t>(~keyinput) & 0x03ff;
    for (unsigned ci = 0; ci < 2; ++ci) {
        const std::uint16_t selected = irq[ci].keycnt & 0x03ff;
        if ((irq[ci].keycnt & 0x4000) == 0 || selected == 0) continue;
        const bool match = (irq[ci].keycnt & 0x8000) ? ((pressed & selected) == selected)
                                                        : ((pressed & selected) != 0);
        if (match) set_irq(ci, 12);
    }
}

std::uint32_t Machine::Impl::io_read(CpuId id, std::uint32_t addr, int width) {
    const unsigned ci = static_cast<unsigned>(id);
    const std::uint32_t off = addr - 0x04000000u;
    if (id == CpuId::Arm9 && addr == 0x04000246u && width == 16)
        return (gpu2d.io_read(addr, 8) & 0xffu) | (static_cast<std::uint32_t>(wramcnt) << 8);
    if (id == CpuId::Arm9 && addr == 0x04000244u && width == 32)
        return (gpu2d.io_read(addr, 32) & 0x00ffffffu) | (static_cast<std::uint32_t>(wramcnt & 0xffu) << 24);
    if (id == CpuId::Arm9 && addr == 0x04000247u && width == 8) return wramcnt & 3u;
    if (id == CpuId::Arm7 && off == 0x240) {
        const std::uint32_t status = gpu2d.vramstat() | ((wramcnt & 3u) << 8);
        return width == 8 ? gpu2d.vramstat() : status;
    }
    if (id == CpuId::Arm7 && off == 0x241 && width == 8) return wramcnt & 3u;
    if (addr == 0x04100000u) { // IPCFIFORECV
        auto& incoming = ipc.fifo[ci];
        if ((ipc.fifo_control[ci] & 0x8000u) == 0) return ipc.last_received[ci];
        if (incoming.empty()) {
            ipc.fifo_control[ci] |= 0x4000u;
            return ipc.last_received[ci];
        }
        const std::uint32_t word = incoming.front();
        incoming.erase(incoming.begin());
        ipc.last_received[ci] = word;
        if (incoming.empty() && (ipc.fifo_control[ci ^ 1] & 0x0004u)) set_irq(ci ^ 1, 17);
        return word;
    }

    if (id == CpuId::Arm7 && (off & ~1u) == 0x0304u)
        return extract(powcnt2, addr, width);
    if (id == CpuId::Arm9 &&
        ((off <= 0x5f && (off < 0x4 || off > 0x7)) || (off >= 0x64 && off <= 0x6f) ||
         (off >= 0x1000 && off <= 0x106f) ||
         (off >= 0x240 && off <= 0x246) || (off >= 0x248 && off <= 0x249) || off == 0x304))
        return gpu2d.io_read(addr, width);
    if (id == CpuId::Arm9 && ((off >= 0x60 && off <= 0x63) || (off >= 0x320 && off <= 0x6a3)))
    {
        const auto value = gpu3d.io_read(addr, width);
        return value;
    }
    if (id == CpuId::Arm7 && off >= 0x400 && off <= 0x51f) return spu.io_read(addr, width);
    if (id == CpuId::Arm7 && off >= 0x1c0 && off <= 0x1c3) return spi.io_read(addr, width);
    if (id == CpuId::Arm7 && off >= 0x138 && off <= 0x13b) return rtc.io_read(addr, width);
    if (card && ((off >= 0x1a0 && off <= 0x1af) || addr == 0x04100010u)) {
        const unsigned card_owner = (exmemcnt & (1u << 11)) ? 1u : 0u;
        if (ci == card_owner) return card->io_read(addr, width);
        return width_mask(width);
    }
    if (id == CpuId::Arm7 && (addr & 0xffff0000u) == 0x04800000u) {
        if ((powcnt2 & 2u) == 0) return 0;
        const std::uint32_t offset = addr & 0x1fffu;
        if (wifi_register_mapped(offset)) {
            const std::uint32_t base = offset & ~3u;
            const std::uint32_t value =
                wifi_register_read(wifi, base) |
                (static_cast<std::uint32_t>(wifi_register_read(wifi, base + 2)) << 16);
            return extract(value, addr, width);
        }
    }

    if (off >= 0x1000) {
        static const bool log_wifi = std::getenv("DSR_LOG_WIFI") != nullptr;
        if (log_wifi && id == CpuId::Arm7)
            std::fprintf(stderr, "[wifi] t=%llu R%d %08x\n", static_cast<unsigned long long>(total_ticks / 560190u), width, addr);
        log_unimplemented(id, addr, width);
        return width_mask(width);
    }
    if (id == CpuId::Arm7 && off >= 0x280 && off <= 0x2c3) {
        log_unimplemented(id, addr, width);
        return 0;
    }

    const auto& state = irq[ci];
    std::uint32_t value = 0;
    const std::uint32_t base = addr & ~3u;
    const std::uint32_t base_off = base - 0x04000000u;
    switch (base_off) {
    case 0x0004:
        value = state.dispstat | (static_cast<std::uint32_t>(scanline) << 16);
        break;
    case 0x0100: case 0x0104: case 0x0108: case 0x010c: {
        const unsigned ti = (base_off - 0x0100) / 4;
        const TimerState& t = timers[ci * 4 + ti];
        value = t.count | (static_cast<std::uint32_t>(t.control) << 16);
        break;
    }
    case 0x0130:
        value = keyinput | (static_cast<std::uint32_t>(state.keycnt) << 16);
        break;
    case 0x0134: {
        std::uint16_t ext = 0x007fu;
        if (input.buttons & kBtnX) ext &= ~1u;
        if (input.buttons & kBtnY) ext &= ~2u;
        if (input.pen_down) ext &= ~0x40u;
        if (input.lid_closed) ext |= 0x80u;
        value = (load_shadow(io_shadow[ci], 0x134) & 0xffffu) |
                (static_cast<std::uint32_t>(ext) << 16);
        break;
    }
    case 0x0138:
        if (id == CpuId::Arm7) return rtc.io_read(addr, width);
        value = load_shadow(io_shadow[ci], 0x138);
        break;
    case 0x0180:
        value = ipc.sync[ci];
        break;
    case 0x0184: {
        const auto& incoming = ipc.fifo[ci];
        const auto& outgoing = ipc.fifo[ci ^ 1];
        std::uint16_t ctrl = ipc.fifo_control[ci] & 0xc404u;
        if (outgoing.empty()) ctrl |= 0x0001;
        if (outgoing.size() >= 16) ctrl |= 0x0002;
        if (incoming.empty()) ctrl |= 0x0100;
        if (incoming.size() >= 16) ctrl |= 0x0200;
        value = ctrl;
        break;
    }
    case 0x0188:
        value = 0; // IPCFIFOSEND is write-only
        break;
    case 0x0204:
        value = ci == 0 ? exmemcnt : static_cast<std::uint32_t>((exmemcnt & 0xff80u) | exmemstat7);
        break;
    case 0x0208: value = state.ime; break;
    case 0x0210: value = state.ie; break;
    case 0x0214: value = state.flags; break;
    case 0x0244: value = load_shadow(io_shadow[ci], 0x244); break;
    case 0x0248: value = load_shadow(io_shadow[ci], 0x248); break;
    case 0x0300: value = postflg[ci] | (load_shadow(io_shadow[ci], 0x300) & 0xffff0000u); break;
    case 0x0304: return gpu2d.io_read(addr, width);
    case 0x0280: value = divcnt; break;
    case 0x0284: case 0x0288: case 0x028c: value = 0; break;
    case 0x0290: value = static_cast<std::uint32_t>(div_numer); break;
    case 0x0294: value = static_cast<std::uint32_t>(div_numer >> 32); break;
    case 0x0298: value = static_cast<std::uint32_t>(div_denom); break;
    case 0x029c: value = static_cast<std::uint32_t>(div_denom >> 32); break;
    case 0x02a0: value = static_cast<std::uint32_t>(div_result); break;
    case 0x02a4: value = static_cast<std::uint32_t>(div_result >> 32); break;
    case 0x02a8: value = static_cast<std::uint32_t>(div_remainder); break;
    case 0x02ac: value = static_cast<std::uint32_t>(div_remainder >> 32); break;
    case 0x02b0: value = sqrtcnt; break;
    case 0x02b4: value = sqrt_result; break;
    case 0x02b8: value = static_cast<std::uint32_t>(sqrt_param); break;
    case 0x02bc: value = static_cast<std::uint32_t>(sqrt_param >> 32); break;
    default:
        if (base_off >= 0x00b0 && base_off <= 0x00dc) {
            const unsigned channel = (base_off - 0x00b0) / 12;
            const unsigned reg = (base_off - 0x00b0) % 12;
            const DmaState& dma = dmas[ci * 4 + channel];
            if (reg == 0) value = dma.src;
            else if (reg == 4) value = dma.dst;
            else if (reg == 8)
                value = (dma.count & detail::dma_count_mask(ci, channel)) | (dma.control << 16);
        } else {
            value = load_shadow(io_shadow[ci], base_off);
            log_unimplemented(id, base, width);
        }
        break;
    }
    return extract(value, addr, width);
}

void Machine::Impl::io_write(CpuId id, std::uint32_t addr, std::uint32_t value, int width) {
    const unsigned ci = static_cast<unsigned>(id);
    const std::uint32_t off = addr - 0x04000000u;
    if (id == CpuId::Arm9 && addr == 0x04000246u && width == 16) {
        gpu2d.io_write(addr, value & 0xffu, 8);
        wramcnt = static_cast<std::uint16_t>((value >> 8) & 3u);
        rebuild_pages(CpuId::Arm9);
        rebuild_pages(CpuId::Arm7);
        return;
    }
    if (id == CpuId::Arm9 && addr == 0x04000244u && width == 32) {
        gpu2d.io_write(addr, value & 0xffu, 8);
        gpu2d.io_write(addr + 1, (value >> 8) & 0xffu, 8);
        gpu2d.io_write(addr + 2, (value >> 16) & 0xffu, 8);
        wramcnt = static_cast<std::uint16_t>((value >> 24) & 3u);
        rebuild_pages(CpuId::Arm9);
        rebuild_pages(CpuId::Arm7);
        return;
    }
    if (id == CpuId::Arm7 && (off & ~1u) == 0x0304u) {
        powcnt2 = static_cast<std::uint16_t>(merge16(powcnt2, value, addr, width) & 3u);
        return;
    }
    if (id == CpuId::Arm9 &&
        ((off <= 0x5f && (off < 0x4 || off > 0x7)) || (off >= 0x64 && off <= 0x6f) ||
         (off >= 0x1000 && off <= 0x106f) ||
         (off >= 0x240 && off <= 0x246) || (off >= 0x248 && off <= 0x249) || off == 0x304)) {
        gpu2d.io_write(addr, value, width);
        if (off == 0x304) powcnt1 = static_cast<std::uint16_t>(gpu2d.io_read(addr & ~1u, 16));
        return;
    }
    if (id == CpuId::Arm9 && ((off >= 0x60 && off <= 0x63) || (off >= 0x320 && off <= 0x6a3))) {
        gpu3d.io_write(addr, value, width);
        // GXSTAT IRQs are level-triggered; mode changes can leave an already-active condition unchanged.
        if ((addr & ~3u) == 0x04000600u && gpu3d.irq_condition()) set_irq(0, 21);
        return;
    }
    if (id == CpuId::Arm7 && off >= 0x400 && off <= 0x51f) { spu.io_write(addr, value, width); return; }
    if (id == CpuId::Arm7 && off >= 0x1c0 && off <= 0x1c3) { spi.io_write(addr, value, width); return; }
    if (id == CpuId::Arm7 && off >= 0x138 && off <= 0x13b) { rtc.io_write(addr, value, width); return; }
    if (card && ((off >= 0x1a0 && off <= 0x1af) || addr == 0x04100010u)) {
        const unsigned card_owner = (exmemcnt & (1u << 11)) ? 1u : 0u;
        static const bool log_card = std::getenv("DSR_LOG_CARD") != nullptr;
        if (log_card && off == 0x1a4 && (value & 0x80000000u) && width == 32) {
            std::fprintf(stderr, "[card] %s start ctrl=0x%08x cmd=", ci == 0 ? "ARM9" : "ARM7", value);
            for (unsigned i = 0; i < 8; ++i) std::fprintf(stderr, "%02x", card->io_read(0x040001a8u + i, 8));
            std::fprintf(stderr, "%s\n", ci == card_owner ? "" : " IGNORED (not slot owner)");
        }
        if (ci == card_owner) card->io_write(addr, value, width);
        return;
    }
    if (id == CpuId::Arm7 && (addr & 0xffff0000u) == 0x04800000u) {
        if ((powcnt2 & 2u) == 0) return;
        const std::uint32_t offset = addr & 0x1fffu;
        if (wifi_register_mapped(offset)) {
            const auto write_halfword = [this](std::uint32_t reg_offset,
                                                std::uint32_t reg_addr,
                                                std::uint32_t reg_value,
                                                int reg_width) {
                const std::uint16_t old_value = wifi_register_read(wifi, reg_offset);
                const std::uint16_t merged =
                    merge16(old_value, reg_value, reg_addr, reg_width);
                wifi_write_register(wifi, reg_offset, merged);
            };
            if (width == 32) {
                write_halfword(offset, addr, value, 16);
                write_halfword(offset + 2, addr + 2, value >> 16, 16);
            } else {
                write_halfword(offset, addr, value, width);
            }
            return;
        }
    }
    if (off >= 0x1000) {
        static const bool log_wifi = std::getenv("DSR_LOG_WIFI") != nullptr;
        if (log_wifi && id == CpuId::Arm7)
            std::fprintf(stderr, "[wifi] t=%llu W%d %08x = %x\n", static_cast<unsigned long long>(total_ticks / 560190u), width, addr, value);
        log_unimplemented(id, addr, width);
        return;
    }
    if (id == CpuId::Arm7 && off >= 0x280 && off <= 0x2c3) {
        log_unimplemented(id, addr, width);
        return;
    }

    const std::uint32_t base = addr & ~3u;
    const std::uint32_t base_off = base - 0x04000000u;
    const std::uint32_t v = value & width_mask(width);
    const auto enter_halt = [this, ci](std::uint8_t value) {
        CpuRuntime& rt = cpus[ci];
        rt.halted = true;
        rt.stopped = (value & 0x80u) != 0;
        rt.halt_until_irq = !rt.stopped;
        rt.waiting_for_irq = false;
        rt.cpu.budget = 0; // stop at the next checkpoint
        rt.halt_wait_mask = 0xffffffffu;
    };
    switch (off) {
    case 0x0004: case 0x0005: {
        const std::uint16_t next = merge16(irq[ci].dispstat, v, addr, width);
        irq[ci].dispstat = static_cast<std::uint16_t>((irq[ci].dispstat & 0x0007u) | (next & 0xffb8u));
        return;
    }
    case 0x0006: case 0x0007:
        return;
    case 0x0247:
        if (id != CpuId::Arm9) return;
        wramcnt = static_cast<std::uint16_t>(v & 3u);
        save_shadow(io_shadow[ci], off, wramcnt, 8);
        rebuild_pages(CpuId::Arm9);
        rebuild_pages(CpuId::Arm7);
        return;
    case 0x0130:
        if (width == 32) {
            irq[ci].keycnt = static_cast<std::uint16_t>((value >> 16) & 0xc3ffu);
            handle_key_irq();
        }
        return;
    case 0x0131:
        return;
    case 0x0132: case 0x0133: {
        irq[ci].keycnt = merge16(irq[ci].keycnt, value, addr, width) & 0xc3ffu;
        handle_key_irq();
        return;
    }
    case 0x0180: case 0x0181: {
        std::uint32_t cur = ipc.sync[ci];
        cur = insert(cur, value, addr, width);
        const std::uint16_t w = static_cast<std::uint16_t>(cur);
        // Bits 8-11: own output (R/W), mirrored into the remote's input
        // bits 0-3 (read-only); bit 13: send IRQ strobe; bit 14: IRQ enable.
        const std::uint16_t out = static_cast<std::uint16_t>((w >> 8) & 0x000fu);
        ipc.sync[ci] = static_cast<std::uint16_t>((ipc.sync[ci] & 0x000fu) | (out << 8) | (w & 0x4000u));
        ipc.sync[ci ^ 1] = static_cast<std::uint16_t>((ipc.sync[ci ^ 1] & ~0x000fu) | out);
        if ((w & 0x2000u) && (ipc.sync[ci ^ 1] & 0x4000u)) set_irq(ci ^ 1, 16);
        return;
    }
    case 0x0184: case 0x0185: {
        // IPCFIFOCNT: bit2 send-empty IRQ, bit3 send clear, bit10 receive
        // not-empty IRQ, bit14 error (write 1 to ack), bit15 enable.
        const std::uint16_t old = ipc.fifo_control[ci];
        const std::uint16_t w = merge16(old, value, addr, width);
        if (w & 0x0008u) ipc.fifo[ci ^ 1].clear();
        const std::uint16_t error = (w & 0x4000u) ? 0u : (old & 0x4000u);
        ipc.fifo_control[ci] = static_cast<std::uint16_t>((w & 0x8404u) | error);
        if ((w & 0x0004u) && ipc.fifo[ci ^ 1].empty()) set_irq(ci, 17);
        if ((w & 0x0400u) && !ipc.fifo[ci].empty()) set_irq(ci, 18);
        return;
    }
    case 0x0188: {
        if ((ipc.fifo_control[ci] & 0x8000u) == 0) return;
        if (ipc.fifo[ci ^ 1].size() >= 16) {
            ipc.fifo_control[ci] |= 0x4000u;
            return;
        }
        std::uint32_t word = value;
        if (width != 32) {
            const std::uint32_t old = load_shadow(io_shadow[ci], 0x188);
            word = insert(old, value, addr, width);
            save_shadow(io_shadow[ci], 0x188, word, 32);
        }
        static const bool log_pxi = std::getenv("DSR_LOG_PXI") != nullptr;
        if (log_pxi) {
            std::fprintf(stderr, "[pxi] t=%llu %s->%s tag=%u data=0x%07x err=%u\n",
                         static_cast<unsigned long long>(owner->ticks() / 560190u),
                         ci == 0 ? "ARM9" : "ARM7", ci == 0 ? "ARM7" : "ARM9", word & 0x1fu,
                         word >> 6, (word >> 5) & 1u);
        }
        const bool was_empty = ipc.fifo[ci ^ 1].empty();
        ipc.fifo[ci ^ 1].push_back(word);
        if (was_empty && (ipc.fifo_control[ci ^ 1] & 0x0400u)) set_irq(ci ^ 1, 18);
        return;
    }
    case 0x0204: case 0x0205: {
        // ARM9 EXMEMCNT owns bits 7-15 (slot access rights, bit 11 = slot-1
        // to ARM7); ARM7's EXMEMSTAT can only change its own bits 0-6.
        std::uint32_t cur = ci == 0 ? exmemcnt : exmemstat7;
        cur = insert(cur, value, addr, width);
        if (ci == 0) {
            exmemcnt = static_cast<std::uint16_t>(cur & 0xff7fu);
        } else {
            exmemstat7 = static_cast<std::uint16_t>(cur & 0x007fu);
        }
        return;
    }
    case 0x0208: case 0x0209: case 0x020a: case 0x020b: {
        std::uint32_t cur = irq[ci].ime;
        cur = insert(cur, value, addr, width);
        irq[ci].ime = cur & 1u;
        if (can_deliver_irq(ci)) cpus[ci].cpu.budget = 0;
        return;
    }
    case 0x0210: case 0x0211: case 0x0212: case 0x0213: {
        std::uint32_t cur = irq[ci].ie;
        cur = insert(cur, value, addr, width);
        irq[ci].ie = cur;
        if ((irq[ci].ie & irq[ci].flags) && cpus[ci].halted) cpus[ci].halted = false;
        if (can_deliver_irq(ci)) cpus[ci].cpu.budget = 0;
        return;
    }
    case 0x0214: case 0x0215: case 0x0216: case 0x0217: {
        std::uint32_t clear = value;
        if (width != 32) clear <<= (addr & 3u) * 8;
        irq[ci].flags &= ~clear;
        // Clearing IF cannot deassert an asserted GX FIFO IRQ source.
        if (ci == 0 && (clear & (1u << 21)) != 0 && gpu3d.irq_condition()) set_irq(0, 21);
        return;
    }
    case 0x0300:
        postflg[ci] = static_cast<std::uint16_t>(v & 1u);
        save_shadow(io_shadow[ci], off, value, width);
        if (id == CpuId::Arm7 && width >= 16) enter_halt(static_cast<std::uint8_t>(value >> 8));
        return;
    case 0x0301:
        if (id == CpuId::Arm7 && width == 8) {
            enter_halt(static_cast<std::uint8_t>(v));
            return;
        }
        break;
    case 0x0280: case 0x0281: case 0x0282: case 0x0283:
        divcnt = merge16(static_cast<std::uint16_t>(divcnt), value, addr, width) & 3u;
        recalculate_divider(*this);
        return;
    case 0x0290: case 0x0291: case 0x0292: case 0x0293:
    case 0x0294: case 0x0295: case 0x0296: case 0x0297:
        write_register64(div_numer, 0x04000290u, addr, value, width);
        recalculate_divider(*this);
        return;
    case 0x0298: case 0x0299: case 0x029a: case 0x029b:
    case 0x029c: case 0x029d: case 0x029e: case 0x029f:
        write_register64(div_denom, 0x04000298u, addr, value, width);
        recalculate_divider(*this);
        return;
    case 0x02b0: case 0x02b1:
        sqrtcnt = merge16(static_cast<std::uint16_t>(sqrtcnt), value, addr, width) & 1u;
        recalculate_sqrt(*this);
        return;
    case 0x02b8: case 0x02b9: case 0x02ba: case 0x02bb:
    case 0x02bc: case 0x02bd: case 0x02be: case 0x02bf:
        write_register64(sqrt_param, 0x040002b8u, addr, value, width);
        recalculate_sqrt(*this);
        return;
    default:
        if (off >= 0x0100 && off <= 0x010f) {
            const unsigned ti = (off - 0x0100) / 4;
            const unsigned sub = (off - 0x0100) & 3u;
            TimerState& t = timers[ci * 4 + ti];
            if (width == 32 && sub == 0) {
                const std::uint16_t old = t.control;
                t.reload = static_cast<std::uint16_t>(value);
                const std::uint16_t next = static_cast<std::uint16_t>((value >> 16) & 0x00c7u);
                if ((old & 0x80u) == 0 && (next & 0x80u)) {
                    t.count = t.reload;
                    t.phase = 0;
                }
                t.control = next;
            } else if (sub < 2) {
                t.reload = merge16(t.reload, value, addr, width);
                if ((t.control & 0x80u) == 0) t.count = t.reload;
            } else {
                const std::uint16_t old = t.control;
                const std::uint16_t next = merge16(old, value, addr, width) & 0x00c7u;
                if ((old & 0x80u) == 0 && (next & 0x80u)) {
                    t.count = t.reload;
                    t.phase = 0;
                }
                t.control = next;
            }
            return;
        }
        if (off >= 0x00b0 && off <= 0x00df) {
            const unsigned channel = (off - 0x00b0) / 12;
            const unsigned sub = (off - 0x00b0) % 12;
            DmaState& dma = dmas[ci * 4 + channel];
            const std::uint32_t count_mask = detail::dma_count_mask(ci, channel);
            if (width == 32 && sub == 0) {
                dma.src = value;
            } else if (width == 32 && sub == 4) {
                dma.dst = value;
            } else if (width == 32 && sub == 8) {
                const std::uint16_t old = static_cast<std::uint16_t>(dma.control);
                const std::uint16_t next = static_cast<std::uint16_t>((value >> 16) & 0xffe0u);
                dma.count = value & count_mask;
                dma.control = next;
                if ((old & 0x8000u) == 0 && (next & 0x8000u))
                    enable_dma(ci, channel);
            } else if (sub < 4) {
                dma.src = insert(dma.src, value, addr, width);
            } else if (sub < 8) {
                dma.dst = insert(dma.dst, value, addr, width);
            } else if (sub < 10) {
                const std::uint16_t old = static_cast<std::uint16_t>(dma.count);
                const std::uint16_t next = merge16(old, value, addr, width);
                dma.count = ((dma.count & ~0xffffu) | next) & count_mask;
            } else {
                std::uint16_t old = static_cast<std::uint16_t>(dma.control);
                if (ci == 0)
                    old |= static_cast<std::uint16_t>((dma.count >> 16) & 0x1fu);
                const std::uint16_t merged = merge16(old, value, addr, width);
                if (ci == 0)
                    dma.count = (dma.count & 0xffffu) |
                                (static_cast<std::uint32_t>(merged & 0x1fu) << 16);
                const std::uint16_t next = merged & 0xffe0u;
                dma.control = next;
                if ((old & 0x8000u) == 0 && (next & 0x8000u))
                    enable_dma(ci, channel);
            }
            return;
        }
        break;
    }

    save_shadow(io_shadow[ci], off, value, width);
    if (off < 0x1000) log_unimplemented(id, addr, width);
}

} // namespace dsr
