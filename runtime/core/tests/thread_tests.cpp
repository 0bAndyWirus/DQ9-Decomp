// Guest thread tests: synthetic generated-code ABI functions exercise the
// context-switch, IRQ-return, and thread-exit paths on both CPUs.
#include "../src/core_internal.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace dsr::tests {
namespace {

// Entirely invented addresses and deliberately different layouts for each CPU.
constexpr std::uint32_t kArm9Save = 0x02110000;
constexpr std::uint32_t kArm9Restore = 0x02110100;
constexpr std::uint32_t kArm9ThreadInfo = 0x02130000;
constexpr std::uint32_t kArm7Save = 0x02210000;
constexpr std::uint32_t kArm7Restore = 0x02210100;
constexpr std::uint32_t kArm7ThreadInfo = 0x02230000;

constexpr std::uint32_t kArm9IrqVector = 0x02120000;
constexpr std::uint32_t kArm9IrqSwitch = 0x02120100;
constexpr std::uint32_t kArm7IrqVector = 0x02220000;
constexpr std::uint32_t kArm7IrqSwitch = 0x02220100;
constexpr std::uint32_t kSwitchCallback[2] = {0x02121000, 0x02221000};
constexpr std::uint32_t kArm7AlarmQueue = 0x02231000;
constexpr std::uint32_t kArm9AlarmQueue = 0x02131000;
constexpr ThreadAbi synthetic_abi(CpuId id) {
    ThreadAbi a{};
    const bool arm9 = id == CpuId::Arm9;
    a.save = arm9 ? kArm9Save : kArm7Save;
    a.restore = arm9 ? kArm9Restore : kArm7Restore;
    a.continuation = a.save + 0x200;
    a.irq_switch = arm9 ? kArm9IrqSwitch : kArm7IrqSwitch;
    a.save_module = arm9 ? kModuleArm9Main : kModuleArm7Wram;
    a.restore_module = a.save_module;
    a.irq_module = arm9 ? kModuleArm9Itcm : kModuleArm7Wram;
    a.thread_info = arm9 ? kArm9ThreadInfo : kArm7ThreadInfo;
    a.alarm_queue = arm9 ? kArm9AlarmQueue : kArm7AlarmQueue;
    a.irq_callback_lr = a.irq_switch + 0x100;
    a.context_cpsr_offset = arm9 ? 0x20 : 0x10;
    a.context_regs_offset = arm9 ? 0x30 : 0x24;
    a.context_pc_offset = arm9 ? 0x78 : 0x70;
    a.context_svc_sp_offset = arm9 ? 0x80 : 0x78;
    a.continuation_bias = 8;
    a.math_state_enabled = arm9 ? 1 : 0;
    a.context_math_offset = 0xD0;
    a.math_div_numer_offset = 0x10;
    a.math_sqrt_param_offset = 4;
    a.math_div_control_offset = 0;
    a.math_sqrt_control_offset = 2;
    a.info_flag_offset = 0x18;
    a.info_active_offset = 0x24;
    a.info_list_offset = 4;
    a.info_callback_offset = 0x2C;
    a.alarm_head_offset = 8;
    a.alarm_tail_offset = 0x14;
    a.ready_state = 7;
    a.state_offset = arm9 ? 0x88 : 0x80;
    a.list_next_offset = arm9 ? 0x8C : 0x84;
    a.alarm_blocked_queue_offset = arm9 ? 0xA0 : 0x90;
    a.alarm_prev_offset = arm9 ? 0xA4 : 0x94;
    a.alarm_next_offset = arm9 ? 0xA8 : 0x98;
    a.stack_top_offset = arm9 ? 0xB0 : 0;
    a.stack_bottom_offset = arm9 ? 0xB4 : 0;
    return a;
}
constexpr ThreadAbi kSyntheticAbi[2] = {synthetic_abi(CpuId::Arm9), synthetic_abi(CpuId::Arm7)};

// Fixed hardware IRQ-vector slots and IO registers, not guest ABI locations.
constexpr std::uint32_t kArm9IrqVectorSlot = 0x00803FFC;
constexpr std::uint32_t kArm7IrqVectorSlot = 0x0380FFFC;
constexpr std::uint32_t kIrqIme = 0x04000208;
constexpr std::uint32_t kIrqIe = 0x04000210;
constexpr std::uint32_t kIrqIf = 0x04000214;
constexpr std::uint32_t kContextState[2] = {kSyntheticAbi[0].state_offset, kSyntheticAbi[1].state_offset};
constexpr std::uint32_t kAlarmNext[2] = {kSyntheticAbi[0].alarm_next_offset, kSyntheticAbi[1].alarm_next_offset};
constexpr std::uint32_t kAlarmBlockedQueue[2] = {
    kSyntheticAbi[0].alarm_blocked_queue_offset, kSyntheticAbi[1].alarm_blocked_queue_offset};
constexpr std::uint32_t kAlarmPrev[2] = {kSyntheticAbi[0].alarm_prev_offset, kSyntheticAbi[1].alarm_prev_offset};
constexpr std::uint32_t kSleepingCtx[2] = {0x02100200, 0x02180200};
constexpr std::uint32_t kSvcStack[2] = {0x02102000, 0x02182000};
constexpr std::uint32_t kSysStack[2] = {0x02103000, 0x02183000};
constexpr std::uint32_t kArm9StackTop = kSyntheticAbi[0].stack_top_offset;
constexpr std::uint32_t kArm9StackBottom = kSyntheticAbi[0].stack_bottom_offset;
constexpr std::uint32_t kIrqStack[2] = {0x02105000, 0x02185000};

// Synthetic guest code and data.
constexpr std::uint32_t kArm9Root = 0x02001000;
constexpr std::uint32_t kArm9Child = 0x02001100;
constexpr std::uint32_t kArm9Exit = 0x02001200;
constexpr std::uint32_t kArm7Root = 0x02201000;
constexpr std::uint32_t kArm7Child = 0x02201100;
constexpr std::uint32_t kArm7Exit = 0x02201200;
constexpr std::uint32_t kArm9ReinitChild = 0x02001300;
constexpr std::uint32_t kArm7ReinitChild = 0x02201300;

constexpr std::uint32_t kRootCtx[2] = {0x02100000, 0x02180000};
constexpr std::uint32_t kChildCtx[2] = {0x02100100, 0x02180100};
constexpr std::uint32_t kChildStack[2] = {0x02104000, 0x02184000};
constexpr std::uint32_t kReinitStack[2] = {0x02106000, 0x02186000};

constexpr std::uint32_t kRootUserLr[2] = {0xA9A90000, 0xA7A70000};

struct Log {
    unsigned child_runs[2]{};
    unsigned root_resumes[2]{};
    unsigned irq_resumes[2]{};
    unsigned switch_callbacks[2]{};
    unsigned reinit_starts[2]{};
    unsigned reinit_resumes[2]{};
    unsigned exits[2]{};
    std::uint32_t child_arg[2]{};
    std::uint32_t reinit_arg[2]{};
    std::uint32_t root_r4_after[2]{};
    std::uint32_t child_resume_sp[2]{};

    std::uint32_t irq_resume_pc[2]{};
    std::uint32_t irq_resume_sp[2]{};
};
Log* log_state = nullptr;

void dummy_guest(Cpu*) {}

unsigned idx(Cpu* cpu) { return cpu->id == CpuId::Arm9 ? 0u : 1u; }
const ThreadAbi& test_abi(Cpu* cpu) { return kSyntheticAbi[idx(cpu)]; }
std::uint32_t save_addr(Cpu* cpu) { return cpu->id == CpuId::Arm9 ? kArm9Save : kArm7Save; }
std::uint32_t restore_addr(Cpu* cpu) { return cpu->id == CpuId::Arm9 ? kArm9Restore : kArm7Restore; }
std::uint32_t info_addr(Cpu* cpu) { return cpu->id == CpuId::Arm9 ? kArm9ThreadInfo : kArm7ThreadInfo; }
std::uint32_t irq_switch_addr(Cpu* cpu) {
    return cpu->id == CpuId::Arm9 ? kArm9IrqSwitch : kArm7IrqSwitch;
}
std::uint32_t context_state_offset(Cpu* cpu) { return kContextState[idx(cpu)]; }

void switch_callback(Cpu* cpu) {
    const unsigned i = idx(cpu);
    assert(cpu->r[0] == kRootCtx[i] && cpu->r[1] == kChildCtx[i]);
    assert(cpu->r[14] == test_abi(cpu).irq_callback_lr);
    ++log_state->switch_callbacks[i];
    cpu->r[0] = cpu->r[1] = cpu->r[4] = 0xBAD00000;
    cpu->n = cpu->z = 1;
}

void irq_handler(Cpu* cpu) {
    cpu->m->write32(cpu->id, kIrqIf, 1);
    dsr_call(cpu, irq_switch_addr(cpu));
}

// SwitchContext(outgoing, incoming), as the guest does it.
void switch_context(Cpu* cpu, std::uint32_t outgoing, std::uint32_t incoming) {
    if (rd32(cpu, outgoing + context_state_offset(cpu)) != 2) {
        cpu->r[0] = outgoing;
        dsr_call(cpu, save_addr(cpu));
        if (cpu->r[0] != 0) return; // resumed
    }
    wr32(cpu, info_addr(cpu) + test_abi(cpu).info_active_offset, incoming);
    cpu->r[0] = incoming;
    dsr_call(cpu, restore_addr(cpu));
}

// Populate an initial context using only the invented test layout.
void populate(Cpu* cpu, std::uint32_t ctx, std::uint32_t entry, std::uint32_t arg, std::uint32_t sp,
              std::uint32_t exit_proc) {
    const ThreadAbi& a = test_abi(cpu);
    for (std::uint32_t off = 0; off < 0x100; off += 4) wr32(cpu, ctx + off, 0);
    wr32(cpu, ctx + a.context_cpsr_offset, kModeSys);
    wr32(cpu, ctx + a.context_regs_offset, arg);
    wr32(cpu, ctx + a.context_regs_offset + 13 * 4, sp);
    wr32(cpu, ctx + a.context_regs_offset + 14 * 4, exit_proc);
    wr32(cpu, ctx + a.context_pc_offset, entry + a.continuation_bias);
    wr32(cpu, ctx + a.context_svc_sp_offset, sp - 0x100);
    wr32(cpu, ctx + context_state_offset(cpu), 0);
    if (cpu->id == CpuId::Arm9) {
        wr32(cpu, ctx + kArm9StackTop, sp - 0x800);
        wr32(cpu, ctx + kArm9StackBottom, sp);
    }
}

void child(Cpu* cpu) {
    const unsigned i = idx(cpu);
    log_state->child_arg[i] = cpu->r[0];
    for (int round = 0; round < 3; ++round) {
        ++log_state->child_runs[i];
        if (round == 1) log_state->child_resume_sp[i] = cpu->r[13];

        switch_context(cpu, kChildCtx[i], kRootCtx[i]);
    }
    // Returning from the entry falls into the exit procedure.
}

void reinit_child(Cpu* cpu) {
    const unsigned i = idx(cpu);
    log_state->reinit_arg[i] = cpu->r[0];
    ++log_state->reinit_starts[i];
    switch_context(cpu, kChildCtx[i], kRootCtx[i]);
}

void exit_proc(Cpu* cpu) {
    const unsigned i = idx(cpu);
    ++log_state->exits[i];
    wr32(cpu, kChildCtx[i] + context_state_offset(cpu), 2);
    // Terminated thread: switch to root without saving (never resumed).
    wr32(cpu, info_addr(cpu) + test_abi(cpu).info_active_offset, kRootCtx[i]);
    cpu->r[0] = kRootCtx[i];
    dsr_call(cpu, restore_addr(cpu));
    assert(false && "terminated thread must not resume");
}

void root(Cpu* cpu) {
    const unsigned i = idx(cpu);
    const std::uint32_t exit = cpu->id == CpuId::Arm9 ? kArm9Exit : kArm7Exit;
    const std::uint32_t entry = cpu->id == CpuId::Arm9 ? kArm9Child : kArm7Child;
    const std::uint32_t irq_vector = cpu->id == CpuId::Arm9 ? kArm9IrqVector : kArm7IrqVector;
    const std::uint32_t irq_slot =
        cpu->id == CpuId::Arm9 ? kArm9IrqVectorSlot : kArm7IrqVectorSlot;
    const std::uint32_t root_pc = cpu->id == CpuId::Arm9 ? kArm9Root : kArm7Root;
    Machine::Impl& machine = detail::MachineAccess::impl(cpu);
    CpuRuntime& rt = machine.runtime(cpu->id);

    rt.banks.switch_mode(*cpu, kModeSys);
    cpu->r[13] = kSysStack[i];
    cpu->r[14] = kRootUserLr[i];

    rt.banks.switch_mode(*cpu, kModeSvc);
    cpu->r[13] = kSvcStack[i];
    rt.banks.switch_mode(*cpu, kModeIrq);
    cpu->r[13] = kIrqStack[i];
    rt.banks.switch_mode(*cpu, kModeSvc);

    const std::uint32_t alarm_queue =
        cpu->id == CpuId::Arm9 ? kArm9AlarmQueue : kArm7AlarmQueue;
    const std::uint32_t sleeping = kSleepingCtx[i];
    wr32(cpu, sleeping + context_state_offset(cpu), 0);
    wr32(cpu, sleeping + kAlarmBlockedQueue[i], 0xDEAD0000u + i);
    wr32(cpu, sleeping + kAlarmPrev[i], 0xDEAD1000u + i);
    wr32(cpu, sleeping + kAlarmNext[i], 0);
    const ThreadAbi& a = test_abi(cpu);
    wr32(cpu, alarm_queue + a.alarm_head_offset, sleeping);
    wr32(cpu, alarm_queue + a.alarm_tail_offset, sleeping);
    wr32(cpu, info_addr(cpu) + a.info_active_offset, kRootCtx[i]);
    if (cpu->id == CpuId::Arm9) {
        wr32(cpu, kRootCtx[i] + kArm9StackTop, kSvcStack[i] - 0x1000);
        wr32(cpu, kRootCtx[i] + kArm9StackBottom, kSysStack[i]);
    }
    wr32(cpu, kRootCtx[i] + context_state_offset(cpu), a.ready_state);
    populate(cpu, kChildCtx[i], entry, 0x13572468u + i, kChildStack[i], exit);
    wr32(cpu, kChildCtx[i] + context_state_offset(cpu), a.ready_state);
    wr32(cpu, kChildCtx[i] + a.list_next_offset, 0);
    wr32(cpu, info_addr(cpu) + a.info_list_offset, kChildCtx[i]);
    wr16(cpu, info_addr(cpu) + a.info_flag_offset, 1);
    wr32(cpu, info_addr(cpu) + a.info_callback_offset, kSwitchCallback[i]);
    if (a.math_state_enabled) {
        for (unsigned reg = 0; reg < 4; ++reg)
            wr32(cpu, 0x04000290 + reg * 4, 0x01010101u * (reg + 1));
        wr32(cpu, 0x040002B8, 0x11112222);
        wr32(cpu, 0x040002BC, 0x33334444);
        wr16(cpu, 0x04000280, 1);
        wr16(cpu, 0x040002B0, 1);
    }

    cpu->m->write32(cpu->id, irq_slot, irq_vector);
    cpu->m->write32(cpu->id, kIrqIme, 1);
    cpu->m->write32(cpu->id, kIrqIe, 1);
    cpu->r[4] = 0xC0DEFFFFu;
    cpu->cpsr_ctrl &= ~kCpsrI;
    cpu->m->raise_irq(cpu->id, 0);
    dsr_checkpoint(cpu, root_pc + 4);
    ++log_state->irq_resumes[i];
    log_state->irq_resume_pc[i] = rd32(cpu, kRootCtx[i] + a.context_pc_offset);
    log_state->irq_resume_sp[i] = cpu->r[13];

    for (int round = 0; round < 3; ++round) {
        cpu->r[4] = 0xC0DE0000u + static_cast<std::uint32_t>(round);
        switch_context(cpu, kRootCtx[i], kChildCtx[i]);
        ++log_state->root_resumes[i];
        log_state->root_r4_after[i] = cpu->r[4];
        if (log_state->exits[i] != 0) break;
    }

    const std::uint32_t reinit_entry =
        cpu->id == CpuId::Arm9 ? kArm9ReinitChild : kArm7ReinitChild;
    populate(cpu, kChildCtx[i], reinit_entry, 0x24681357u + i, kReinitStack[i], exit);
    wr32(cpu, kChildCtx[i] + context_state_offset(cpu), a.ready_state);
    switch_context(cpu, kRootCtx[i], kChildCtx[i]);
    ++log_state->reinit_resumes[i];

    for (;;) { // park this CPU like an idle loop
        cpu->budget = 0;
        dsr_checkpoint(cpu, root_pc);
    }
}

struct TestImage {
    std::array<GuestFn, 16> slots9{}, slots7{};
    std::array<FnEntry, 16> entries9{}, entries7{};
    std::size_t n9 = 0, n7 = 0;
    void add(CpuId id, std::uint32_t addr, std::int32_t module, GuestFn fn) {
        if (id == CpuId::Arm9) {
            slots9[n9] = fn;
            entries9[n9] = FnEntry{addr, 4, module, 0, 0x1000 + n9, &slots9[n9], "test"};
            ++n9;
        } else {
            slots7[n7] = fn;
            entries7[n7] = FnEntry{addr, 4, module, 0, 0x2000 + n7, &slots7[n7], "test"};
            ++n7;
        }
    }
};

} // namespace

void run_thread_tests() {
    TestImage image;
    image.add(CpuId::Arm9, kArm9Save, kModuleArm9Main, dummy_guest);
    image.add(CpuId::Arm9, kArm9Restore, kModuleArm9Main, dummy_guest);
    image.add(CpuId::Arm9, kArm9IrqVector, kModuleArm9Itcm, irq_handler);
    image.add(CpuId::Arm9, kArm9IrqSwitch, kModuleArm9Itcm, dummy_guest);
    image.add(CpuId::Arm9, kSwitchCallback[0], kModuleArm9Main, switch_callback);
    image.add(CpuId::Arm9, kArm9Root, kModuleArm9Main, root);
    image.add(CpuId::Arm9, kArm9Child, kModuleArm9Main, child);
    image.add(CpuId::Arm9, kArm9Exit, kModuleArm9Main, exit_proc);
    image.add(CpuId::Arm9, kArm9ReinitChild, kModuleArm9Main, reinit_child);
    image.add(CpuId::Arm7, kArm7Save, kModuleArm7Wram, dummy_guest);
    image.add(CpuId::Arm7, kArm7Restore, kModuleArm7Wram, dummy_guest);
    image.add(CpuId::Arm7, kArm7IrqVector, kModuleArm7Wram, irq_handler);
    image.add(CpuId::Arm7, kArm7IrqSwitch, kModuleArm7Wram, dummy_guest);
    image.add(CpuId::Arm7, kSwitchCallback[1], kModuleArm7Main, switch_callback);
    image.add(CpuId::Arm7, kArm7Root, kModuleArm7Main, root);
    image.add(CpuId::Arm7, kArm7Child, kModuleArm7Main, child);
    image.add(CpuId::Arm7, kArm7Exit, kModuleArm7Main, exit_proc);
    image.add(CpuId::Arm7, kArm7ReinitChild, kModuleArm7Main, reinit_child);

    MachineConfig config{};
    config.direct_boot = false;
    config.arm9_table = FnTable{image.entries9.data(), image.n9};
    config.arm7_table = FnTable{image.entries7.data(), image.n7};
    {
        Machine unconfigured(config);
        assert(image.slots9[0] == dummy_guest && image.slots7[0] == dummy_guest);
        assert(detail::current_thread_context(detail::MachineAccess::impl(&unconfigured.cpu(CpuId::Arm9)),
                                              CpuId::Arm9) == 0);
    }
    {
        MachineConfig only_arm9 = config;
        only_arm9.guest_abi.arm9 = kSyntheticAbi[0];
        Machine configured(only_arm9);
        assert(image.slots9[0] != dummy_guest && image.slots7[0] == dummy_guest);
    }
    assert(image.slots9[0] == dummy_guest && image.slots7[0] == dummy_guest);
    config.guest_abi.arm9 = kSyntheticAbi[0];
    config.guest_abi.arm7 = kSyntheticAbi[1];
    Log log{};
    log_state = &log;
    {
        Machine machine(config);
        machine.start_cpu(CpuId::Arm9, kArm9Root);
        machine.start_cpu(CpuId::Arm7, kArm7Root);
        machine.run_ticks(4096);
        for (unsigned i = 0; i < 2; ++i) {
            const CpuId id = i == 0 ? CpuId::Arm9 : CpuId::Arm7;
            const ThreadAbi& a = kSyntheticAbi[i];
            const std::uint32_t root_pc = i == 0 ? kArm9Root : kArm7Root;
            // IRQ changes active context, executes child, and resumes root.
            assert(log.irq_resumes[i] == 1);
            assert(log.switch_callbacks[i] == 1);
            assert(log.irq_resume_pc[i] == root_pc + 4 + a.continuation_bias);
            assert(log.irq_resume_sp[i] == kSvcStack[i]);
            // Child ran three rounds, exited, and was reinitialized fresh.
            assert(log.child_arg[i] == 0x13572468u + i);
            assert(log.child_runs[i] == 3);
            assert(log.exits[i] == 1);
            if (a.math_state_enabled) {
                for (unsigned reg = 0; reg < 4; ++reg)
                    assert(machine.read32(id, kRootCtx[i] + a.context_math_offset +
                                          a.math_div_numer_offset + reg * 4) == 0x01010101u * (reg + 1));
                assert(machine.read32(id, kRootCtx[i] + a.context_math_offset + a.math_sqrt_param_offset) == 0x11112222);
                assert(machine.read32(id, kRootCtx[i] + a.context_math_offset + a.math_sqrt_param_offset + 4) == 0x33334444);
                assert(machine.read16(id, kRootCtx[i] + a.context_math_offset + a.math_div_control_offset) == 1);
                assert(machine.read16(id, kRootCtx[i] + a.context_math_offset + a.math_sqrt_control_offset) == 1);
            }
            assert(log.root_resumes[i] == 3);
            assert(log.reinit_starts[i] == 1);
            assert(log.reinit_arg[i] == 0x24681357u + i);
            assert(log.reinit_resumes[i] == 1);
            assert(log.child_resume_sp[i] == kChildStack[i]);
            assert(machine.read32(id, kRootCtx[i] + a.context_regs_offset + 13 * 4) == kSysStack[i]);
            assert(machine.read32(id, kRootCtx[i] + a.context_regs_offset + 14 * 4) == kRootUserLr[i]);
            assert(machine.read32(id, kChildCtx[i] + a.context_regs_offset + 13 * 4) == kReinitStack[i]);
            assert(machine.read32(id, kChildCtx[i] + a.context_regs_offset + 14 * 4) ==
                   (i == 0 ? kArm9Exit : kArm7Exit));
            assert(log.root_r4_after[i] == 0xC0DE0002u);
            assert(machine.read32(id, kRootCtx[i] + a.context_pc_offset) == a.continuation);
            assert(machine.read32(id, kRootCtx[i] + a.context_regs_offset) == 1u);
            const std::uint32_t thread_info = i == 0 ? kArm9ThreadInfo : kArm7ThreadInfo;
            assert(machine.read32(id, thread_info + a.info_active_offset) == kRootCtx[i]);
            const std::uint32_t alarm_queue =
                i == 0 ? kArm9AlarmQueue : kArm7AlarmQueue;
            assert(machine.read32(id, alarm_queue + a.alarm_head_offset) == 0);
            assert(machine.read32(id, alarm_queue + a.alarm_tail_offset) == 0);
            assert(machine.read32(id, kSleepingCtx[i] + kContextState[i]) == a.ready_state);
            assert(machine.read32(id, kSleepingCtx[i] + kAlarmBlockedQueue[i]) == 0);
            assert(machine.read32(id, kSleepingCtx[i] + kAlarmPrev[i]) == 0);
            assert(machine.read32(id, kSleepingCtx[i] + kAlarmNext[i]) == 0);
        }
    }
    config.guest_abi = {};
    {
        Machine unconfigured(config);
        assert(image.slots9[0] == dummy_guest && image.slots7[0] == dummy_guest);
        dsr_call(&unconfigured.cpu(CpuId::Arm9), kArm9Save);
        dsr_call(&unconfigured.cpu(CpuId::Arm7), kArm7Save);
    }
    log_state = nullptr;
}

} // namespace dsr::tests
