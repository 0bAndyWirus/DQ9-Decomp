#include "core_internal.h"
#include "swi.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace dsr {
namespace {
std::uint64_t guest_hash(Cpu* cpu, std::uint32_t addr, std::uint32_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::uint32_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint8_t>(rd8(cpu, addr + i));
        hash *= 0x100000001b3ull;
    }
    return hash;
}

const FnEntry* resolve_overlay_hash(Cpu* cpu, const std::vector<const FnEntry*>& candidates,
                                   std::uint32_t& checked_size) {
    checked_size = 0;
    for (const FnEntry* entry : candidates) {
        if (entry->size == 0) continue;
        checked_size = std::max(checked_size, entry->size);
        if (guest_hash(cpu, entry->addr, entry->size) == entry->hash) return entry;
    }
    return nullptr;
}

bool snapshot_matches(Cpu* cpu, std::uint32_t addr,
                      const Machine::Impl::DispatchTarget& target) {
    if (!target.snapshot_valid) return false;
    std::uint32_t offset = 0;
    while (offset < target.checked_size) {
        const auto* page = cpu->page_r[addr >> kPageShift];
        if (!page) return false; // Slow/IO reads must retain their original effects.
        const auto size = std::min(target.checked_size - offset, kPageSize - (addr & kPageMask));
        if (std::memcmp(page + (addr & kPageMask), target.snapshot.data() + offset, size) != 0)
            return false;
        addr += size;
        offset += size;
    }
    return true;
}

void save_snapshot(Cpu* cpu, std::uint32_t addr, Machine::Impl::DispatchTarget& target) {
    target.snapshot_valid = false;
    std::uint32_t offset = 0;
    while (offset < target.checked_size) {
        const auto* page = cpu->page_r[addr >> kPageShift];
        if (!page) return;
        const auto size = std::min(target.checked_size - offset, kPageSize - (addr & kPageMask));
        std::memcpy(target.snapshot.data() + offset, page + (addr & kPageMask), size);
        addr += size;
        offset += size;
    }
    target.snapshot_valid = true;
}

std::uint64_t guest_region_hash(Cpu* cpu, const DispatchRegionEntry& region) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    std::uint32_t addr = region.begin;
    const std::uint32_t end = region.begin + region.size;
    std::size_t range_index = 0;
    while (addr < end) {
        while (range_index < region.data_range_count &&
               region.data_ranges[range_index].begin + region.data_ranges[range_index].size <= addr)
            ++range_index;
        if (range_index < region.data_range_count) {
            const GuestAddressRange& range = region.data_ranges[range_index];
            if (addr >= range.begin && addr - range.begin < range.size) {
                addr = std::min(end, range.begin + range.size);
                continue;
            }
        }
        hash ^= static_cast<std::uint8_t>(rd8(cpu, addr));
        hash *= 0x100000001b3ull;
        ++addr;
    }
    return hash;
}

[[noreturn]] void unknown_target(Cpu* cpu, std::uint32_t target, std::uint32_t caller,
                                 std::uint64_t memory_hash) {
    std::fprintf(stderr,
                 "[dsr] fatal: no recompiled %s entry for 0x%08x (caller 0x%08x, image FNV-1a64 0x%016llx)\n",
                 cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", target, caller,
                 static_cast<unsigned long long>(memory_hash));
    std::fprintf(stderr,
                 "[dsr] hint: the program reached code its generated tables do not contain. This usually means the image "
                 "differs from the one tools/build_port.py was run on (another region or revision, or a modified file); "
                 "rebuild with that exact file: python tools/build_port.py --rebuild-workspace --rom <your .nds>.\n");
    std::fprintf(stderr, "[dsr] registers:");
    for (unsigned i = 0; i < 15; ++i)
        std::fprintf(stderr, " r%u=%08x", i, cpu->r[i]);
    std::fprintf(stderr, " CPSR=%08x\n", dsr_mrs_cpsr(cpu));
    std::fprintf(stderr,
                 "[dsr] add a function entry with --extra-entries: func_%08x kind:function(arm,size=0x4) addr:0x%08x\n",
                 target & ~1u, target & ~1u);
#if defined(_WIN32)
    const wchar_t* path = _wgetenv(L"DSR_DUMP_ON_FATAL");
#else
    const char* path = std::getenv("DSR_DUMP_ON_FATAL");
#endif
    if (path) {
        // Diagnostics: main RAM image for offline comparison.
#if defined(_WIN32)
        std::FILE* out = _wfopen(path, L"wb");
#else
        std::FILE* out = std::fopen(path, "wb");
#endif
        if (out) {
            for (std::uint32_t a = 0x02000000; a < 0x02400000; a += 4) {
                const std::uint32_t word = rd32(cpu, a);
                std::fwrite(&word, 4, 1, out);
            }
            std::fclose(out);
        }
    }
    std::fflush(nullptr);
    std::abort();
}

[[noreturn]] void unknown_swi(Cpu* cpu, std::uint32_t number, std::uint32_t pc) {
    std::fprintf(stderr, "[dsr] fatal: unimplemented %s BIOS SWI 0x%02x at 0x%08x\n",
                 cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", number, pc);
    std::fprintf(stderr, "[dsr] registers:");
    for (unsigned i = 0; i < 15; ++i)
        std::fprintf(stderr, " r%u=%08x", i, cpu->r[i]);
    std::fprintf(stderr, " CPSR=%08x\n", dsr_mrs_cpsr(cpu));
    std::fflush(nullptr);
    std::abort();
}

std::uint32_t raw_swi_number(std::uint32_t imm, bool thumb) {
    return thumb ? (imm & 0xffu) : ((imm >> 16) & 0xffu);
}
} // namespace

void Machine::Impl::build_dispatch_index() {
    if (dispatch_index_built) return;
    const char* verify = std::getenv("DSR_VERIFY_DISPATCH");
    verify_dispatch = verify && *verify && std::strcmp(verify, "0") != 0;
    for (const CpuId id : {CpuId::Arm9, CpuId::Arm7}) {
        const std::size_t cpu_index = static_cast<unsigned>(id);
        const FnTable& table = id == CpuId::Arm9 ? config.arm9_table : config.arm7_table;
        for (std::size_t i = 0; i < table.count; ++i) {
            const FnEntry& entry = table.entries[i];
            if (!entry.slot) continue;
            dispatch_index[(static_cast<std::uint64_t>(cpu_index) << 32) | entry.addr]
                .candidates.push_back(&entry);
        }
        const DispatchRegionTable& regions =
            id == CpuId::Arm9 ? config.arm9_regions : config.arm7_regions;
        for (std::size_t i = 0; i < regions.count; ++i) {
            const DispatchRegionEntry& region = regions.entries[i];
            if (!region.body || region.size == 0) continue;
            dispatch_regions[cpu_index].push_back(&region);
        }
    }
    for (auto& [key, target] : dispatch_index) {
        if (target.candidates.size() == 1) continue;
        std::uint32_t size = 0;
        for (const FnEntry* entry : target.candidates) size = std::max(size, entry->size);
        target.snapshot.resize(size);
    }
    dispatch_index_built = true;
}

const FnEntry* Machine::Impl::find_entry(Cpu* cpu, std::uint32_t target) {
    build_dispatch_index();
    const std::uint32_t addr = target & ~1u;
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<unsigned>(cpu->id)) << 32) | addr;
    const auto it = dispatch_index.find(key);
    if (it == dispatch_index.end()) return nullptr;
    DispatchTarget& bucket = it->second;
    if (bucket.candidates.size() == 1) return bucket.candidates.front();
    // No write hooks or cache-maintenance assumptions: compare every byte
    // that affected the previous resolution, using the current page mappings.
    const bool reused = snapshot_matches(cpu, addr, bucket);
    if (!reused) {
        bucket.cached_entry = resolve_overlay_hash(cpu, bucket.candidates, bucket.checked_size);
        save_snapshot(cpu, addr, bucket);
    }
    // A miss already used the reference resolver. Verify hits only: their
    // checked prefix is directly mapped, so a second read has no IO effects.
    if (verify_dispatch && reused) [[unlikely]] {
        std::uint32_t checked_size;
        const FnEntry* reference = resolve_overlay_hash(cpu, bucket.candidates, checked_size);
        if (reference != bucket.cached_entry) {
            std::fprintf(stderr, "[dsr] fatal: cached %s dispatch differs from byte hash at 0x%08x\n",
                         cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", addr);
            std::abort();
        }
    }
    return bucket.cached_entry;
}

const DispatchRegionEntry* Machine::Impl::find_region(Cpu* cpu, std::uint32_t target) {
    build_dispatch_index();
    const std::uint32_t addr = target & ~1u;
    const std::uint32_t thumb = target & 1u;
    const auto& regions = dispatch_regions[static_cast<unsigned>(cpu->id)];
    for (const DispatchRegionEntry* region : regions) {
        if (region->thumb != thumb || addr < region->begin || addr - region->begin >= region->size)
            continue;
        if (guest_region_hash(cpu, *region) == region->hash) return region;
    }
    return nullptr;
}

[[noreturn]] void Machine::Impl::dispatch_failure(Cpu* cpu, std::uint32_t target) {
    const std::uint32_t addr = target & ~1u;
    const std::uint64_t key =
        (static_cast<std::uint64_t>(static_cast<unsigned>(cpu->id)) << 32) | addr;
    if (const auto it = dispatch_index.find(key); it != dispatch_index.end()) {
        for (const FnEntry* entry : it->second.candidates) {
            std::fprintf(stderr, "[dsr] candidate %s module %d size 0x%x: expected %016llx, memory %016llx\n",
                         entry->name, entry->module, entry->size,
                         static_cast<unsigned long long>(entry->hash),
                         static_cast<unsigned long long>(guest_hash(cpu, entry->addr, entry->size)));
        }
        std::fprintf(stderr, "[dsr] memory at 0x%08x:", addr);
        for (unsigned i = 0; i < 64; i += 4) std::fprintf(stderr, " %08x", rd32(cpu, addr + i));
        std::fprintf(stderr, "\n");
    }
    unknown_target(cpu, target, runtime(cpu->id).current_pc, guest_hash(cpu, addr, 4));
}

void Machine::Impl::call(Cpu* cpu, std::uint32_t target) {
    const FnEntry* entry = find_entry(cpu, target);
    const DispatchRegionEntry* region = entry ? nullptr : find_region(cpu, target);
    if (!entry && !region) dispatch_failure(cpu, target);
    const std::uint32_t return_thumb = cpu->r[14] & 1u;
    cpu->cpsr_ctrl = (cpu->cpsr_ctrl & ~kCpsrT) | ((target & 1u) ? kCpsrT : 0u);
    runtime(cpu->id).current_pc = target & ~1u;
    if (entry) (*entry->slot)(cpu);
    else region->body(cpu, target & ~1u);
    cpu->cpsr_ctrl = (cpu->cpsr_ctrl & ~kCpsrT) | (return_thumb ? kCpsrT : 0u);
}

void Machine::Impl::enter_irq(CpuRuntime& rt, std::uint32_t pc) {
    Cpu& c = rt.cpu;
    const std::uint32_t old_cpsr = rt.banks.cpsr(c);
    const bool nested = rt.in_irq;
    const std::uint32_t old_irq_spsr = rt.banks.spsr[2];
    const std::uint32_t old_irq_lr = (c.cpsr_ctrl & kCpsrModeMask) == kModeIrq
        ? c.r[14] : rt.banks.sp_lr[2][1];
    const std::array<std::uint32_t, 5> saved = {c.r[0], c.r[1], c.r[2], c.r[3], c.r[12]};
    rt.banks.switch_mode(c, kModeIrq);
    rt.banks.write_spsr(c, old_cpsr, 0xf);
    c.cpsr_ctrl = (c.cpsr_ctrl | kCpsrI) & ~kCpsrT;
    const std::uint32_t irq_lr = pc + 4;
    c.r[14] = irq_lr;
    const std::uint32_t old_sp = c.r[13];
    const std::uint32_t frame_sp = old_sp - 6 * 4;
    for (unsigned i = 0; i < saved.size(); ++i)
        write(c.id, frame_sp + i * 4, saved[i], 32);
    write(c.id, frame_sp + 20, irq_lr, 32);
    c.r[13] = frame_sp;

    const std::uint32_t vector = c.id == CpuId::Arm9
        ? ((runtime(CpuId::Arm9).dtcm_region & 0xfffff000u) + 0x3ffcu)
        : 0x0380fffcu;
    const std::uint32_t handler = read(c.id, vector, 32);
    const std::uint32_t old_ctx = nested ? 0u : detail::current_thread_context(*this, c.id);
    rt.in_irq = true;
    if (handler != 0) call(&c, handler);
    if (nested) rt.banks.spsr[2] = old_irq_spsr;
    rt.in_irq = nested;

    // The guest IRQ-return override performs scheduling bookkeeping and
    // changes only the active context; this interrupt frame remains intact.
    const std::uint32_t new_ctx = nested ? 0u : detail::current_thread_context(*this, c.id);

    for (unsigned i = 0; i < 4; ++i) c.r[i] = read(c.id, frame_sp + i * 4, 32);
    c.r[12] = read(c.id, frame_sp + 16, 32);
    c.r[14] = old_irq_lr;
    c.r[13] = old_sp;
    rt.banks.write_cpsr(c, old_cpsr, 0xf);
    rt.current_pc = pc;

    if (!nested && new_ctx != old_ctx && new_ctx != 0 && old_ctx != 0) {
        detail::irq_thread_switch(*this, &c, old_ctx, new_ctx);
    }
}

std::uint32_t Machine::Impl::cp15_read(Cpu* cpu, unsigned cp, unsigned op1, unsigned crn,
                                       unsigned crm, unsigned op2, std::uint32_t pc) {
    if (cp != 15 || cpu->id != CpuId::Arm9) {
        std::fprintf(stderr, "[dsr] unsupported CP%u MRC on %s at 0x%08x\n",
                     cp, cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", pc);
        return 0;
    }
    CpuRuntime& rt = runtime(cpu->id);
    if (crn == 0) {
        if (crm == 0 && op2 == 0) return 0x41059461;
        if (crm == 0 && op2 == 1) return 0x0f0d2112;
        if (crm == 0 && op2 == 2) return 0x00140180;
    }
    if (crn == 1 && crm == 0 && op2 == 0) return rt.cp15_control;
    if (crn == 9 && crm == 1 && op2 == 0) return rt.dtcm_region;
    if (crn == 9 && crm == 1 && op2 == 1) return rt.itcm_region;
    if (crn == 7) return 0; // cache/debug operations read as zero on this core
    if (crn == 5 || crn == 6 || crn == 8) return 0;
    std::fprintf(stderr, "[dsr] unimplemented CP15 MRC c%u,c%u,%u op1=%u op2=%u at 0x%08x\n",
                 crn, crm, op2, op1, op2, pc);
    return 0;
}

void Machine::Impl::cp15_write(Cpu* cpu, unsigned cp, unsigned op1, unsigned crn,
                               unsigned crm, unsigned op2, std::uint32_t value,
                               std::uint32_t pc) {
    if (cp != 15 || cpu->id != CpuId::Arm9) {
        std::fprintf(stderr, "[dsr] unsupported CP%u MCR on %s at 0x%08x\n",
                     cp, cpu->id == CpuId::Arm9 ? "ARM9" : "ARM7", pc);
        return;
    }
    CpuRuntime& rt = runtime(cpu->id);
    if (crn == 1 && crm == 0 && op2 == 0) {
        rt.cp15_control = value;
        rebuild_pages(CpuId::Arm9);
        return;
    }
    if (crn == 9 && crm == 1 && op2 == 0) {
        rt.dtcm_region = value;
        rebuild_pages(CpuId::Arm9);
        return;
    }
    if (crn == 9 && crm == 1 && op2 == 1) {
        rt.itcm_region = value;
        rebuild_pages(CpuId::Arm9);
        return;
    }
    if (crn == 7 && crm == 0 && op2 == 4) { // wait for interrupt
        halt(cpu);
        return;
    }
    // c2/c3 cacheability/write-buffer, c5 access permissions, c6 protection
    // regions, c7 cache maintenance, c8 unused, c9 cache lockdown: no effect
    // on a host without the ARM946E-S caches/protection unit.
    if (crn == 2 || crn == 3 || crn == 7 || crn == 9 || crn == 5 || crn == 6 || crn == 8) return;
    std::fprintf(stderr, "[dsr] unimplemented CP15 MCR c%u,c%u,%u op1=%u op2=%u value=0x%08x at 0x%08x\n",
                 crn, crm, op2, op1, op2, value, pc);
}

void Machine::Impl::swi(Cpu* cpu, std::uint32_t imm, std::uint32_t pc, bool thumb) {
    const std::uint32_t number = raw_swi_number(imm, thumb);
    if (const auto handler_cycles = detail::handle_data_swi(cpu, number)) {
        // Reference SWI timing is handler()+3; generated code charges base 1.
        cpu->budget -= 2 + static_cast<std::int64_t>(*handler_cycles);
        return;
    }
    CpuRuntime& rt = runtime(cpu->id);
    switch (number) {
    case 0: // SoftReset: restart the cartridge after leaving the active guest stack.
        if (!card) unknown_swi(cpu, number, pc);
        soft_reset_requested = true;
        rt.halted = true;
        rt.fiber_done = true;
        Fiber::yield();
        return;
    case 3: { // WaitByLoop
        const std::uint32_t delay = cpu->r[0];
        cpu->r[0] = 0;
        const std::int64_t cycles = cpu->id == CpuId::Arm7
            ? 4 : ((rt.cp15_control & ((1u << 16) | (1u << 18))) ? 2 : 8);
        cpu->budget -= 2 + static_cast<std::int64_t>(delay) * cycles;
        if (cpu->budget <= 0) checkpoint(cpu, pc + (thumb ? 2u : 4u));
        return;
    }
    case 4: case 5: // IntrWait / VBlankIntrWait
        if (number == 5) {
            cpu->r[0] = 1;
            cpu->r[1] = 1;
        }
        intr_wait(cpu, number == 5 ? 1u : cpu->r[1], number == 5 || cpu->r[0] != 0,
                  pc + (thumb ? 2u : 4u));
        return;
    case 6: // Halt
        halt(cpu);
        return;
    case 7: // ARM7 Sleep; the ARM9 BIOS entry is a no-op.
        if (cpu->id == CpuId::Arm9) return;
        rt.halted = true;
        rt.stopped = true;
        rt.waiting_for_irq = false;
        rt.halt_until_irq = false;
        return;
    case 8: // ARM7 SoundBias; the ARM9 BIOS entry is a no-op.
        if (cpu->id == CpuId::Arm9) return;
        {
            constexpr std::uint32_t sound_bias = 0x04000504u;
            const std::uint32_t current = rd32(cpu, sound_bias);
            const std::uint32_t next = current == 0 ? 0 : 0x0200u;
            const std::uint32_t cycles =
                next > current ? next - current : current - next;
            wr32(cpu, sound_bias, next);
            cpu->budget -= static_cast<std::int64_t>(cpu->r[1]) * cycles;
            if (cpu->budget <= 0) checkpoint(cpu, pc + (thumb ? 2u : 4u));
            return;
        }
    default:
        unknown_swi(cpu, number, pc);
    }
}

} // namespace dsr

namespace dsr {

void dsr_checkpoint(Cpu* c, std::uint32_t pc) {
    detail::MachineAccess::impl(c).checkpoint(c, pc);
}

void dsr_call(Cpu* c, std::uint32_t target) {
    detail::MachineAccess::impl(c).call(c, target);
}

[[noreturn]] void dsr_unresolved_target(Cpu* c, std::uint32_t target) {
    detail::MachineAccess::impl(c).dispatch_failure(c, target);
}

void dsr_swi(Cpu* c, std::uint32_t imm, std::uint32_t pc, bool thumb) {
    detail::MachineAccess::impl(c).swi(c, imm, pc, thumb);
}

std::uint32_t dsr_mrc(Cpu* c, unsigned cp, unsigned op1, unsigned crn, unsigned crm,
                      unsigned op2, std::uint32_t pc) {
    return detail::MachineAccess::impl(c).cp15_read(c, cp, op1, crn, crm, op2, pc);
}

void dsr_mcr(Cpu* c, unsigned cp, unsigned op1, unsigned crn, unsigned crm, unsigned op2,
             std::uint32_t value, std::uint32_t pc) {
    detail::MachineAccess::impl(c).cp15_write(c, cp, op1, crn, crm, op2, value, pc);
}

[[noreturn]] void dsr_undefined(Cpu* c, std::uint32_t pc, std::uint32_t insn) {
    std::fprintf(stderr, "[dsr] fatal: undefined instruction cpu=%s pc=0x%08x insn=0x%08x\n",
                 c->id == CpuId::Arm9 ? "ARM9" : "ARM7", pc, insn);
    std::fprintf(stderr, "[dsr] registers:");
    for (unsigned i = 0; i < 15; ++i) std::fprintf(stderr, " r%u=%08x", i, c->r[i]);
    std::fprintf(stderr, " CPSR=%08x\n", dsr_mrs_cpsr(c));
    std::fflush(nullptr);
    std::abort();
}

namespace {
std::uint32_t g_last_return[2];
}

void dsr_note_return(Cpu* c, std::uint32_t target) {
    g_last_return[static_cast<unsigned>(c->id)] = target;
}

void dsr_check_return(Cpu* c, std::uint32_t expected) {
    const std::uint32_t got = g_last_return[static_cast<unsigned>(c->id)];
    if ((got & ~1u) != (expected & ~1u)) {
        std::fprintf(stderr,
                     "[dsr] fatal: callee returned to 0x%08x but the caller expected 0x%08x (cpu=%s)\n",
                     got, expected, c->id == CpuId::Arm9 ? "ARM9" : "ARM7");
        std::fflush(nullptr);
        std::abort();
    }
}

void dsr_trace_entry(Cpu* c, std::int32_t module_id, std::uint32_t addr) {
    static std::FILE* out = [] {
#if defined(_WIN32)
        const wchar_t* path = _wgetenv(L"DSR_TRACE_FILE");
        return path ? _wfopen(path, L"w") : nullptr;
#else
        const char* path = std::getenv("DSR_TRACE_FILE");
        return path ? std::fopen(path, "w") : nullptr;
#endif
    }();
    static const unsigned long long limit = [] {
        const char* text = std::getenv("DSR_TRACE_LIMIT");
        return text ? std::strtoull(text, nullptr, 0) : 2000000ull;
    }();
    static const std::uint64_t first_frame = [] {
        const char* text = std::getenv("DSR_TRACE_START_FRAME");
        return text ? std::strtoull(text, nullptr, 0) : 0ull;
    }();
    static const std::uint64_t last_frame = [] {
        const char* text = std::getenv("DSR_TRACE_END_FRAME");
        return text ? std::strtoull(text, nullptr, 0) : UINT64_MAX;
    }();
    static const std::uint32_t address_filter = [] {
        const char* text = std::getenv("DSR_TRACE_ADDR");
        return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0))
                    : std::numeric_limits<std::uint32_t>::max();
    }();
    if (address_filter != std::numeric_limits<std::uint32_t>::max() && addr != address_filter) return;
    static unsigned long long rows = 0;
    if (!out) return;
    const std::uint64_t frame = detail::MachineAccess::impl(c).owner->ticks() / 560190u;
    if (frame < first_frame || frame > last_frame || rows >= limit) return;
    ++rows;
    std::fprintf(out, "%llu %u %x %x %x %x %x %x %x %d\n", static_cast<unsigned long long>(frame),
                 static_cast<unsigned>(c->id), addr, c->r[0], c->r[1], c->r[2], c->r[3], c->r[13], c->r[14],
                 static_cast<int>(module_id));
    if (rows == limit || (rows & 0x3ff) == 0) std::fflush(out);
}

} // namespace dsr
