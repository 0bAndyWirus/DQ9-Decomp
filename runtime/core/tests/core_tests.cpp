#include "core_internal.h"
#include "dsr/machine.h"
#include "dsr/mem.h"
#include "dsr/spu.h"
#include "dsr/rt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dsr::tests {
void run_thread_tests();
void run_abi_config_tests();
namespace {
// Guest locations, code, and payloads below are invented fixtures unless a
// comment identifies a fixed DS hardware or BIOS memory-map location.
int failures = 0;
int irq_calls = 0;
int nested_irq_calls = 0;
bool nested_started = false;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "[core-test] FAIL: %s\n", message);
    ++failures;
}

void irq_entry(Cpu* cpu) {
    ++irq_calls;
    if (!nested_started) {
        nested_started = true;
        wr32(cpu, 0x04000214, 1u << 5);
        cpu->m->raise_irq(CpuId::Arm9, 6);
        const std::uint32_t cpsr = dsr_mrs_cpsr(cpu);
        dsr_msr_cpsr(cpu, cpsr & ~kCpsrI, 1);
        dsr_checkpoint(cpu, 0x02002000);
    } else {
        wr32(cpu, 0x04000214, 1u << 6);
        ++nested_irq_calls;
    }
}

void guest_entry(Cpu* cpu) {
    dsr_checkpoint(cpu, 0x02001000);
}

std::array<std::uint16_t, 2> frame_key_samples{};
unsigned frame_key_sample_count = 0;

void frame_key_irq_entry(Cpu* cpu) {
    if (frame_key_sample_count < frame_key_samples.size())
        frame_key_samples[frame_key_sample_count++] =
            cpu->m->read16(cpu->id, 0x04000130);
    cpu->m->write32(cpu->id, 0x04000214, 1u);
}

void frame_key_waiter(Cpu* cpu) {
    for (;;) {
        cpu->budget = 0;
        dsr_checkpoint(cpu, 0x02001000);
    }
}

std::array<std::array<std::uint64_t, 3>, 2> timer_irq_ticks{};
std::array<unsigned, 2> timer_irq_counts{};
std::array<std::uint64_t, 3> timer_guest_ticks{};
unsigned timer_guest_count = 0;

void timer_irq_entry(Cpu* cpu) {
    const unsigned ci = static_cast<unsigned>(cpu->id);
    if (timer_irq_counts[ci] < timer_irq_ticks[ci].size())
        timer_irq_ticks[ci][timer_irq_counts[ci]] = cpu->m->ticks();
    ++timer_irq_counts[ci];
    cpu->m->write32(cpu->id, 0x04000214, 0x78u);
}

void timer_irq_waiter(Cpu* cpu) {
    for (;;) {
        if (timer_guest_count < timer_guest_ticks.size())
            timer_guest_ticks[timer_guest_count] = cpu->m->ticks();
        ++timer_guest_count;
        cpu->budget = 0;
        dsr_checkpoint(cpu, 0x02001000);
    }
}


int overlay_choice = 0;
bool call_overlay = false;

void overlay_main(Cpu* cpu) {
    if (call_overlay) dsr_call(cpu, 0x02003000);
}

void overlay_a(Cpu*) { overlay_choice = 1; }
void overlay_b(Cpu*) { overlay_choice = 2; }
void overlay_replacement(Cpu*) { overlay_choice = 3; }

bool thumb_entry_observed = false;

void region_overlay_a(Cpu* cpu, std::uint32_t entry_address) {
    cpu->r[0] = entry_address;
    cpu->r[1] = 1;
    cpu->r[2] = (cpu->cpsr_ctrl & kCpsrT) != 0;
}

void region_overlay_b(Cpu* cpu, std::uint32_t entry_address) {
    cpu->r[0] = entry_address;
    cpu->r[1] = 2;
    cpu->r[2] = (cpu->cpsr_ctrl & kCpsrT) != 0;
}

void region_thumb(Cpu* cpu, std::uint32_t entry_address) {
    cpu->r[0] = entry_address;
    cpu->r[1] = (cpu->cpsr_ctrl & kCpsrT) != 0;
}

void test_shared_region_fallback_dispatch() {
    constexpr std::uint32_t arm_begin = 0x02008000;
    constexpr std::uint32_t thumb_begin = 0x02009000;
    constexpr std::array<std::uint8_t, 8> image = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    const std::uint64_t hash = fnv1a64(image.data(), image.size());
    const std::uint64_t arm_hash = fnv1a64(image.data(), image.size() - 2);
    const GuestAddressRange literal_pool[] = {{arm_begin + 6, 2}};
    const DispatchRegionEntry regions[] = {
        {arm_begin, image.size(), kModuleArm9Main, 0, arm_hash ^ 1u, literal_pool, 1, region_overlay_a},
        {arm_begin, image.size(), 0, 0, arm_hash, literal_pool, 1, region_overlay_b},
        {thumb_begin, image.size(), kModuleArm9Main, 1, hash, nullptr, 0, region_thumb},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_regions = {regions, 3};
    Machine machine(config);
    for (std::size_t i = 0; i < image.size(); ++i) {
        machine.write8(CpuId::Arm9, arm_begin + static_cast<std::uint32_t>(i), image[i]);
        machine.write8(CpuId::Arm9, thumb_begin + static_cast<std::uint32_t>(i), image[i]);
    }
    machine.write16(CpuId::Arm9, arm_begin + 6, 0xbeefu);
    Cpu& cpu = machine.cpu(CpuId::Arm9);
    cpu.cpsr_ctrl = kModeSvc;
    dsr_call(&cpu, arm_begin + 4);
    check(cpu.r[0] == arm_begin + 4 && cpu.r[1] == 2 && cpu.r[2] == 0,
          "unregistered ARM interior entry uses the loaded shared-region hash and seeded address");
    check((cpu.cpsr_ctrl & kCpsrT) == 0,
          "shared-region ARM fallback restores caller instruction state");

    cpu.r[1] = 0;
    dsr_call(&cpu, thumb_begin + 5);
    check(cpu.r[0] == thumb_begin + 4 && cpu.r[1] == 1,
          "shared-region Thumb fallback receives a normalized interior address in Thumb state");
    check((cpu.cpsr_ctrl & kCpsrT) == 0,
          "shared-region Thumb fallback restores caller instruction state");
}
void thumb_entry(Cpu* cpu) {
    thumb_entry_observed = (cpu->cpsr_ctrl & kCpsrT) != 0;
}

void test_interworking_return_state() {
    GuestFn slot = thumb_entry;
    FnEntry entries[] = {
        {0x02006000, 4, kModuleArm9Main, 1, 0, &slot, "thumb_interworking_test"},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries, 1};
    Machine machine(config);
    Cpu& cpu = machine.cpu(CpuId::Arm9);
    cpu.cpsr_ctrl = kModeSvc;
    cpu.r[14] = 0x02007000;
    thumb_entry_observed = false;
    dsr_call(&cpu, 0x02006001);
    check(thumb_entry_observed, "dynamic interworking call enters Thumb state");
    check((cpu.cpsr_ctrl & kCpsrT) == 0,
          "dynamic interworking return restores the caller instruction state");
    // Return verification: the callee notes its return target, the caller
    // checks it against the address it expects.
    dsr_note_return(&cpu, cpu.r[14]);
    dsr_check_return(&cpu, 0x02007000);
}

void test_main_ram_write_watch_expands_mirrors() {
    using dsr::detail::GuestWriteWatchRange;
    const std::vector<GuestWriteWatchRange> requested = {
        {0x02901000, 0x02901001},
        {0x04000000, 0x04000004},
    };
    const auto expanded = dsr::detail::expand_main_ram_write_watch_aliases(requested);
    const auto contains = [&](std::uint32_t address) {
        for (const auto& range : expanded)
            if (address >= range.first && static_cast<std::uint64_t>(address) < range.end)
                return true;
        return false;
    };
    check(contains(0x02101000) && contains(0x02501000) &&
          contains(0x02901000) && contains(0x02d01000),
          "main-RAM write watches expand from any mirror to every mirror");
    check(!contains(0x02100fff) && !contains(0x02101001) &&
          contains(0x04000000) && contains(0x04000003) && !contains(0x04000004),
          "write-watch expansion preserves the requested byte range and non-RAM watches");
}

void test_memory_map() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    machine.write32(CpuId::Arm9, 0x02000000, 0x12345678);
    check(machine.read32(CpuId::Arm7, 0x02400000) == 0x12345678,
          "main RAM mirrors at +4 MiB");
    machine.write8(CpuId::Arm7, 0x02ffffff, 0x9a);
    check(machine.read8(CpuId::Arm9, 0x023fffff) == 0x9a,
          "main RAM mirrors at the top of the 16 MiB aperture");

    dsr_mcr(&machine.cpu(CpuId::Arm9), 15, 0, 9, 1, 0, 0x0080000c, 0);
    machine.write32(CpuId::Arm9, 0x00800004, 0xaabbccdd);
    check(machine.read32(CpuId::Arm9, 0x00804004) == 0xaabbccdd,
          "DTCM region larger than backing memory mirrors physically");
    dsr_mcr(&machine.cpu(CpuId::Arm9), 15, 0, 9, 1, 1, 0x0000000e, 0);
    machine.write32(CpuId::Arm9, 0x00000100, 0x87654321);
    check(machine.read32(CpuId::Arm9, 0x00008100) == 0x87654321,
          "ITCM mirrors according to its CP15 region size");

    machine.write8(CpuId::Arm9, 0x04000247, 0);
    machine.write8(CpuId::Arm9, 0x03000000, 0x51);
    check(machine.read8(CpuId::Arm7, 0x03000000) == 0,
          "WRAM mode 0 assigns shared WRAM only to ARM9");
    check(machine.read8(CpuId::Arm7, 0x04000241) == 0,
          "WRAMSTAT reports the mode-0 bank assignment");
    machine.write8(CpuId::Arm9, 0x04000247, 1);
    check(machine.read8(CpuId::Arm7, 0x04000241) == 1,
          "WRAMSTAT follows ARM9 WRAMCNT writes");
    machine.write8(CpuId::Arm7, 0x03000000, 0x42);
    check(machine.read8(CpuId::Arm9, 0x03000000) == 0,
          "WRAM mode 1 separates the two 16 KiB banks");
    machine.write8(CpuId::Arm9, 0x04000247, 2);
    check(machine.read8(CpuId::Arm9, 0x03000000) == 0x42,
          "WRAMCNT swaps ARM9 onto the other bank");
    machine.write8(CpuId::Arm9, 0x04000247, 3);
    machine.write8(CpuId::Arm7, 0x03000000, 0x77);
    check(machine.read8(CpuId::Arm7, 0x03007fff) == 0,
          "WRAM mode 3 exposes a 32 KiB ARM7 window");
    check(machine.read8(CpuId::Arm9, 0x03000000) == 0,
          "WRAM mode 3 removes the shared window from ARM9");
}

void test_irq_and_nested_return() {
    GuestFn main_slot = guest_entry;
    GuestFn irq_slot = irq_entry;
    FnEntry entries[] = {
        {0x02001000, 4, kModuleArm9Main, 0, 0, &main_slot, "guest_entry"},
        {0x02002000, 4, kModuleArm9Main, 0, 0, &irq_slot, "irq_entry"},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries, 2};
    Machine machine(config);
    irq_calls = 0;
    nested_irq_calls = 0;
    nested_started = false;

    Cpu& cpu = machine.cpu(CpuId::Arm9);
    cpu.cpsr_ctrl = kModeIrq; // permit IRQ entry and give its banked stack a valid region
    cpu.r[13] = 0x00803fa0;
    cpu.r[0] = 0x13579bdf;
    machine.write32(CpuId::Arm9, 0x04000208, 1); // IME
    machine.write32(CpuId::Arm9, 0x04000210, (1u << 5) | (1u << 6));
    machine.write32(CpuId::Arm9, 0x00803ffc, 0x02002000);
    machine.raise_irq(CpuId::Arm9, 5);
    machine.start_cpu(CpuId::Arm9, 0x02001000);
    machine.run_ticks(1);

    check(irq_calls == 2, "IRQ handler is entered for outer and nested IRQs");
    check(nested_irq_calls == 1, "nested IRQ dispatch completes once");
    check(cpu.r[0] == 0x13579bdf, "BIOS IRQ wrapper restores guest registers");
    check(cpu.cpsr_ctrl == kModeIrq, "BIOS IRQ wrapper restores the interrupted mode");
    check((machine.read32(CpuId::Arm9, 0x04000214) & ((1u << 5) | (1u << 6))) == 0,
          "nested IRQ handler acknowledges both IF bits");
}

void test_timer_irq_event_boundaries() {
    struct TimerCase {
        const char* name;
        unsigned root;
        unsigned irq_timer;
        std::array<std::uint16_t, 4> reloads;
        unsigned prescaler;
        std::uint64_t warmup;
        bool change_reload;
        std::uint16_t next_reload;
        std::array<std::uint64_t, 3> expected;
    };
    constexpr TimerCase cases[] = {
        {"timer 0 /17", 0, 0, {0xffef}, 0, 0, false, 0, {17, 34, 51}},
        {"timer 2 /63", 2, 2, {0, 0, 0xffc1}, 0, 0, false, 0, {63, 126, 189}},
        {"prescale 64, phase 47", 0, 0, {0xffff}, 1, 47, false, 0, {64, 128, 192}},
        {"prescale 256, phase 193", 0, 0, {0xffff}, 2, 193, false, 0, {256, 512, 768}},
        {"prescale 1024, phase 1007", 0, 0, {0xffff}, 3, 1007, false, 0, {1024, 2048, 3072}},
        {"changed reload /17 then /63", 0, 0, {0xffef}, 0, 5, true, 0xffc1, {17, 80, 143}},
        {"one cascade", 0, 1, {0xfffb, 0xfffd}, 0, 0, false, 0, {15, 30, 45}},
        {"two cascades /63", 0, 2, {0xfff9, 0xfffd, 0xfffd}, 0, 0, false, 0, {63, 126, 189}},
        {"three cascades, /1 source", 0, 3, {0xffff, 0xfffd, 0xfffd, 0xfff9}, 0, 0, false, 0, {63, 126, 189}},
    };
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        for (bool chunked : {false, true}) {
            for (const TimerCase& test : cases) {
                GuestFn main_slot = timer_irq_waiter;
                GuestFn irq_slot = timer_irq_entry;
                const std::int32_t module = id == CpuId::Arm9 ? kModuleArm9Main : kModuleArm7MainRam;
                FnEntry entries[] = {
                    {0x02001000, 4, module, 0, 0, &main_slot, "timer_irq_waiter"},
                    {0x02002000, 4, module, 0, 0, &irq_slot, "timer_irq_entry"},
                };
                MachineConfig config;
                config.direct_boot = false;
                if (id == CpuId::Arm9) config.arm9_table = {entries, 2};
                else config.arm7_table = {entries, 2};
                Machine machine(config);
                Cpu& cpu = machine.cpu(id);
                cpu.cpsr_ctrl = kModeIrq;
                cpu.r[13] = id == CpuId::Arm9 ? 0x00803fa0 : 0x0380ffa0;
                machine.write32(id, id == CpuId::Arm9 ? 0x00803ffc : 0x0380fffc, 0x02002000);
                machine.write32(id, 0x04000208, 1);
                machine.write32(id, 0x04000210, 1u << (3 + test.irq_timer));
                timer_irq_ticks = {};
                timer_irq_counts = {};
                timer_guest_ticks = {};
                timer_guest_count = 0;
                for (unsigned ti = test.root; ti <= test.irq_timer; ++ti) {
                    const unsigned control = 0x80u |
                        (ti == test.root ? test.prescaler : 4u) |
                        (ti == test.irq_timer ? 0x40u : 0u);
                    machine.write32(id, 0x04000100 + ti * 4,
                                    (control << 16) | test.reloads[ti]);
                }
                machine.run_ticks(test.warmup);
                if (test.change_reload)
                    machine.write16(id, 0x04000100 + test.root * 4, test.next_reload);
                machine.start_cpu(id, 0x02001000);
                const std::uint64_t end = test.expected.back() + 1;
                constexpr std::array<unsigned, 5> chunks = {5, 19, 3, 41, 7};
                unsigned chunk_index = 0;
                while (machine.ticks() < end) {
                    std::uint64_t step = end - machine.ticks();
                    if (chunked && step > chunks[chunk_index % chunks.size()])
                        step = chunks[chunk_index % chunks.size()];
                    machine.run_ticks(step);
                    ++chunk_index;
                }
                const unsigned ci = static_cast<unsigned>(id);
                const auto& observed = timer_irq_ticks[ci];
                char message[320];
                std::snprintf(message, sizeof(message),
                    "%s %s %s IRQ ticks [%llu,%llu,%llu], calls %u; expected [%llu,%llu,%llu], calls 3",
                    id == CpuId::Arm9 ? "ARM9" : "ARM7", test.name,
                    chunked ? "chunked" : "single run",
                    static_cast<unsigned long long>(observed[0]),
                    static_cast<unsigned long long>(observed[1]),
                    static_cast<unsigned long long>(observed[2]), timer_irq_counts[ci],
                    static_cast<unsigned long long>(test.expected[0]),
                    static_cast<unsigned long long>(test.expected[1]),
                    static_cast<unsigned long long>(test.expected[2]));
                check(timer_irq_counts[ci] == test.expected.size() && observed == test.expected, message);
                check(machine.ticks() == end, "timer IRQ boundaries preserve the requested run_ticks duration");
                if (!chunked && test.irq_timer == 3)
                    check(timer_guest_count == 4 &&
                          timer_guest_ticks == std::array<std::uint64_t, 3>{0, 63, 126},
                          "a /1 cascade source does not force per-clock CPU slices before its downstream IRQ");
            }
        }
    }
}

// Deliberately scalar: one bus tick and one counter increment at a time,
// without the runtime's overflow division or deadline arithmetic.
struct ScalarTimerReference {
    struct Timer {
        std::uint16_t reload = 0;
        std::uint16_t count = 0;
        std::uint16_t control = 0;
        std::uint64_t phase = 0;
    };
    std::array<Timer, 8> timers{};
    std::array<std::uint32_t, 2> flags{};
    std::uint64_t ticks = 0;

    void increment(unsigned ci, unsigned ti) {
        Timer& timer = timers[ci * 4 + ti];
        if ((timer.control & 0x80u) == 0) return;
        if (timer.count != 0xffffu) {
            ++timer.count;
            return;
        }
        timer.count = timer.reload;
        if ((timer.control & 0x40u) != 0) flags[ci] |= 1u << (3 + ti);
        if (ti != 3 && (timers[ci * 4 + ti + 1].control & 0x84u) == 0x84u)
            increment(ci, ti + 1);
    }

    void advance(std::uint64_t elapsed) {
        constexpr unsigned dividers[4] = {1, 64, 256, 1024};
        for (std::uint64_t tick = 0; tick < elapsed; ++tick) {
            for (unsigned ci = 0; ci < 2; ++ci) {
                for (unsigned ti = 0; ti < 4; ++ti) {
                    Timer& timer = timers[ci * 4 + ti];
                    if ((timer.control & 0x84u) != 0x80u) continue;
                    ++timer.phase;
                    const unsigned divider = dividers[timer.control & 3u];
                    while (timer.phase >= divider) {
                        timer.phase -= divider;
                        increment(ci, ti);
                    }
                }
            }
        }
        ticks += elapsed;
    }

    std::uint64_t irq_boundary(std::uint64_t limit) const {
        ScalarTimerReference probe = *this;
        for (std::uint64_t elapsed = 1; elapsed < limit; ++elapsed) {
            probe.flags = {};
            probe.advance(1);
            if (probe.flags[0] != 0 || probe.flags[1] != 0) return elapsed;
        }
        return limit;
    }

    void write(unsigned ci, unsigned ti, unsigned sub, std::uint32_t value, int width) {
        if (width == 16) sub &= ~1u;
        if (width == 32) sub = 0;
        Timer& timer = timers[ci * 4 + ti];
        const auto replace_half = [sub, value, width](std::uint16_t old) {
            if (width != 8) return static_cast<std::uint16_t>(value);
            const unsigned shift = (sub & 1u) * 8u;
            const unsigned mask = 0xffu << shift;
            return static_cast<std::uint16_t>((old & ~mask) | ((value & 0xffu) << shift));
        };
        std::uint16_t next_control;
        if (width == 32) {
            timer.reload = static_cast<std::uint16_t>(value);
            next_control = static_cast<std::uint16_t>((value >> 16) & 0xc7u);
        } else if (sub < 2) {
            timer.reload = replace_half(timer.reload);
            if ((timer.control & 0x80u) == 0) timer.count = timer.reload;
            return;
        } else {
            next_control = replace_half(timer.control) & 0xc7u;
        }
        if ((timer.control & 0x80u) == 0 && (next_control & 0x80u) != 0) {
            timer.count = timer.reload;
            timer.phase = 0;
        }
        timer.control = next_control;
    }

    std::uint32_t word(unsigned ci, unsigned ti) const {
        const Timer& timer = timers[ci * 4 + ti];
        return timer.count | (static_cast<std::uint32_t>(timer.control) << 16);
    }
};

struct TimerEquivalence {
    static MachineConfig config() {
        MachineConfig result;
        result.direct_boot = false;
        return result;
    }

    explicit TimerEquivalence(std::uint32_t seed, bool split = false)
        : actual(config()), seed(seed), split(split) {}

    Machine actual;
    ScalarTimerReference reference{};
    std::uint32_t seed;
    bool split;
    unsigned operation = 0;
    bool healthy = true;

    Machine::Impl& impl() {
        return detail::MachineAccess::impl(&actual.cpu(CpuId::Arm9));
    }

    bool expect(bool condition, const char* message) {
        if (condition) return true;
        if (healthy) {
            std::fprintf(stderr, "[timer-test] seed=%08x split=%u operation=%u tick=%llu\n",
                         seed, static_cast<unsigned>(split), operation,
                         static_cast<unsigned long long>(reference.ticks));
            check(false, message);
        }
        healthy = false;
        return false;
    }

    bool compare_flags() {
        for (unsigned ci = 0; ci < 2; ++ci) {
            const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
            // Check IF before any timer read can materialize lazy state.
            if (!expect(actual.read32(id, 0x04000214) == reference.flags[ci],
                        "timer IF matches the scalar before observing counts")) return false;
        }
        return true;
    }

    void advance(std::uint64_t ticks) {
        ++operation;
        // This observes the lazy epoch without a timer-register read.
        const auto limit = std::min<std::uint64_t>(ticks, 128);
        if (limit != 0)
            expect(impl().timer_irq_boundary(limit) == reference.irq_boundary(limit),
                   "lazy timer IRQ boundaries match the seeded scalar before synchronization");
        reference.advance(ticks);
        actual.run_ticks(ticks);
        expect(actual.ticks() == reference.ticks, "timer service does not change elapsed time");
        compare_flags();
    }

    void write(unsigned ci, unsigned ti, unsigned sub, std::uint32_t value, int width = 16) {
        ++operation;
        reference.write(ci, ti, sub, value, width);
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        const std::uint32_t addr = 0x04000100u + ti * 4 + sub;
        if (width == 8) actual.write8(id, addr, static_cast<std::uint8_t>(value));
        else if (width == 16) actual.write16(id, addr, static_cast<std::uint16_t>(value));
        else actual.write32(id, addr, value);
        compare_flags();
    }

    void clear_flags(unsigned ci, unsigned sub, std::uint32_t value, int width) {
        ++operation;
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        const std::uint32_t addr = 0x04000214u + sub;
        if (width == 8) actual.write8(id, addr, static_cast<std::uint8_t>(value));
        else if (width == 16) actual.write16(id, addr, static_cast<std::uint16_t>(value));
        else actual.write32(id, addr, value);
        if (width == 8) value = (value & 0xffu) << (sub * 8);
        else if (width == 16) value = (value & 0xffffu) << ((sub & 2u) * 8);
        reference.flags[ci] &= ~value;
        compare_flags();
    }

    void read(unsigned ci, unsigned ti, unsigned sub, int width) {
        ++operation;
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        const std::uint32_t addr = 0x04000100u + ti * 4 + sub;
        std::uint32_t value;
        if (width == 8) value = actual.read8(id, addr);
        else if (width == 16) value = actual.read16(id, addr);
        else value = actual.read32(id, addr);
        std::uint32_t expected = reference.word(ci, ti);
        if (width == 8) expected = (expected >> (sub * 8)) & 0xffu;
        else if (width == 16) expected = (expected >> ((sub & 2u) * 8)) & 0xffffu;
        expect(value == expected, "byte, halfword and word timer reads match the scalar");
    }

    void compare() {
        if (!healthy || !compare_flags()) return;
        for (unsigned ci = 0; ci < 2; ++ci) {
            for (unsigned ti = 0; ti < 4; ++ti) {
                read(ci, ti, 0, 32);
                read(ci, ti, 0, 16);
                read(ci, ti, 2, 16);
                for (unsigned sub = 0; sub < 4; ++sub) read(ci, ti, sub, 8);
                // The public reads above synchronize every timer. Private
                // phase/reload comparisons never inspect stale lazy state.
                const auto& got = impl().timers[ci * 4 + ti];
                const auto& want = reference.timers[ci * 4 + ti];
                if (!expect(got.reload == want.reload && got.count == want.count &&
                            got.control == want.control && got.phase == want.phase,
                            "timer reload/count/control and retained prescaler phase match the scalar"))
                    return;
            }
        }
        expect(impl().timers_synced_tick == reference.ticks,
               "observing timer registers synchronizes all eight timers");
        expect(impl().timers_due_tick > reference.ticks,
               "the next timer deadline is strictly after the synchronized tick");
        compare_flags();
    }

    void reset() {
        ++operation;
        impl().reset();
        reference = {};
        expect(impl().timers_synced_tick == 0 && impl().timers_due_tick == detail::kNoTimerDue,
               "reset discards both the lazy timer epoch and pending deadline");
        compare();
    }
};

void test_timer_deadline_edges() {
    TimerEquivalence e(0x71de1a9u);
    e.advance(4097);
    e.expect(e.impl().timers_synced_tick == 0 &&
             e.impl().timers_due_tick == detail::kNoTimerDue,
             "stopped timers do not require per-step synchronization");
    e.compare();
    e.write(0, 0, 0, 0x00830000u, 32); // enabled, divider 1024, reload zero
    const auto synced = e.impl().timers_synced_tick;
    const auto deadline = e.impl().timers_due_tick;
    e.advance(4097);
    e.expect(e.impl().timers_synced_tick == synced,
             "a running timer remains lazy before its first overflow");
    e.read(0, 0, 2, 8); // A control-only read must synchronize counts/phase too.
    e.expect(e.impl().timers_synced_tick == e.actual.ticks() &&
             e.impl().timers_due_tick == deadline,
             "read synchronization preserves the absolute first-overflow deadline");
    e.compare();

    constexpr unsigned dividers[4] = {1, 64, 256, 1024};
    for (unsigned ci = 0; ci < 2 && e.healthy; ++ci) {
        for (unsigned prescaler = 0; prescaler < 4; ++prescaler) {
            e.reset();
            e.write(ci, 0, 0, ((0xc0u | prescaler) << 16) | 0xffffu, 32);
            e.write(ci, 1, 0, 0x00c4fffeu, 32);
            e.advance(dividers[prescaler] - 1);
            e.read(ci, 0, 3, 8);
            e.compare();
            e.advance(1);
            e.compare();
            e.clear_flags(ci, 0, 0xffu, 8);
            e.advance(3 * dividers[prescaler] + 17);
            e.compare();
        }

        e.reset();
        e.write(ci, 0, 0, 0x00001234u, 32);
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        e.expect(e.actual.read16(id, 0x04000100) == 0,
                 "a stopped 32-bit write changes reload but does not copy it into count");
        e.write(ci, 0, 1, 0xffu, 8);
        e.write(ci, 0, 3, 0xffu, 8); // Ignored control high byte.
        e.write(ci, 0, 2, 0xc1u, 8);
        e.advance(63);
        e.write(ci, 0, 2, 0x41u);
        e.advance(4001);
        e.write(ci, 0, 0, 0xffffu);
        e.compare(); // A stopped reload write copies count, but retains phase.
        e.write(ci, 0, 2, 0xc1u);
        e.advance(63);
        e.compare(); // A rising enable edge cleared the old phase.
        e.advance(1);
        e.write(ci, 0, 0, 0x00c1fffeu, 32);
        e.advance(64);
        e.compare(); // Already-enabled word writes do not restart the count.
        e.write(ci, 0, 0, 0x00009876u, 32);
        e.advance(4097);
        e.compare(); // Disabling by word write retains the current count/phase.

        e.reset();
        e.write(ci, 0, 0, 0x00c3ffffu, 32);
        e.write(ci, 1, 0, 0x00c40000u, 32);
        e.advance(1023);
        e.write(ci, 0, 2, 0xc0u);
        e.expect(e.impl().timers_due_tick == e.actual.ticks() + 1,
                 "a divider reduction with excess phase is due on the next positive tick");
        e.compare();
        e.advance(0);
        e.compare(); // Reads and writes at the same tick must not consume excess phase.
        e.advance(1);
        e.expect(e.actual.read16(id, 0x04000104) == 1024,
                 "all retained-phase overflows cascade, including reload period one");
        e.compare();

        e.reset();
        e.write(ci, 0, 0, 0x00c0fffau, 32);
        e.advance(3);
        e.write(ci, 0, 0, 0xfffeu);
        e.expect(e.actual.read16(id, 0x04000100) == 0xfffdu,
                 "an enabled reload write does not replace the live count");
        e.advance(2);
        e.compare();
        e.advance(1);
        e.expect(e.actual.read16(id, 0x04000100) == 0xfffeu,
                 "the next overflow uses the newly written reload value");
        e.compare();

        e.reset();
        e.write(ci, 0, 0, 0x00c00000u, 32);
        e.advance(65535);
        e.compare();
        e.advance(1); // Full 65536-count reload period.
        e.compare();

        e.reset();
        for (unsigned ti = 0; ti < 4; ++ti)
            e.write(ci, ti, 0, 0x00c4ffffu, 32);
        e.advance(4097);
        e.expect(e.impl().timers_due_tick == detail::kNoTimerDue &&
                 e.reference.flags[ci] == 0,
                 "timer 0 bit 2 freezes the whole enabled cascade chain");
        e.compare();
        e.write(ci, 0, 2, 0xc0u);
        e.advance(513);
        e.compare(); // Multiple overflows propagate through all four timers.
        e.clear_flags(ci, 0, 0xffffffffu, 32);
        e.write(ci, 1, 2, 0x44u);
        e.advance(129);
        e.expect(e.reference.flags[ci] == (1u << 3),
                 "a disabled middle timer stops cascading into later enabled timers");
        e.compare();
        e.write(ci, 1, 2, 0xc4u);
        e.advance(1);
        e.compare();

        e.reset();
        e.write(ci, 1, 0, 0x00c3ffffu, 32);
        e.advance(511);
        e.write(ci, 1, 2, 0xc4u);
        e.write(ci, 0, 0, 0x00c0ffffu, 32);
        e.advance(17);
        e.compare(); // Cascading does not consume the timer's old clocked phase.
        e.write(ci, 1, 2, 0xc0u);
        e.clear_flags(ci, 0, 0xffu, 16);
        e.compare();
        e.advance(1);
        e.compare(); // Returning to clocked mode consumes retained phase once.
    }
    e.reset();
    e.advance(4097);
    e.compare(); // No stale pre-reset deadline or IRQ survives.
}

void test_timer_scalar_equivalence() {
    constexpr std::array<std::uint32_t, 4> seeds = {
        0x9d15a7cu, 0x7a1e0001u, 0xdeadbeefu, 0x31415926u,
    };
    constexpr std::array<std::uint16_t, 9> reloads = {
        0, 1, 0x1234, 0x8000, 0xff00, 0xfffc, 0xfffd, 0xfffe, 0xffff,
    };
    constexpr std::array<std::uint32_t, 16> intervals = {
        0, 1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1023, 1024, 1025, 4097,
    };
    for (const auto seed : seeds) {
        for (const bool split : {false, true}) {
            TimerEquivalence e(seed, split);
            std::mt19937 random(seed);
            std::mt19937 partitions(seed ^ 0x9e3779b9u);
            const auto next = [&] { return static_cast<std::uint32_t>(random()); };
            const auto advance = [&](std::uint64_t ticks) {
                if (!split) {
                    e.advance(ticks);
                    return;
                }
                e.advance(0);
                while (ticks != 0 && e.healthy) {
                    const auto step = std::min<std::uint64_t>(ticks, 1u + partitions() % 211u);
                    e.advance(step);
                    ticks -= step;
                }
            };
            for (unsigned ci = 0; ci < 2; ++ci) {
                for (unsigned ti = 0; ti < 4; ++ti) {
                    const auto control = 0xc0u | (ti & 1u ? 4u : ci + ti);
                    e.write(ci, ti, 0, (control << 16) | reloads[(ci * 4 + ti) % reloads.size()], 32);
                }
            }
            for (unsigned iteration = 0; iteration < 768 && e.healthy; ++iteration) {
                advance(intervals[next() % intervals.size()]);
                const unsigned ci = next() & 1u;
                const unsigned ti = next() & 3u;
                const auto reload = (next() & 3u) == 0
                    ? static_cast<std::uint16_t>(next()) : reloads[next() % reloads.size()];
                const auto control = next();
                const auto old_control = e.reference.timers[ci * 4 + ti].control;
                switch (next() % 16u) {
                case 0: e.write(ci, ti, 0, reload & 0xffu, 8); break;
                case 1: e.write(ci, ti, 1, reload >> 8, 8); break;
                case 2: e.write(ci, ti, 0, reload); break;
                case 3: e.write(ci, ti, 2, control, 8); break;
                case 4: e.write(ci, ti, 3, control, 8); break;
                case 5: e.write(ci, ti, 2, control); break;
                case 6: e.write(ci, ti, 0, (control << 16) | reload, 32); break;
                case 7: e.write(ci, ti, 2, old_control ^ 0x80u); break;
                case 8: e.write(ci, ti, 2, old_control ^ 4u); break;
                case 9: e.write(ci, ti, 2, (old_control & ~3u) | (next() & 3u)); break;
                case 10: {
                    const unsigned width = next() % 3u;
                    const unsigned sub = next() & 3u;
                    e.clear_flags(ci, sub, next(), width == 0 ? 8 : width == 1 ? 16 : 32);
                    break;
                }
                case 11:
                    e.read(ci, ti, next() & 3u, 8);
                    break;
                case 12:
                    // Public bus alignment plus combined reload/control writes.
                    e.write(ci, ti, next() & 3u, (control << 16) | reload, 32);
                    break;
                case 13: advance(next() % 8193u); break;
                case 14:
                    for (unsigned burst = 0; burst < 3; ++burst)
                        advance(1u + next() % 1025u);
                    break;
                default:
                    if ((next() & 15u) == 0) e.reset();
                    else e.write(ci, ti, 3, old_control | 0x80u, 16);
                    break;
                }
                // Leave multiple scheduler intervals unobserved between
                // checks; every write and individual read still must sync.
                if (iteration % 11u == 0) e.compare();
            }
            e.compare();
        }
    }
}

struct TimerBoundarySample {
    std::uint64_t tick = 0;
    std::array<std::uint32_t, 2> flags{};
    std::array<std::uint32_t, 8> timers{};
};
std::array<TimerBoundarySample, 256> timer_boundary_samples{};
unsigned timer_boundary_sample_count = 0;

void timer_boundary_guest(Cpu* cpu) {
    for (;;) {
        if (timer_boundary_sample_count < timer_boundary_samples.size()) {
            auto& sample = timer_boundary_samples[timer_boundary_sample_count++];
            sample.tick = cpu->m->ticks();
            for (unsigned ci = 0; ci < 2; ++ci) {
                const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
                sample.flags[ci] = cpu->m->read32(id, 0x04000214);
                for (unsigned ti = 0; ti < 4; ++ti)
                    sample.timers[ci * 4 + ti] = cpu->m->read32(id, 0x04000100u + ti * 4);
                cpu->m->write32(id, 0x04000214, 0x78u);
            }
        }
        cpu->budget = 0;
        dsr_checkpoint(cpu, 0x02005000);
    }
}

void test_timer_scheduler_boundaries() {
    GuestFn slot = timer_boundary_guest;
    FnEntry entries[] = {
        {0x02005000, 4, kModuleArm9Main, 0, 0, &slot, "timer_boundary_guest"},
    };
    MachineConfig config = TimerEquivalence::config();
    config.arm9_table = {entries, 1};
    Machine machine(config);
    ScalarTimerReference reference;
    for (unsigned ci = 0; ci < 2; ++ci) {
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        for (unsigned ti = 0; ti < 4; ++ti) {
            const auto reload = ti == 0 ? (ci == 0 ? 0xff7fu : 0xffbdu)
                : ti == 1 ? 0xfffeu : 0xffffu;
            const auto control = ti == 0 ? 0xc0u : ti == 2 ? 0xc1u : 0xc4u;
            const auto value = (control << 16) | reload;
            reference.write(ci, ti, 0, value, 32);
            machine.write32(id, 0x04000100u + ti * 4, value);
        }
    }
    timer_boundary_sample_count = 0;
    machine.start_cpu(CpuId::Arm9, 0x02005000);
    constexpr std::uint64_t duration = 2 * detail::kLineTicks + 75;
    machine.run_ticks(duration);

    unsigned expected_samples = 0;
    while (reference.ticks < duration) {
        if (expected_samples < timer_boundary_sample_count) {
            const auto& sample = timer_boundary_samples[expected_samples];
            check(sample.tick == reference.ticks,
                  "lazy timer service preserves exact IRQ, HBlank and line boundaries");
            check(sample.flags == reference.flags,
                  "timer IRQs and cascades become visible at the exact upstream event boundary");
            for (unsigned ci = 0; ci < 2; ++ci) {
                for (unsigned ti = 0; ti < 4; ++ti)
                    check(sample.timers[ci * 4 + ti] == reference.word(ci, ti),
                          "scheduler-boundary timer observations match the scalar");
            }
        }
        ++expected_samples;
        reference.flags = {}; // The guest acknowledges after each observation.
        const auto line_tick = reference.ticks % detail::kLineTicks;
        const auto boundary = line_tick < detail::kHBlankTick
            ? detail::kHBlankTick - line_tick : detail::kLineTicks - line_tick;
        const auto limit = std::min<std::uint64_t>({duration - reference.ticks, boundary, 128});
        const auto step = reference.irq_boundary(limit);
        reference.advance(step);
    }
    check(timer_boundary_sample_count == expected_samples,
          "timer service preserves the exact number of CPU slices");
    check(machine.ticks() == duration, "timer deadlines do not change the requested interval");
    for (unsigned ci = 0; ci < 2; ++ci) {
        const CpuId id = ci == 0 ? CpuId::Arm9 : CpuId::Arm7;
        check(machine.read32(id, 0x04000214) == reference.flags[ci],
              "the final unobserved timer step raises the same IF bits");
        for (unsigned ti = 0; ti < 4; ++ti)
            check(machine.read32(id, 0x04000100u + ti * 4) == reference.word(ci, ti),
                  "the final timer counts and controls match after boundary sampling");
    }
}

void test_timers_dma_and_ipc() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    machine.write16(CpuId::Arm9, 0x04000100, 0xffff);
    machine.write16(CpuId::Arm9, 0x04000102, 0x00c0); // enable + overflow IRQ, prescale 1
    machine.run_ticks(1);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 3)) != 0,
          "timer 0 overflow raises its IRQ flag");

    machine.write32(CpuId::Arm9, 0x02000000, 0xdecafbad);

    machine.write32(CpuId::Arm9, 0x040000d4, 0x02000000); // DMA3 source
    machine.write32(CpuId::Arm9, 0x040000d8, 0x02001000); // DMA3 destination
    machine.write32(CpuId::Arm9, 0x040000dc, (0xc400u << 16) | 1u); // immediate, 32-bit, IRQ
    check(machine.read32(CpuId::Arm9, 0x02001000) == 0xdecafbad,
          "immediate DMA copies one word");
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 11)) != 0,
          "DMA3 completion raises its IRQ flag");

    machine.write16(CpuId::Arm9, 0x04000184, 0x8000); // FIFO enable
    machine.write16(CpuId::Arm7, 0x04000184, 0x8000);
    machine.write32(CpuId::Arm9, 0x04000188, 0x1234abcd);
    check((machine.read16(CpuId::Arm7, 0x04000184) & 0x0100u) == 0,
          "IPC FIFO receiver is non-empty after a send");
    check(machine.read32(CpuId::Arm7, 0x04100000) == 0x1234abcd,
          "IPC FIFO delivers the word at IPCFIFORECV on the other CPU");
    check((machine.read16(CpuId::Arm7, 0x04000184) & 0x0100u) != 0,
          "IPC FIFO reports receive-empty after draining");

    machine.write16(CpuId::Arm9, 0x04000180, 0x0500); // ARM9 output = 5
    check((machine.read16(CpuId::Arm7, 0x04000180) & 0x000fu) == 5,
          "IPCSYNC output bits 8-11 appear in the remote input bits 0-3");
    check((machine.read16(CpuId::Arm9, 0x04000180) & 0x0f00u) == 0x0500,
          "IPCSYNC keeps the writer's own output bits");
}

void test_core_register_contracts() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    // MMU_IPCSync and registers.h: send=bit13, receive-enable=bit14.
    machine.write16(CpuId::Arm7, 0x04000180, 0x4000);
    machine.write16(CpuId::Arm9, 0x04000180, 0x2500);
    check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 16)) != 0,
          "IPCSYNC bit13 sends an IRQ to a bit14-enabled receiver");
    check(machine.read16(CpuId::Arm9, 0x04000180) == 0x0500 &&
          machine.read16(CpuId::Arm7, 0x04000180) == 0x4005,
          "IPCSYNC send strobe is not retained and receive enable is readable");
    machine.write32(CpuId::Arm7, 0x04000214, 1u << 16);
    machine.write16(CpuId::Arm7, 0x04000180, 0);
    machine.write16(CpuId::Arm9, 0x04000180, 0x2000);
    check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 16)) == 0,
          "IPCSYNC does not interrupt a receiver with bit14 disabled");

    machine.write16(CpuId::Arm9, 0x04000184, 0x8004);
    machine.write32(CpuId::Arm9, 0x04000188, 0x12345678);
    machine.write32(CpuId::Arm9, 0x04000214, 1u << 17);
    machine.write16(CpuId::Arm9, 0x04000184, 0x800c);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 17)) != 0,
          "clearing IPC send FIFO raises its already-enabled empty IRQ");

    // The two 16-KiB halves are selected oppositely for ARM9 and ARM7.
    machine.write8(CpuId::Arm9, 0x04000247, 0);
    machine.write8(CpuId::Arm9, 0x03000000, 0x11);
    machine.write8(CpuId::Arm9, 0x03004000, 0x22);
    machine.write8(CpuId::Arm9, 0x04000247, 1);
    check(machine.read8(CpuId::Arm9, 0x03000000) == 0x22 &&
          machine.read8(CpuId::Arm7, 0x03000000) == 0x11,
          "WRAM mode1 maps upper shared half to ARM9, lower to ARM7");
    machine.write8(CpuId::Arm9, 0x04000247, 2);
    check(machine.read8(CpuId::Arm9, 0x03000000) == 0x11 &&
          machine.read8(CpuId::Arm7, 0x03000000) == 0x22,
          "WRAM mode2 maps lower shared half to ARM9, upper to ARM7");
    machine.write8(CpuId::Arm7, 0x04000247, 3);
    check(machine.read8(CpuId::Arm9, 0x04000247) == 2,
          "ARM7 cannot program ARM9 WRAMCNT");
    machine.write8(CpuId::Arm7, 0x03800000, 0x33);
    check(machine.read8(CpuId::Arm7, 0x03810000) == 0x33 &&
          machine.read8(CpuId::Arm7, 0x03ff0000) == 0x33,
          "ARM7 private WRAM mirrors across the high 8-MiB region");
    machine.write8(CpuId::Arm9, 0x04000247, 0);
    check(machine.read8(CpuId::Arm7, 0x03000000) == 0x33,
          "ARM7 low WRAM window aliases private RAM when shared RAM belongs to ARM9");

    machine.write16(CpuId::Arm9, 0x04000004, 0x00a0); // compare=256, IRQ enabled
    check((machine.read16(CpuId::Arm9, 0x04000004) & 0x0080) != 0,
          "DISPSTAT retains VCOUNT compare bit8 in register bit7");
    machine.run_ticks(255u * detail::kLineTicks);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 2)) == 0,
          "VCOUNT compare256 does not alias compare0");
    machine.run_ticks(detail::kLineTicks);
    check((machine.read16(CpuId::Arm9, 0x04000004) & 4u) != 0 &&
          (machine.read32(CpuId::Arm9, 0x04000214) & (1u << 2)) != 0,
          "nine-bit VCOUNT comparison raises its IRQ at line256");

    InputState keys;
    keys.buttons = kBtnA;
    machine.set_input(keys);
    machine.write16(CpuId::Arm9, 0x04000132, 0x4003);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 12)) != 0,
          "KEYCNT bit14 enables OR-mode keypad IRQ for any selected key");
    machine.write32(CpuId::Arm9, 0x04000214, 1u << 12);
    machine.write16(CpuId::Arm9, 0x04000132, 0xc003);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 12)) == 0,
          "KEYCNT bit15 selects AND-mode rather than IRQ enable");
    keys.buttons |= kBtnB;
    machine.set_input(keys);
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 12)) != 0,
          "AND-mode keypad IRQ asserts when every selected key is pressed");
}

void test_rtc_core_io_contract() {
    MachineConfig config;
    config.direct_boot = false;
    config.deterministic_rtc = true;
    config.rtc_epoch_seconds = 1704067200; // Monday, 2024-01-01 00:00:00 UTC.
    Machine machine(config);
    const auto gpio = [&](std::uint8_t value) {
        machine.write8(CpuId::Arm7, 0x04000138, value);
    };
    const auto send = [&](std::uint8_t value) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto low = static_cast<std::uint8_t>(0x74u | ((value >> bit) & 1u));
            gpio(low);
            gpio(low | 2u);
        }
    };
    const auto receive = [&] {
        std::uint8_t value = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            gpio(0x64);
            value |= (machine.read8(CpuId::Arm7, 0x04000138) & 1u) << bit;
            gpio(0x66);
        }
        return value;
    };
    const auto begin = [&](std::uint8_t command) {
        gpio(0x72);
        gpio(0x76);
        send(command);
    };
    begin(0xa6); // Date/time read.
    std::array<std::uint8_t, 7> date{};
    for (auto& byte : date) byte = receive();
    gpio(0x62);
    check(date[3] == 1, "RTC weekday agrees with oracle Sunday=0, Monday=1 convention");
    begin(0x46); // Status2 write, mirrored command order.
    send(0x04);
    gpio(0x62);
    begin(0xc6); // Status2 read, mirrored command order.
    check(receive() == 0x04, "RTC status2 write completes after one parameter byte");
    gpio(0x62);
}

void test_gx_fifo_dma_starts_on_empty_fifo() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    constexpr std::uint32_t kWords = 300; // more than two 112-word bursts
    for (std::uint32_t i = 0; i < kWords; ++i) {
        machine.write32(CpuId::Arm9, 0x02000000 + i * 4, 0); // packed NOP commands
    }
    machine.write32(CpuId::Arm9, 0x040000b0, 0x02000000); // DMA0 source
    machine.write32(CpuId::Arm9, 0x040000b4, 0x04000400); // GXFIFO
    // Enable, 32-bit, start mode 7 (geometry FIFO), with an empty FIFO.
    machine.write32(CpuId::Arm9, 0x040000b8, (0xbc00u << 16) | kWords);
    check((machine.read32(CpuId::Arm9, 0x04000600) & (1u << 25)) == 0,
          "GX DMA fills an empty FIFO past half on enable");
    machine.write32(CpuId::Arm9, 0x04000600, 1u << 30); // IRQ when FIFO drops below half
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 21)) == 0,
          "GX FIFO IRQ stays inactive while the queue is at least half full");
    machine.run_ticks(2'000'000);
    check((machine.read16(CpuId::Arm9, 0x040000ba) & 0x8000u) == 0,
          "GX DMA delivers every word as the FIFO drains below half");
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 21)) != 0,
          "GX FIFO IRQ asserts when draining crosses below half");
}

void test_gx_fifo_irq_is_level_triggered() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    constexpr std::uint32_t kGxFifoIrq = 1u << 21;

    machine.write32(CpuId::Arm9, 0x04000600, 2u << 30); // IRQ when FIFO is empty
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) != 0,
          "GX FIFO IRQ asserts while the empty-FIFO condition is active");
    machine.write32(CpuId::Arm9, 0x04000214, kGxFifoIrq); // acknowledge IF
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) != 0,
          "empty-FIFO GX IRQ relatches when IF is acknowledged");
    machine.write32(CpuId::Arm9, 0x04000600, 1u << 30); // switch to IRQ when below half
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) != 0,
          "GX FIFO IRQ remains asserted across an active-condition mode change");
    machine.write32(CpuId::Arm9, 0x04000214, kGxFifoIrq);
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) != 0,
          "below-half GX IRQ relatches when IF is acknowledged while its condition remains active");

    machine.write32(CpuId::Arm9, 0x04000600, 0); // disable GX FIFO IRQ
    machine.write32(CpuId::Arm9, 0x04000214, kGxFifoIrq);
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) == 0,
          "disabled GX FIFO IRQ remains cleared after IF acknowledgement");
    machine.write32(CpuId::Arm9, 0x04000600, 3u << 30); // reserved IRQ mode
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) == 0,
          "reserved GX FIFO IRQ mode does not request an interrupt");

    machine.write32(CpuId::Arm9, 0x04000600, 0);
    for (unsigned i = 0; i < 128; ++i)
        machine.write32(CpuId::Arm9, 0x04000400, 0); // queue NOPs without advancing the GPU
    machine.write32(CpuId::Arm9, 0x04000600, 1u << 30);
    check((machine.read32(CpuId::Arm9, 0x04000214) & kGxFifoIrq) == 0,
          "GX FIFO IRQ stays inactive at the half-full threshold");
}

void test_slot1_read_ready_latency() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path rom_path =
        std::filesystem::temp_directory_path() / ("dsr-core-slot1-" + std::to_string(stamp) + ".nds");
    std::filesystem::path save_path = rom_path;
    save_path.replace_extension(".sav");
    {
        std::ofstream output(rom_path, std::ios::binary);
        std::array<std::uint8_t, 0x20000> rom{};
        rom[0x15e] = 0x34;
        rom[0x15f] = 0x12;
        for (unsigned i = 0; i < 512; ++i)
            rom[0x8000u + i] = static_cast<std::uint8_t>(i * 13u + 7u);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
        check(output.good(), "slot-1 timing ROM fixture is written");
    }
    struct DelayCase {
        std::uint32_t gap;
        bool fast_clock;
    };
    constexpr std::array<DelayCase, 4> cases{{
        {0u, false},
        {0x1fffu, false},
        {0u, true},
        {0x1fffu, true},
    }};
    constexpr std::uint32_t kDataReady = 1u << 23;
    for (const DelayCase scenario : cases) {
        MachineConfig config;
        config.rom_path = rom_path;
        config.save_path = save_path;
        config.direct_boot = false;
        Machine machine(config);
        machine.write32(CpuId::Arm9, 0x04000204, 1u << 11); // give slot-1 to ARM7
        machine.write16(CpuId::Arm7, 0x040001a0, 0xc000); // ROM transfer IRQ
        const std::array<std::uint8_t, 8> command = {0xb7, 0, 0, 0x80, 0, 0, 0, 0};
        for (std::size_t i = 0; i < command.size(); ++i)
            machine.write8(CpuId::Arm7, 0x040001a8u + i, command[i]);
        const std::uint32_t clocks = scenario.fast_clock ? 8u : 5u;
        const std::uint32_t clock_select = scenario.fast_clock ? 1u << 27 : 0u;
        const std::uint32_t rom_control = 0x81000000u | clock_select | scenario.gap;
        machine.write32(CpuId::Arm7, 0x040001a4, rom_control); // 512-byte block

        // NDS_RescheduleReadSlot1 doubles the 33-MHz transfer clocks because
        // DeSmuME's nds_timer is 66 MHz. Machine ticks use the same 33-MHz
        // unit as GameCard::run, so the equivalent delay is (8+gap)*clocks+4.
        const std::uint64_t native_delay =
            (8u + scenario.gap) * clocks + 4u;
        machine.run_ticks(native_delay - 1);
        check((machine.read32(CpuId::Arm7, 0x040001a4) & kDataReady) == 0,
              "slot-1 data-ready remains clear before the Oracle ROMCTRL delay");
        machine.run_ticks(1);
        check((machine.read32(CpuId::Arm7, 0x040001a4) & kDataReady) != 0,
              "slot-1 data-ready asserts at the Oracle ROMCTRL delay");
        for (unsigned word = 0; word < 128; ++word) {
            check((machine.read32(CpuId::Arm7, 0x040001a4) & 0x80800000u) == 0x80800000u,
                  "polled card holds READY and BUSY until its last word");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) == 0,
                  "polled card transfer IRQ waits for its last word");
            std::uint32_t expected = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                expected |= (((word * 4u + byte) * 13u + 7u) & 0xffu) << (byte * 8);
            check(machine.read32(CpuId::Arm7, 0x04100010) == expected,
                  "successive distinct card words are readable without advancing time");
        }
        check((machine.read32(CpuId::Arm7, 0x040001a4) & 0x80800000u) == 0,
              "last polled word clears card READY and BUSY");
        check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) != 0,
              "last polled word raises the card transfer IRQ");
        machine.write32(CpuId::Arm7, 0x04000214, 1u << 19);
        machine.run_ticks(native_delay);
        check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) == 0,
              "acknowledged card completion IRQ does not repeat");
    }

    MachineConfig config;
    config.rom_path = rom_path;
    config.save_path = save_path;
    config.direct_boot = false;
    Machine machine(config);
    machine.write32(CpuId::Arm9, 0x04000204, 1u << 11);
    const std::array<std::uint8_t, 8> no_data_command = {0x3c, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < no_data_command.size(); ++i)
        machine.write8(CpuId::Arm7, 0x040001a8u + i, no_data_command[i]);
    machine.write32(CpuId::Arm7, 0x040001a4, 0x80001fffu);
    check((machine.read32(CpuId::Arm7, 0x040001a4) & (1u << 31)) == 0,
          "zero-length slot-1 commands complete without a data-ready delay");
    config.direct_boot = true;
    Machine booted(config);
    check(booted.read32(CpuId::Arm9, 0x027ff804) ==
          booted.read32(CpuId::Arm9, 0x027ff800),
          "direct boot publishes the secure chip ID at shared header offset4");
    check(booted.read16(CpuId::Arm9, 0x027ff808) == 0x1234,
          "direct boot publishes the ROM header CRC rather than boot flag1");
}

void test_dma_latches_and_timing() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path rom_path =
        std::filesystem::temp_directory_path() / ("dsr-core-dma-" + std::to_string(stamp) + ".nds");
    std::filesystem::path save_path = rom_path;
    save_path.replace_extension(".sav");
    std::vector<std::uint8_t> rom(0x20000, 0);
    for (std::size_t i = 0; i < rom.size(); ++i)
        rom[i] = static_cast<std::uint8_t>((i * 13u + 7u) & 0xffu);
    for (unsigned i = 0; i < 16; ++i)
        rom[0x8000u + i] = static_cast<std::uint8_t>(0x30u + i);
    {
        std::ofstream output(rom_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
        check(output.good(), "DMA card-test ROM fixture is written");
        output.close();

        MachineConfig config;
        config.rom_path = rom_path;
        config.save_path = save_path;
        config.direct_boot = false;
        Machine machine(config);

        constexpr std::uint32_t card_data = 0x04100010;
        constexpr std::uint32_t arm7_dst = 0x02001000;
        constexpr std::uint32_t arm9_nonowner_dst = 0x02002000;
        constexpr std::uint32_t sentinel = 0xa5a5a5a5;
        const auto word_at = [&rom](unsigned index) {
            const std::size_t offset = 0x8000u + index * 4u;
            return static_cast<std::uint32_t>(rom[offset]) |
                   (static_cast<std::uint32_t>(rom[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(rom[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(rom[offset + 3]) << 24);
        };

        machine.write32(CpuId::Arm9, 0x04000204, 1u << 11); // give slot-1 to ARM7
        machine.write16(CpuId::Arm7, 0x040001a0, 0xc000); // ROM transfer IRQ
        machine.write32(CpuId::Arm7, arm7_dst - 4, sentinel);
        machine.write32(CpuId::Arm7, 0x02001000, sentinel);
        machine.write32(CpuId::Arm7, 0x02001004, sentinel);
        machine.write32(CpuId::Arm7, 0x02001008, sentinel);
        machine.write32(CpuId::Arm9, arm9_nonowner_dst, sentinel);

        // A non-owner ARM9 channel must not consume a ready callback; ARM7
        // channel 2's mode 2 is the cartridge event (not HBlank).
        machine.write32(CpuId::Arm9, 0x040000b0, card_data);
        machine.write32(CpuId::Arm9, 0x040000b4, arm9_nonowner_dst);
        machine.write32(CpuId::Arm9, 0x040000b8, (0xac00u << 16) | 1u);
        machine.write32(CpuId::Arm7, 0x040000c8, card_data);
        machine.write32(CpuId::Arm7, 0x040000cc, arm7_dst);
        machine.write32(CpuId::Arm7, 0x040000d0, (0xe760u << 16) | 2u); // fixed card source, repeat/reload + IRQ
        machine.write16(CpuId::Arm7, 0x040000d0, 1u); // count is already latched as 2

        for (unsigned block = 0; block < 2; ++block) {
            const std::array<std::uint8_t, 8> command = {
                0xb7, 0, 0, 0x80, static_cast<std::uint8_t>(block * 8u), 0, 0, 0};
            for (std::size_t i = 0; i < command.size(); ++i)
                machine.write8(CpuId::Arm7, 0x040001a8u + i, command[i]);
            machine.write32(CpuId::Arm7, 0x040001a4, 0x81000000u); // 512-byte block

            machine.run_ticks(43);
            check(machine.read32(CpuId::Arm7, arm7_dst) ==
                      (block == 0 ? sentinel : word_at(0)),
                  "card DMA waits until the initial READY delay");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 10)) == 0,
                  "card DMA IRQ is clear before the readiness callback");
            machine.run_ticks(1);
            check(machine.read32(CpuId::Arm7, arm7_dst) == word_at(block * 2),
                  "card DMA starts each repeat at the reloaded destination");
            check(machine.read32(CpuId::Arm7, arm7_dst + 4) == word_at(block * 2 + 1),
                  "one READY callback transfers both distinct programmed DMA words");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 10)) != 0,
                  "one READY callback completes the latched DMA count and raises IRQ");
            check(machine.read32(CpuId::Arm9, arm9_nonowner_dst) == sentinel,
                  "card DMA readiness is routed only to the slot owner");
            check(machine.read32(CpuId::Arm7, 0x040000d0) == ((0xe760u << 16) | 1u),
                  "repeat DMA remains enabled with separate programmed count readback");
            check((machine.read32(CpuId::Arm7, 0x040001a4) & 0x80800000u) == 0x80800000u,
                  "DMA stops at its own count while the longer card block holds READY");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) == 0,
                  "short DMA completion does not complete the longer card command");
            machine.write32(CpuId::Arm7, 0x04000214, 1u << 10);
            machine.run_ticks(100);
            check(machine.read32(CpuId::Arm7, arm7_dst) == word_at(block * 2) &&
                      machine.read32(CpuId::Arm7, arm7_dst + 4) == word_at(block * 2 + 1),
                  "rearmed DMA cannot recursively drain a second programmed block");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 10)) == 0,
                  "DMA completion IRQ does not repeat without a new callback");
            for (unsigned word = 2; word < 128; ++word)
                check(machine.read32(CpuId::Arm7, card_data) == word_at(block * 2 + word),
                      "polling consumes remaining card data without another DMA block");
            check((machine.read32(CpuId::Arm7, 0x040001a4) & 0x80800000u) == 0,
                  "exhausting the card block clears READY and BUSY");
            check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) != 0,
                  "card IRQ follows card exhaustion, not short DMA completion");
            machine.write32(CpuId::Arm7, 0x04000214, 1u << 19);
            machine.run_ticks(100);
            check((machine.read32(CpuId::Arm7, 0x04000214) & ((1u << 10) | (1u << 19))) == 0,
                  "acknowledged card and DMA IRQs do not repeat");
        }
        check(machine.read32(CpuId::Arm7, 0x040000c8) == card_data &&
                  machine.read32(CpuId::Arm7, 0x040000cc) == arm7_dst,
              "DMA source and destination registers retain their programmed readback values");
        check(machine.read32(CpuId::Arm7, arm7_dst + 8) == sentinel,
              "destination reload keeps repeated cartridge blocks within the programmed range");
        check(machine.read32(CpuId::Arm7, arm7_dst - 4) == sentinel,
              "repeated card DMA preserves the preceding destination guard");

        // Card exhaustion must not synthesize the second word of a DMA
        // programmed for two words or signal its completion prematurely.
        machine.write16(CpuId::Arm7, 0x040000d2, 0);
        machine.write32(CpuId::Arm7, arm7_dst, sentinel);
        machine.write32(CpuId::Arm7, arm7_dst + 4, sentinel);
        machine.write32(CpuId::Arm7, 0x040000d0, (0xe500u << 16) | 2u);
        const std::array<std::uint8_t, 8> short_command = {0xb7, 0, 0, 0x80, 0, 0, 0, 0};
        for (std::size_t i = 0; i < short_command.size(); ++i)
            machine.write8(CpuId::Arm7, 0x040001a8u + i, short_command[i]);
        machine.write32(CpuId::Arm7, 0x040001a4, 0x87000000u);
        machine.run_ticks(44);
        check(machine.read32(CpuId::Arm7, arm7_dst) == word_at(0),
              "incomplete DMA transfers the one word the card supplies");
        check(machine.read32(CpuId::Arm7, arm7_dst + 4) == sentinel &&
                  machine.read32(CpuId::Arm7, arm7_dst + 8) == sentinel &&
                  machine.read32(CpuId::Arm7, arm7_dst - 4) == sentinel,
              "card exhaustion leaves unwritten DMA words and destination guards intact");
        check((machine.read32(CpuId::Arm7, 0x040001a4) & 0x80800000u) == 0,
              "short card exhaustion clears READY and BUSY");
        check((machine.read32(CpuId::Arm7, 0x04000214) & (1u << 19)) != 0 &&
                  (machine.read32(CpuId::Arm7, 0x04000214) & (1u << 10)) == 0,
              "card exhaustion raises only the card IRQ while DMA remains incomplete");
        check(machine.read32(CpuId::Arm7, 0x040000d0) == ((0xe500u << 16) | 2u) &&
                  machine.read32(CpuId::Arm7, 0x040000c8) == card_data &&
                  machine.read32(CpuId::Arm7, 0x040000cc) == arm7_dst,
              "incomplete DMA retains enable and programmed register readback");
        machine.write32(CpuId::Arm7, 0x04000214, 1u << 19);
        machine.run_ticks(100);
        check(machine.read32(CpuId::Arm7, arm7_dst + 4) == sentinel &&
                  (machine.read32(CpuId::Arm7, 0x04000214) & ((1u << 10) | (1u << 19))) == 0,
              "exhausted card cannot resume DMA or repeat completion IRQs");
        check(machine.read32(CpuId::Arm9, arm9_nonowner_dst) == sentinel,
              "short card callback also leaves the non-owner DMA untouched");

        machine.write32(CpuId::Arm9, 0x040000c4, 0x00054321u);
        check(machine.read32(CpuId::Arm9, 0x040000c4) == 0x00054321u,
              "ARM9 DMA count readback retains all 21 count bits");
        machine.write16(CpuId::Arm9, 0x040000c4, 0xbeefu);
        check(machine.read32(CpuId::Arm9, 0x040000c4) == 0x0005beefu,
              "partial ARM9 count writes preserve the upper five count bits");
        machine.write8(CpuId::Arm9, 0x040000c7, 0x12u);
        check(machine.read32(CpuId::Arm9, 0x040000c4) == 0x1205beefu,
              "partial ARM9 control writes preserve upper count bits in readback");

        machine.write16(CpuId::Arm7, 0x040000b8, 0xffffu);
        machine.write16(CpuId::Arm7, 0x040000dc, 0xffffu);
        check(machine.read16(CpuId::Arm7, 0x040000b8) == 0x3fffu,
              "ARM7 DMA channels 0-2 mask count registers to 14 bits");
        check(machine.read16(CpuId::Arm7, 0x040000dc) == 0xffffu,
              "ARM7 DMA channel 3 retains its 16-bit count register");
    }
    std::error_code error;
    std::filesystem::remove(rom_path, error);
    std::filesystem::remove(save_path, error);
}

void test_video_timing() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    check(machine.read16(CpuId::Arm9, 0x04000006) == 0 &&
          machine.read16(CpuId::Arm7, 0x04000006) == 0,
          "both CPUs observe initial HStart0 before the first guest instruction");
    check((machine.read16(CpuId::Arm9, 0x04000004) & 4u) != 0,
          "initial HStart evaluates the default VCOUNT0 comparison");
    machine.write16(CpuId::Arm9, 0x04000004, 1u << 4);
    machine.write32(CpuId::Arm9, 0x04000208, 1);
    machine.write32(CpuId::Arm9, 0x04000210, (1u << 1) | (1u << 2));
    machine.run_ticks(1606);
    check((machine.read16(CpuId::Arm9, 0x04000004) & (1u << 1)) != 0,
          "HBlank status asserts at the programmed line boundary");
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 1)) != 0,
          "enabled HBlank interrupt raises IF");
    machine.write16(CpuId::Arm9, 0x04000004, (1u << 8) | (1u << 5));
    machine.run_ticks(524);
    check((machine.read16(CpuId::Arm9, 0x04000004) & (1u << 2)) != 0,
          "VCOUNT match status updates at the next scanline");
    check((machine.read32(CpuId::Arm9, 0x04000214) & (1u << 2)) != 0,
          "enabled VCOUNT match interrupt raises IF");
}
void test_frame_boundary_services_key_poll() {
    GuestFn main_slot = frame_key_waiter;
    GuestFn irq_slot = frame_key_irq_entry;
    FnEntry entries[] = {
        {0x02001000, 4, kModuleArm9Main, 0, 0, &main_slot, "frame_key_waiter"},
        {0x02002000, 4, kModuleArm9Main, 0, 0, &irq_slot, "frame_key_irq"},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries, 2};
    Machine machine(config);
    Cpu& cpu = machine.cpu(CpuId::Arm9);
    cpu.cpsr_ctrl = kModeIrq;
    cpu.r[13] = 0x00803fa0;
    machine.write32(CpuId::Arm9, 0x00803ffc, 0x02002000);
    machine.write32(CpuId::Arm9, 0x04000208, 1); // IME
    machine.write32(CpuId::Arm9, 0x04000210, 1); // VBlank IRQ
    machine.write16(CpuId::Arm9, 0x04000004, 1u << 3); // VBlank IRQ enable
    frame_key_samples.fill(0xffffu);
    frame_key_sample_count = 0;
    InputState pressed;
    pressed.buttons = kBtnA;
    machine.set_input(pressed);
    machine.start_cpu(CpuId::Arm9, 0x02001000);

    machine.run_frame();
    check(frame_key_sample_count == 1,
          "run_frame services the current frame's VBlank key poll");
    check(frame_key_samples[0] == 0x03feu,
          "VBlank key poll sees an A press in the same frame");
    check(machine.ticks() == (detail::kLinesPerFrame - 1) * detail::kLineTicks,
          "first run_frame advances from initial HStart0 to HStart262");
    check((machine.read32(CpuId::Arm9, 0x04000004) >> 16) ==
              detail::kLinesPerFrame - 1,
          "run_frame ends at the oracle's VCOUNT=262 boundary");
    check((machine.read16(CpuId::Arm9, 0x04000004) & 1u) == 0,
          "VBlank status clears at the frame boundary");

    machine.set_input(InputState{});
    machine.run_frame();
    check(frame_key_sample_count == 2,
          "the next run_frame services the next VBlank key poll");
    check(frame_key_samples[1] == 0x03ffu,
          "VBlank key poll sees a release in the same frame");
    check(machine.ticks() == (2 * detail::kLinesPerFrame - 1) * detail::kLineTicks,
          "subsequent run_frame advances a full 263-line frame");
}


void test_extended_keypad_bits() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    for (unsigned state = 0; state < 16; ++state) {
        InputState input;
        if (state & 1u) input.buttons |= kBtnX;
        if (state & 2u) input.buttons |= kBtnY;
        input.pen_down = (state & 4u) != 0;
        input.lid_closed = (state & 8u) != 0;
        machine.set_input(input);
        const std::uint16_t expected = 0x3cu |
            ((state & 1u) ? 0u : 1u) |
            ((state & 2u) ? 0u : 2u) |
            ((state & 4u) ? 0u : 0x40u) |
            ((state & 8u) ? 0x80u : 0u);
        check(machine.read16(CpuId::Arm7, 0x04000136) == expected,
              "EXTKEYIN uses active-low X/Y/pen and active-high closed lid");
    }
    machine.set_input(InputState{});
    check(machine.read16(CpuId::Arm7, 0x04000136) == 0x007fu,
          "released inputs restore open-lid state after all transitions");
}

void test_math_and_fibers() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    machine.write32(CpuId::Arm9, 0x04000290, 100);
    machine.write32(CpuId::Arm9, 0x04000298, 9);
    check((machine.read16(CpuId::Arm9, 0x04000280) & (1u << 15)) != 0,
          "divider reports busy immediately after a parameter write");
    machine.run_ticks(18);
    check(machine.read32(CpuId::Arm9, 0x040002a0) == 11,
          "32-bit division completes after its hardware latency");
    check(machine.read32(CpuId::Arm9, 0x040002a8) == 1,
          "32-bit divider reports the completed remainder");
    check((machine.read16(CpuId::Arm9, 0x04000280) & (1u << 15)) == 0,
          "divider clears its busy bit when the result completes");
    machine.write32(CpuId::Arm9, 0x04000290, 3);
    machine.write32(CpuId::Arm9, 0x04000294, 1);
    machine.write32(CpuId::Arm9, 0x04000298, 3);
    machine.write32(CpuId::Arm9, 0x0400029c, 1);
    machine.write16(CpuId::Arm9, 0x04000280, 3);
    machine.run_ticks(34);
    check(machine.read32(CpuId::Arm9, 0x040002a0) == 0x55555556,
          "divider mode 3 uses a 64-bit numerator and signed 32-bit denominator");
    check(machine.read32(CpuId::Arm9, 0x040002a8) == 1,
          "divider mode 3 reports the 32-bit denominator remainder");
    machine.write32(CpuId::Arm9, 0x04000290, 5);
    machine.write32(CpuId::Arm9, 0x04000298, 0);
    machine.write32(CpuId::Arm9, 0x0400029c, 1);
    machine.write16(CpuId::Arm9, 0x04000280, 0);
    check((machine.read16(CpuId::Arm9, 0x04000280) & (1u << 14)) == 0,
          "divider zero flag tests the complete 64-bit denominator");
    machine.write32(CpuId::Arm9, 0x0400029c, 0);
    check((machine.read16(CpuId::Arm9, 0x04000280) & (1u << 14)) != 0,
          "divider zero flag asserts when all denominator bits are zero");
    machine.write32(CpuId::Arm9, 0x040002b8, 81);
    check((machine.read16(CpuId::Arm9, 0x040002b0) & (1u << 15)) != 0,
          "square root reports busy immediately after a parameter write");
    machine.run_ticks(13);
    check(machine.read32(CpuId::Arm9, 0x040002b4) == 9,
          "square root result becomes available after its hardware latency");

    std::vector<int> sequence;
    Fiber fiber([&] {
        sequence.push_back(1);
        Fiber::yield();
        sequence.push_back(2);
    });
    Fiber::switch_to(fiber);
    check(sequence == std::vector<int>{1}, "fiber yields to the host context");
    Fiber::switch_to(fiber);
    check(sequence == (std::vector<int>{1, 2}) && fiber.finished(),
          "fiber resumes and reports completion");
    Fiber::switch_to(fiber);
    check(sequence == (std::vector<int>{1, 2}),
          "switching to a completed fiber is a no-op");
    check(Fiber::current() == nullptr, "fiber completion restores the host identity");

    // Switching directly between guest continuations still yields to the host,
    // not to the most recent caller. This is the scheduler's thread-switch seam.
    sequence.clear();
    Fiber second([&] {
        sequence.push_back(3);
        Fiber::switch_to(*Fiber::current());
        Fiber::yield();
        sequence.push_back(4);
    });
    Fiber first([&] {
        sequence.push_back(1);
        Fiber::switch_to(second);
        sequence.push_back(2);
    });
    Fiber::switch_to(first);
    check(sequence == (std::vector<int>{1, 3}) && Fiber::current() == nullptr,
          "a nested fiber switch yields to the implicit host");
    Fiber::switch_to(second);
    Fiber::switch_to(first);
    check(sequence == (std::vector<int>{1, 3, 4, 2}) && first.finished() && second.finished(),
          "parked callers resume with their original continuation");

    const int original_rounding = std::fegetround();
    Fiber floating_point([&] {
        std::fesetround(FE_DOWNWARD);
        Fiber::yield();
        check(std::fegetround() == FE_DOWNWARD, "fiber resumes with its own floating-point control");
    });
    Fiber::switch_to(floating_point);
    check(std::fegetround() == original_rounding, "yield restores the host floating-point control");
    Fiber::switch_to(floating_point);
    check(std::fegetround() == original_rounding, "completion restores the host floating-point control");

    std::fesetround(FE_UPWARD);
    Fiber inherited_floating_point([&] {
        check(std::fegetround() == FE_UPWARD, "a new fiber inherits its creator's floating-point control");
        std::fesetround(FE_TOWARDZERO);
        Fiber::yield();
        check(std::fegetround() == FE_TOWARDZERO, "an inherited fiber retains its changed control on resume");
    });
    Fiber::switch_to(inherited_floating_point);
    check(std::fegetround() == FE_UPWARD, "yield restores a non-default host floating-point control");
    Fiber::switch_to(inherited_floating_point);
    check(std::fegetround() == FE_UPWARD, "completion restores a non-default host floating-point control");
    std::fesetround(original_rounding);

    bool rejected = false;
    std::thread wrong_thread([&] {
        try { Fiber::switch_to(first); }
        catch (const std::logic_error&) { rejected = true; }
    });
    wrong_thread.join();
    check(rejected, "fibers cannot migrate to another host thread");

    std::thread worker([&] {
        for (int session = 0; session < 2; ++session) {
            Fiber large_stack([&] {
                volatile std::uint8_t stack[192 * 1024]{};
                stack[0] = 1;
                stack[sizeof(stack) - 1] = 2;
                Fiber::yield();
                check(stack[0] == 1 && stack[sizeof(stack) - 1] == 2,
                      "the minimum host stack survives a yield");
            });
            Fiber::switch_to(large_stack);
            Fiber::switch_to(large_stack);
            check(large_stack.finished() && Fiber::current() == nullptr,
                  "successive worker sessions release and recreate their fiber host");
        }
    });
    worker.join();
}

void test_overlay_hash_dispatch() {
    constexpr std::array<std::uint8_t, 4> image = {0xd4, 0xc3, 0xb2, 0xa1};
    const std::uint64_t hash = fnv1a64(image.data(), image.size());
    GuestFn main_slot = overlay_main;
    GuestFn wrong_slot = overlay_a;
    GuestFn matching_slot = overlay_b;
    FnEntry entries[] = {
        {0x02004000, 4, kModuleArm9Main, 0, 0, &main_slot, "overlay_test_main"},
        {0x02003000, 4, 0, 0, hash ^ 1u, &wrong_slot, "overlay_0"},
        {0x02003000, 4, 1, 0, hash, &matching_slot, "overlay_1"},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries, 3};
    Machine machine(config);
    machine.write32(CpuId::Arm9, 0x02003000, 0xa1b2c3d4);
    overlay_choice = 0;
    call_overlay = true;
    machine.start_cpu(CpuId::Arm9, 0x02004000);
    machine.run_ticks(1);
    check(overlay_choice == 2, "ambiguous overlay dispatch selects matching image hash");
    check(machine.replace_function(CpuId::Arm9, 1, 0x02003000, hash, overlay_replacement),
          "native override accepts a matching guest function hash");
    dsr_call(&machine.cpu(CpuId::Arm9), 0x02003000);
    check(overlay_choice == 3, "override replaces the selected overlay slot");
    call_overlay = false;
}

const FnEntry* reference_overlay_entry(Cpu* cpu, const FnEntry* entries, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        if (!entry.slot || entry.size == 0) continue;
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (std::uint32_t j = 0; j < entry.size; ++j)
            hash = (hash ^ rd8(cpu, entry.addr + j)) * 0x100000001b3ull;
        if (hash == entry.hash) return &entry;
    }
    return nullptr;
}

void test_overlay_snapshot_dispatch() {
    // Halfword-aligned entry and odd-sized bodies straddle a 16 KiB page.
    constexpr std::uint32_t address = 0x02007fde;
    constexpr std::array<std::uint32_t, 8> sizes = {0, 257, 7, 19, 67, 131, 67, 17};
    std::array<std::array<std::uint8_t, 257>, sizes.size()> images{};
    std::mt19937 random(0x9d15a7cu);
    for (auto& image : images)
        for (auto& byte : image) byte = static_cast<std::uint8_t>(random());
    images[6] = images[4]; // Two matching candidates must still choose the first.
    // Candidate 1 is longer than candidate 2 but has the same short prefix.
    // Rejected bytes beyond the selected body's end must also be validated.
    for (std::size_t i = 0; i < sizes[2]; ++i) images[2][i] = images[1][i];
    images[2][200] = images[1][200] ^ 1u;
    GuestFn slot = overlay_a;
    std::array<FnEntry, sizes.size()> arm9{}, arm7{};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        arm9[i] = {address, sizes[i], static_cast<std::int32_t>(i), 0,
                   fnv1a64(images[i].data(), sizes[i]),
                   i == sizes.size() - 1 ? nullptr : &slot, "snapshot_test"};
        arm7[sizes.size() - 1 - i] = arm9[i];
    }
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {arm9.data(), arm9.size()};
    config.arm7_table = {arm7.data(), arm7.size()};
    Machine machine(config);
    Cpu& cpu9 = machine.cpu(CpuId::Arm9);
    Cpu& cpu7 = machine.cpu(CpuId::Arm7);
    auto& impl = detail::MachineAccess::impl(&cpu9);
    const auto compare = [&](Cpu& cpu, const auto& entries) {
        const FnEntry* expected = reference_overlay_entry(&cpu, entries.data(), entries.size());
        check(impl.find_entry(&cpu, address) == expected,
              "snapshot resolver equals original byte hash after an overlay write");
        check(impl.find_entry(&cpu, address | 1u) == expected,
              "unchanged snapshot preserves first match and target normalization");
    };
    const auto write_image = [&](const auto& image) {
        for (std::size_t i = 0; i < image.size(); ++i)
            wr8(&cpu7, address + 0x00800000u + static_cast<std::uint32_t>(i), image[i]);
    };
    write_image(images[2]);
    compare(cpu9, arm9);
    check(impl.find_entry(&cpu9, address) == &arm9[2],
          "short candidate selected after rejecting longer candidate");
    auto& cached = impl.dispatch_index.at(address);
    check(cached.snapshot_valid && cached.checked_size == 257,
          "snapshot includes rejected earlier body's full extent");
    // No CP15, DMA, card, or other invalidation signal accompanies this write.
    wr8(&cpu7, address + 200, images[1][200]);
    // Restore the rest of the earlier long body, leaving the selected prefix unchanged.
    for (std::uint32_t i = sizes[2]; i < sizes[1]; ++i)
        wr8(&cpu7, address + i, images[1][i]);
    compare(cpu9, arm9);
    check(impl.find_entry(&cpu9, address) == &arm9[1],
          "write beyond cached selected body can enable an earlier candidate");

    for (unsigned iteration = 0; iteration < 4096; ++iteration) {
        write_image(images[random() % images.size()]);
        if ((iteration & 3u) == 0) {
            const auto offset = random() % images[0].size();
            const auto a = address + static_cast<std::uint32_t>(offset);
            // Direct host writes (e.g. a cartridge upload) also need no hooks.
            cpu9.page_w[a >> kPageShift][a & kPageMask] ^= 1u;
        }
        if ((iteration & 7u) == 0) {
            wr16(&cpu9, address + random() % 254u, static_cast<std::uint16_t>(random()));
            wr32(&cpu7, address + random() % 252u, random());
        }
        if ((iteration & 31u) == 0)
            dsr_mcr(&cpu9, 15, 0, 7, 5, 0, 0, 0); // Optional, never relied upon.
        compare(cpu9, arm9);
        compare(cpu7, arm7); // Separate per-CPU ordering at the same guest address.
    }
    write_image(images[4]);
    compare(cpu9, arm9);
    check(impl.find_entry(&cpu9, address) == &arm9[4],
          "identical hashes retain original candidate order");

    const std::uint32_t page = (address + 34) >> kPageShift;
    auto* mapped = cpu9.page_r[page];
    cpu9.page_r[page] = nullptr;
    compare(cpu9, arm9);
    check(!cached.snapshot_valid, "partly slow-backed range does not cache reads");
    cpu9.page_r[page] = mapped;
    compare(cpu9, arm9);
    check(cached.snapshot_valid, "restored direct page mapping can cache again");
    impl.reset();
    compare(cpu9, arm9);
    write_image(images[6]);
    compare(cpu9, arm9);
}

void test_overlay_snapshot_remapping_and_io() {
    constexpr std::uint32_t address = 0x03000000;
    constexpr std::array<std::uint8_t, 4> a = {1, 2, 3, 4}, b = {5, 6, 7, 8};
    GuestFn slot = overlay_a;
    const FnEntry entries[] = {
        {address, 4, 0, 0, fnv1a64(a.data(), a.size()), &slot, "wram_a"},
        {address, 4, 1, 0, fnv1a64(b.data(), b.size()), &slot, "wram_b"},
        {0x04000130, 2, 0, 0, fnv1a64(a.data(), 2), &slot, "io_a"},
        {0x04000130, 2, 1, 0, fnv1a64(b.data(), 2), &slot, "io_b"},
    };
    MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries, 4};
    Machine machine(config);
    Cpu& cpu = machine.cpu(CpuId::Arm9);
    auto& impl = detail::MachineAccess::impl(&cpu);
    machine.write8(CpuId::Arm9, 0x04000247, 0);
    for (unsigned i = 0; i < 4; ++i) {
        wr8(&cpu, address + i, a[i]);
        wr8(&cpu, address + 0x4000 + i, b[i]);
    }
    check(impl.find_entry(&cpu, address) == &entries[0], "initial WRAM image resolves");
    machine.write8(CpuId::Arm9, 0x04000247, 1);
    check(impl.find_entry(&cpu, address) == &entries[1],
          "WRAM remapping observes different bytes at the same virtual address");
    machine.write8(CpuId::Arm9, 0x04000247, 2);
    check(impl.find_entry(&cpu, address) == &entries[0],
          "WRAM remapping back restores the original candidate");
    check(impl.find_entry(&cpu, 0x04000130) == nullptr, "unmatched IO-backed candidate is null");
    check(impl.find_entry(&cpu, 0x04000130) == nullptr &&
          !impl.dispatch_index.at(0x04000130).snapshot_valid,
          "IO-backed ranges never suppress their original reads");
}

void test_bios_memory_swis() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    Cpu& cpu = machine.cpu(CpuId::Arm9);
    constexpr std::uint32_t source = 0x02005000;
    constexpr std::uint32_t destination = 0x02005200;
    const std::uint8_t crc_data[] = {'1','2','3','4','5','6','7','8','9'};
    for (unsigned i = 0; i < sizeof(crc_data); ++i)
        machine.write8(CpuId::Arm9, source + i, crc_data[i]);
    cpu.r[0] = 0xffff;
    cpu.r[1] = source;
    cpu.r[2] = sizeof(crc_data) - 1;
    dsr_swi(&cpu, 0x000e0000, 0x02004000, false);
    check(cpu.r[0] == 0x37dd, "GetCRC16 matches CRC-16/ARC over complete halfwords");

    const std::uint8_t lz77[] = {0x10, 3, 0, 0, 0, 'A', 'B', 'C'};
    for (unsigned i = 0; i < sizeof(lz77); ++i)
        machine.write8(CpuId::Arm9, source + 0x100 + i, lz77[i]);
    cpu.r[0] = source + 0x100;
    cpu.r[1] = destination;
    dsr_swi(&cpu, 0x00110000, 0x02004004, false);
    check(machine.read8(CpuId::Arm9, destination) == 'A' &&
          machine.read8(CpuId::Arm9, destination + 2) == 'C',
          "LZ77 WRAM SWI expands a literal block");

    const std::uint8_t rl[] = {0x30, 3, 0, 0, 2, 'X', 'Y', 'Z'};
    for (unsigned i = 0; i < sizeof(rl); ++i)
        machine.write8(CpuId::Arm9, source + 0x200 + i, rl[i]);
    cpu.r[0] = source + 0x200;
    cpu.r[1] = destination + 0x10;
    dsr_swi(&cpu, 0x00140000, 0x02004008, false);
    check(machine.read8(CpuId::Arm9, destination + 0x10) == 'X' &&
          machine.read8(CpuId::Arm9, destination + 0x12) == 'Z',
          "RL WRAM SWI expands a literal block");

    const std::uint8_t diff8[] = {0x80, 3, 0, 0, 10, 2, 0xfe};
    for (unsigned i = 0; i < sizeof(diff8); ++i)
        machine.write8(CpuId::Arm9, source + 0x300 + i, diff8[i]);
    cpu.r[0] = source + 0x300;
    cpu.r[1] = destination + 0x20;
    dsr_swi(&cpu, 0x00160000, 0x0200400c, false);
    check(machine.read8(CpuId::Arm9, destination + 0x20) == 10 &&
          machine.read8(CpuId::Arm9, destination + 0x22) == 10,
          "Diff8 SWI restores a delta-coded byte sequence");
    machine.write8(CpuId::Arm9, 0x04000240, 0x80); // Bank A in LCDC mode.
    machine.write16(CpuId::Arm9, 0x06800000, 0xa55a);
    machine.write8(CpuId::Arm9, 0x06800000, 0xff);
    machine.write8(CpuId::Arm9, 0x06800001, 0x00);
    check(machine.read16(CpuId::Arm9, 0x06800000) == 0xa55a,
          "ARM9 byte writes to mapped VRAM are ignored by the width-limited port");
    machine.write16(CpuId::Arm9, 0x05000002, 0x1357);
    machine.write8(CpuId::Arm9, 0x05000002, 0xff);
    check(machine.read16(CpuId::Arm9, 0x05000002) == 0x1357,
          "ARM9 byte writes to palette RAM are ignored");
    machine.write16(CpuId::Arm9, 0x07000002, 0x2468);
    machine.write8(CpuId::Arm9, 0x07000002, 0xff);
    check(machine.read16(CpuId::Arm9, 0x07000002) == 0x2468,
          "ARM9 byte writes to OAM are ignored");
}
void test_powcnt2_roundtrip_preserves_headphone_audio() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    for (std::uint32_t offset = 0; offset < 16; offset += 2)
        machine.write16(CpuId::Arm7, 0x02000000 + offset, 0x7fff);
    machine.write32(CpuId::Arm7, 0x04000404, 0x02000000);
    machine.write32(CpuId::Arm7, 0x04000500, 0x807f);
    machine.write32(CpuId::Arm7, 0x04000408, 0xffff);
    machine.write32(CpuId::Arm7, 0x0400040c, 4);
    machine.write32(CpuId::Arm7, 0x04000400,
                    0x80000000u | (1u << 27) | (1u << 29) | (127u << 16) | 127u);
    check(machine.read8(CpuId::Arm7, 0x04808000) == 0 &&
          machine.read16(CpuId::Arm7, 0x04808000) == 0 &&
          machine.read32(CpuId::Arm7, 0x04808000) == 0,
          "powered-off Wi-Fi registers read zero at every width");
    check(machine.read16(CpuId::Arm7, 0x04000304) == 1,
          "POWCNT2 reset defaults to internal speaker enabled");
    machine.write16(CpuId::Arm7, 0x04000304, 0);
    check(machine.read16(CpuId::Arm7, 0x04000304) == 0,
          "ARM7 POWCNT2 reads back the internal speaker-disable bit");
    machine.spu().run(dsr::Spu::kTicksPerSample);
    std::array<std::int16_t, 2> audio{};
    check(machine.spu().drain(audio.data(), 1) == 1 && audio[1] != 0,
          "disabling the internal speaker leaves headphone audio available");
    machine.write16(CpuId::Arm7, 0x04000304, 3);
    check(machine.read16(CpuId::Arm7, 0x04000304) == 3,
          "ARM7 POWCNT2 preserves speaker and Wi-Fi power bits");
    machine.write16(CpuId::Arm7, 0x0480815a, 0);
    machine.write16(CpuId::Arm7, 0x04808158, 0x5006);
    check(machine.read16(CpuId::Arm7, 0x0480815e) == 0,
          "Wi-Fi BB_BUSY is clear after a completed baseband transfer");
    machine.spu().run(dsr::Spu::kTicksPerSample);
    check(machine.spu().drain(audio.data(), 1) == 1 && audio[1] != 0,
          "enabling POWCNT2 speaker output also leaves audio available");
}
void test_wifi_baseband_and_rf_transactions() {
    MachineConfig config;
    config.direct_boot = false;
    Machine machine(config);
    machine.write16(CpuId::Arm7, 0x04000304, 3);
    machine.write16(CpuId::Arm7, 0x04808158, 0x6000);
    check(machine.read16(CpuId::Arm7, 0x0480815c) == 0x006d,
          "Wi-Fi baseband default is initialized on machine reset");
    check(machine.read16(CpuId::Arm7, 0x04808168) == 0x800d,
          "Wi-Fi BB_POWER reset value matches hardware defaults");
    machine.write16(CpuId::Arm7, 0x04808168, 0xffff);
    check(machine.read16(CpuId::Arm7, 0x04808168) == 0x800f,
          "Wi-Fi baseband power register masks unwritable bits");
    machine.write16(CpuId::Arm7, 0x0480817c, 0x0080);
    check(machine.read16(CpuId::Arm7, 0x0480817e) == 0x0007 &&
          machine.read16(CpuId::Arm7, 0x04808178) == 0x0080,
          "Wi-Fi RF register reset values and mirrored data port read back");
    machine.write16(CpuId::Arm7, 0x0480817c, 0x009c);
    check(machine.read16(CpuId::Arm7, 0x0480817e) == 0x4578 &&
          machine.read16(CpuId::Arm7, 0x0480817c) == 0x009d,
          "Wi-Fi RF readback preserves the 16-bit data word and high bits");
    machine.write16(CpuId::Arm7, 0x0480815a, 0x00a5);
    machine.write16(CpuId::Arm7, 0x04808158, 0x5005);
    check(machine.read16(CpuId::Arm7, 0x0480815e) == 0,
          "Wi-Fi baseband busy clears synchronously after a write");
    machine.write16(CpuId::Arm7, 0x04808158, 0x6005);
    check(machine.read16(CpuId::Arm7, 0x0480815c) == 0x00a5 &&
          machine.read16(CpuId::Arm7, 0x04808158) == 0x00a5,
          "Wi-Fi baseband writes can be read back through BBREAD and BBCNT");

    machine.write16(CpuId::Arm7, 0x0480815a, 0);
    machine.write16(CpuId::Arm7, 0x04808158, 0x5000);
    machine.write16(CpuId::Arm7, 0x04808158, 0x6000);
    check(machine.read16(CpuId::Arm7, 0x0480815c) == 0x006d,
          "Wi-Fi baseband read-only defaults survive attempted writes");
    machine.write16(CpuId::Arm7, 0x04808158, 0x6069);
    check(machine.read16(CpuId::Arm7, 0x0480815c) == 0,
          "out-of-range Wi-Fi baseband reads return zero");

    machine.write16(CpuId::Arm7, 0x0480817e, 0x005a);
    machine.write16(CpuId::Arm7, 0x0480817c, 0x0004);
    check(machine.read16(CpuId::Arm7, 0x04808180) == 0,
          "Wi-Fi RF busy clears synchronously after a transfer");
    machine.write16(CpuId::Arm7, 0x0480817c, 0x0084);
    check(machine.read16(CpuId::Arm7, 0x0480817e) == 0x005a &&
          machine.read16(CpuId::Arm7, 0x0480817c) == 0x0084,
          "Wi-Fi RF register writes can be read back through RF data ports");

    machine.write16(CpuId::Arm7, 0x0480817e, 0x0034);
    machine.write16(CpuId::Arm7, 0x0480817c, static_cast<std::uint16_t>(13u << 2));
    machine.write16(CpuId::Arm7, 0x0480817c,
                    static_cast<std::uint16_t>(0x80u | (13u << 2)));
    check(machine.read16(CpuId::Arm7, 0x0480817e) == 0,
          "unused Wi-Fi RF register writes are ignored");
    machine.write16(CpuId::Arm7, 0x0480817e, 0x00ff);
    machine.write16(CpuId::Arm7, 0x0480817c, static_cast<std::uint16_t>(31u << 2));
    machine.write16(CpuId::Arm7, 0x0480817c, 0x0080);
    check(machine.read16(CpuId::Arm7, 0x0480817e) == 0x0007,
          "Wi-Fi RF reset command restores RF2958 defaults");
}
} // namespace

int run_core_tests() {
    test_main_ram_write_watch_expands_mirrors();
    test_irq_and_nested_return();
    test_timer_irq_event_boundaries();
    test_timers_dma_and_ipc();
    test_timer_deadline_edges();
    test_timer_scalar_equivalence();
    test_timer_scheduler_boundaries();
    test_core_register_contracts();
    test_rtc_core_io_contract();
    test_gx_fifo_dma_starts_on_empty_fifo();
    test_gx_fifo_irq_is_level_triggered();
    test_slot1_read_ready_latency();
    test_dma_latches_and_timing();
    test_video_timing();
    test_frame_boundary_services_key_poll();
    test_extended_keypad_bits();
    test_interworking_return_state();
    test_math_and_fibers();
    test_overlay_hash_dispatch();
    test_overlay_snapshot_dispatch();
    test_overlay_snapshot_remapping_and_io();
    test_shared_region_fallback_dispatch();
    test_bios_memory_swis();
    test_powcnt2_roundtrip_preserves_headphone_audio();
    test_wifi_baseband_and_rf_transactions();
    run_thread_tests();
    run_abi_config_tests();
    return failures == 0 ? 0 : 1;
}
} // namespace dsr::tests

int main() { return dsr::tests::run_core_tests(); }
