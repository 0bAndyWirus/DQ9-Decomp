#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"
#include "dsr/machine.h"
#include "dsr/mem.h"
#include "dsr/spu.h"

#include "dsr/fiber.h"
namespace dsr {
class Fiber;

namespace detail {

inline constexpr std::uint32_t kMainRamBase = 0x02000000;
inline constexpr std::uint32_t kMainRamSize = 4 * 1024 * 1024;
struct GuestWriteWatchRange {
    std::uint32_t first;
    std::uint64_t end;
};
std::vector<GuestWriteWatchRange> expand_main_ram_write_watch_aliases(
    const std::vector<GuestWriteWatchRange>& ranges);
inline constexpr std::uint32_t kSharedWramBase = 0x03000000;
inline constexpr std::uint32_t kSharedWramSize = 32 * 1024;
inline constexpr std::uint32_t kArm7WramBase = 0x03800000;
inline constexpr std::uint32_t kArm7WramSize = 64 * 1024;
inline constexpr std::uint64_t kLineTicks = 2130;
inline constexpr std::uint64_t kHBlankTick = 1606;
inline constexpr unsigned kLinesPerFrame = 263;
inline constexpr unsigned kVBlankLine = 192;
inline constexpr std::uint64_t kNoTimerDue = std::numeric_limits<std::uint64_t>::max();

enum class Bank : unsigned { UsrSys, Fiq, Irq, Svc, Abt, Und };

struct CpuBanks {
    std::array<std::uint32_t, 5> user_r8_r12{};
    std::array<std::uint32_t, 5> fiq_r8_r12{};
    std::array<std::array<std::uint32_t, 2>, 6> sp_lr{};
    std::array<std::uint32_t, 6> spsr{};

    static Bank bank_for(std::uint32_t mode);
    void switch_mode(Cpu& cpu, std::uint32_t new_mode);
    std::uint32_t read_user(const Cpu& cpu, unsigned reg) const;
    void write_user(Cpu& cpu, unsigned reg, std::uint32_t value);
    std::uint32_t cpsr(const Cpu& cpu) const;
    void write_cpsr(Cpu& cpu, std::uint32_t value, std::uint32_t field_mask);
    std::uint32_t read_spsr(const Cpu& cpu) const;
    void write_spsr(const Cpu& cpu, std::uint32_t value, std::uint32_t field_mask);
    void exception_return(Cpu& cpu);
};

struct ParkedThread {
    Fiber* fiber = nullptr;
    std::uint32_t continuation = 0; // context +0x40 when parked
    std::uint32_t guest_sp = 0;     // guest SP at the park point
};
struct CpuRuntime {
    Cpu cpu{};
    std::array<std::uint8_t*, kPageCount> page_read{};
    std::array<std::uint8_t*, kPageCount> page_write{};
    CpuBanks banks{};
    std::uint32_t cp15_control = 0x00052078;
    std::uint32_t dtcm_region = 0x0080000A; // 16 KiB region at 0x00800000
    std::uint32_t itcm_region = 0x0000000C; // 32 KiB region at 0
    bool halted = false;
    bool stopped = false;
    bool entered = false;
    bool fiber_done = false;
    bool in_irq = false;
    bool halt_until_irq = false;
    bool waiting_for_irq = false;
    bool halt_clear_on_wake = false;
    std::uint32_t halt_wait_mask = 0xffffffffu;
    std::uint32_t entry = 0;
    std::uint32_t current_pc = 0;
    // An IRQ checkpoint uses budget <= 0 as a sentinel, not as consumed time.
    // Keep its displaced positive cycles separate from charged negative debt.
    std::int64_t irq_budget_reserve = 0;

    void force_irq_checkpoint() {
        if (cpu.budget <= 0) return;
        irq_budget_reserve += cpu.budget;
        cpu.budget = 0;
    }

    void restore_irq_budget() {
        cpu.budget += irq_budget_reserve;
        irq_budget_reserve = 0;
    }

    std::unique_ptr<Fiber> fiber;  // boot thread (runs the CPU entry point)
    // Guest threads on fibers (see thread.cpp). active_fiber is the fiber the
    // scheduler resumes for this CPU.
    Fiber* active_fiber = nullptr;
    std::vector<std::unique_ptr<Fiber>> thread_fibers;
    std::unordered_map<std::uint32_t, ParkedThread> parked;    // context -> waiting fiber
    std::unordered_map<const Fiber*, std::uint32_t> saved_ctx; // fiber -> its last SaveContext
    std::vector<Fiber*> dead_fibers;                           // never resumed again
};

struct IrqState {
    std::uint32_t ime = 0;
    std::uint32_t ie = 0;
    std::uint32_t flags = 0;
    std::uint16_t dispstat = 0;
    std::uint16_t keycnt = 0;
};

struct TimerState {
    std::uint16_t reload = 0;
    std::uint16_t count = 0;
    std::uint16_t control = 0;
    std::uint64_t phase = 0;
};

struct DmaState {
    // Programmed register values remain visible through I/O readback.
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
    std::uint32_t count = 0;
    std::uint32_t control = 0;
    // Transfer-local state is latched when DMA is enabled.
    std::uint32_t current_src = 0;
    std::uint32_t current_dst = 0;
    std::uint32_t latched_dst = 0;
    std::uint32_t latched_count = 0;
    std::uint32_t remaining = 0;
    bool in_transfer = false;
};

inline constexpr std::uint32_t dma_count_mask(unsigned cpu_index, unsigned channel) {
    if (cpu_index == 0) return 0x1fffffu;
    return channel == 3 ? 0xffffu : 0x3fffu;
}

inline constexpr std::uint32_t dma_transfer_count(unsigned cpu_index, unsigned channel,
                                                   std::uint32_t count) {
    const std::uint32_t mask = dma_count_mask(cpu_index, channel);
    count &= mask;
    return count == 0 ? mask + 1u : count;
}

inline constexpr unsigned dma_timing_mode(unsigned cpu_index, unsigned channel,
                                          std::uint32_t control) {
    if (cpu_index == 0) return (control >> 11) & 7u;
    const unsigned mode = (control >> 12) & 3u;
    if (mode == 2) return 5; // DS cartridge slot
    if (mode == 3) return channel == 1 || channel == 3 ? 6u : 8u; // GBA slot vs. wireless
    return mode;
}

struct IpcState {
    std::uint16_t sync[2]{};
    std::uint16_t fifo_control[2]{};
    std::vector<std::uint32_t> fifo[2]; // words sent to each receiver
    std::uint32_t last_received[2]{};
};
struct WifiState {
    std::array<std::uint8_t, 0x69> baseband{};
    std::array<std::uint32_t, 32> rf_registers{};
    std::uint16_t bb_power = 0x800d;
    std::uint16_t bb_write = 0;
    std::uint16_t bb_read = 0;
    std::uint16_t bb_control = 0;
    std::uint16_t rf_data1 = 0;
    std::uint16_t rf_data2 = 0;
    std::uint16_t rf_control = 0;

    void reset_rf() {
        // RF2958 reset values cross-checked against a reference emulator.
        rf_registers.fill(0);
        rf_registers[0] = 0x0007; // CFG1
        rf_registers[1] = 0x5003; // IFPLL1
        rf_registers[2] = 0x0022; // IFPLL2
        rf_registers[3] = 0x1ff78; // IFPLL3
        rf_registers[4] = 0x5003; // RFPLL1
        rf_registers[5] = 0x01780; // RFPLL2
        rf_registers[7] = 0x14578; // RFPLL4
        rf_registers[8] = 0x1e742; // CAL1
        rf_registers[9] = 0x00120; // TXRX1
    }

    void reset() {
        baseband.fill(0);
        baseband[0] = 0x6d;
        baseband[0x5d] = 0x01;
        baseband[0x64] = 0xff;
        reset_rf();
        bb_power = 0x800d;
        bb_write = bb_read = bb_control = 0;
    }
};

struct MachineAccess {
    static Machine::Impl& impl(Cpu* cpu);
    static const Machine::Impl& impl(const Cpu* cpu);
};

} // namespace detail

using detail::CpuRuntime;
using detail::DmaState;
using detail::IrqState;
using detail::IpcState;
using detail::TimerState;
using detail::WifiState;

struct Machine::Impl {
    explicit Impl(const MachineConfig& config);
    ~Impl();

    Machine* owner = nullptr;
    MachineConfig config;
    struct ThreadOverrideSlot {
        GuestFn* slot = nullptr;
        GuestFn original = nullptr;
        GuestFn installed = nullptr;
    };
    std::array<ThreadOverrideSlot, 6> thread_override_slots{};
    std::size_t thread_override_count = 0;
    CpuRuntime cpus[2];
    std::array<std::uint8_t, detail::kMainRamSize> main_ram{};
    std::array<std::uint8_t, 32 * 1024> itcm{};
    std::array<std::uint8_t, 16 * 1024> dtcm{};
    std::array<std::uint8_t, detail::kSharedWramSize> shared_wram{};
    std::array<std::uint8_t, detail::kArm7WramSize> arm7_wram{};
    std::array<std::uint8_t, 0x200> rom_header{};
    std::array<std::uint8_t, 0x1000> io_shadow[2]{};
    std::array<IrqState, 2> irq{};
    std::array<TimerState, 2 * 4> timers{};
    // Counts and prescaler phases are materialized at timers_synced_tick.
    std::uint64_t timers_synced_tick = 0;
    std::uint64_t timers_due_tick = detail::kNoTimerDue;
    std::array<DmaState, 2 * 4> dmas{};
    bool gx_dma_active = false;
    IpcState ipc{};
    WifiState wifi{};
    std::uint16_t wramcnt = 3;
    std::uint16_t exmemcnt = 0;   // ARM9 EXMEMCNT (bits 7-15 also seen by ARM7)
    std::uint16_t exmemstat7 = 0; // ARM7 EXMEMSTAT bits 0-6
    std::uint16_t postflg[2]{};
    std::uint16_t powcnt1 = 0;
    std::uint16_t powcnt2 = 1;
    std::uint16_t keyinput = 0x03ff;
    InputState input{};
    std::uint64_t total_ticks = 0;
    unsigned scanline = 0;
    std::uint64_t line_tick = 0;
    std::uint32_t divcnt = 0;
    std::uint64_t div_numer = 0;
    std::uint64_t div_denom = 0;
    std::uint64_t div_result = 0;
    std::uint64_t div_remainder = 0;
    std::uint64_t div_pending_result = 0;
    std::uint64_t div_pending_remainder = 0;
    std::uint64_t div_busy_ticks = 0;
    std::uint32_t sqrtcnt = 0;
    std::uint64_t sqrt_param = 0;
    std::uint32_t sqrt_result = 0;
    std::uint32_t sqrt_pending_result = 0;
    std::uint64_t sqrt_busy_ticks = 0;
    std::unique_ptr<GameCard> card;
    Spi spi;
    Rtc rtc;
    Gpu2D gpu2d;
    Gpu3D gpu3d;
    Spu spu;
    struct DispatchTarget {
        std::vector<const FnEntry*> candidates;
        // Only bytes examined before the previous first match are needed.
        // A byte-exact comparison also validates rejected earlier candidates.
        std::vector<std::uint8_t> snapshot;
        const FnEntry* cached_entry = nullptr;
        std::uint32_t checked_size = 0;
        bool snapshot_valid = false;
    };
    bool soft_reset_requested = false;
    std::unordered_set<std::uint32_t> logged_unimplemented;

    // (cpu << 32 | addr) -> every FnEntry for that address (overlays may share
    // addresses). Region descriptors are grouped by CPU and range-scanned.
    std::unordered_map<std::uint64_t, DispatchTarget> dispatch_index;
    std::array<std::vector<const DispatchRegionEntry*>, 2> dispatch_regions;
    bool dispatch_index_built = false;
    bool verify_dispatch = false;
    void reset();
    void boot();
    void rebuild_pages(CpuId id);
    void rebuild_vram_pages();

    std::uint32_t read(CpuId id, std::uint32_t addr, int width);
    void write(CpuId id, std::uint32_t addr, std::uint32_t value, int width,
               std::uint32_t guest_pc = 0);
    std::uint32_t io_read(CpuId id, std::uint32_t addr, int width);
    void io_write(CpuId id, std::uint32_t addr, std::uint32_t value, int width);
    void run_ticks(std::uint64_t ticks);
    void advance_ticks(std::uint64_t ticks);
    void process_hblank();
    void process_line_start();
    std::uint64_t timer_irq_boundary(std::uint64_t limit) const;
    void sync_timers();
    void refresh_timer_deadline();
    void timer_increment(unsigned cpu_index, unsigned timer_index, std::uint64_t increments);
    void enable_dma(unsigned cpu_index, unsigned channel);
    void trigger_dma(unsigned cpu_index, unsigned mode);
    void trigger_one_dma(unsigned cpu_index, unsigned channel);
    void set_irq(unsigned cpu_index, unsigned bit);
    bool can_deliver_irq(unsigned cpu_index) const;
    void handle_key_irq();
    void run_cpu_slice(unsigned cpu_index, std::uint64_t ticks);
    void cpu_fiber_main(unsigned cpu_index);
    void checkpoint(Cpu* cpu, std::uint32_t pc);
    void call(Cpu* cpu, std::uint32_t target);
    void halt(Cpu* cpu);
    void intr_wait(Cpu* cpu, std::uint32_t mask, bool discard_old, std::uint32_t resume_pc);
    void build_dispatch_index();
    const FnEntry* find_entry(Cpu* cpu, std::uint32_t target);
    const DispatchRegionEntry* find_region(Cpu* cpu, std::uint32_t target);
    [[noreturn]] void dispatch_failure(Cpu* cpu, std::uint32_t target);
    void enter_irq(CpuRuntime& runtime, std::uint32_t pc);
    std::uint32_t cp15_read(Cpu* cpu, unsigned cp, unsigned op1, unsigned crn,
                            unsigned crm, unsigned op2, std::uint32_t pc);
    void cp15_write(Cpu* cpu, unsigned cp, unsigned op1, unsigned crn,
                    unsigned crm, unsigned op2, std::uint32_t value,
                    std::uint32_t pc);
    void swi(Cpu* cpu, std::uint32_t imm, std::uint32_t pc, bool thumb);
    void log_unimplemented(CpuId id, std::uint32_t addr, int width);

    CpuRuntime& runtime(CpuId id) { return cpus[static_cast<unsigned>(id)]; }
    const CpuRuntime& runtime(CpuId id) const { return cpus[static_cast<unsigned>(id)]; }
};

} // namespace dsr

namespace dsr::detail {

// Guest thread layer (thread.cpp).
void install_thread_overrides(Machine::Impl& machine);
void uninstall_thread_overrides(Machine::Impl& machine);
void reset_thread_overrides(Machine::Impl& machine);
// Current guest thread context, or zero when that CPU has no supplied ABI.
std::uint32_t current_thread_context(Machine::Impl& machine, CpuId id);
// Called by enter_irq when the guest IRQ path switched threads.
void irq_thread_switch(Machine::Impl& machine, Cpu* cpu, std::uint32_t old_ctx, std::uint32_t new_ctx);

} // namespace dsr::detail
