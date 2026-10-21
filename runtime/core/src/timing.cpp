#include "core_internal.h"

#include <algorithm>
#include <cstdint>

namespace dsr {
namespace {
constexpr std::uint32_t kTimerPrescalers[4] = {1, 64, 256, 1024};
constexpr unsigned kTimerPrescalerShift[4] = {0, 6, 8, 10}; // dividers 1, 64, 256, 1024
} // namespace

std::uint64_t Machine::Impl::timer_irq_boundary(std::uint64_t limit) const {
    // No timer can raise an IRQ before the earliest clocked overflow.
    if (timers_due_tick == detail::kNoTimerDue || timers_due_tick - total_ticks >= limit)
        return limit;
    const std::uint64_t elapsed = total_ticks - timers_synced_tick;
    for (unsigned ci = 0; ci < 2; ++ci) {
        for (unsigned ti = 0; ti < 4; ++ti) {
            const TimerState& timer = timers[ci * 4 + ti];
            if ((timer.control & 0x84u) != 0x80u) continue;
            const std::uint64_t divisor = kTimerPrescalers[timer.control & 3u];
            const std::uint64_t clocks = (0x10000ull - timer.count) * divisor;
            // advance_ticks materializes every overflow, so no cascade has
            // changed since this epoch. Project elapsed clocks into phase
            // instead of treating the lazy count/phase as current state.
            const std::uint64_t phase = timer.phase + elapsed;
            // Work in prescaler clocks until selecting an IRQ. Keeping phase
            // separate also handles a divisor change with clocks already due.
            std::uint64_t deadline = clocks;
            if (deadline > phase && deadline - phase >= limit) continue;
            if ((timer.control & 0x40u) != 0) {
                limit = deadline > phase ? deadline - phase : 1;
                continue;
            }
            std::uint64_t period = (0x10000ull - timer.reload) * divisor;
            for (unsigned next = ti + 1; next < 4; ++next) {
                const TimerState& cascade = timers[ci * 4 + next];
                if ((cascade.control & 0x84u) != 0x84u) break;
                const std::uint64_t overflows = 0xffffu - cascade.count;
                // Saturate the search at this slice, not at every upstream
                // overflow: a /1 source can feed a long, non-IRQ cascade.
                const std::uint64_t window = phase + limit - 1;
                if (overflows != 0 && period > (window - deadline) / overflows) break;
                deadline += overflows * period;
                if ((cascade.control & 0x40u) != 0) {
                    limit = deadline > phase ? deadline - phase : 1;
                    break;
                }
                const std::uint64_t reload_period = 0x10000ull - cascade.reload;
                period = period > window / reload_period ? window + 1 : period * reload_period;
            }
        }
    }
    return limit;
}

void Machine::Impl::timer_increment(unsigned cpu_index, unsigned timer_index, std::uint64_t increments) {
    if (timer_index >= 4 || increments == 0) return;
    TimerState& timer = timers[cpu_index * 4 + timer_index];
    if ((timer.control & 0x80u) == 0) return;

    const std::uint64_t first_period = 0x10000ull - timer.count;
    std::uint64_t overflow_count = 0;
    if (increments < first_period) {
        timer.count = static_cast<std::uint16_t>(timer.count + increments);
        return;
    }
    increments -= first_period;
    overflow_count = 1;
    const std::uint64_t period = 0x10000ull - timer.reload;
    overflow_count += increments / period;
    const std::uint64_t remainder = increments % period;
    timer.count = static_cast<std::uint16_t>(timer.reload + remainder);
    if ((timer.control & 0x40u) != 0) set_irq(cpu_index, 3 + timer_index);
    if (timer_index < 3) {
        TimerState& next = timers[cpu_index * 4 + timer_index + 1];
        if ((next.control & 0x84u) == 0x84u) timer_increment(cpu_index, timer_index + 1, overflow_count);
    }
}

void Machine::Impl::sync_timers() {
    const std::uint64_t ticks = total_ticks - timers_synced_tick;
    // Control writes retain phase unless they enable a stopped timer. In
    // particular, reducing the divider must not consume that phase at t=0.
    if (ticks == 0) return;
    timers_synced_tick = total_ticks;
    for (unsigned ci = 0; ci < 2; ++ci) {
        for (unsigned ti = 0; ti < 4; ++ti) {
            TimerState& timer = timers[ci * 4 + ti];
            if ((timer.control & 0x80u) == 0 || (timer.control & 4u) != 0) continue;
            timer.phase += ticks;
            const unsigned shift = kTimerPrescalerShift[timer.control & 3u];
            const std::uint64_t increments = timer.phase >> shift;
            timer.phase &= (std::uint64_t{1} << shift) - 1u;
            timer_increment(ci, ti, increments);
        }
    }
    refresh_timer_deadline();
}

void Machine::Impl::refresh_timer_deadline() {
    timers_due_tick = detail::kNoTimerDue;
    for (const TimerState& timer : timers) {
        // Preserve bit 2's existing behavior even on timer 0: only a
        // predecessor's overflow can advance a count-up timer.
        if ((timer.control & 0x84u) != 0x80u) continue;
        const unsigned shift = kTimerPrescalerShift[timer.control & 3u];
        const std::uint64_t first_period_ticks = (0x10000ull - timer.count) << shift;
        // A divider reduction can leave more phase than the new first
        // period. The old per-step service consumes it on the next
        // positive step, never during another I/O access at this tick.
        const std::uint64_t remaining = timer.phase < first_period_ticks
            ? first_period_ticks - timer.phase : 1;
        timers_due_tick = std::min(timers_due_tick, total_ticks + remaining);
    }
}

void Machine::Impl::trigger_dma(unsigned cpu_index, unsigned mode) {
    if (cpu_index >= 2) return;
    const bool gx_dma = cpu_index == 0 && mode == 7;
    if (gx_dma) {
        // GX FIFO DMA is level-triggered: it runs while GXSTAT bit 25 (FIFO
        // less than half full) stays asserted, 112 words per burst, and the
        // FIFO writes it performs must not re-enter this loop.
        if (gx_dma_active) return;
        gx_dma_active = true;
        // Bounded by the latched transfer count; repeat mode re-arms only on
        // the next FIFO notification so a draining FIFO cannot spin forever.
        std::uint32_t budget = 0;
        for (unsigned channel = 0; channel < 4; ++channel) budget += dmas[channel].remaining;
        while (budget != 0 && gpu3d.fifo_below_half()) {
            unsigned active = 4;
            for (unsigned channel = 0; channel < 4; ++channel) {
                const DmaState& dma = dmas[channel];
                if ((dma.control & 0x8000u) != 0 && dma.remaining != 0 &&
                    detail::dma_timing_mode(0, channel, dma.control) == 7) {
                    active = channel; // lowest channel has priority
                    break;
                }
            }
            if (active == 4) break;
            const std::uint32_t before = dmas[active].remaining;
            trigger_one_dma(0, active);
            const std::uint32_t after = dmas[active].remaining;
            const std::uint32_t moved = (dmas[active].control & 0x8000u) == 0 || after > before
                ? before : before - after;
            if (moved == 0) break;
            budget = moved >= budget ? 0 : budget - moved;
        }
        gx_dma_active = false;
        return;
    }

    for (unsigned channel = 0; channel < 4; ++channel) {
        const DmaState& dma = dmas[cpu_index * 4 + channel];
        if ((dma.control & 0x8000u) != 0 &&
            detail::dma_timing_mode(cpu_index, channel, dma.control) == mode) {
            trigger_one_dma(cpu_index, channel);
        }
    }
}

void Machine::Impl::enable_dma(unsigned cpu_index, unsigned channel) {
    if (cpu_index >= 2 || channel >= 4) return;
    DmaState& dma = dmas[cpu_index * 4 + channel];
    dma.current_src = dma.src;
    dma.current_dst = dma.dst;
    dma.latched_dst = dma.dst;
    dma.latched_count = detail::dma_transfer_count(cpu_index, channel, dma.count);
    dma.remaining = dma.latched_count;
    dma.in_transfer = false;
    const unsigned mode = detail::dma_timing_mode(cpu_index, channel, dma.control);
    if (mode == 0)
        trigger_one_dma(cpu_index, channel);
    else if (mode == 7 && cpu_index == 0)
        trigger_dma(0, 7); // FIFO may already be below half (e.g. empty)
}

void Machine::Impl::trigger_one_dma(unsigned cpu_index, unsigned channel) {
    if (cpu_index >= 2 || channel >= 4) return;
    DmaState& dma = dmas[cpu_index * 4 + channel];
    const std::uint16_t control = static_cast<std::uint16_t>(dma.control);
    if ((control & 0x8000u) == 0 || dma.remaining == 0 || dma.in_transfer) return;

    const unsigned mode = detail::dma_timing_mode(cpu_index, channel, control);
    const bool gx_dma = mode == 7 && cpu_index == 0;
    const bool word = gx_dma || (control & (1u << 10)) != 0;
    const std::uint32_t width = word ? 4u : 2u;
    const std::uint32_t units = gx_dma ? std::min(dma.remaining, 112u) : dma.remaining;
    const unsigned source_control = (control >> 7) & 3u;
    const unsigned destination_control = (control >> 5) & 3u;

    dma.current_src &= ~(width - 1u);
    dma.current_dst &= ~(width - 1u);
    dma.in_transfer = true;
    for (std::uint32_t n = 0; n < units; ++n) {
        // Slot-1 holds READY for the block. Stop at card exhaustion rather
        // than synthesizing stale words if the DMA count is longer. The
        // latched loop bound also limits repeating DMA to one completion.
        if (mode == 5 &&
            (!card || (card->io_read(0x040001a4u, 32) & 0x80800000u) != 0x80800000u))
            break;
        const std::uint32_t destination = gx_dma ? 0x04000400u : dma.current_dst;
        const std::uint32_t transfer = read(cpu_index == 0 ? CpuId::Arm9 : CpuId::Arm7,
                                            dma.current_src, word ? 32 : 16);
        write(cpu_index == 0 ? CpuId::Arm9 : CpuId::Arm7,
              destination, transfer, word ? 32 : 16);
        if (source_control == 0 || source_control == 3) dma.current_src += width;
        else if (source_control == 1) dma.current_src -= width;
        if (!gx_dma) {
            if (destination_control == 0 || destination_control == 3) dma.current_dst += width;
            else if (destination_control == 1) dma.current_dst -= width;
        }
        --dma.remaining;
    }
    dma.in_transfer = false;

    if (dma.remaining != 0) return;
    const bool repeat = (control & (1u << 9)) != 0 && mode != 0;
    if (repeat) {
        dma.remaining = dma.latched_count;
        if (destination_control == 3) dma.current_dst = dma.latched_dst;
    } else {
        dma.control &= ~0x8000u;
    }
    if ((control & (1u << 14)) != 0) set_irq(cpu_index, 8 + channel);
}

void Machine::Impl::process_hblank() {
    for (unsigned ci = 0; ci < 2; ++ci) {
        irq[ci].dispstat |= 1u << 1;
        if (irq[ci].dispstat & (1u << 4)) set_irq(ci, 1);
    }
    if (scanline < 192) gpu2d.render_line(static_cast<int>(scanline));
    if (scanline < detail::kVBlankLine) trigger_dma(0, 2);
    trigger_dma(0, 4);
}

void Machine::Impl::process_line_start() {
    scanline = (scanline + 1) % detail::kLinesPerFrame;
    for (unsigned ci = 0; ci < 2; ++ci) {
        IrqState& state = irq[ci];
        state.dispstat &= ~(1u << 1);
        if (scanline == detail::kVBlankLine) state.dispstat |= 1u;
        else if (scanline == detail::kLinesPerFrame - 1) state.dispstat &= ~1u;
        const unsigned compare = ((state.dispstat >> 8) & 0xffu) |
                                 ((state.dispstat & 0x80u) << 1);
        const bool match = compare == scanline;
        if (match) state.dispstat |= 1u << 2;
        else state.dispstat &= ~(1u << 2);
        if (match && (state.dispstat & (1u << 5))) set_irq(ci, 2);
    }
    if (scanline == detail::kVBlankLine) {
        gpu2d.vblank_start();
        gpu3d.vblank_start();
        for (unsigned ci = 0; ci < 2; ++ci) {
            if (irq[ci].dispstat & (1u << 3)) set_irq(ci, 0);
            trigger_dma(ci, 1);
        }
    }
    // 3D VBlank ends at214, after the swap/flush at192 and guest texture
    // uploads. DeSmuME's rigorous timing uses this same render-start boundary.
    if (scanline == 214) gpu3d.render_start();
    if (scanline < 192) {
        trigger_dma(0, 3);
        trigger_dma(1, 3);
    }
}

void Machine::Impl::advance_ticks(std::uint64_t ticks) {
    while (ticks != 0) {
        const std::uint64_t boundary = line_tick < detail::kHBlankTick
            ? detail::kHBlankTick - line_tick
            : detail::kLineTicks - line_tick;
        const std::uint64_t step = std::min(ticks, boundary);
        total_ticks += step;
        if (config.deterministic_rtc) rtc.set_emulated_ticks(total_ticks);
        line_tick += step;
        if (div_busy_ticks != 0) {
            if (step >= div_busy_ticks) {
                div_result = div_pending_result;
                div_remainder = div_pending_remainder;
                div_busy_ticks = 0;
                divcnt &= ~0x8000u;
            } else {
                div_busy_ticks -= step;
            }
        }
        if (sqrt_busy_ticks != 0) {
            if (step >= sqrt_busy_ticks) {
                sqrt_result = sqrt_pending_result;
                sqrt_busy_ticks = 0;
                sqrtcnt &= ~0x8000u;
            } else {
                sqrt_busy_ticks -= step;
            }
        }
        // Keep the original step boundaries and peripheral/IRQ ordering.
        if (total_ticks >= timers_due_tick) sync_timers();
        gpu3d.run(static_cast<std::int64_t>(step));
        spu.run(static_cast<std::int64_t>(step));
        if (card) card->run(static_cast<std::int64_t>(step));
        ticks -= step;
        if (line_tick == detail::kHBlankTick) process_hblank();
        if (line_tick == detail::kLineTicks) {
            line_tick = 0;
            process_line_start();
        }
    }
}

} // namespace dsr
