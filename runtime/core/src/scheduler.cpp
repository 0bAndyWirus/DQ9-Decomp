#include "core_internal.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace dsr {

void Machine::Impl::cpu_fiber_main(unsigned cpu_index) {
    CpuRuntime& rt = cpus[cpu_index];
    rt.entered = true;
    call(&rt.cpu, rt.entry);
    rt.fiber_done = true;
    rt.halted = true;
}

void Machine::Impl::run_cpu_slice(unsigned cpu_index, std::uint64_t ticks) {
    CpuRuntime& rt = cpus[cpu_index];
    if (rt.fiber_done || rt.stopped || rt.entry == 0) return;
    if (rt.halted) {
        if ((irq[cpu_index].ie & irq[cpu_index].flags) == 0) return;
        rt.halted = false;
        rt.stopped = false;
    }
    if (!rt.fiber) {
        rt.fiber = std::make_unique<Fiber>([this, cpu_index] { cpu_fiber_main(cpu_index); });
        rt.active_fiber = rt.fiber.get();
    }
    // Free fibers of terminated/abandoned guest threads (never the running one).
    for (Fiber* dead : rt.dead_fibers) {
        if (dead == rt.active_fiber) continue;
        std::erase_if(rt.thread_fibers, [dead](const std::unique_ptr<Fiber>& f) { return f.get() == dead; });
    }
    std::erase_if(rt.dead_fibers, [&rt](Fiber* f) { return f != rt.active_fiber; });
    // Cycles a CPU overran (long BIOS waits, block granularity) are paid
    // back from later slices so both CPUs keep the same timebase.
    const std::int64_t debt = std::min<std::int64_t>(rt.cpu.budget, 0);
    const std::uint64_t cycles = ticks * (cpu_index == 0 ? 2u : 1u);
    rt.cpu.budget = debt + static_cast<std::int64_t>(std::min<std::uint64_t>(
        cycles, static_cast<std::uint64_t>(INT64_MAX / 2)));
    if (rt.cpu.budget <= 0) return;
    if (can_deliver_irq(cpu_index)) rt.force_irq_checkpoint();
    Fiber::switch_to(*rt.active_fiber);
    if (rt.active_fiber->finished()) {
        rt.fiber_done = true;
        rt.halted = true;
    }
}

void Machine::Impl::run_ticks(std::uint64_t ticks) {
    const auto apply_soft_reset = [this] {
        if (!soft_reset_requested) return;
        reset();
        detail::reset_thread_overrides(*this);
        boot();
    };
    apply_soft_reset();
    while (ticks != 0) {
        apply_soft_reset();
        std::uint64_t to_event = line_tick < detail::kHBlankTick
            ? detail::kHBlankTick - line_tick
            : detail::kLineTicks - line_tick;
        if (to_event == 0) to_event = detail::kLineTicks;
        const std::uint64_t slice = timer_irq_boundary(
            std::min<std::uint64_t>({ticks, to_event, 128}));
        run_cpu_slice(0, slice);
        if (!soft_reset_requested) run_cpu_slice(1, slice);
        advance_ticks(slice);
        ticks -= slice;
        apply_soft_reset();
    }
}

void Machine::Impl::checkpoint(Cpu* cpu, std::uint32_t pc) {
    CpuRuntime& rt = runtime(cpu->id);
    rt.current_pc = pc;
    rt.restore_irq_budget();
    if (can_deliver_irq(static_cast<unsigned>(cpu->id))) enter_irq(rt, pc);
    if (cpu->budget <= 0) {
        Fiber::yield();
        rt.restore_irq_budget();
        if (can_deliver_irq(static_cast<unsigned>(cpu->id))) enter_irq(rt, pc);
    }
}

void Machine::Impl::halt(Cpu* cpu) {
    CpuRuntime& rt = runtime(cpu->id);
    rt.halted = true;
    rt.irq_budget_reserve = 0;
    cpu->budget = 0;
    Fiber::yield(); // the scheduler resumes this CPU once IE & IF != 0
}

void Machine::Impl::intr_wait(Cpu* cpu, std::uint32_t mask, bool discard_old, std::uint32_t resume_pc) {
    // BIOS IntrWait: IME=1, then halt until the guest IRQ handler has set a
    // requested bit in the BIOS IRQ check word, which is consumed here.
    const unsigned ci = static_cast<unsigned>(cpu->id);
    const std::uint32_t check = cpu->id == CpuId::Arm9
        ? ((runtime(CpuId::Arm9).dtcm_region & 0xfffff000u) + 0x3ff8u)
        : 0x0380fff8u;
    irq[ci].ime = 1;
    if (discard_old) wr32(cpu, check, rd32(cpu, check) & ~mask);
    for (;;) {
        const std::uint32_t flags = rd32(cpu, check);
        if (flags & mask) {
            wr32(cpu, check, flags & ~mask);
            return;
        }
        if ((irq[ci].ie & irq[ci].flags) == 0) {
            halt(cpu);
        } else {
            // A request is pending but did not satisfy the wait (or is not
            // acknowledged by the handler): give up the slice so time moves.
            runtime(cpu->id).irq_budget_reserve = 0;
            cpu->budget = 0;
            Fiber::yield();
        }
        // The BIOS waits with IRQs enabled: take the request now.
        CpuRuntime& rt = runtime(cpu->id);
        const std::uint32_t saved = cpu->cpsr_ctrl;
        cpu->cpsr_ctrl &= ~kCpsrI;
        if (can_deliver_irq(ci)) enter_irq(rt, resume_pc);
        cpu->cpsr_ctrl = (cpu->cpsr_ctrl & ~kCpsrI) | (saved & kCpsrI);
    }
}

} // namespace dsr
