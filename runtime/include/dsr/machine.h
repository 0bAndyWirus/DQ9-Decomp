// Nintendo DS machine: two statically recompiled CPUs and platform services.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include "dsr/abi_config.h"
#include "dsr/cpu.h"
#include "dsr/frontend.h" // InputState
#include "dsr/periph.h"
#include "dsr/rt.h"

namespace dsr {

namespace detail {
struct MachineAccess;
}

class Gpu2D;
class Gpu3D;
class Spu;

struct MachineConfig {
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    FirmwareSettings firmware{};
    FnTable arm9_table{};
    FnTable arm7_table{};
    bool deterministic_rtc = false;
    std::int64_t rtc_epoch_seconds = 0;
    std::int32_t rtc_timezone_offset_minutes = 0;
    bool direct_boot = true;
    DispatchRegionTable arm9_regions{};
    DispatchRegionTable arm7_regions{};
    GuestAbiConfig guest_abi{};
};
struct CpuDebugSnapshot {
    std::array<std::uint32_t, 16> registers{};
    std::uint32_t current_pc = 0;
    std::uint32_t entry_pc = 0;
    std::uint32_t cpsr_control = 0;
};

class Machine {
public:
    explicit Machine(const MachineConfig& config);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    Machine(Machine&&) = delete;
    Machine& operator=(Machine&&) = delete;

    // Frames end at the oracle's VCOUNT=262 return point.
    void run_frame();
    // Advance emulated bus time. Primarily useful to deterministic harnesses.
    void run_ticks(std::uint64_t ticks);
    void start_cpu(CpuId id, std::uint32_t entry);

    Cpu& cpu(CpuId id);
    const Cpu& cpu(CpuId id) const;
    std::uint8_t read8(CpuId id, std::uint32_t addr);
    std::uint16_t read16(CpuId id, std::uint32_t addr);
    std::uint32_t read32(CpuId id, std::uint32_t addr);
    void write8(CpuId id, std::uint32_t addr, std::uint8_t value);
    void write16(CpuId id, std::uint32_t addr, std::uint16_t value);
    void write32(CpuId id, std::uint32_t addr, std::uint32_t value);

    std::uint64_t ticks() const;
    std::uint16_t keyinput() const;
    void set_input(const InputState& input);
    void raise_irq(CpuId id, unsigned irq);
    bool irq_deliverable(CpuId id) const;
    // One-line CPU/IRQ state for diagnostics (halt state, last checkpoint
    // pc, IME/IE/IF, key registers).
    std::string debug_status() const;
    CpuDebugSnapshot debug_cpu_snapshot(CpuId id) const;
    std::size_t unimplemented_io_count() const;

    // Replace one generated function after confirming that the requested
    // guest hash matches its FnEntry. All generated direct calls use this slot.
    bool replace_function(CpuId id, std::int32_t module, std::uint32_t addr,
                         std::uint64_t expected_hash, GuestFn replacement);

    Gpu2D& gpu2d();

    const Gpu2D& gpu2d() const;
    const Gpu3D& gpu3d() const;
    Gpu3D& gpu3d();
    Spu& spu();
    GameCard* game_card();
    Spi& spi();
    Rtc& rtc();

    struct Impl;

private:
    friend struct detail::MachineAccess;
    Impl* impl_;
};

} // namespace dsr
