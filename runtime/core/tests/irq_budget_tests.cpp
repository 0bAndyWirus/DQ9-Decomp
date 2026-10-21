#include "core_internal.h"
#include "dsr/rt.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace dsr::tests {
namespace {
constexpr std::uint32_t kGuest = 0x02001000;
constexpr std::uint32_t kHandler = 0x02002000;
constexpr std::uint32_t kRequest = 1u << 5;
int failures = 0;
std::array<unsigned, 2> guest_cycles{};
std::array<unsigned, 2> handler_cycles{};
std::array<unsigned, 2> handler_calls{};
std::array<std::int64_t, 2> handler_budget{};
enum class Scenario { Count, EnableIme, EnableIe, RawYield, IntrWaitHalt, IntrWaitPending };
Scenario scenario = Scenario::Count;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "[irq-budget-test] FAIL: %s\n", message);
    ++failures;
}

std::uint32_t check_word(CpuId id) {
    return id == CpuId::Arm9 ? 0x00803ff8u : 0x0380fff8u;
}

void handler(Cpu* cpu) {
    const unsigned ci = static_cast<unsigned>(cpu->id);
    handler_budget[ci] = cpu->budget;
    ++handler_calls[ci];
    cpu->budget -= 3;
    handler_cycles[ci] += 3;
    if (cpu->budget <= 0) dsr_checkpoint(cpu, kHandler);
    wr32(cpu, 0x04000214, kRequest);
}

void guest(Cpu* cpu) {
    const unsigned ci = static_cast<unsigned>(cpu->id);
    if (scenario == Scenario::RawYield) Fiber::yield();
    if (scenario == Scenario::EnableIme || scenario == Scenario::EnableIe) {
        cpu->budget -= 2;
        guest_cycles[ci] += 2;
        cpu->m->raise_irq(cpu->id, 5);
        const std::uint32_t reg = scenario == Scenario::EnableIme ? 0x04000208u : 0x04000210u;
        const std::uint32_t value = scenario == Scenario::EnableIme ? 1u : kRequest;
        wr32(cpu, reg, value);
        wr32(cpu, reg, value); // repeated forcing must not reserve the same grant twice
        cpu->budget -= 2; // instructions charged between the force and the checkpoint
        guest_cycles[ci] += 2;
    }
    if (scenario == Scenario::IntrWaitHalt || scenario == Scenario::IntrWaitPending) {
        cpu->m->raise_irq(cpu->id, 5);
        if (scenario == Scenario::IntrWaitHalt) wr32(cpu, 0x04000214, kRequest);
        detail::MachineAccess::impl(cpu).intr_wait(cpu, 1u, true, kGuest);
    }
    for (;;) {
        if (cpu->budget <= 0) dsr_checkpoint(cpu, kGuest);
        --cpu->budget;
        ++guest_cycles[ci];
    }
}

template<class Body>
void with_machine(CpuId id, Scenario selected, Body body) {
    GuestFn guest_slot = guest;
    GuestFn handler_slot = handler;
    const std::int32_t module = id == CpuId::Arm9 ? kModuleArm9Main : kModuleArm7MainRam;
    FnEntry entries[] = {
        {kGuest, 4, module, 0, 0, &guest_slot, "irq_budget_guest"},
        {kHandler, 4, module, 0, 0, &handler_slot, "irq_budget_handler"},
    };
    MachineConfig config;
    config.direct_boot = false;
    if (id == CpuId::Arm9) config.arm9_table = {entries, 2};
    else config.arm7_table = {entries, 2};
    Machine machine(config);
    Cpu& cpu = machine.cpu(id);
    cpu.cpsr_ctrl = kModeIrq;
    cpu.r[13] = id == CpuId::Arm9 ? 0x00803fa0u : 0x0380ffa0u;
    machine.write32(id, id == CpuId::Arm9 ? 0x00803ffcu : 0x0380fffcu, kHandler);
    machine.write32(id, 0x04000208, selected == Scenario::EnableIme ? 0 : 1);
    machine.write32(id, 0x04000210, selected == Scenario::EnableIe ? 0 : kRequest);
    machine.start_cpu(id, kGuest);
    guest_cycles = {};
    handler_cycles = {};
    handler_calls = {};
    handler_budget = {};
    scenario = selected;
    body(machine, detail::MachineAccess::impl(&cpu), cpu, static_cast<unsigned>(id));
}

void test_counting_guest_and_handler() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        for (bool resumed : {false, true}) {
            with_machine(id, Scenario::Count, [resumed](Machine& machine, Machine::Impl& impl, Cpu&, unsigned ci) {
                const unsigned rate = ci == 0 ? 2u : 1u;
                if (resumed) impl.run_cpu_slice(ci, 1);
                const unsigned before = guest_cycles[ci];
                machine.raise_irq(static_cast<CpuId>(ci), 5);
                impl.run_cpu_slice(ci, 8);
                check(handler_calls[ci] == 1, "pending IRQ enters its handler once");
                check(handler_budget[ci] == 8 * rate, "handler receives the positive grant before guest execution");
                check(guest_cycles[ci] - before + handler_cycles[ci] == 8 * rate,
                      "IRQ dispatch charges handler and guest against the same complete slice");
                check(guest_cycles[ci] > before, "IRQ handler does not force a second yield of a funded slice");
                check((machine.read32(static_cast<CpuId>(ci), 0x04000214) & kRequest) == 0,
                      "funded handler acknowledges the pending request in the same slice");
            });
        }
    }
}

void test_register_enable_and_instruction_debt() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        for (Scenario selected : {Scenario::EnableIme, Scenario::EnableIe}) {
            with_machine(id, selected, [](Machine&, Machine::Impl& impl, Cpu&, unsigned ci) {
                const unsigned grant = ci == 0 ? 16u : 8u;
                impl.run_cpu_slice(ci, 8);
                check(handler_budget[ci] == grant - 4,
                      "IME/IE forcing preserves charges before and after the first force");
                check(handler_calls[ci] == 1 && guest_cycles[ci] + handler_cycles[ci] == grant,
                      "repeated IME/IE writes neither lose nor duplicate positive cycles");
            });
        }
    }
}

void test_target_cpu_forcing() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        with_machine(id, Scenario::Count, [](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned ci) {
            Cpu& source = machine.cpu(static_cast<CpuId>(ci ^ 1));
            source.budget = 29;
            cpu.budget = 73;
            machine.raise_irq(cpu.id, 5);
            machine.raise_irq(cpu.id, 5);
            machine.write32(cpu.id, 0x04000208, 1);
            machine.write32(cpu.id, 0x04000210, kRequest);
            check(cpu.budget == 0 && source.budget == 29,
                  "set_irq forces only its target CPU and repeated forces retain the sentinel");
            cpu.budget -= 11;
            impl.checkpoint(&cpu, kGuest);
            check(handler_budget[ci] == 62 && cpu.budget == 59,
                  "set_irq restores its target grant additively over subsequent charged debt");
            impl.checkpoint(&cpu, kGuest);
            check(cpu.budget == 59 && source.budget == 29,
                  "a second checkpoint cannot restore a consumed reserve again");
        });
    }
}

void test_negative_debt_survives_forcing() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        with_machine(id, Scenario::Count, [](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned ci) {
            const unsigned rate = ci == 0 ? 2u : 1u;
            cpu.budget = -static_cast<std::int64_t>(3 * rate + 1);
            machine.raise_irq(cpu.id, 5);
            machine.write32(cpu.id, 0x04000208, 1);
            machine.write32(cpu.id, 0x04000210, kRequest);
            check(cpu.budget == -static_cast<std::int64_t>(3 * rate + 1),
                  "set_irq and IME/IE forcing do not erase existing negative debt");
            impl.run_cpu_slice(ci, 3);
            check(cpu.budget == -1 && handler_calls[ci] == 0 && guest_cycles[ci] == 0,
                  "scheduler repays existing debt before dispatching a pending IRQ");
            impl.run_cpu_slice(ci, 8);
            check(guest_cycles[ci] + handler_cycles[ci] == 8 * rate - 1,
                  "IRQ carry adds to the funded slice without forgiving its remaining debt");
        });
    }
}

void test_reserve_survives_without_checkpoint() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        with_machine(id, Scenario::RawYield, [](Machine& machine, Machine::Impl& impl, Cpu&, unsigned ci) {
            const unsigned grant = ci == 0 ? 16u : 8u;
            machine.raise_irq(static_cast<CpuId>(ci), 5);
            impl.run_cpu_slice(ci, 8);
            check(handler_calls[ci] == 0 && guest_cycles[ci] == 0,
                  "raw fiber yield leaves forced cycles unconsumed");
            impl.run_cpu_slice(ci, 8);
            check(handler_calls[ci] == 1 && guest_cycles[ci] + handler_cycles[ci] == 2 * grant,
                  "unconsumed reserve survives the next scheduler grant and is restored once");
            impl.run_cpu_slice(ci, 8);
            check(guest_cycles[ci] + handler_cycles[ci] == 3 * grant,
                  "later slices do not regrant an already restored reserve");
        });
    }
}

void test_idle_discards_reserve() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        with_machine(id, Scenario::Count, [](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned ci) {
            cpu.budget = 73;
            machine.raise_irq(cpu.id, 5);
            machine.write32(cpu.id, 0x04000214, kRequest);
            impl.halt(&cpu);
            impl.checkpoint(&cpu, kGuest);
            check(cpu.budget == 0 && impl.cpus[ci].halted,
                  "BIOS halt discards unused IRQ reserve rather than funding idle time");
        });
        for (Scenario selected : {Scenario::IntrWaitHalt, Scenario::IntrWaitPending}) {
            with_machine(id, selected, [](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned ci) {
                const unsigned grant = ci == 0 ? 16u : 8u;
                impl.run_cpu_slice(ci, 8);
                check(guest_cycles[ci] == 0 && handler_calls[ci] == 0,
                      "IntrWait idles without consuming the forced reserve");
                machine.write32(cpu.id, check_word(cpu.id), 1);
                machine.write32(cpu.id, 0x04000208, 0);
                machine.raise_irq(cpu.id, 5);
                impl.run_cpu_slice(ci, 8);
                check(guest_cycles[ci] == grant && handler_calls[ci] == 0,
                      "IntrWait resumes with only its new grant after either idle path");
            });
        }
    }
    for (std::uint8_t haltcnt : {0x00, 0x80}) {
        with_machine(CpuId::Arm7, Scenario::Count, [haltcnt](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned) {
            cpu.budget = 73;
            machine.raise_irq(cpu.id, 5);
            machine.write32(cpu.id, 0x04000214, kRequest);
            machine.write8(cpu.id, 0x04000301, haltcnt);
            impl.checkpoint(&cpu, kGuest);
            check(cpu.budget == 0, "HALTCNT halt/stop discards unused IRQ reserve");
        });
    }
}

void test_cpu_budget_lifecycle() {
    for (CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        with_machine(id, Scenario::Count, [](Machine& machine, Machine::Impl& impl, Cpu& cpu, unsigned ci) {
            cpu.budget = 73;
            machine.raise_irq(cpu.id, 5);
            machine.write32(cpu.id, 0x04000214, kRequest);
            machine.start_cpu(cpu.id, kGuest);
            impl.run_cpu_slice(ci, 1);
            check(guest_cycles[ci] == 73 + (ci == 0 ? 2u : 1u),
                  "starting a CPU fiber preserves unused per-CPU IRQ funding, like charged debt");
            cpu.budget = 73;
            machine.raise_irq(cpu.id, 5);
            impl.reset();
            machine.start_cpu(cpu.id, kGuest);
            guest_cycles = {};
            impl.run_cpu_slice(ci, 1);
            check(guest_cycles[ci] == (ci == 0 ? 2u : 1u),
                  "full machine reset clears unused IRQ reserve along with charged debt");
        });
    }
}
} // namespace

int run_irq_budget_tests() {
    test_counting_guest_and_handler();
    test_register_enable_and_instruction_debt();
    test_target_cpu_forcing();
    test_negative_debt_survives_forcing();
    test_reserve_survives_without_checkpoint();
    test_idle_discards_reserve();
    test_cpu_budget_lifecycle();
    std::fprintf(stderr, "[irq-budget-test] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
} // namespace dsr::tests

int main() { return dsr::tests::run_irq_budget_tests(); }
