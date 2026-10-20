#include "core_internal.h"
#include "swi.h"
#include "dsr/machine.h"
#include "dsr/mem.h"
#include "dsr/rt.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

#include <utility>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
namespace {
// Invented RAM/code locations used only by the synthetic differential cases.
constexpr std::uint32_t kBase = 0x02010000u;
constexpr std::uint32_t kSource = kBase + 0x1000u;
constexpr std::uint32_t kDestination = kBase + 0x3000u;
constexpr std::uint32_t kTextureDestination = 0x06800000u;
constexpr std::uint32_t kInfo = kBase + 0x5000u;
constexpr std::size_t kMemSize = 0x6000;
constexpr std::size_t kSourceBytes = 0x400;
constexpr std::size_t kDestinationBytes = 0x400;
constexpr std::size_t kVramBytes = 0x20000;
using Registers = std::array<std::uint32_t, 4>;
int failures = 0;

struct Image {
    std::array<std::uint8_t, kMemSize> bytes{};
    std::array<std::uint8_t, kVramBytes> vram{};

    std::size_t index(std::uint32_t address) const {
        if (address < kBase || address - kBase >= bytes.size()) {
            std::fprintf(stderr, "[swi-diff] oracle address out of range: %08x\n", address);
            std::abort();
        }
        return address - kBase;
    }
    std::uint8_t read8(std::uint32_t address) const {
        if (address >= kTextureDestination && address - kTextureDestination < vram.size())
            return vram[address - kTextureDestination];
        return bytes[index(address)];
    }
    std::uint16_t read16(std::uint32_t address) const {
        address &= ~1u;
        return static_cast<std::uint16_t>(read8(address) |
            (static_cast<std::uint16_t>(read8(address + 1)) << 8));
    }
    std::uint32_t read32(std::uint32_t address) const {
        address &= ~3u;
        return static_cast<std::uint32_t>(read8(address)) |
            (static_cast<std::uint32_t>(read8(address + 1)) << 8) |
            (static_cast<std::uint32_t>(read8(address + 2)) << 16) |
            (static_cast<std::uint32_t>(read8(address + 3)) << 24);
    }
    void write8(std::uint32_t address, std::uint8_t value) {
        if (address >= kTextureDestination && address - kTextureDestination < vram.size()) {
            vram[address - kTextureDestination] = value;
            return;
        }
        bytes[index(address)] = value;
    }
    void write16(std::uint32_t address, std::uint32_t value) {
        address &= ~1u;
        write8(address, static_cast<std::uint8_t>(value));
        write8(address + 1, static_cast<std::uint8_t>(value >> 8));
    }
    void write32(std::uint32_t address, std::uint32_t value) {
        address &= ~3u;
        for (unsigned i = 0; i < 4; ++i) write8(address + i, static_cast<std::uint8_t>(value >> (8 * i)));
    }
};

constexpr std::uint32_t kCallbackTable = kBase + 0x5400u;
constexpr std::uint32_t kCallbackBase = kBase + 0x400u;
constexpr std::uint32_t kCallbackOpen = kCallbackBase;
constexpr std::uint32_t kCallbackClose = kCallbackBase + 4;
constexpr std::uint32_t kCallbackGet8 = kCallbackBase + 8;
constexpr std::uint32_t kCallbackGet16 = kCallbackBase + 12;
constexpr std::uint32_t kCallbackGet32 = kCallbackBase + 16;
struct CallbackState {
    unsigned opens=0, closes=0, reads8=0, reads16=0, reads32=0;
    std::uint32_t open_source=0, open_destination=0, open_arg=0, close_source=0;
} callback_state;

void callback_dispatch(dsr::Cpu* cpu, std::uint32_t address) {
    dsr::Machine* machine=cpu->m;
    std::uint32_t result=0;
    switch(address) {
    case kCallbackOpen:
        ++callback_state.opens;
        callback_state.open_source=cpu->r[0];
        callback_state.open_destination=cpu->r[1];
        callback_state.open_arg=cpu->r[2];
        result=machine->read32(dsr::CpuId::Arm9,cpu->r[0]);
        break;
    case kCallbackClose:
        ++callback_state.closes;
        callback_state.close_source=cpu->r[0];
        break;
    case kCallbackGet8:
        ++callback_state.reads8;
        result=machine->read8(dsr::CpuId::Arm9,cpu->r[0]);
        break;
    case kCallbackGet16:
        ++callback_state.reads16;
        result=machine->read16(dsr::CpuId::Arm9,cpu->r[0]);
        break;
    case kCallbackGet32:
        ++callback_state.reads32;
        result=machine->read32(dsr::CpuId::Arm9,cpu->r[0]);
        break;
    default:
        std::abort();
    }
    for(unsigned i=1;i<=14;++i) cpu->r[i]=0xdead0000u|i;
    cpu->n=cpu->z=cpu->cf=cpu->v=cpu->q=1;
    cpu->cpsr_ctrl=dsr::kModeSys|dsr::kCpsrT;
    cpu->r[0]=result;
}
constexpr dsr::GuestAddressRange kCallbackCodeData{kCallbackBase,0x20};
constexpr dsr::DispatchRegionEntry kCallbackRegion{
    kCallbackBase,0x20,dsr::kModuleArm9Main,0,0xcbf29ce484222325ull,
    &kCallbackCodeData,1,callback_dispatch};

void reference_div(Registers& r) {
    const auto n = std::bit_cast<std::int32_t>(r[0]);
    const auto d = std::bit_cast<std::int32_t>(r[1]);
    if (d == 0) return; // Reference behavior leaves guest registers unchanged.
    std::int32_t q;
    std::int32_t rem;
    if (n == std::numeric_limits<std::int32_t>::min() && d == -1) {
        q = std::numeric_limits<std::int32_t>::min();
        rem = 0;
    } else {
        q = n / d;
        rem = n % d;
    }
    r[0] = std::bit_cast<std::uint32_t>(q);
    r[1] = std::bit_cast<std::uint32_t>(rem);
    r[3] = q < 0 ? 0u - r[0] : r[0];
}
void reference_sqrt(Registers& r) {
    std::uint32_t value = r[0];
    std::uint32_t result = 0;
    std::uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else result >>= 1;
        bit >>= 2;
    }
    r[0] = result;
}
void reference_crc(Image& m, Registers& r) {
    std::uint16_t crc = static_cast<std::uint16_t>(r[0]);
    const std::uint32_t size = r[2] >> 1;
    std::uint16_t last = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        last = m.read16(r[1] + i * 2);
        for (unsigned byte = 0; byte < 2; ++byte) {
            crc ^= static_cast<std::uint8_t>(last >> (byte * 8));
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = static_cast<std::uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
        }
    }
    r[0] = crc;
    r[3] = last;
}
void reference_cpuset(Image& m, Registers& r) {
    std::uint32_t src = r[0], dst = r[1], mode = r[2];
    const std::uint32_t count = mode & 0x1fffffu;
    const bool fill = (mode & (1u << 24)) != 0;
    if ((mode & (1u << 26)) != 0) {
        src &= ~3u; dst &= ~3u;
        const std::uint32_t value = fill && count ? m.read32(src) : 0;
        for (std::uint32_t i = 0; i < count; ++i)
            m.write32(dst + 4 * i, fill ? value : m.read32(src + 4 * i));
    } else {
        src &= ~1u; dst &= ~1u;
        const std::uint16_t value = fill && count ? m.read16(src) : 0;
        for (std::uint32_t i = 0; i < count; ++i)
            m.write16(dst + 2 * i, fill ? value : m.read16(src + 2 * i));
    }
}
void reference_fastset(Image& m, Registers& r) {
    std::uint32_t src = r[0] & ~3u, dst = r[1] & ~3u;
    const std::uint32_t count = r[2] & 0x1fffffu;
    const bool fill = (r[2] & (1u << 24)) != 0;
    const std::uint32_t value = fill && count ? m.read32(src) : 0;
    // Sequential-word values cross-checked against a reference emulator.
    for (std::uint32_t i = 0; i < count; ++i)
        m.write32(dst + i * 4, fill ? value : m.read32(src + i * 4));
}
void reference_bit_unpack(Image& m, Registers& r) {
    std::uint32_t src = r[0], dst = r[1];
    const std::uint32_t info = r[2];
    int remaining = m.read16(info);
    const unsigned src_width = m.read8(info + 2);
    const unsigned dst_width = m.read8(info + 3);
    std::uint32_t offset = m.read32(info + 4);
    const bool add_zero = (offset & 0x80000000u) != 0;
    offset &= 0x7fffffffu;
    if ((src_width != 1 && src_width != 2 && src_width != 4 && src_width != 8) ||
        (dst_width != 1 && dst_width != 2 && dst_width != 4 && dst_width != 8 && dst_width != 16 && dst_width != 32)) return;
    const unsigned mask = (1u << src_width) - 1;
    std::uint32_t data = 0;
    unsigned bit_write_count = 0;
    while (remaining-- > 0) {
        unsigned packed = m.read8(src++);
        for (unsigned bit_count = 0; bit_count < 8; bit_count += src_width) {
            unsigned value = packed & mask;
            if (value != 0 || add_zero) value += offset;
            data |= value << bit_write_count;
            bit_write_count += dst_width;
            if (bit_write_count >= 32) {
                m.write32(dst, data); dst += 4; data = 0; bit_write_count = 0;
            }
            packed >>= src_width;
        }
    }
}

void reference_lz(Image& m, std::uint32_t src, std::uint32_t dst, bool vram) {
    std::uint32_t header = m.read32(src); src += 4;
    int remaining = static_cast<int>(header >> 8);
    if ((src & 0x0e000000u) == 0 || ((src + static_cast<std::uint32_t>(remaining)) & 0x0e000000u) == 0) return;
    std::uint32_t output = dst;
    std::uint8_t pending = 0;
    unsigned byte_count = 0;
    auto put = [&](std::uint8_t value) {
        if (!vram) m.write8(output++, value);
        else {
            if (byte_count == 0) pending = value;
            else { m.write16(output, pending | (static_cast<std::uint32_t>(value) << 8)); output += 2; }
            byte_count ^= 1;
        }
    };
    while (remaining > 0) {
        std::uint8_t flags = m.read8(src++);
        for (unsigned bit = 0; bit < 8 && remaining > 0; ++bit, flags <<= 1) {
            if ((flags & 0x80u) == 0) {
                put(m.read8(src++)); --remaining;
            } else {
                const std::uint8_t high = m.read8(src++);
                const std::uint8_t low = m.read8(src++);
                const std::uint16_t code = static_cast<std::uint16_t>((high << 8) | low);
                const int length = (code >> 12) + 3;
                const std::uint32_t back = (code & 0x0fffu) + 1;
                for (int i = 0; i < length && remaining > 0; ++i) {
                    const std::uint32_t window_offset=output+(vram?byte_count:0u)-back;
                    const std::uint8_t value = m.read8(window_offset);
                    put(value); --remaining;
                }
            }
        }
    }
}
void reference_rl(Image& m, std::uint32_t src, std::uint32_t dst, bool vram) {
    std::uint32_t header = m.read32(src); src += 4;
    int remaining = static_cast<int>(header >> 8);
    if ((src & 0x0e000000u) == 0 || ((src + static_cast<std::uint32_t>(remaining)) & 0x0e000000u) == 0) return;
    std::uint32_t output = dst;
    std::uint8_t pending = 0;
    unsigned byte_count = 0;
    auto put = [&](std::uint8_t value) {
        if (!vram) m.write8(output++, value);
        else {
            if (byte_count == 0) pending = value;
            else { m.write16(output, pending | (static_cast<std::uint32_t>(value) << 8)); output += 2; }
            byte_count ^= 1;
        }
    };
    while (remaining > 0) {
        const std::uint8_t control = m.read8(src++);
        if (control & 0x80u) {
            const int run = (control & 0x7f) + 3;
            const std::uint8_t value = m.read8(src++);
            for (int i = 0; i < run && remaining > 0; ++i, --remaining) put(value);
        } else {
            const int run = (control & 0x7f) + 1;
            for (int i = 0; i < run && remaining > 0; ++i, --remaining) put(m.read8(src++));
        }
    }
}
void reference_huffman(Image& m, Registers& r) {
    std::uint32_t src=r[0], dst=r[1];
    const std::uint32_t header=m.read32(src); src+=4;
    if ((src&0x0e000000u)==0 || ((src+(header>>8))&0x0e000000u)==0) return;
    const std::uint8_t tree_size=m.read8(src++);
    const std::uint32_t tree=src;
    src+=((static_cast<std::uint32_t>(tree_size)+1u)<<1)-1u;
    int remaining=static_cast<int>(header>>8);
    std::uint32_t mask=0x80000000u;
    std::uint32_t data=m.read32(src); src+=4;
    int pos=0;
    const std::uint8_t root=m.read8(tree);
    std::uint8_t node=root;
    std::uint32_t word=0;
    unsigned byte_count=0, byte_shift=0;
    if ((header&0x0fu)==8) {
        while(remaining>0) {
            if(pos==0) ++pos;
            else pos+=((node&0x3fu)+1u)<<1;
            const bool right=(data&mask)!=0;
            const bool leaf=(node&(right?0x40u:0x80u))!=0;
            node=m.read8(tree+static_cast<std::uint32_t>(pos)+(right?1u:0u));
            if(leaf) {
                word|=static_cast<std::uint32_t>(node)<<byte_shift;
                ++byte_count; byte_shift+=8; pos=0; node=root;
                if(byte_count==4) {
                    m.write32(dst,word); dst+=4; word=0; byte_count=0; byte_shift=0; remaining-=4;
                }
            }
            mask>>=1;
            if(mask==0) { mask=0x80000000u; data=m.read32(src); src+=4; }
        }
    } else {
        int half_len=0, value=0;
        while(remaining>0) {
            if(pos==0) ++pos;
            else pos+=((node&0x3fu)+1u)<<1;
            const bool right=(data&mask)!=0;
            const bool leaf=(node&(right?0x40u:0x80u))!=0;
            node=m.read8(tree+static_cast<std::uint32_t>(pos)+(right?1u:0u));
            if(leaf) {
                value|=half_len==0?node:(node<<4);
                half_len+=4;
                if(half_len==8) {
                    word|=static_cast<std::uint32_t>(value)<<byte_shift;
                    ++byte_count; byte_shift+=8; half_len=0; value=0;
                    if(byte_count==4) {
                        m.write32(dst,word); dst+=4; word=0; byte_count=0; byte_shift=0; remaining-=4;
                    }
                }
                pos=0; node=root;
            }
            mask>>=1;
            if(mask==0) { mask=0x80000000u; data=m.read32(src); src+=4; }
        }
    }
}
void reference_diff8(Image& m, std::uint32_t src, std::uint32_t dst) {
    std::uint32_t size = m.read32(src) >> 8; src += 4;
    if (size == 0) return; // malformed zero-length headers are outside the BIOS contract
    std::uint8_t value = m.read8(src++); m.write8(dst++, value); --size;
    while (size-- != 0) { value = static_cast<std::uint8_t>(value + m.read8(src++)); m.write8(dst++, value); }
}
void reference_diff16(Image& m, std::uint32_t src, std::uint32_t dst) {
    std::uint32_t size = m.read32(src) >> 8; src += 4;
    if (size < 2) return;
    std::uint16_t value = m.read16(src); src += 2; m.write16(dst, value); dst += 2; size -= 2;
    while (size >= 2) { value = static_cast<std::uint16_t>(value + m.read16(src)); src += 2; m.write16(dst, value); dst += 2; size -= 2; }
}

void write_case_memory(dsr::Machine& machine, const Image& image) {
    for (std::uint32_t i = 0; i < kSourceBytes; ++i)
        machine.write8(dsr::CpuId::Arm9, kSource + i, image.read8(kSource + i));
    for (std::uint32_t i = 0; i < kDestinationBytes + 0x100; ++i)
        machine.write8(dsr::CpuId::Arm9, kDestination - 0x100 + i,
                       image.read8(kDestination - 0x100 + i));
    for (std::uint32_t i = 0; i < 12; ++i)
        machine.write8(dsr::CpuId::Arm9, kInfo + i, image.read8(kInfo + i));
    for (std::uint32_t i = 0; i < 256; i += 2)
        machine.write16(dsr::CpuId::Arm9, kTextureDestination + i,
                        image.read16(kTextureDestination + i));
}
template <class Reference>
bool compare_case(dsr::Machine& machine, Image& image, unsigned swi, Registers regs,
                  Reference reference, std::uint32_t output_size, const char* name) {
    const Image initial = image;
    const Registers input_regs = regs;
    reference(image, regs);
    write_case_memory(machine, initial);
    dsr::Cpu& cpu = machine.cpu(dsr::CpuId::Arm9);
    for (unsigned i = 0; i < 4; ++i) cpu.r[i] = input_regs[i];
    dsr_swi(&cpu, swi << 16, 0x02004000, false);
    bool equal = true;
    for (unsigned i = 0; i < 4; ++i) equal &= cpu.r[i] == regs[i];
    for (std::uint32_t i = 0; i < output_size; ++i)
        equal &= machine.read8(dsr::CpuId::Arm9, regs[1] + i) == image.read8(regs[1] + i);
    if (!equal) std::fprintf(stderr, "[swi-diff] mismatch in %s case\n", name);
    return equal;
}

std::uint32_t random32(std::mt19937& rng) { return static_cast<std::uint32_t>(rng()); }
std::uint8_t random8(std::mt19937& rng) { return static_cast<std::uint8_t>(rng()); }
void report(const char* name, unsigned cases, unsigned mismatches) {
    std::printf("[swi-diff] %-16s cases=%u matches=%u diverges=%u\n", name, cases, cases - mismatches, mismatches);
    failures += static_cast<int>(mismatches);
}
template<class Reference>
bool compare_callback_case(dsr::Machine& machine, Image& image, unsigned swi,
                           Registers regs, Reference reference,
                           std::uint32_t output_size, const char* name) {
    Image expected_image=image;
    Registers expected_regs=regs;
    reference(expected_image,expected_regs);
    expected_regs[0]=output_size;
    write_case_memory(machine,image);
    machine.write32(dsr::CpuId::Arm9,kCallbackTable,kCallbackOpen);
    machine.write32(dsr::CpuId::Arm9,kCallbackTable+4,kCallbackClose);
    machine.write32(dsr::CpuId::Arm9,kCallbackTable+8,kCallbackGet8);
    machine.write32(dsr::CpuId::Arm9,kCallbackTable+12,kCallbackGet16);
    machine.write32(dsr::CpuId::Arm9,kCallbackTable+16,kCallbackGet32);
    callback_state={};
    dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9);
    for(unsigned i=0;i<4;++i) cpu.r[i]=regs[i];
    cpu.n=cpu.z=cpu.cf=cpu.v=cpu.q=0;
    cpu.cpsr_ctrl=dsr::kModeSys;
    dsr_swi(&cpu,swi<<16,0x02004000,false);
    bool equal=true;
    for(unsigned i=0;i<4;++i) equal &= cpu.r[i]==expected_regs[i];
    equal &= cpu.n==0 && cpu.z==0 && cpu.cf==0 && cpu.v==0 && cpu.q==0;
    equal &= (cpu.cpsr_ctrl&dsr::kCpsrT)==0;
    equal &= callback_state.opens==1 && callback_state.closes==1;
    equal &= callback_state.open_source==regs[0] &&
             callback_state.open_destination==regs[1] &&
             callback_state.open_arg==regs[2];
    equal &= callback_state.reads8==5 &&
             callback_state.close_source==regs[0]+(swi==0x13?12u:9u);
    equal &= callback_state.reads32==(swi==0x13?1u:0u);
    for(std::uint32_t i=0;i<output_size;++i)
        equal &= machine.read8(dsr::CpuId::Arm9,regs[1]+i)==
                 expected_image.read8(regs[1]+i);
    if(!equal) std::fprintf(stderr,"[swi-diff] callback mismatch in %s\n",name);
    return equal;
}

void test_callback_streams(dsr::Machine& machine,std::mt19937& rng) {
    unsigned lz_mismatch=0,rl_mismatch=0,huff_mismatch=0;
    for(unsigned i=0;i<1000;++i) {
        Image lz;
        lz.write32(kSource,(4u<<8)|0x10u);
        lz.write8(kSource+4,0);
        for(unsigned j=0;j<4;++j) lz.write8(kSource+5+j,random8(rng));
        Registers lr{kSource,kTextureDestination,kBase+0x5800u,kCallbackTable};
        auto lz_ref=[](Image& im,Registers& r){reference_lz(im,r[0],r[1],true);};
        if(!compare_callback_case(machine,lz,0x12,lr,lz_ref,4,"LZ77 VRAM"))
            ++lz_mismatch;

        Image rl;
        rl.write32(kSource,(4u<<8)|0x30u);
        rl.write8(kSource+4,0x03);
        for(unsigned j=0;j<4;++j) rl.write8(kSource+5+j,random8(rng));
        Registers rr{kSource,kTextureDestination,kBase+0x5800u,kCallbackTable};
        auto rl_ref=[](Image& im,Registers& r){reference_rl(im,r[0],r[1],true);};
        if(!compare_callback_case(machine,rl,0x15,rr,rl_ref,4,"RL VRAM"))
            ++rl_mismatch;

        Image huff;
        huff.write32(kSource,(4u<<8)|0x28u);
        huff.write8(kSource+4,1);
        huff.write8(kSource+5,0xc0);
        huff.write8(kSource+6,0x11);
        huff.write8(kSource+7,0x22);
        huff.write32(kSource+8,random32(rng));
        Registers hr{kSource,kDestination,kBase+0x5800u,kCallbackTable};
        if(!compare_callback_case(machine,huff,0x13,hr,reference_huffman,4,
                                  "Huffman")) ++huff_mismatch;
    }
    report("LZ77 callback",1000,lz_mismatch);
    report("RL callback",1000,rl_mismatch);
    report("Huffman callback",1000,huff_mismatch);
}

void test_is_debugger(dsr::Machine& machine) {
    unsigned mismatch=0;
    for(dsr::CpuId id : {dsr::CpuId::Arm9,dsr::CpuId::Arm7}) {
        const std::uint32_t address=id==dsr::CpuId::Arm9?0x023ffff8u:0x023ffffau;
        const std::uint32_t scratch=address+0x00400000u;
        machine.write16(id,address,0x1357);
        dsr::Cpu& cpu=machine.cpu(id);
        cpu.r[0]=0xaaaaaaaa;cpu.r[1]=0x11111111;cpu.r[2]=0x22222222;cpu.r[3]=0x33333333;
        dsr_swi(&cpu,0x000f0000,0x02004000,false);
        mismatch+=cpu.r[0]!=0 || cpu.r[1]!=0x11111111 ||
                  cpu.r[2]!=0x22222222 || cpu.r[3]!=0x33333333 ||
                  machine.read16(id,address)!=(0x1357u^0xffffu) ||
                  machine.read16(id,scratch)!=(0x1357u^0xffffu);
    }
    report("IsDebugger",2,mismatch);
}

std::vector<std::uint32_t> formula_table(std::string_view name) {
    // Analytic BIOS lookup expectations, not copied table data.
    const std::size_t count = name == "getsinetbl" ? 64 : name == "getpitchtbl" ? 768 : 724;
    std::vector<std::uint32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const long double index = static_cast<long double>(i);
        long double value = 0;
        long double limit = std::numeric_limits<std::uint32_t>::max();
        if (name == "getsinetbl") {
            value = 32767.L * std::sin(index * std::numbers::pi_v<long double> / 128.L);
        } else if (name == "getpitchtbl") {
            value = 65536.L * (std::pow(2.L, index / 768.L) - 1.L);
        } else if (i != 0) {
            if (i < 483) {
                value = 0.5L * std::pow(10.L, (index - 0.525L) / 200.L);
                limit = 127;
            } else if (i < 603) {
                value = 32.L * std::pow(10.L, (index - 483.L + 0.8L) / 200.L);
                limit = 126;
            } else if (i < 663) {
                value = 64.L * std::pow(10.L, (index - 603.L + 0.408L) / 200.L);
                limit = 126;
            } else {
                value = 64.L * std::exp(0.0115L * (index - 663.L + 0.245L));
                limit = 127;
            }
        }
        values[i] = static_cast<std::uint32_t>(std::min(limit, std::round(value)));
    }
    return values;
}

std::vector<std::uint32_t> reference_table(const char* name) {
    if (DSR_BIOS_REFERENCE_SOURCE[0] == '\0') return formula_table(name);
    std::ifstream file(DSR_BIOS_REFERENCE_SOURCE);
    if(!file) { std::fprintf(stderr,"cannot read supplied reference table source\n"); std::abort(); }
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    const std::size_t marker=source.find(name);
    const std::size_t begin=marker==std::string::npos?marker:source.find('{',marker);
    const std::size_t end=begin==std::string::npos?begin:source.find('}',begin);
    if(begin==std::string::npos || end==std::string::npos) {
        std::fprintf(stderr,"reference table %s not found\n",name); std::abort();
    }
    std::vector<std::uint32_t> values;
    const char* p=source.c_str()+begin+1;
    const char* limit=source.c_str()+end;
    while(p<limit) {
        if(p+2<limit && p[0]=='0' && (p[1]=='x'||p[1]=='X')) {
            char* after=nullptr;
            values.push_back(static_cast<std::uint32_t>(std::strtoul(p,&after,0)));
            p=after;
        } else ++p;
    }
    return values;
}

void test_sound_tables(dsr::Machine& machine) {
    const std::array<const char*,3> names{"getsinetbl","getpitchtbl","getvoltbl"};
    const std::array<unsigned,3> swis{0x1a,0x1b,0x1c};
    const std::array<const char*,3> labels{"GetSineTable","GetPitchTable","GetVolumeTable"};
    for(unsigned table=0;table<names.size();++table) {
        const auto expected=reference_table(names[table]);
        unsigned mismatch=0;
        for(std::uint32_t index=0;index<expected.size();++index) {
            dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm7);
            cpu.r[0]=index;cpu.r[1]=0x11111111;cpu.r[2]=0x22222222;cpu.r[3]=0x33333333;
            dsr_swi(&cpu,swis[table]<<16,0x02004000,false);
            mismatch+=cpu.r[0]!=expected[index] || cpu.r[1]!=0x11111111 ||
                      cpu.r[2]!=0x22222222 || cpu.r[3]!=0x33333333;
        }
        report(labels[table],static_cast<unsigned>(expected.size()),mismatch);
    }
}

void test_math_copy_and_crc(dsr::Machine& machine, std::mt19937& rng) {
    unsigned mismatch = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        Registers r{random32(rng), random32(rng), random32(rng), random32(rng)};
        Registers expected = r;
        const auto numerator = std::bit_cast<std::int32_t>(r[0]);
        const auto denominator = std::bit_cast<std::int32_t>(r[1]);
        if (denominator != 0) {
            if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1) { expected[0] = 0x80000000u; expected[1] = 0; expected[3] = 0x80000000u; }
            else { const std::int32_t q = numerator / denominator; expected[0] = std::bit_cast<std::uint32_t>(q); expected[1] = std::bit_cast<std::uint32_t>(numerator % denominator); expected[3] = q < 0 ? 0u - expected[0] : expected[0]; }
        }
        dsr::Cpu& cpu = machine.cpu(dsr::CpuId::Arm9); for (unsigned j=0;j<4;++j) cpu.r[j]=r[j];
        dsr_swi(&cpu, 0x00090000, 0x02004000, false);
        for (unsigned j=0;j<4;++j) if (cpu.r[j] != expected[j]) ++mismatch;
    }
    report("Div", 1000, mismatch);

    mismatch = 0;
    for (unsigned i=0;i<1000;++i) {
        Registers r{random32(rng), random32(rng), random32(rng), random32(rng)};
        Registers expected=r; reference_sqrt(expected);
        dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9); for(unsigned j=0;j<4;++j) cpu.r[j]=r[j];
        dsr_swi(&cpu,0x000d0000,0x02004000,false);
        for(unsigned j=0;j<4;++j) if(cpu.r[j]!=expected[j]) ++mismatch;
    }
    report("Sqrt",1000,mismatch);

    mismatch=0;
    for(unsigned i=0;i<1000;++i) {
        Image m; for(unsigned j=0;j<128;++j) m.write8(kSource+j,random8(rng));
        Registers r{random32(rng)&0xffffu,kSource,static_cast<std::uint32_t>(rng()%129),random32(rng)};
        Registers expected=r; reference_crc(m,expected);
        write_case_memory(machine,m); dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9); for(unsigned j=0;j<4;++j) cpu.r[j]=r[j];
        dsr_swi(&cpu,0x000e0000,0x02004000,false);
        for(unsigned j=0;j<4;++j) if(cpu.r[j]!=expected[j]) ++mismatch;
    }
    // Odd lengths truncate to complete halfwords; zero-length r3 is zero in the BIOS reference.
    report("GetCRC16",1000,mismatch);

    for (unsigned mode : {0u,1u}) {
        mismatch=0;
        for(unsigned i=0;i<1000;++i) {
            Image m;
            const unsigned width = (rng() & 1u) ? 32u : 16u;
            const std::uint32_t count = rng()%65;
            const bool fill=(rng()&1u)!=0;
            for(unsigned j=0;j<256;++j) m.write8(kSource+j,random8(rng));
            for(unsigned j=0;j<256;++j) m.write8(kDestination+j,random8(rng));
            const Image initial=m;
            Registers r{kSource+1,kDestination+1,count|(fill?1u<<24:0)|
                        (mode==0 && width==32 ? 1u<<26:0),random32(rng)};
            Registers expected=r;
            if(mode==0) reference_cpuset(m,expected);
            else reference_fastset(m,expected);
            write_case_memory(machine,initial);
            dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9);
            for(unsigned j=0;j<4;++j)cpu.r[j]=r[j];
            dsr_swi(&cpu,(mode==0?0x000b0000:0x000c0000),0x02004000,false);
            for(unsigned j=0;j<4;++j)if(cpu.r[j]!=expected[j])++mismatch;
            const unsigned bytes=mode==0?(width==32?4:2)*count:4*count;
            for(unsigned j=0;j<bytes;++j)
                if(machine.read8(dsr::CpuId::Arm9,kDestination+j)!=m.read8(kDestination+j))++mismatch;
        }
        report(mode==0?"CpuSet":"CpuFastSet",1000,mismatch);
    }
}
void test_math_edges(dsr::Machine& machine) {
    unsigned div_mismatch=0;
    const std::array<std::pair<std::uint32_t,std::uint32_t>,5> cases{{
        {0x80000000u,0xffffffffu},{0x80000000u,0},{7u,0},
        {0xfffffff9u,3u},{7u,0xfffffffdu}}};
    for(const auto [n,d] : cases) {
        Registers regs{n,d,0x22222222u,0x33333333u};
        Registers expected=regs; reference_div(expected);
        dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9);
        for(unsigned i=0;i<4;++i) cpu.r[i]=regs[i];
        dsr_swi(&cpu,0x00090000,0x02004000,false);
        for(unsigned i=0;i<4;++i) div_mismatch+=cpu.r[i]!=expected[i];
    }
    report("Div edge cases",5,div_mismatch);

    unsigned sqrt_mismatch=0;
    for(std::uint32_t value : {0u,1u,2u,0x7fffffffu,0x80000000u,0xffffffffu}) {
        Registers regs{value,0x11111111u,0x22222222u,0x33333333u};
        Registers expected=regs; reference_sqrt(expected);
        dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9);
        for(unsigned i=0;i<4;++i) cpu.r[i]=regs[i];
        dsr_swi(&cpu,0x000d0000,0x02004000,false);
        for(unsigned i=0;i<4;++i) sqrt_mismatch+=cpu.r[i]!=expected[i];
    }
    report("Sqrt edge cases",6,sqrt_mismatch);
}

void test_copy_overlap(dsr::Machine& machine) {
    Image original;
    for(unsigned i=0;i<9;++i) original.write32(kSource+i*4,0x12340000u+i);

    Image cpu_set=original;
    Registers set_regs{kSource,kSource+4,4u|(1u<<26),0};
    const bool set_match=compare_case(machine,cpu_set,0x0b,set_regs,
                                      reference_cpuset,16,"CpuSet overlap");
    if(!set_match) ++failures;

    Image fast=original;
    Registers fast_regs{kSource,kSource+4,8u,0};
    const bool fast_matches_desmume=compare_case(machine,fast,0x0c,fast_regs,
                                                 reference_fastset,32,
                                                 "CpuFastSet overlap");
    bool block_semantics=true;
    for(unsigned i=0;i<32;++i)
        block_semantics &= machine.read8(dsr::CpuId::Arm9,kSource+4+i)==
                           original.read8(kSource+i);
    if(fast_matches_desmume || !block_semantics) ++failures;
    std::printf("[swi-diff] CpuFastSet overlap: native preserves LDMIA/STMIA block "
                "semantics; DeSmuME sequential-word HLE differs (GBATEK specifies "
                "8-word blocks)\n");
}
void test_direct_byte_write_rules(dsr::Machine& machine) {
    constexpr std::array<std::pair<std::uint32_t,std::uint16_t>,3> cases{{
        {0x06800000u,0x1234u},{0x05000000u,0x5678u},{0x07000000u,0x9abcu}}};
    unsigned mismatch=0;
    dsr::Cpu& cpu=machine.cpu(dsr::CpuId::Arm9);
    for(const auto [address,value] : cases) {
        machine.write16(dsr::CpuId::Arm9,address,value);
        dsr::wr8(&cpu,address,static_cast<std::uint8_t>(value^0xffu));
        mismatch+=machine.read16(dsr::CpuId::Arm9,address)!=value;
    }
    report("ARM9 wr8 VRAM/palette/OAM",3,mismatch);
}

void test_control_swis(dsr::Machine& machine) {
    unsigned mismatch=0;
    for(const auto [id,cycles] : {std::pair{dsr::CpuId::Arm9,2},
                                  std::pair{dsr::CpuId::Arm7,4}}) {
        dsr::Cpu& cpu=machine.cpu(id);
        cpu.r[0]=7;cpu.r[1]=0x11;cpu.r[2]=0x22;cpu.r[3]=0x33;cpu.budget=10000;
        dsr_swi(&cpu,0x00030000,0x02004000,false);
        mismatch+=cpu.r[0]!=0 || cpu.r[1]!=0x11 || cpu.r[2]!=0x22 ||
                  cpu.r[3]!=0x33 || cpu.budget!=10000-2-7*cycles;
    }
    dsr::Cpu& arm7=machine.cpu(dsr::CpuId::Arm7);
    machine.write32(dsr::CpuId::Arm7,0x04000504,0x100);
    arm7.r[0]=0xabcdef01;arm7.r[1]=2;arm7.r[2]=3;arm7.r[3]=4;arm7.budget=10000;
    dsr_swi(&arm7,0x00080000,0x02004000,false);
    mismatch+=arm7.r[0]!=0xabcdef01 || arm7.r[1]!=2 || arm7.r[2]!=3 ||
              arm7.r[3]!=4 || arm7.budget!=10000-256*2 ||
              machine.read32(dsr::CpuId::Arm7,0x04000504)!=0x200;
    report("WaitByLoop ARM7/9 + SoundBias",3,mismatch);
}

void test_swi_timing(dsr::Machine& machine) {
    unsigned cases=0, mismatch=0;
    // A SWI costs handler()+3: generated code charges one base cycle and
    // HLE supplies the other two dispatch cycles plus the handler result.
    const auto check=[&](dsr::CpuId id,unsigned swi,Registers regs,
                         std::uint32_t handler_cycles,const char* name) {
        for(bool thumb : {false,true}) {
            dsr::Cpu& cpu=machine.cpu(id);
            for(unsigned i=0;i<4;++i) cpu.r[i]=regs[i];
            constexpr std::int64_t budget=10000;
            cpu.budget=budget-1;
            cpu.cycle_fraction=37;
            dsr_swi(&cpu,thumb?swi:swi<<16,0x02004000,thumb);
            const std::int64_t actual=budget-cpu.budget;
            const std::int64_t expected=3+handler_cycles;
            ++cases;
            if(actual!=expected || cpu.cycle_fraction!=37) {
                ++mismatch;
                std::fprintf(stderr,
                    "[swi-diff] %s ARM%u %s SWI %02x cycles=%lld expected=%lld fraction=%u\n",
                    name,id==dsr::CpuId::Arm9?9u:7u,thumb?"Thumb":"ARM",swi,
                    static_cast<long long>(actual),static_cast<long long>(expected),
                    cpu.cycle_fraction);
            }
        }
    };
    for(dsr::CpuId id : {dsr::CpuId::Arm9,dsr::CpuId::Arm7}) {
        for(std::uint32_t denominator : {3u,0u,0xffffffffu}) {
            const Registers regs{0x80000000u,denominator,0x22222222u,0x33333333u};
            check(id,0x09,regs,denominator?6u:0u,"Div");
            Registers expected=regs;
            reference_div(expected);
            for(unsigned i=0;i<4;++i)
                if(machine.cpu(id).r[i]!=expected[i]) ++mismatch;
        }
        for(std::uint32_t value : {0u,0xffffffffu})
            check(id,0x0d,{value,0,0,0},1,"Sqrt");
        for(unsigned swi : {0x0bu,0x0cu}) {
            for(std::uint32_t count : {0u,8u,64u}) {
                machine.write32(id,kSource,0x12345678);
                check(id,swi,{kSource,kDestination,count|(1u<<26),0},1,"Copy");
                check(id,swi,{kSource,kDestination,count|(1u<<24)|(1u<<26),0},1,"Fill");
            }
        }
        for(std::uint32_t size : {0u,1u,8u,64u})
            check(id,0x0e,{0xffffu,kSource,size,0},1,"CRC16");
        check(id,0x0f,{0,0,0,0},1,"IsDebugger");
        for(const auto [source_width,destination_width] :
            {std::pair{1u,8u},std::pair{8u,32u},std::pair{0u,8u},
             std::pair{3u,8u},std::pair{1u,0u},std::pair{1u,3u},
             std::pair{1u,33u}}) {
            for(unsigned size : {0u,4u}) {
                machine.write16(id,kInfo,size);
                machine.write8(id,kInfo+2,source_width);
                machine.write8(id,kInfo+3,destination_width);
                machine.write32(id,kInfo+4,0);
                const bool valid=(source_width==1 || source_width==8) &&
                                 (destination_width==8 || destination_width==32);
                check(id,0x10,{kSource,kDestination,kInfo,0},valid?1u:0u,"BitUnPack");
            }
        }
        for(unsigned swi : {0x11u,0x12u,0x14u,0x15u}) {
            const bool lz=swi==0x11 || swi==0x12;
            machine.write32(id,kSource,lz?0x10u:0x30u);
            check(id,swi,{kSource,kDestination,0,0},1,"Empty decode");
            machine.write32(id,kSource,(4u<<8)|(lz?0x10u:0x30u));
            machine.write8(id,kSource+4,lz?0u:3u);
            for(unsigned i=0;i<4;++i) machine.write8(id,kSource+5+i,'A'+i);
            check(id,swi,{kSource,kDestination,0,0},0,"Literal decode");
            machine.write32(id,kSource,(8u<<8)|(lz?0x10u:0x30u));
            if(lz) {
                machine.write8(id,kSource+4,0x10);
                machine.write8(id,kSource+5,'A');
                machine.write8(id,kSource+6,'B');
                machine.write8(id,kSource+7,'C');
                machine.write8(id,kSource+8,0x20);
                machine.write8(id,kSource+9,2);
            } else {
                machine.write8(id,kSource+4,0x85);
                machine.write8(id,kSource+5,'A');
            }
            check(id,swi,{kSource,kDestination,0,0},0,"Compressed decode");
            // Unmapped reads supply a zero header without changing native
            // decoder behavior solely for the reference's range check.
            check(id,swi,{0x10000000u,kDestination,0,0},0,"Invalid decode range");
        }
        for(unsigned width : {4u,8u}) {
            for(unsigned size : {0u,4u}) {
                machine.write32(id,kSource,(size<<8)|0x20u|width);
                machine.write8(id,kSource+4,1);
                machine.write8(id,kSource+5,0xc0);
                machine.write8(id,kSource+6,1);
                machine.write8(id,kSource+7,2);
                machine.write32(id,kSource+8,0x55555555u);
                check(id,0x13,{kSource,kDestination,0,0},1,"Huffman");
            }
        }
        check(id,0x13,{0x10000000u,kDestination,0,0},0,"Invalid Huffman range");
        if(id==dsr::CpuId::Arm9) {
            for(unsigned swi : {0x16u,0x18u}) {
                machine.write32(id,kSource,(4u<<8)|0x80u|(swi==0x16?1u:2u));
                check(id,swi,{kSource,kDestination,0,0},1,"Diff");
            }
        } else {
            for(unsigned swi : {0x1au,0x1bu,0x1cu})
                for(std::uint32_t index : {0u,32u,0xffffffffu})
                    check(id,swi,{index,0,0,0},1,"Table");
        }
        for(unsigned delay : {0u,1u,100u})
            check(id,3,{delay,0,0,0},delay*(id==dsr::CpuId::Arm9?2u:4u),
                  "WaitByLoop");
    }

    dsr::Cpu& arm9=machine.cpu(dsr::CpuId::Arm9);
    const std::uint32_t control=dsr_mrc(&arm9,15,0,1,0,0,0);
    for(std::uint32_t tcm : {0u,1u<<16,1u<<18,(1u<<16)|(1u<<18)}) {
        dsr_mcr(&arm9,15,0,1,0,0,(control&~((1u<<16)|(1u<<18)))|tcm,0);
        for(unsigned delay : {0u,1u,100u})
            check(dsr::CpuId::Arm9,3,{delay,0,0,0},delay*(tcm?2u:8u),
                  "WaitByLoop TCM policy");
    }
    dsr_mcr(&arm9,15,0,1,0,0,control,0);

    // A rejected open is handled with zero handler cycles, not unsupported;
    // it must retain the callback result and never call close/read.
    for(unsigned swi : {0x12u,0x13u,0x15u}) {
        machine.write32(dsr::CpuId::Arm9,kSource,0x80000000u);
        machine.write32(dsr::CpuId::Arm9,kCallbackTable,kCallbackOpen);
        machine.write32(dsr::CpuId::Arm9,kCallbackTable+4,kCallbackClose);
        machine.write32(dsr::CpuId::Arm9,kCallbackTable+8,kCallbackGet8);
        callback_state={};
        check(dsr::CpuId::Arm9,swi,{kSource,kDestination,kInfo,kCallbackTable},0,
              "Failed stream open");
        mismatch+=arm9.r[0]!=0x80000000u || callback_state.opens!=2 ||
                  callback_state.closes!=0 || callback_state.reads8!=0 ||
                  callback_state.reads16!=0 || callback_state.reads32!=0;
    }

    // Preserve cycle debt and fraction when HLE crosses a slice boundary.
    arm9.r[0]=9;
    arm9.budget=0; // generated base cycle has exhausted this slice
    arm9.cycle_fraction=63;
    dsr_swi(&arm9,0x000d0000,0x02004000,false);
    ++cases;
    mismatch+=arm9.r[0]!=3 || arm9.budget!=-3 || arm9.cycle_fraction!=63;

    // Optional zero is handled, whereas wrong-CPU/control/unknown SWIs are
    // absent. Already-decoded numbers must not alias through low-byte masking.
    arm9.r[1]=0;
    const auto zero=dsr::detail::handle_data_swi(&arm9,0x09);
    ++cases;
    mismatch+=!zero.has_value() || *zero!=0;
    for(unsigned swi : {0x03u,0x1au,0xffu,0x109u}) {
        ++cases;
        mismatch+=dsr::detail::handle_data_swi(&arm9,swi).has_value();
    }
    for(unsigned swi : {0x16u,0x18u}) {
        ++cases;
        mismatch+=dsr::detail::handle_data_swi(&machine.cpu(dsr::CpuId::Arm7),swi).has_value();
    }
    ++cases;
    mismatch+=dsr::detail::handle_data_swi(nullptr,0x09).has_value();
    report("Exact HLE timing",cases,mismatch);
}

void test_bit_unpack(dsr::Machine& machine, std::mt19937& rng) {
    unsigned mismatch=0;
    constexpr std::array<unsigned,4> src_widths{1,2,4,8};
    constexpr std::array<unsigned,6> dst_widths{1,2,4,8,16,32};
    for(unsigned i=0;i<1000;++i) {
        Image m;
        const unsigned sw=src_widths[rng()%src_widths.size()];
        unsigned dw=dst_widths[rng()%dst_widths.size()];
        while(dw<sw) dw=dst_widths[rng()%dst_widths.size()];
        const unsigned src_bytes=4u*(1u+rng()%16u);
        const std::uint32_t src_mask=(1u<<sw)-1u;
        const std::uint32_t dst_mask=dw==32?0xffffffffu:(1u<<dw)-1u;
        const std::uint32_t max_offset=dst_mask-src_mask;
        const unsigned offset=random32(rng)%(max_offset+1u);
        const bool add_zero=(rng()&1u)!=0;
        for(unsigned j=0;j<src_bytes;++j) m.write8(kSource+j,random8(rng));
        m.write16(kInfo,src_bytes); m.write8(kInfo+2,sw); m.write8(kInfo+3,dw);
        m.write32(kInfo+4,offset|(add_zero?0x80000000u:0));
        Registers r{kSource,kDestination,kInfo,random32(rng)};
        const unsigned out_bytes=(src_bytes*8/sw*dw)/8;
        if(!compare_case(machine,m,0x10,r,reference_bit_unpack,out_bytes,"BitUnPack")) ++mismatch;
    }
    report("BitUnPack",1000,mismatch);
}

void test_huffman(dsr::Machine& machine, std::mt19937& rng) {
    unsigned mismatch=0;
    for(unsigned i=0;i<1000;++i) {
        Image m;
        const unsigned width=(i&1u)?4u:8u;
        m.write32(kSource,(4u<<8)|0x20u|width);
        m.write8(kSource+4,1); // four-byte, two-leaf tree
        m.write8(kSource+5,0xc0);
        m.write8(kSource+6,width==8?0x11:1);
        m.write8(kSource+7,width==8?0x22:2);
        m.write32(kSource+8,random32(rng));
        Registers r{kSource,kDestination,0x02008000u,0xaabbccddu};
        if(!compare_case(machine,m,0x13,r,reference_huffman,4,"Huffman"))++mismatch;
    }
    report("Huffman",1000,mismatch);
}

void test_lz_rl_diff(dsr::Machine& machine, std::mt19937& rng) {
    for (bool vram : {false,true}) {
        unsigned mismatch=0;
        for(unsigned i=0;i<1000;++i) {
            Image m; const unsigned size=rng()%128;
            m.write32(kSource,(size<<8)|0x10u);
            std::uint32_t cursor=kSource+4,produced=0;
            while(produced<size) {
                const std::uint32_t flag_address=cursor++;
                std::uint8_t flags=0;
                for(unsigned bit=0;bit<8 && produced<size;++bit) {
                    const unsigned remaining=size-produced;
                    const bool compressed=produced!=0 && remaining>=3 &&
                                           (rng()&1u)!=0;
                    if(compressed) {
                        const unsigned max_length=std::min(18u,remaining);
                        const unsigned length=3u+rng()%(max_length-2u);
                        const unsigned distance=1u+rng()%produced;
                        const std::uint16_t code=static_cast<std::uint16_t>(
                            ((length-3u)<<12)|(distance-1u));
                        m.write8(cursor++,code>>8);
                        m.write8(cursor++,code);
                        flags|=static_cast<std::uint8_t>(0x80u>>bit);
                        produced+=length;
                    } else {
                        m.write8(cursor++,random8(rng));
                        ++produced;
                    }
                }
                m.write8(flag_address,flags);
            }
            const std::uint32_t dst=vram?kTextureDestination:kDestination;
            for(unsigned j=1;j<=0x100;++j) m.write8(kDestination-j,random8(rng));
            for(unsigned j=0;j<256;++j) m.write8(dst+j,random8(rng));
            Registers r{kSource,dst,0x11223344u,0xaabbccddu};
            auto ref=[vram](Image& im,Registers& regs){reference_lz(im,regs[0],regs[1],vram);};
            if(!compare_case(machine,m,vram?0x12:0x11,r,ref,size,"LZ77")) ++mismatch;
        }
        report(vram?"LZ77VRAM":"LZ77WRAM",1000,mismatch);
    }
    for(bool vram : {false,true}) {
        unsigned mismatch=0;
        for(unsigned i=0;i<1000;++i) {
            Image m; const unsigned size=rng()%128;
            m.write32(kSource,(size<<8)|0x30u);
            unsigned produced=0; std::uint32_t cursor=kSource+4;
            while(produced<size) {
                if((rng()&1u)!=0) {
                    const unsigned run=std::min<unsigned>(size-produced,3u+rng()%16u);
                    m.write8(cursor++,static_cast<std::uint8_t>(0x80u|(run-3u)));m.write8(cursor++,random8(rng));produced+=run;
                } else {
                    const unsigned run=std::min<unsigned>(size-produced,1u+rng()%16u);
                    m.write8(cursor++,static_cast<std::uint8_t>(run-1u));
                    for(unsigned j=0;j<run;++j) m.write8(cursor++,random8(rng));
                    produced+=run;
                }
            }
            const std::uint32_t dst=vram?kTextureDestination:kDestination;
            for(unsigned j=0;j<256;++j) m.write8(dst+j,random8(rng));
            Registers r{kSource,dst,0x12345678u,0x87654321u};
            auto ref=[vram](Image& im,Registers& regs){reference_rl(im,regs[0],regs[1],vram);};
            if(!compare_case(machine,m,vram?0x15:0x14,r,ref,size,"RL")) ++mismatch;
        }
        report(vram?"RLVRAM":"RLWRAM",1000,mismatch);
    }
    unsigned mismatch8=0;
    for(unsigned i=0;i<1000;++i) {
        Image m; const unsigned size=1u+rng()%128;m.write32(kSource,(size<<8)|0x81u);m.write8(kSource+4,random8(rng));
        for(unsigned j=1;j<size;++j)m.write8(kSource+4+j,random8(rng));
        Registers r{kSource,kDestination,0x11111111u,0x22222222u};
        auto ref=[](Image& im,Registers& regs){reference_diff8(im,regs[0],regs[1]);};
        if(!compare_case(machine,m,0x16,r,ref,size,"Diff8"))++mismatch8;
    }
    report("Diff8",1000,mismatch8);
    unsigned mismatch16=0;
    for(unsigned i=0;i<1000;++i) {
        Image m; const unsigned size=2u*(1u+rng()%64);m.write32(kSource,(size<<8)|0x82u);
        for(unsigned j=0;j<size;j+=2)m.write16(kSource+4+j,random32(rng));
        Registers r{kSource,kDestination,0x11111111u,0x22222222u};
        auto ref=[](Image& im,Registers& regs){reference_diff16(im,regs[0],regs[1]);};
        if(!compare_case(machine,m,0x18,r,ref,size,"Diff16"))++mismatch16;
    }
    report("Diff16",1000,mismatch16);
}

int run() {
    dsr::MachineConfig config; config.direct_boot=false;
    config.arm9_regions={&kCallbackRegion,1};
    dsr::Machine machine(config);
    machine.write8(dsr::CpuId::Arm9,0x04000240,0x80);
    std::mt19937 rng(0xD09B105u);
    test_direct_byte_write_rules(machine);
    test_is_debugger(machine);
    test_sound_tables(machine);
    test_math_copy_and_crc(machine,rng);
    test_math_edges(machine);
    test_copy_overlap(machine);
    test_control_swis(machine);
    test_bit_unpack(machine,rng);
    test_huffman(machine,rng);
    test_callback_streams(machine,rng);
    test_lz_rl_diff(machine,rng);
    test_swi_timing(machine);
    return failures == 0 ? 0 : 1;
}
} // namespace
int main() { return run(); }
