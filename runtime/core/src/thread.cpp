// Caller-configured guest threads on host fibers.
//
// Normal ready/blocked scheduling, priorities, mutexes, and ordinary context
// selection stay in recompiled guest code. Native overrides provide the two
// context primitives plus the narrow IRQ-return scheduling seam:
//
//   SaveContext(ctx)    writes the guest context frame exactly like the
//                       guest routine, remembers that the running fiber is
//                       resumable through ctx, and returns 0.
//   RestoreContext(ctx) switches to the fiber parked on ctx; if ctx was
//                       (re)initialised by the guest instead, a new fiber
//                       starts executing ctx's registers.
//   IRQ return          wakes alarm waiters, scans the guest ready list,
//                       invokes its optional switch callback, and updates
//                       the active-context pointer.
//
// A fiber parked by SaveContext resumes inside its RestoreContext call with
// its registers reloaded from its own context. enter_irq() restores the
// hardware IRQ frame before the IRQ seam parks/resumes guest Fibers.
#include "core_internal.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace dsr::detail {
namespace {

const ThreadAbi& abi(const Machine::Impl& m, CpuId id) {
    return *(id == CpuId::Arm9 ? m.config.guest_abi.arm9 : m.config.guest_abi.arm7);
}

const ThreadAbi& abi(Cpu* cpu) {
    return abi(MachineAccess::impl(cpu), cpu->id);
}

[[noreturn]] void thread_fatal(Cpu* cpu, const char* what, std::uint32_t ctx) {
    std::fprintf(stderr, "[dsr] fatal: %s thread layer: %s (context 0x%08x)\n",
                 cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", what, ctx);
    std::fflush(nullptr);
    std::abort();
}

struct GuestCpuState {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t n = 0;
    std::uint32_t z = 0;
    std::uint32_t c = 0;
    std::uint32_t v = 0;
    std::uint32_t q = 0;
    std::uint32_t cpsr_ctrl = 0;
    CpuBanks banks{};
    std::uint32_t current_pc = 0;
};

GuestCpuState capture_guest_state(Cpu* cpu) {
    CpuRuntime& rt = MachineAccess::impl(cpu).runtime(cpu->id);
    GuestCpuState state{};
    for (unsigned reg = 0; reg < 16; ++reg) state.r[reg] = cpu->r[reg];
    state.n = cpu->n;
    state.z = cpu->z;
    state.c = cpu->cf;
    state.v = cpu->v;
    state.q = cpu->q;
    state.cpsr_ctrl = cpu->cpsr_ctrl;
    state.banks = rt.banks;
    state.current_pc = rt.current_pc;
    return state;
}

void restore_guest_state(Cpu* cpu, const GuestCpuState& state) {
    CpuRuntime& rt = MachineAccess::impl(cpu).runtime(cpu->id);
    for (unsigned reg = 0; reg < 16; ++reg) cpu->r[reg] = state.r[reg];
    cpu->n = state.n;
    cpu->z = state.z;
    cpu->cf = state.c;
    cpu->v = state.v;
    cpu->q = state.q;
    cpu->cpsr_ctrl = state.cpsr_ctrl;
    rt.banks = state.banks;
    rt.current_pc = state.current_pc;
}

void call_guest_preserving(Cpu* cpu, std::uint32_t target, std::uint32_t r0,
                           std::uint32_t r1 = 0, bool set_r1 = false,
                           std::uint32_t lr = 0, bool set_lr = false) {
    const GuestCpuState saved = capture_guest_state(cpu);
    cpu->r[0] = r0;
    if (set_r1) cpu->r[1] = r1;
    if (set_lr) cpu->r[14] = lr;
    dsr_call(cpu, target);
    restore_guest_state(cpu, saved);
}

// Load the guest register file in its banked mode; the saved GPRs are the
// user/system bank even when the saved CPSR is privileged.
void load_context(Machine::Impl& m, Cpu* cpu, std::uint32_t ctx) {
    CpuRuntime& rt = m.runtime(cpu->id);
    const ThreadAbi& a = abi(m, cpu->id);
    const std::uint32_t cpsr = rd32(cpu, ctx + a.context_cpsr_offset);
    rt.banks.switch_mode(*cpu, kModeSys);
    for (unsigned reg = 0; reg < 13; ++reg)
        cpu->r[reg] = rd32(cpu, ctx + a.context_regs_offset + reg * 4);
    rt.banks.write_user(*cpu, 13, rd32(cpu, ctx + a.context_regs_offset + 13 * 4));
    rt.banks.write_user(*cpu, 14, rd32(cpu, ctx + a.context_regs_offset + 14 * 4));
    rt.banks.sp_lr[static_cast<unsigned>(Bank::Svc)][0] = rd32(cpu, ctx + a.context_svc_sp_offset);
    rt.banks.write_cpsr(*cpu, cpsr, 0xf);
    rt.current_pc = rd32(cpu, ctx + a.context_pc_offset) - a.continuation_bias;
}

constexpr std::uint32_t kDivNumer = 0x04000290;
constexpr std::uint32_t kDivControl = 0x04000280;
constexpr std::uint32_t kSqrtParam = 0x040002B8;
constexpr std::uint32_t kSqrtControl = 0x040002B0;

void save_math_state(Cpu* cpu, std::uint32_t ctx) {
    const ThreadAbi& a = abi(cpu);
    for (std::uint32_t offset = 0; offset < 0x10; offset += 4)
        wr32(cpu, ctx + a.context_math_offset + a.math_div_numer_offset + offset,
             rd32(cpu, kDivNumer + offset));
    for (std::uint32_t offset = 0; offset < 8; offset += 4)
        wr32(cpu, ctx + a.context_math_offset + a.math_sqrt_param_offset + offset,
             rd32(cpu, kSqrtParam + offset));
    wr16(cpu, ctx + a.context_math_offset + a.math_div_control_offset,
         rd16(cpu, kDivControl) & 3u);
    wr16(cpu, ctx + a.context_math_offset + a.math_sqrt_control_offset,
         rd16(cpu, kSqrtControl) & 1u);
}

void restore_math_state(Cpu* cpu, std::uint32_t ctx) {
    const ThreadAbi& a = abi(cpu);
    for (std::uint32_t offset = 0; offset < 0x10; offset += 4)
        wr32(cpu, kDivNumer + offset,
             rd32(cpu, ctx + a.context_math_offset + a.math_div_numer_offset + offset));
    for (std::uint32_t offset = 0; offset < 8; offset += 4)
        wr32(cpu, kSqrtParam + offset,
             rd32(cpu, ctx + a.context_math_offset + a.math_sqrt_param_offset + offset));
    wr16(cpu, kDivControl,
         rd16(cpu, ctx + a.context_math_offset + a.math_div_control_offset) & 3u);
    wr16(cpu, kSqrtControl,
         rd16(cpu, ctx + a.context_math_offset + a.math_sqrt_control_offset) & 1u);
}

// Resume (or start) the guest thread whose context frame is `ctx`, parking
// the running fiber on `own_ctx` (0: the running thread is terminated and is
// never resumed). Returns when the running fiber is resumed.
std::uint32_t context_guest_sp(Cpu* cpu, std::uint32_t ctx) {
    const ThreadAbi& a = abi(cpu);
    const std::uint32_t cpsr = rd32(cpu, ctx + a.context_cpsr_offset);
    return (cpsr & kCpsrModeMask) == kModeSvc
        ? rd32(cpu, ctx + a.context_svc_sp_offset)
        : rd32(cpu, ctx + a.context_regs_offset + 13 * 4);
}
bool stack_checks_enabled() {
#ifndef NDEBUG
    return true;
#else
    static const bool enabled = [] {
        const char* value = std::getenv("DSR_CHECK_THREAD_STACKS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#endif
}

bool thread_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DSR_TRACE_THREADS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

int fiber_id(const CpuRuntime& rt, const Fiber* fiber) {
    if (!fiber) return -1;
    if (fiber == rt.fiber.get()) return 0;
    for (std::size_t i = 0; i < rt.thread_fibers.size(); ++i) {
        if (fiber == rt.thread_fibers[i].get()) return static_cast<int>(i + 1);
    }
    return -1;
}

void trace_thread_event(Cpu* cpu, const char* event,
                        std::uint32_t outgoing, std::uint32_t incoming) {
    if (!thread_trace_enabled()) return;
    Machine::Impl& m = MachineAccess::impl(cpu);
    const CpuRuntime& rt = m.runtime(cpu->id);
    const std::uint32_t outgoing_sp =
        outgoing ? context_guest_sp(cpu, outgoing) : 0;
    const std::uint32_t incoming_sp =
        incoming ? context_guest_sp(cpu, incoming) : 0;
    const std::uint64_t frame =
        m.total_ticks / (detail::kLineTicks * detail::kLinesPerFrame);
    std::fprintf(stderr,
                 "[dsr-thread] frame=%llu cpu=%u event=%s out=%08x/%08x in=%08x/%08x pc=%08x fiber=%d host=%p\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned>(cpu->id), event, outgoing, outgoing_sp,
                 incoming, incoming_sp, rt.current_pc,
                 fiber_id(rt, Fiber::current()),
                 static_cast<const void*>(Fiber::current()));
}

struct GuestStackInfo {
    std::uint32_t context;
    std::uint32_t sp;
    std::uint32_t top;
    std::uint32_t bottom;
    bool valid;
};

GuestStackInfo guest_stack_info(Cpu* cpu, const ThreadAbi& a,
                                std::uint32_t context, std::uint32_t sp) {
    if (!context || !a.stack_top_offset || !a.stack_bottom_offset)
        return {context, sp, 0, 0, false};
    const std::uint32_t top = rd32(cpu, context + a.stack_top_offset);
    const std::uint32_t bottom = rd32(cpu, context + a.stack_bottom_offset);
    return {context, sp, top, bottom, top < bottom};
}

[[noreturn]] void stack_invariant_failure(Cpu* cpu, const char* event,
                                          const GuestStackInfo& current,
                                          const GuestStackInfo& peer) {
    Machine::Impl& m = MachineAccess::impl(cpu);
    const CpuRuntime& rt = m.runtime(cpu->id);
    const std::uint64_t frame =
        m.total_ticks / (detail::kLineTicks * detail::kLinesPerFrame);
    std::fprintf(stderr,
                 "[dsr] stack invariant frame=%llu cpu=%u event=%s pc=%08x fiber=%d host=%p "
                 "ctx=%08x sp=%08x bounds=[%08x,%08x] peer=%08x sp=%08x bounds=[%08x,%08x]\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned>(cpu->id), event, rt.current_pc,
                 fiber_id(rt, Fiber::current()),
                 static_cast<const void*>(Fiber::current()), current.context,
                 current.sp, current.top, current.bottom, peer.context, peer.sp,
                 peer.top, peer.bottom);
    std::fflush(stderr);
#ifndef NDEBUG
    assert(false && "guest thread stack identity invariant failed");
#endif
    thread_fatal(cpu, "guest thread stack identity invariant failed",
                 current.context);
}

void validate_context_stack(Cpu* cpu, const ThreadAbi& a,
                            std::uint32_t context, std::uint32_t sp,
                            std::uint32_t related_context, const char* event) {
    if (!stack_checks_enabled() || !context || !a.stack_top_offset ||
        !a.stack_bottom_offset)
        return;

    const GuestStackInfo current = guest_stack_info(cpu, a, context, sp);
    const std::uint32_t active =
        detail::current_thread_context(MachineAccess::impl(cpu), cpu->id);
    if (!current.valid || sp < current.top || sp > current.bottom) {
        const std::uint32_t peer_context =
            related_context && related_context != context ? related_context : active;
        const GuestStackInfo peer =
            guest_stack_info(cpu, a, peer_context,
                             peer_context ? context_guest_sp(cpu, peer_context) : 0);
        stack_invariant_failure(cpu, event, current, peer);
    }

    const auto check_peer = [&](std::uint32_t peer_context) {
        if (!peer_context || peer_context == context) return;
        const GuestStackInfo peer =
            guest_stack_info(cpu, a, peer_context, context_guest_sp(cpu, peer_context));
        if (peer.valid && current.top < peer.bottom && peer.top < current.bottom)
            stack_invariant_failure(cpu, event, current, peer);
    };

    check_peer(related_context);
    check_peer(active);

    const std::uint32_t head = rd32(cpu, a.thread_info + a.info_list_offset);
    std::uint32_t listed = head;
    for (unsigned count = 0; listed && count < 512; ++count) {
        check_peer(listed);
        const std::uint32_t next = rd32(cpu, listed + a.list_next_offset);
        if (next == listed) break;
        listed = next;
    }

    const CpuRuntime& rt = MachineAccess::impl(cpu).runtime(cpu->id);
    for (const auto& [parked_context, parked] : rt.parked) {
        (void)parked;
        check_peer(parked_context);
    }
    for (const auto& [fiber, saved_context] : rt.saved_ctx) {
        (void)fiber;
        check_peer(saved_context);
    }
}
// Resume (or start) the guest thread whose context frame is `ctx`, parking
// the running fiber on `own_ctx` (0: the running thread is terminated and is
// never resumed). Returns when the running fiber is resumed.
void transfer_to(Machine::Impl& m, Cpu* cpu, std::uint32_t own_ctx, std::uint32_t ctx) {
    const CpuId id = cpu->id;
    CpuRuntime& rt = m.runtime(id);
    Fiber* self = Fiber::current();
    const ThreadAbi& a = abi(m, id);
    const std::uint32_t own_sp = own_ctx ? context_guest_sp(cpu, own_ctx) : 0;
    if (own_ctx == ctx) {
        validate_context_stack(cpu, a, ctx, context_guest_sp(cpu, ctx), 0,
                               "same-context resume");
        trace_thread_event(cpu, "same-context", own_ctx, ctx);
        load_context(m, cpu, ctx);
        return;
    }
    validate_context_stack(cpu, a, own_ctx, own_sp, ctx, "park");
    validate_context_stack(cpu, a, ctx, context_guest_sp(cpu, ctx), own_ctx,
                           "resume");
    trace_thread_event(cpu, "fiber-switch", own_ctx, ctx);


    Fiber* target = nullptr;
    if (const auto it = rt.parked.find(ctx); it != rt.parked.end()) {
        if (it->second.continuation == rd32(cpu, ctx + a.context_pc_offset) &&
            it->second.guest_sp == context_guest_sp(cpu, ctx)) {
            target = it->second.fiber;
        } else {
            rt.dead_fibers.push_back(it->second.fiber); // context was reinitialised
        }
        rt.parked.erase(it);
    }

    if (own_ctx) {
        rt.parked[own_ctx] = ParkedThread{self, rd32(cpu, own_ctx + a.context_pc_offset), own_sp};
    } else if (self) {
        rt.dead_fibers.push_back(self);
    }

    if (!target) {
        if (rd32(cpu, ctx + a.context_pc_offset) == a.continuation) {
            thread_fatal(cpu, "restoring a saved context whose fiber is gone", ctx);
        }
        Machine::Impl* mp = &m;
        rt.thread_fibers.push_back(std::make_unique<Fiber>([mp, id, ctx] {
            Cpu* c = &mp->runtime(id).cpu;
            const ThreadAbi& layout = abi(*mp, id);
            load_context(*mp, c, ctx);
            const std::uint32_t entry =
                (rd32(c, ctx + layout.context_pc_offset) - layout.continuation_bias) |
                ((c->cpsr_ctrl & kCpsrT) ? 1u : 0u);
            const std::uint32_t exit_proc = c->r[14];
            dsr_call(c, entry);
            // The entry returned: fall into the context's initial lr (the
            // configured thread-exit procedure), which never returns.
            dsr_call(c, exit_proc);
            thread_fatal(c, "thread exit procedure returned", ctx);
        }));
        target = rt.thread_fibers.back().get();
    }

    rt.active_fiber = target;
    Fiber::switch_to(*target);

    // Resumed inside this RestoreContext or checkpoint frame.
    rt.active_fiber = self;
    load_context(m, cpu, own_ctx);
    if (cpu->r[13] != own_sp)
        thread_fatal(cpu, "resumed guest SP differs from its saved context", own_ctx);
}

void write_context_frame(Cpu* cpu, std::uint32_t ctx, std::uint32_t continuation,
                         bool save_context_return) {
    CpuRuntime& rt = MachineAccess::impl(cpu).runtime(cpu->id);
    const ThreadAbi& a = abi(cpu);
    const std::uint32_t saved_cpsr = rt.banks.cpsr(*cpu);
    wr32(cpu, ctx + a.context_cpsr_offset, saved_cpsr);
    wr32(cpu, ctx + a.context_regs_offset,
         save_context_return ? 1u : cpu->r[0]);
    // The supported save contract preserves a write cursor in r1 and CPSR in r2.
    for (unsigned reg = 1; reg < 13; ++reg) {
        std::uint32_t value = rt.banks.read_user(*cpu, reg);
        if (save_context_return && reg == 1) value = ctx + a.context_regs_offset;
        if (save_context_return && reg == 2) value = saved_cpsr;
        wr32(cpu, ctx + a.context_regs_offset + reg * 4, value);
    }
    wr32(cpu, ctx + a.context_regs_offset + 13 * 4, rt.banks.read_user(*cpu, 13));
    wr32(cpu, ctx + a.context_regs_offset + 14 * 4, rt.banks.read_user(*cpu, 14));
    wr32(cpu, ctx + a.context_pc_offset, continuation);
    const bool svc = (cpu->cpsr_ctrl & kCpsrModeMask) == kModeSvc;
    wr32(cpu, ctx + a.context_svc_sp_offset,
         svc ? cpu->r[13] : rt.banks.sp_lr[static_cast<unsigned>(Bank::Svc)][0]);
}

template <CpuId Id>
void save_context_override(Cpu* cpu) {
    const ThreadAbi& a = abi(cpu);
    const std::uint32_t ctx = cpu->r[0];
    CpuRuntime& rt = MachineAccess::impl(cpu).runtime(Id);
    if (a.math_state_enabled) save_math_state(cpu, ctx);
    write_context_frame(cpu, ctx, a.continuation, true);
    validate_context_stack(cpu, a, ctx, context_guest_sp(cpu, ctx),
                           detail::current_thread_context(
                               MachineAccess::impl(cpu), Id),
                           "SaveContext");
    trace_thread_event(cpu, "SaveContext", ctx, 0);
    Fiber* self = Fiber::current();
    if (!self) thread_fatal(cpu, "SaveContext ran outside its CPU fiber", ctx);
    rt.saved_ctx[self] = ctx;
    cpu->r[2] = rt.banks.cpsr(*cpu);
    cpu->r[0] = 0;
    DSR_RET(cpu, cpu->r[14]);
}

template <CpuId Id>
void restore_context_override(Cpu* cpu) {
    Machine::Impl& m = MachineAccess::impl(cpu);
    CpuRuntime& rt = m.runtime(Id);
    const ThreadAbi& a = abi(m, Id);
    const std::uint32_t ctx = cpu->r[0];
    const std::uint32_t caller_lr = cpu->r[14];
    std::uint32_t own = 0;
    if (const auto it = rt.saved_ctx.find(Fiber::current()); it != rt.saved_ctx.end()) {
        own = it->second;
        rt.saved_ctx.erase(it);
    }
    trace_thread_event(cpu, "RestoreContext", own, ctx);
    if (a.math_state_enabled) restore_math_state(cpu, ctx);
    transfer_to(m, cpu, own, ctx);
    // The resumed thread's lr is that of its SaveContext call; this host
    // frame returns to the RestoreContext call site (same SwitchContext).
    dsr_note_return(cpu, caller_lr);
}

void irq_switch_override(Cpu* cpu) {
    const ThreadAbi& a = abi(cpu);
    const GuestCpuState saved = capture_guest_state(cpu);

    std::uint32_t sleeping = rd32(cpu, a.alarm_queue + a.alarm_head_offset);
    if (sleeping != 0) {
        do {
            const std::uint32_t next = rd32(cpu, sleeping + a.alarm_next_offset);
            wr32(cpu, sleeping + a.state_offset, a.ready_state);
            wr32(cpu, sleeping + a.alarm_blocked_queue_offset, 0);
            wr32(cpu, sleeping + a.alarm_prev_offset, 0);
            wr32(cpu, sleeping + a.alarm_next_offset, 0);
            sleeping = next;
        } while (sleeping != 0);
        wr32(cpu, a.alarm_queue + a.alarm_head_offset, 0);
        wr32(cpu, a.alarm_queue + a.alarm_tail_offset, 0);
        wr16(cpu, a.thread_info + a.info_flag_offset, 1);
    }

    if (rd16(cpu, a.thread_info + a.info_flag_offset) != 0) {
        wr16(cpu, a.thread_info + a.info_flag_offset, 0);
        std::uint32_t incoming = rd32(cpu, a.thread_info + a.info_list_offset);
        while (incoming != 0 && rd16(cpu, incoming + a.state_offset) != a.ready_state)
            incoming = rd32(cpu, incoming + a.list_next_offset);
        const std::uint32_t outgoing = rd32(cpu, a.thread_info + a.info_active_offset);
        if (incoming != 0 && incoming != outgoing) {
            const std::uint32_t callback = rd32(cpu, a.thread_info + a.info_callback_offset);
            if (callback != 0)
                call_guest_preserving(cpu, callback, outgoing, incoming, true,
                                      a.irq_callback_lr, true);
            wr32(cpu, a.thread_info + a.info_active_offset, incoming);
        }
    }

    restore_guest_state(cpu, saved);
}
void install_one(Machine::Impl& m, CpuId id, std::int32_t module,
                 std::uint32_t addr, GuestFn fn) {
    const FnTable& table = id == CpuId::Arm9 ? m.config.arm9_table : m.config.arm7_table;
    for (std::size_t i = 0; i < table.count; ++i) {
        const FnEntry& entry = table.entries[i];
        if (entry.addr == addr && entry.module == module && entry.slot) {
            assert(m.thread_override_count < m.thread_override_slots.size());
            m.thread_override_slots[m.thread_override_count++] = {entry.slot, *entry.slot, fn};
            *entry.slot = fn;
            return;
        }
    }
    if (table.count > 64) {
        std::fprintf(stderr, "[dsr] warning: %s thread primitive 0x%08x (module %d) not in the function table\n",
                     id == CpuId::Arm9 ? "ARM9" : "ARM7", addr, module);
    }
}

} // namespace

std::uint32_t current_thread_context(Machine::Impl& m, CpuId id) {
    const auto& layout = id == CpuId::Arm9 ? m.config.guest_abi.arm9 : m.config.guest_abi.arm7;
    return layout ? m.read(id, layout->thread_info + layout->info_active_offset, 32) : 0;
}

void irq_thread_switch(Machine::Impl& m, Cpu* cpu, std::uint32_t old_ctx, std::uint32_t new_ctx) {
    CpuRuntime& rt = m.runtime(cpu->id);
    Fiber* self = Fiber::current();
    if (!self || old_ctx == 0 || new_ctx == 0)
        thread_fatal(cpu, "IRQ switch has no active guest fiber/context", old_ctx);

    // enter_irq has already restored the interrupted CPU frame.
    const ThreadAbi& a = abi(m, cpu->id);
    trace_thread_event(cpu, "IRQ-switch", old_ctx, new_ctx);
    write_context_frame(cpu, old_ctx, rt.current_pc + a.continuation_bias, false);
    if (a.math_state_enabled) {
        save_math_state(cpu, old_ctx);
        restore_math_state(cpu, new_ctx);
    }
    rt.saved_ctx.erase(self);
    transfer_to(m, cpu, old_ctx, new_ctx);
}

void install_thread_overrides(Machine::Impl& m) {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        const auto& layout = id == CpuId::Arm9 ? m.config.guest_abi.arm9 : m.config.guest_abi.arm7;
        if (!layout) {
            std::fprintf(stderr, "[dsr] %s thread overrides disabled (no ABI configuration supplied)\n",
                         id == CpuId::Arm9 ? "ARM9" : "ARM7");
            continue;
        }
        const ThreadAbi& a = *layout;
        install_one(m, id, a.save_module, a.save, id == CpuId::Arm9
                    ? &save_context_override<CpuId::Arm9> : &save_context_override<CpuId::Arm7>);
        install_one(m, id, a.restore_module, a.restore, id == CpuId::Arm9
                    ? &restore_context_override<CpuId::Arm9> : &restore_context_override<CpuId::Arm7>);
        install_one(m, id, a.irq_module, a.irq_switch, &irq_switch_override);
    }
}

void uninstall_thread_overrides(Machine::Impl& m) {
    while (m.thread_override_count != 0) {
        const auto& saved = m.thread_override_slots[--m.thread_override_count];
        if (*saved.slot == saved.installed) *saved.slot = saved.original;
    }
}

void reset_thread_overrides(Machine::Impl& m) {
    for (auto& rt : m.cpus) {
        rt.parked.clear();
        rt.saved_ctx.clear();
        rt.dead_fibers.clear();
        rt.thread_fibers.clear();
        rt.active_fiber = nullptr;
    }
}

} // namespace dsr::detail
