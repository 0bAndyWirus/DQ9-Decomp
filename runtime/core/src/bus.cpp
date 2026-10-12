#include "core_internal.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace dsr {
namespace {
std::uint32_t region_size(std::uint32_t reg) {
    const unsigned code = (reg >> 1) & 31u;
    if (code > 23) return 0x80000000u;
    return 512u << code;
}

void map_alias(CpuRuntime& rt, std::uint32_t base, std::uint32_t length,
               std::uint8_t* memory, std::uint32_t physical_size, bool writable = true) {
    if (!length || !physical_size) return;
    const std::uint64_t end = static_cast<std::uint64_t>(base) + length;
    std::uint32_t page = base >> kPageShift;
    const std::uint32_t last = static_cast<std::uint32_t>((end - 1) >> kPageShift);
    for (; page <= last; ++page) {
        const std::uint32_t address = page << kPageShift;
        const std::uint32_t offset = (address - base) % physical_size;
        rt.page_read[page] = memory + offset;
        if (writable) rt.page_write[page] = memory + offset;
    }
}

std::uint32_t load_width(const std::uint8_t* p, int width) {
    if (width == 8) return *p;
    if (width == 16) {
        std::uint16_t value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    std::uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

void store_width(std::uint8_t* p, int width, std::uint32_t value) {
    if (width == 8) { *p = static_cast<std::uint8_t>(value); return; }
    if (width == 16) {
        const std::uint16_t h = static_cast<std::uint16_t>(value);
        std::memcpy(p, &h, sizeof(h));
        return;
    }
    std::memcpy(p, &value, sizeof(value));
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    return value;
}

bool parse_watch_number(std::string_view text, std::uint64_t& value) {
    text = trim(text);
    int base = 10;
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return false;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    return error == std::errc{} && end == text.data() + text.size();
}

std::vector<detail::GuestWriteWatchRange> parse_guest_write_watch() {
    std::vector<detail::GuestWriteWatchRange> ranges;
    const char* const env = std::getenv("DSR_WATCH");
    if (!env || !*env) return ranges;

    std::string_view rest(env);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view item = trim(rest.substr(0, comma));
        const std::string_view tail =
            comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const std::size_t colon = item.find(':');
        const std::string_view address_text = item.substr(0, colon);
        const std::string_view length_text =
            colon == std::string_view::npos ? std::string_view{"1"} : item.substr(colon + 1);
        std::uint64_t first = 0;
        std::uint64_t length = 0;
        const bool valid = (colon == std::string_view::npos ||
                            item.find(':', colon + 1) == std::string_view::npos) &&
                           parse_watch_number(address_text, first) &&
                           parse_watch_number(length_text, length) &&
                           first <= 0xffffffffu && length != 0 &&
                           length <= 0x100000000ull - first;
        if (valid) {
            ranges.push_back({static_cast<std::uint32_t>(first), first + length});
        } else {
            std::fprintf(stderr, "[dsr] ignoring invalid DSR_WATCH range '%.*s'\n",
                         static_cast<int>(item.size()), item.data());
        }
        if (comma == std::string_view::npos) break;
        rest = tail;
    }
    return detail::expand_main_ram_write_watch_aliases(ranges);
}

const std::vector<detail::GuestWriteWatchRange>& guest_write_watch_ranges() {
    static const std::vector<detail::GuestWriteWatchRange> ranges = parse_guest_write_watch();
    return ranges;
}

bool guest_write_is_watched(std::uint32_t addr, int width) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + width / 8;
    for (const auto& range : guest_write_watch_ranges()) {
        if (addr < range.end && range.first < end) return true;
    }
    return false;
}

void route_watched_pages_slow(CpuRuntime& rt) {
    for (const auto& range : guest_write_watch_ranges()) {
        const std::uint32_t first_page = range.first >> kPageShift;
        const std::uint32_t last_page = static_cast<std::uint32_t>((range.end - 1) >> kPageShift);
        for (std::uint32_t page = first_page; page <= last_page; ++page)
            rt.page_write[page] = nullptr;
    }
}
} // namespace

std::vector<detail::GuestWriteWatchRange> detail::expand_main_ram_write_watch_aliases(
    const std::vector<detail::GuestWriteWatchRange>& ranges) {
    constexpr std::uint64_t mirror_size = detail::kMainRamSize;
    constexpr std::uint64_t mirror_count = 4;
    const std::uint64_t mirror_base = detail::kMainRamBase;
    const std::uint64_t mirror_end = mirror_base + mirror_size * mirror_count;
    std::vector<detail::GuestWriteWatchRange> expanded = ranges;

    for (const auto& range : ranges) {
        const std::uint64_t first = std::max<std::uint64_t>(range.first, mirror_base);
        const std::uint64_t end = std::min(range.end, mirror_end);
        if (first >= end) continue;
        for (std::uint64_t bank = 0; bank < mirror_count; ++bank) {
            const std::uint64_t bank_base = mirror_base + bank * mirror_size;
            const std::uint64_t overlap_first = std::max(first, bank_base);
            const std::uint64_t overlap_end = std::min(end, bank_base + mirror_size);
            if (overlap_first >= overlap_end) continue;
            const std::uint64_t offset = overlap_first - bank_base;
            const std::uint64_t length = overlap_end - overlap_first;
            for (std::uint64_t alias = 0; alias < mirror_count; ++alias) {
                const std::uint64_t alias_first = mirror_base + alias * mirror_size + offset;
                expanded.push_back(
                    {static_cast<std::uint32_t>(alias_first), alias_first + length});
            }
        }
    }

    std::sort(expanded.begin(), expanded.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    std::size_t unique_count = 0;
    for (const auto& range : expanded) {
        if (unique_count != 0 && range.first <= expanded[unique_count - 1].end) {
            expanded[unique_count - 1].end =
                std::max(expanded[unique_count - 1].end, range.end);
        } else {
            expanded[unique_count++] = range;
        }
    }
    expanded.resize(unique_count);
    return expanded;
}


void Machine::Impl::rebuild_pages(CpuId id) {
    CpuRuntime& rt = runtime(id);
    rt.page_read.fill(nullptr);
    rt.page_write.fill(nullptr);
    map_alias(rt, detail::kMainRamBase, 16 * 1024 * 1024, main_ram.data(), detail::kMainRamSize);

    const unsigned cpu_index = static_cast<unsigned>(id);
    const unsigned mode = wramcnt & 3u;
    if (cpu_index == 0) {
        if (mode == 0) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                  shared_wram.data(), detail::kSharedWramSize);
        else if (mode == 1) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                      shared_wram.data() + 16 * 1024, 16 * 1024);
        else if (mode == 2) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                      shared_wram.data(), 16 * 1024);
    } else {
        if (mode == 0) map_alias(rt, detail::kSharedWramBase, 8 * 1024 * 1024,
                                 arm7_wram.data(), detail::kArm7WramSize);
        if (mode == 1) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                 shared_wram.data(), 16 * 1024);
        else if (mode == 2) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                      shared_wram.data() + 16 * 1024, 16 * 1024);
        else if (mode == 3) map_alias(rt, detail::kSharedWramBase, 16 * 1024 * 1024,
                                      shared_wram.data(), detail::kSharedWramSize);
        map_alias(rt, detail::kArm7WramBase, 8 * 1024 * 1024,
                  arm7_wram.data(), detail::kArm7WramSize);
    }

    if (id == CpuId::Arm9) {
        const std::uint32_t itcm_size = region_size(rt.itcm_region);
        const std::uint32_t dtcm_size = region_size(rt.dtcm_region);
        if ((rt.cp15_control & (1u << 16)) && itcm_size >= kPageSize) {
            const std::uint32_t base = rt.itcm_region & 0xfffff000u;
            map_alias(rt, base, itcm_size, itcm.data(), static_cast<std::uint32_t>(itcm.size()));
        }
        if ((rt.cp15_control & (1u << 18)) && dtcm_size >= kPageSize) {
            const std::uint32_t base = rt.dtcm_region & 0xfffff000u;
            map_alias(rt, base, dtcm_size, dtcm.data(), static_cast<std::uint32_t>(dtcm.size()));
        }
    }
    rebuild_vram_pages();
}

void Machine::Impl::rebuild_vram_pages() {
    for (unsigned ci = 0; ci < 2; ++ci) {
        CpuRuntime& rt = cpus[ci];
        auto* page_r = rt.page_read.data();
        auto* page_w = rt.page_write.data();
        for (std::uint32_t addr = 0x06000000; addr < 0x07000000; addr += kPageSize) {
            const std::uint32_t page = addr >> kPageShift;
            std::uint8_t* p = ci == 0 ? gpu2d.arm9_vram_page(addr) : gpu2d.arm7_vram_page(addr);
            page_r[page] = p;
            page_w[page] = p;
        }
        route_watched_pages_slow(rt);
    }
}

std::uint32_t Machine::Impl::read(CpuId id, std::uint32_t addr, int width) {
    const unsigned index = static_cast<unsigned>(id);
    if (width == 16) addr &= ~1u;
    if (width == 32) addr &= ~3u;
    CpuRuntime& rt = cpus[index];
    const std::uint32_t page = addr >> kPageShift;
    if (rt.page_read[page]) return load_width(rt.page_read[page] + (addr & kPageMask), width);

    const auto in_tcm = [](std::uint32_t a, std::uint32_t reg, bool enabled) {
        return enabled && (static_cast<std::uint32_t>(a - (reg & 0xfffff000u)) < region_size(reg));
    };
    if (id == CpuId::Arm9 && in_tcm(addr, rt.itcm_region, (rt.cp15_control & (1u << 16)) != 0)) {
        const std::uint32_t offset = (addr - (rt.itcm_region & 0xfffff000u)) % itcm.size();
        return load_width(itcm.data() + offset, width);
    }
    if (id == CpuId::Arm9 && in_tcm(addr, rt.dtcm_region, (rt.cp15_control & (1u << 18)) != 0)) {
        const std::uint32_t offset = (addr - (rt.dtcm_region & 0xfffff000u)) % dtcm.size();
        return load_width(dtcm.data() + offset, width);
    }
    if (addr >= 0x05000000 && addr < 0x08000000) {
        if (id == CpuId::Arm9) return gpu2d.arm9_read(addr, width);
        return gpu2d.arm7_read(addr, width);
    }
    if (addr >= 0x04000000 && addr < 0x05000000) {
        return io_read(id, addr, width);
    }
    if (id == CpuId::Arm7 && addr < 0x00004000) return 0;
    if (id == CpuId::Arm9 && addr >= 0xffff0000) return 0;
    if (addr >= 0x08000000 && addr < 0x0a000000) return width == 8 ? 0xff : width == 16 ? 0xffff : 0xffffffffu;
    return 0;
}

void Machine::Impl::write(CpuId id, std::uint32_t addr, std::uint32_t value,
                          int width, std::uint32_t guest_pc) {
    const unsigned index = static_cast<unsigned>(id);
    if (width == 16) addr &= ~1u;
    if (width == 32) addr &= ~3u;
    // ARM9 VRAM has a halfword-only write port. The direct-page fast path
    // below bypasses Gpu2D::arm9_write(), where this restriction is enforced.
    if (id == CpuId::Arm9 && width == 8 && addr >= 0x06000000u &&
        addr < 0x07000000u) {
        return;
    }
    CpuRuntime& rt = cpus[index];
    if (guest_write_is_watched(addr, width)) {
        const std::uint32_t old_value = read(id, addr, width);
        const Fiber* const fiber = Fiber::current();
        int fiber_id = -1;
        if (fiber == rt.fiber.get()) {
            fiber_id = 0;
        } else {
            for (std::size_t i = 0; i < rt.thread_fibers.size(); ++i) {
                if (fiber == rt.thread_fibers[i].get()) {
                    fiber_id = static_cast<int>(i + 1);
                    break;
                }
            }
        }
        const std::uint32_t new_value =
            width == 8 ? value & 0xffu : width == 16 ? value & 0xffffu : value;
        const std::uint64_t frame =
            total_ticks / (detail::kLineTicks * detail::kLinesPerFrame);
        const Cpu& cpu = rt.cpu;
        std::fprintf(stderr,
                     "[dsr-watch] frame=%llu cpu=%u pc=%08x ctx=%08x fiber=%d host=%p addr=%08x width=%d %08x->%08x",
                     static_cast<unsigned long long>(frame), index,
                     guest_pc ? guest_pc : rt.current_pc,
                     detail::current_thread_context(*this, id), fiber_id,
                     static_cast<const void*>(fiber), addr, width, old_value,
                     new_value);
        for (unsigned reg = 0; reg < 15; ++reg)
            std::fprintf(stderr, "%s r%u=%08x", reg == 0 ? " regs=" : ",",
                         reg, cpu.r[reg]);
        std::fprintf(stderr, " cpsr=%08x\n", rt.banks.cpsr(cpu));
    }
    const std::uint32_t page = addr >> kPageShift;
    if (rt.page_write[page]) {
        store_width(rt.page_write[page] + (addr & kPageMask), width, value);
        return;
    }
    if (rt.page_read[page]) {
        store_width(rt.page_read[page] + (addr & kPageMask), width, value);
        return;
    }
    const auto in_tcm = [](std::uint32_t a, std::uint32_t reg, bool enabled) {
        return enabled && (static_cast<std::uint32_t>(a - (reg & 0xfffff000u)) < region_size(reg));
    };
    if (id == CpuId::Arm9 && in_tcm(addr, rt.itcm_region, (rt.cp15_control & (1u << 16)) != 0)) {
        const std::uint32_t offset = (addr - (rt.itcm_region & 0xfffff000u)) % itcm.size();
        store_width(itcm.data() + offset, width, value);
        return;
    }
    if (id == CpuId::Arm9 && in_tcm(addr, rt.dtcm_region, (rt.cp15_control & (1u << 18)) != 0)) {
        const std::uint32_t offset = (addr - (rt.dtcm_region & 0xfffff000u)) % dtcm.size();
        store_width(dtcm.data() + offset, width, value);
        return;
    }
    if (addr >= 0x05000000 && addr < 0x08000000) {
        if (id == CpuId::Arm9) gpu2d.arm9_write(addr, value, width);
        else gpu2d.arm7_write(addr, value, width);
        return;
    }
    if (addr >= 0x04000000 && addr < 0x05000000) {
        io_write(id, addr, value, width);
        return;
    }
}

std::uint32_t slow_read32(Cpu* c, std::uint32_t addr) {
    return detail::MachineAccess::impl(c).read(c->id, addr, 32);
}
std::uint32_t slow_read16(Cpu* c, std::uint32_t addr) {
    return detail::MachineAccess::impl(c).read(c->id, addr, 16);
}
std::uint32_t slow_read8(Cpu* c, std::uint32_t addr) {
    return detail::MachineAccess::impl(c).read(c->id, addr, 8);
}
void slow_write32(Cpu* c, std::uint32_t addr, std::uint32_t value,
                  std::uint32_t guest_pc) {
    detail::MachineAccess::impl(c).write(c->id, addr, value, 32, guest_pc);
}
void slow_write16(Cpu* c, std::uint32_t addr, std::uint32_t value,
                  std::uint32_t guest_pc) {
    detail::MachineAccess::impl(c).write(c->id, addr, value, 16, guest_pc);
}
void slow_write8(Cpu* c, std::uint32_t addr, std::uint32_t value,
                 std::uint32_t guest_pc) {
    detail::MachineAccess::impl(c).write(c->id, addr, value, 8, guest_pc);
}

} // namespace dsr
