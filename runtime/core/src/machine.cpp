#include "core_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dsr {
namespace {
// EU perspective builder, shared by camera updates. Unlike changing the
// constructor's default aspect, intercepting the builder also covers cameras
// whose aspect is subsequently changed and cameras created in overlays.
constexpr std::uint32_t kCameraProjection = 0x020c28b0;
constexpr std::uint64_t kCameraProjectionHash = 0x1cb108bf62a813e5ULL;

// Validate the guest version without mutating process-global generated slots.
bool validate_camera_projection(Machine& machine) {
    const auto& impl = detail::MachineAccess::impl(&machine.cpu(CpuId::Arm9));
    for (std::size_t i = 0; i < impl.config.arm9_table.count; ++i) {
        const auto& entry = impl.config.arm9_table.entries[i];
        if (entry.module == kModuleArm9Main && entry.addr == kCameraProjection)
            return entry.hash == kCameraProjectionHash && entry.slot && *entry.slot;
    }
    return false;
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void put32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}
} // namespace

bool Machine::copy_snapshot_ram(std::uint32_t address, void* destination, std::size_t size) const noexcept {
    if (!destination || size == 0) return false;
    const auto within = [address, size](std::uint32_t first, std::uint32_t end) {
        return address >= first && address < end && size <= end - address;
    };
    if (!within(0, 0x8000) && !within(0x00800000, 0x00804000) &&
        !within(0x01ff8000, 0x02000000) && !within(0x02000000, 0x02400000))
        return false;
    const auto& arm9 = impl_->cpus[static_cast<unsigned>(CpuId::Arm9)];
    const auto region_size = [](std::uint32_t reg) -> std::uint64_t {
        const unsigned code = (reg >> 1) & 31u;
        return code > 23 ? 0x80000000u : 512u << code;
    };
    const auto in_region = [&](std::uint32_t pos, std::uint32_t reg, unsigned bit) {
        const auto base = reg & 0xfffff000u;
        return (arm9.cp15_control & (1u << bit)) &&
               static_cast<std::uint32_t>(pos - base) < region_size(reg);
    };
    // Match the bus page mapping, then its ITCM-before-DTCM slow path.
    // Validate first so a failed request does not partially modify destination.
    for (int pass = 0; pass < 2; ++pass) {
        std::size_t done = 0;
        while (done < size) {
            const auto pos = address + static_cast<std::uint32_t>(done);
            auto count = std::min(size - done, std::size_t(kPageSize - (pos & kPageMask)));
            const std::uint8_t* source = nullptr;
            if (arm9.page_read[pos >> kPageShift]) {
                const auto* page = arm9.page_read[pos >> kPageShift];
                const auto map_tcm = [&](const auto& backing) {
                    const auto page_address = reinterpret_cast<std::uintptr_t>(page);
                    const auto base_address = reinterpret_cast<std::uintptr_t>(backing.data());
                    if (page_address < base_address || page_address - base_address >= backing.size())
                        return false;
                    const auto offset = (page_address - base_address + (pos & kPageMask)) % backing.size();
                    source = backing.data() + offset;
                    count = std::min(count, backing.size() - offset);
                    return true;
                };
                if (!map_tcm(impl_->itcm) && !map_tcm(impl_->dtcm))
                    source = page + (pos & kPageMask);
            } else if (in_region(pos, arm9.itcm_region, 16)) {
                const auto offset = (pos - (arm9.itcm_region & 0xfffff000u)) % impl_->itcm.size();
                source = impl_->itcm.data() + offset;
                count = std::min(count, impl_->itcm.size() - offset);
                count = std::min(count, static_cast<std::size_t>(region_size(arm9.itcm_region) -
                    static_cast<std::uint32_t>(pos - (arm9.itcm_region & 0xfffff000u))));
            } else if (in_region(pos, arm9.dtcm_region, 18)) {
                const auto offset = (pos - (arm9.dtcm_region & 0xfffff000u)) % impl_->dtcm.size();
                source = impl_->dtcm.data() + offset;
                count = std::min(count, impl_->dtcm.size() - offset);
                count = std::min(count, static_cast<std::size_t>(region_size(arm9.dtcm_region) -
                    static_cast<std::uint32_t>(pos - (arm9.dtcm_region & 0xfffff000u))));
                const auto itcm_base = arm9.itcm_region & 0xfffff000u;
                if ((arm9.cp15_control & (1u << 16)) && itcm_base > pos)
                    count = std::min(count, static_cast<std::size_t>(itcm_base - pos));
            } else {
                return false;
            }
            if (pass) std::memcpy(static_cast<std::uint8_t*>(destination) + done, source, count);
            done += count;
        }
    }
    return true;
}

Machine::Impl::Impl(const MachineConfig& cfg) : config(cfg), spi(cfg.firmware) {
    if (!cfg.rom_path.empty()) {
        auto save = cfg.save_path;
        if (save.empty()) {
            save = cfg.rom_path;
            save.replace_extension(".sav");
        }
        card = std::make_unique<GameCard>(cfg.rom_path, save, cfg.guest_abi.backup_type);
    }

    for (unsigned i = 0; i < 2; ++i) {
        auto& rt = cpus[i];
        rt.cpu.id = i == 0 ? CpuId::Arm9 : CpuId::Arm7;
        rt.cpu.m = owner;
        rt.cpu.page_r = rt.page_read.data();
        rt.cpu.page_w = rt.page_write.data();
    }
    gpu2d.attach_gpu3d(&gpu3d);
    gpu3d.attach_gpu2d(&gpu2d);
    gpu2d.on_vram_remap = [this] { rebuild_vram_pages(); };
    gpu3d.on_fifo_state_change = [this] {
        // Latch the level IRQ before GX DMA can refill the FIFO and deassert it.
        if (gpu3d.irq_condition()) set_irq(0, 21);
        if (gpu3d.fifo_below_half()) trigger_dma(0, 7);
    };
    spu.read8 = [this](std::uint32_t addr) { return read(CpuId::Arm7, addr, 8); };
    spu.write8 = [this](std::uint32_t addr, std::uint8_t value) {
        write(CpuId::Arm7, addr, value, 8);
    };
    if (card) {
        card->on_transfer_done_irq = [this] {
            const unsigned owner_cpu = (exmemcnt & (1u << 11)) ? 1u : 0u;
            set_irq(owner_cpu, 19);
        };
        card->on_data_ready = [this] {
            const unsigned card_owner = (exmemcnt & (1u << 11)) ? 1u : 0u;
            trigger_dma(card_owner, 5);
        };
    }
    spi.on_irq = [this] { set_irq(1, 23); };
    reset();
    if (config.widescreen_camera) {
        gpu3d.set_enhanced_geometry(true);
        gpu2d.set_enhanced_presentation(true);
    }
    if (config.direct_boot && card) boot();
}

Machine::Impl::~Impl() {
    // Stop all render workers while their callbacks/VRAM still exist. A Godot
    // session reaches this on its owner thread, before either library unloads.
    gpu3d.shutdown_render_workers();
    if (card) card->flush_save();
}

void Machine::Impl::reset() {
    main_ram.fill(0);
    itcm.fill(0);
    dtcm.fill(0);
    shared_wram.fill(0);
    arm7_wram.fill(0);
    rom_header.fill(0);
    io_shadow[0].fill(0);
    io_shadow[1].fill(0);
    irq = {};
    timers = {};
    timers_synced_tick = 0;
    timers_due_tick = detail::kNoTimerDue;
    dmas = {};
    gx_dma_active = false;
    ipc = {};
    wramcnt = 3;
    exmemcnt = 0;
    exmemstat7 = 0;
    postflg[0] = postflg[1] = 0;
    powcnt1 = 0;
    powcnt2 = 1;
    keyinput = 0x03ff;
    total_ticks = 0;
    scanline = detail::kLinesPerFrame - 1;
    line_tick = 0;
    divcnt = 0;
    div_numer = div_denom = 0;
    div_result = div_remainder = 0;
    div_pending_result = div_pending_remainder = 0;
    div_busy_ticks = 0;
    sqrtcnt = 0;
    sqrt_param = 0;
    sqrt_result = sqrt_pending_result = 0;
    sqrt_busy_ticks = 0;
    soft_reset_requested = false;
    logged_unimplemented.clear();
    for (unsigned i = 0; i < 2; ++i) {
        auto& rt = cpus[i];
        std::memset(&rt.cpu, 0, sizeof(rt.cpu));
        rt.cpu.id = i == 0 ? CpuId::Arm9 : CpuId::Arm7;
        rt.cpu.m = owner;
        rt.cpu.page_r = rt.page_read.data();
        rt.cpu.page_w = rt.page_write.data();
        rt.cpu.cpsr_ctrl = static_cast<std::uint32_t>(kModeSvc) | kCpsrI | kCpsrF;
        rt.banks = {};
        rt.cp15_control = 0x00052078;
        rt.dtcm_region = 0x0080000A;
        rt.itcm_region = 0x0000000C;
        rt.halted = false;
        rt.stopped = false;
        rt.entered = false;
        rt.fiber_done = false;
        rt.in_irq = false;
        rt.halt_until_irq = false;
        rt.waiting_for_irq = false;
        rt.halt_clear_on_wake = false;
        rt.halt_wait_mask = 0xffffffffu;
        rt.entry = 0;
        rt.current_pc = 0;
        rt.irq_budget_reserve = 0;
        rt.fiber.reset();
        rt.page_read.fill(nullptr);
        rt.page_write.fill(nullptr);
    }
    if (card) card->reset();
    wifi.reset();
    spi.reset();
    rtc.reset();
    if (config.deterministic_rtc) {
        rtc.set_deterministic_clock(config.rtc_epoch_seconds, config.rtc_timezone_offset_minutes);
        rtc.set_emulated_ticks(total_ticks);
    }
    gpu2d.reset();
    gpu3d.reset();
    spu.reset();
    rebuild_pages(CpuId::Arm9);
    rebuild_pages(CpuId::Arm7);
    // The oracle seeds the *end* of line262 and processes HStart0 at t=0.
    // Guests must not execute a full extra line while observing VCOUNT262.
    process_line_start();
}

void Machine::Impl::boot() {
    if (!card) return;
    card->read_rom(0, rom_header.data(), static_cast<std::uint32_t>(rom_header.size()));
    const std::uint32_t arm9_offset = get32(rom_header.data() + 0x20);
    const std::uint32_t arm9_entry = get32(rom_header.data() + 0x24);
    const std::uint32_t arm9_dest = get32(rom_header.data() + 0x28);
    const std::uint32_t arm9_size = get32(rom_header.data() + 0x2c);
    const std::uint32_t arm7_offset = get32(rom_header.data() + 0x30);
    const std::uint32_t arm7_entry = get32(rom_header.data() + 0x34);
    const std::uint32_t arm7_dest = get32(rom_header.data() + 0x38);
    const std::uint32_t arm7_size = get32(rom_header.data() + 0x3c);

    auto copy_rom = [this](std::uint32_t source, std::uint32_t destination, std::uint32_t size) {
        std::array<std::uint8_t, 4096> buffer{};
        while (size != 0) {
            const std::uint32_t n = std::min<std::uint32_t>(size, buffer.size());
            card->read_rom(source, buffer.data(), n);
            for (std::uint32_t i = 0; i < n; ++i)
                write(CpuId::Arm9, destination + i, buffer[i], 8);
            source += n;
            destination += n;
            size -= n;
        }
    };

    copy_rom(arm9_offset, arm9_dest, arm9_size);
    copy_rom(arm7_offset, arm7_dest, arm7_size);
    for (std::uint32_t i = 0; i < rom_header.size(); ++i)
        write(CpuId::Arm9, 0x027ffe00u + i, rom_header[i], 8);

    const std::uint32_t chip = card->chip_id();
    write(CpuId::Arm9, 0x027ff800, chip, 32);
    write(CpuId::Arm9, 0x027ff804, chip, 32);
    write(CpuId::Arm9, 0x027ffc00, chip, 32);
    const auto* firmware = spi.firmware();
    const std::uint32_t firmware_size = spi.firmware_size();
    if (firmware && firmware_size >= 0x70) {
        const std::uint32_t settings_offset = firmware_size >= 0x200 ? firmware_size - 0x200 : 0;
        for (std::uint32_t i = 0; i < 0x70; ++i)
            write(CpuId::Arm9, 0x027ffc80u + i, firmware[settings_offset + i], 8);
    }
    write(CpuId::Arm9, 0x027ff808,
          rom_header[0x15e] | (static_cast<std::uint32_t>(rom_header[0x15f]) << 8), 16);
    write(CpuId::Arm9, 0x027ffc08, 0x00000001, 32);

    wramcnt = 3;
    powcnt1 = 0x820f;
    gpu2d.io_write(0x04000304u, powcnt1, 16);
    postflg[0] = 1;
    postflg[1] = 1;
    const auto set_stack = [](CpuRuntime& runtime, std::uint32_t irq_sp,
                              std::uint32_t svc_sp, std::uint32_t sys_sp) {
        Cpu& c = runtime.cpu;
        runtime.banks.switch_mode(c, kModeIrq);
        c.r[13] = irq_sp;
        runtime.banks.switch_mode(c, kModeSvc);
        c.r[13] = svc_sp;
        runtime.banks.switch_mode(c, kModeSys);
        c.r[13] = sys_sp;
        runtime.banks.switch_mode(c, kModeSvc);
    };
    set_stack(cpus[0], 0x00803fa0, 0x00803fc0, 0x00803f00);
    set_stack(cpus[1], 0x0380ff80, 0x0380ffc0, 0x0380fd80);
    cpus[0].entry = arm9_entry;
    cpus[1].entry = arm7_entry;
    cpus[0].cpu.r[0] = 0;
    cpus[1].cpu.r[0] = 0;
    rebuild_pages(CpuId::Arm9);
    rebuild_pages(CpuId::Arm7);
}

Machine::Machine(const MachineConfig& config) : impl_(new Impl(config)) {
    impl_->owner = this;
    for (auto& rt : impl_->cpus) rt.cpu.m = this;
    // Full mode requires the expected guest projection builder.
    if (config.widescreen_camera && !validate_camera_projection(*this)) {
        delete impl_;
        throw std::runtime_error("DQ9 widescreen camera: guest projection hash mismatch or missing function");
    }
    detail::install_thread_overrides(*impl_);
}

Machine::~Machine() {
    detail::reset_thread_overrides(*impl_);
    detail::uninstall_thread_overrides(*impl_);
    delete impl_;
}

void Machine::run_frame() {
    // Match the oracle's VCOUNT=262 frame boundary so frame-indexed input is
    // applied over the same hardware interval.
    constexpr unsigned kFrameEndLine = detail::kLinesPerFrame - 1;
    const std::uint64_t lines =
        (kFrameEndLine + detail::kLinesPerFrame - impl_->scanline) %
        detail::kLinesPerFrame;
    std::uint64_t until = lines * detail::kLineTicks;
    if (until <= impl_->line_tick) until += detail::kLineTicks * detail::kLinesPerFrame;
    until -= impl_->line_tick;
    impl_->run_ticks(until);
}

void Machine::run_ticks(std::uint64_t ticks) { impl_->run_ticks(ticks); }
Cpu& Machine::cpu(CpuId id) { return impl_->runtime(id).cpu; }
const Cpu& Machine::cpu(CpuId id) const { return impl_->runtime(id).cpu; }
std::uint8_t Machine::read8(CpuId id, std::uint32_t addr) { return static_cast<std::uint8_t>(impl_->read(id, addr, 8)); }
std::uint16_t Machine::read16(CpuId id, std::uint32_t addr) { return static_cast<std::uint16_t>(impl_->read(id, addr, 16)); }
std::uint32_t Machine::read32(CpuId id, std::uint32_t addr) { return impl_->read(id, addr, 32); }
void Machine::write8(CpuId id, std::uint32_t addr, std::uint8_t value) { impl_->write(id, addr, value, 8); }
void Machine::write16(CpuId id, std::uint32_t addr, std::uint16_t value) { impl_->write(id, addr, value, 16); }
void Machine::write32(CpuId id, std::uint32_t addr, std::uint32_t value) { impl_->write(id, addr, value, 32); }
std::uint64_t Machine::ticks() const { return impl_->total_ticks; }
std::uint16_t Machine::keyinput() const { return impl_->keyinput; }

void Machine::set_input(const InputState& input) {
    const InputState previous = impl_->input;
    impl_->input = input;
    const std::uint16_t pressed = input.buttons;
    impl_->keyinput = static_cast<std::uint16_t>(~pressed) & 0x03ff;
    impl_->spi.set_touch(input.pen_down, input.pen_x, input.pen_y);
    impl_->handle_key_irq();
    if (impl_->cpus[1].stopped &&
        (previous.buttons != input.buttons || (previous.lid_closed && !input.lid_closed))) {
        impl_->cpus[1].stopped = false;
        impl_->cpus[1].halted = false;
    }
}

void Machine::raise_irq(CpuId id, unsigned irq) { impl_->set_irq(static_cast<unsigned>(id), irq); }
bool Machine::irq_deliverable(CpuId id) const { return impl_->can_deliver_irq(static_cast<unsigned>(id)); }

bool Machine::replace_function(CpuId id, std::int32_t module, std::uint32_t addr,
                               std::uint64_t expected_hash, GuestFn replacement) {
    if (!replacement) return false;
    const FnTable& table = id == CpuId::Arm9 ? impl_->config.arm9_table : impl_->config.arm7_table;
    for (std::size_t i = 0; i < table.count; ++i) {
        const FnEntry& entry = table.entries[i];
        if (entry.addr != (addr & ~1u) || entry.module != module) continue;
        // The generated table hash identifies the exact code the body was
        // built from; guest memory may not hold that code yet (autoloads,
        // overlays), so it is not consulted here.
        if (entry.hash != expected_hash || !entry.slot) return false;
        *entry.slot = replacement;
        return true;
    }
    return false;
}
Gpu2D& Machine::gpu2d() { return impl_->gpu2d; }
const Gpu2D& Machine::gpu2d() const { return impl_->gpu2d; }

void Machine::start_cpu(CpuId id, std::uint32_t entry) {
    CpuRuntime& rt = impl_->runtime(id);
    rt.fiber.reset();
    rt.entry = entry;
    rt.halted = false;
    rt.stopped = false;
    rt.entered = false;
    rt.fiber_done = false;
}
const Gpu3D& Machine::gpu3d() const { return impl_->gpu3d; }

Gpu3D& Machine::gpu3d() { return impl_->gpu3d; }
Spu& Machine::spu() { return impl_->spu; }
GameCard* Machine::game_card() { return impl_->card.get(); }
Spi& Machine::spi() { return impl_->spi; }
Rtc& Machine::rtc() { return impl_->rtc; }

Machine::Impl& detail::MachineAccess::impl(Cpu* cpu) { return *cpu->m->impl_; }
const Machine::Impl& detail::MachineAccess::impl(const Cpu* cpu) { return *cpu->m->impl_; }

} // namespace dsr

namespace dsr {

std::string Machine::debug_status() const {
    std::string out;
    char buf[256];
    for (unsigned i = 0; i < 2; ++i) {
        const auto& rt = impl_->cpus[i];
        const auto& irq = impl_->irq[i];
        std::snprintf(buf, sizeof buf,
                      "%s[pc=%08x halted=%d stopped=%d done=%d ime=%u ie=%08x if=%08x cpsr=%02x sp=%08x lr=%08x "
                      "sync=%04x fifocnt=%04x rx=%zu] ",
                      i == 0 ? "ARM9" : "ARM7", rt.current_pc, rt.halted, rt.stopped, rt.fiber_done,
                      static_cast<unsigned>(irq.ime), irq.ie, irq.flags, rt.cpu.cpsr_ctrl, rt.cpu.r[13],
                      rt.cpu.r[14], impl_->ipc.sync[i], impl_->ipc.fifo_control[i], impl_->ipc.fifo[i].size());
        out += buf;
    }
    return out;
}
CpuDebugSnapshot Machine::debug_cpu_snapshot(CpuId id) const {
    const auto& runtime = impl_->cpus[static_cast<unsigned>(id)];
    CpuDebugSnapshot snapshot;
    std::copy(std::begin(runtime.cpu.r), std::end(runtime.cpu.r), snapshot.registers.begin());
    snapshot.current_pc = runtime.current_pc;
    snapshot.entry_pc = runtime.entry;
    snapshot.cpsr_control = runtime.cpu.cpsr_ctrl;
    return snapshot;
}

std::size_t Machine::unimplemented_io_count() const {
    return impl_->logged_unimplemented.size();
}

} // namespace dsr
