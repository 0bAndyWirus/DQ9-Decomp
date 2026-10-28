#include "swi.h"

#include <array>
#include <algorithm>

#include <limits>

#include <bit>
#include <cmath>
#include <cstdint>

#include "dsr/mem.h"
#include "dsr/rt.h"
#include <cstdio>
#include <cstdlib>
#include "dsr/gpu2d.h"
#include "dsr/machine.h"

namespace dsr::detail {
namespace {
struct SwiTraceContext {
    Cpu* cpu=nullptr;
    unsigned swi=0;
    std::array<std::uint32_t,4> regs{};
    std::uint64_t frame=0;
    std::uint32_t writes=0, slot_mask=0, width_mask=0;
    std::uint32_t first_address=0, last_address=0;
};
thread_local SwiTraceContext* active_swi_trace=nullptr;

std::FILE* swi_trace_file() {
    static std::FILE* file=[] {
#if defined(_WIN32)
        const wchar_t* path = _wgetenv(L"DSR_SWI_VRAM_TRACE_FILE");
        return path && *path ? _wfopen(path, L"w") : nullptr;
#else
        const char* path=std::getenv("DSR_SWI_VRAM_TRACE_FILE");
        return path && *path ? std::fopen(path,"w") : nullptr;
#endif
    }();
    return file;
}

class SwiTraceScope {
public:
    SwiTraceScope(Cpu* cpu,unsigned swi) : previous_(active_swi_trace) {
        std::FILE* file=swi_trace_file();
        if(!file) return;
        const std::uint64_t frame=cpu->m->ticks()/560190u;
        if(frame<28000u || frame>31000u) return;
        context_.cpu=cpu;
        context_.swi=swi;
        context_.frame=frame;
        for(unsigned i=0;i<4;++i) context_.regs[i]=cpu->r[i];
        active_swi_trace=&context_;
    }
    ~SwiTraceScope() {
        if(active_swi_trace==&context_) active_swi_trace=previous_;
        if(context_.writes==0) return;
        std::FILE* file=swi_trace_file();
        if(!file) return;
        std::fprintf(file,
            "frame=%llu cpu=%u swi=%02x r0=%08x r1=%08x r2=%08x r3=%08x "
            "writes=%u slots=%x widths=%x first=%08x last=%08x\n",
            static_cast<unsigned long long>(context_.frame),
            static_cast<unsigned>(context_.cpu->id),context_.swi,
            context_.regs[0],context_.regs[1],context_.regs[2],context_.regs[3],
            context_.writes,context_.slot_mask,context_.width_mask,
            context_.first_address,context_.last_address);
        std::fflush(file);
    }
private:
    SwiTraceContext context_{};
    SwiTraceContext* previous_;
};

void trace_swi_vram_write(Cpu* cpu,std::uint32_t address,unsigned width) {
    SwiTraceContext* context=active_swi_trace;
    if(!context || cpu->id!=CpuId::Arm9 || address<0x06800000u ||
       address>=0x06880000u) return;
    if(!cpu->m->gpu2d().arm9_vram_page(address)) return;
    if(context->writes++==0) context->first_address=address;
    context->last_address=address;
    context->slot_mask|=1u<<((address-0x06800000u)>>17);
    context->width_mask|=1u<<(width==8?0u:width==16?1u:2u);
}

void swi_wr8(Cpu* c,std::uint32_t a,std::uint32_t v) {
    trace_swi_vram_write(c,a,8); wr8(c,a,v);
}
void swi_wr16(Cpu* c,std::uint32_t a,std::uint32_t v) {
    trace_swi_vram_write(c,a,16); wr16(c,a,v);
}
void swi_wr32(Cpu* c,std::uint32_t a,std::uint32_t v) {
    trace_swi_vram_write(c,a,32); wr32(c,a,v);
}

struct CallbackTable {
    std::uint32_t open = 0;
    std::uint32_t close = 0;
    std::uint32_t get8 = 0;
    std::uint32_t get16 = 0;
    std::uint32_t get32 = 0;

    bool active() const { return open != 0 && get8 != 0; }
};

CallbackTable load_callbacks(Cpu* c, std::uint32_t address) {
    if (address == 0) return {};
    return {rd32(c, address), rd32(c, address + 4), rd32(c, address + 8),
            rd32(c, address + 12), rd32(c, address + 16)};
}

std::uint32_t call_callback(Cpu* c, std::uint32_t target, std::uint32_t arg0,
                            std::uint32_t arg1 = 0, std::uint32_t arg2 = 0,
                            unsigned arg_count = 1) {
    std::array<std::uint32_t, 14> saved_regs{};
    for (unsigned i = 1; i <= 14; ++i) saved_regs[i - 1] = c->r[i];
    const std::uint32_t saved_n = c->n;
    const std::uint32_t saved_z = c->z;
    const std::uint32_t saved_cf = c->cf;
    const std::uint32_t saved_v = c->v;
    const std::uint32_t saved_q = c->q;
    const std::uint32_t saved_ctrl = c->cpsr_ctrl;

    c->r[0] = arg0;
    if (arg_count >= 2) c->r[1] = arg1;
    if (arg_count >= 3) c->r[2] = arg2;
    dsr_call(c, target);
    const std::uint32_t result = c->r[0];

    for (unsigned i = 1; i <= 14; ++i) c->r[i] = saved_regs[i - 1];
    c->n = saved_n;
    c->z = saved_z;
    c->cf = saved_cf;
    c->v = saved_v;
    c->q = saved_q;
    c->cpsr_ctrl = saved_ctrl;
    c->r[0] = result;
    return result;
}

struct InputStream {
    Cpu* cpu;
    std::uint32_t position;
    CallbackTable callbacks;

    std::uint8_t get8() {
        const std::uint32_t value = callbacks.active()
                                        ? call_callback(cpu, callbacks.get8, position)
                                        : rd8(cpu, position);
        ++position;
        return static_cast<std::uint8_t>(value);
    }

    std::uint32_t get32() {
        std::uint32_t value;
        if (callbacks.active() && callbacks.get32 != 0) {
            value = call_callback(cpu, callbacks.get32, position);
            position += 4;
        } else if (callbacks.active()) {
            value = static_cast<std::uint32_t>(get8()) << 24;
            value |= static_cast<std::uint32_t>(get8()) << 16;
            value |= static_cast<std::uint32_t>(get8()) << 8;
            value |= get8();
        } else {
            value = rd32(cpu, position);
            position += 4;
        }
        return value;
    }
};

bool open_stream(Cpu* c, InputStream& stream, std::uint32_t source,
                 std::uint32_t destination, std::uint32_t callback_arg,
                 std::uint32_t& header) {
    stream.position = source;
    if (stream.callbacks.active()) {
        header = call_callback(c, stream.callbacks.open, source, destination,
                               callback_arg, 3);
        if (std::bit_cast<std::int32_t>(header) < 0) {
            c->r[0] = header;
            return false;
        }
        stream.position += 4;
    } else {
        header = rd32(c, source);
        stream.position += 4;
    }
    return true;
}

void close_stream(Cpu* c, const InputStream& stream, std::uint32_t output_size) {
    if (!stream.callbacks.active()) return;
    if (stream.callbacks.close != 0) {
        const std::uint32_t result = call_callback(c, stream.callbacks.close,
                                                   stream.position);
        if (std::bit_cast<std::int32_t>(result) < 0) {
            c->r[0] = result;
            return;
        }
    }
    c->r[0] = output_size;
}

struct OutputStream {
    Cpu* cpu;
    std::uint32_t destination;
    bool halfword_writes;
    std::uint32_t size = 0;
    std::uint8_t pending = 0;

    void put(std::uint8_t value) {
        if (!halfword_writes) {
            swi_wr8(cpu, destination + size, value);
        } else if ((size & 1u) == 0) {
            pending = value;
        } else {
            swi_wr16(cpu, destination + size - 1,
                 static_cast<std::uint32_t>(pending) |
                     (static_cast<std::uint32_t>(value) << 8));
        }
        ++size;
    }

};

std::uint32_t isqrt(std::uint32_t value) {
    std::uint32_t result = 0;
    std::uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// Check the decoded range for timing without an extra source read.
// Project compatibility target: a reference emulator's observable output.
bool oracle_stream_range_valid(std::uint32_t source, std::uint32_t header) {
    return (source & 0x0e000000u) != 0 &&
           ((source + ((header >> 8) & 0x1fffffu)) & 0x0e000000u) != 0;
}

std::uint32_t divide(Cpu* c) {
    const std::int32_t numerator = std::bit_cast<std::int32_t>(c->r[0]);
    const std::int32_t denominator = std::bit_cast<std::int32_t>(c->r[1]);
    if (denominator == 0) return 0;
    std::int32_t quotient;
    std::int32_t remainder;
    if (numerator == std::numeric_limits<std::int32_t>::min() &&
        denominator == -1) {
        // ARM signed division wraps this otherwise-overflowing quotient.
        quotient = std::numeric_limits<std::int32_t>::min();
        remainder = 0;
    } else {
        quotient = numerator / denominator;
        remainder = numerator % denominator;
    }
    c->r[0] = std::bit_cast<std::uint32_t>(quotient);
    c->r[1] = std::bit_cast<std::uint32_t>(remainder);
    const std::uint32_t quotient_bits = c->r[0];
    c->r[3] = (quotient_bits & 0x80000000u) != 0 ? 0u - quotient_bits
                                                    : quotient_bits;
    return 6;
}

void cpu_set(Cpu* c) {
    const std::uint32_t source = c->r[0];
    const std::uint32_t destination = c->r[1];
    const std::uint32_t mode = c->r[2];
    const std::uint32_t count = mode & 0x001fffffu;
    const bool fill = (mode & (1u << 24)) != 0;
    const bool word = (mode & (1u << 26)) != 0;
    if (word) {
        if (fill && count != 0) {
            const std::uint32_t value = rd32(c, source);
            for (std::uint32_t i = 0; i < count; ++i) {
                swi_wr32(c, destination + i * 4, value);
            }
        } else {
            for (std::uint32_t i = 0; i < count; ++i) {
                swi_wr32(c, destination + i * 4, rd32(c, source + i * 4));
            }
        }
    } else if (fill && count != 0) {
        const std::uint32_t value = rd16(c, source);
        for (std::uint32_t i = 0; i < count; ++i) {
            swi_wr16(c, destination + i * 2, value);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            swi_wr16(c, destination + i * 2, rd16(c, source + i * 2));
        }
    }
}

void cpu_fast_set(Cpu* c) {
    const std::uint32_t source = c->r[0];
    const std::uint32_t destination = c->r[1];
    const std::uint32_t mode = c->r[2];
    const std::uint32_t count = mode & 0x001fffffu;
    const bool fill = (mode & (1u << 24)) != 0;
    const std::uint32_t fill_value = fill && count != 0 ? rd32(c, source) : 0;

    std::array<std::uint32_t, 8> block{};
    std::uint32_t index = 0;
    for (; index + 8 <= count; index += 8) {
        if (fill) {
            block.fill(fill_value);
        } else {
            for (unsigned i = 0; i < block.size(); ++i) {
                block[i] = rd32(c, source + (index + i) * 4);
            }
        }
        for (unsigned i = 0; i < block.size(); ++i) {
            swi_wr32(c, destination + (index + i) * 4, block[i]);
        }
    }
    for (; index < count; ++index) {
        swi_wr32(c, destination + index * 4,
             fill ? fill_value : rd32(c, source + index * 4));
    }
}

std::uint32_t bit_unpack(Cpu* c) {
    const std::uint32_t source = c->r[0];
    std::uint32_t destination = c->r[1];
    const std::uint32_t info = c->r[2];
    const std::uint32_t source_bytes = rd16(c, info);
    const unsigned source_width = rd8(c, info + 2);
    const unsigned destination_width = rd8(c, info + 3);
    const std::uint32_t unpack_config = rd32(c, info + 4);
    const std::uint32_t offset = unpack_config & 0x7fffffffu;
    const bool add_to_zero = (unpack_config & 0x80000000u) != 0;
    if ((source_width != 1 && source_width != 2 && source_width != 4 &&
         source_width != 8) ||
        destination_width == 0 || destination_width > 32 ||
        (destination_width != 1 && destination_width != 2 &&
         destination_width != 4 && destination_width != 8 &&
         destination_width != 16 && destination_width != 32)) {
        return 0;
    }

    const std::uint32_t value_mask = destination_width == 32
                                         ? 0xffffffffu
                                         : (1u << destination_width) - 1u;
    std::uint32_t output_word = 0;
    unsigned output_bits = 0;
    const std::uint32_t source_count = (source_bytes * 8) / source_width;
    const std::uint32_t source_mask = (1u << source_width) - 1u;
    for (std::uint32_t i = 0; i < source_count; ++i) {
        const std::uint32_t source_bit = i * source_width;
        const std::uint32_t packed = rd8(c, source + (source_bit >> 3));
        std::uint32_t value = (packed >> (source_bit & 7u)) & source_mask;
        if (value != 0 || add_to_zero) value += offset;
        output_word |= (value & value_mask) << output_bits;
        output_bits += destination_width;
        if (output_bits == 32) {
            swi_wr32(c, destination, output_word);
            destination += 4;
            output_word = 0;
            output_bits = 0;
        }
    }
    if (output_bits != 0) swi_wr32(c, destination, output_word);
    return 1;
}

std::uint32_t lz77_decode(Cpu* c, std::uint32_t source, std::uint32_t destination,
                 CallbackTable callbacks, std::uint32_t callback_arg,
                 bool vram_output) {
    InputStream input{c, source, callbacks};
    std::uint32_t header = 0;
    if (!open_stream(c, input, source, destination, callback_arg, header)) return 0;
    const std::uint32_t output_size = header >> 8;
    // Nonempty completions have zero modeled cycles.
    const std::uint32_t cycles =
        output_size == 0 && oracle_stream_range_valid(input.position, header) ? 1u : 0u;
    OutputStream output{c, destination, vram_output};
    std::array<std::uint8_t, 0x1000> window{};
    std::uint32_t produced = 0;
    std::uint8_t flags = 0;
    unsigned bits_left = 0;

    while (produced < output_size) {
        if (bits_left == 0) {
            flags = input.get8();
            bits_left = 8;
        }
        const bool compressed = (flags & 0x80u) != 0;
        flags <<= 1;
        --bits_left;

        if (!compressed) {
            const std::uint8_t value = input.get8();
            output.put(value);
            window[produced & 0xfffu] = value;
            ++produced;
            continue;
        }

        const std::uint32_t first = input.get8();
        const std::uint32_t second = input.get8();
        const std::uint32_t length = (first >> 4) + 3;
        const std::uint32_t distance = (((first & 0x0fu) << 8) | second) + 1;
        for (std::uint32_t i = 0; i < length && produced < output_size; ++i) {
            const std::uint32_t source_address=destination+produced-distance;
            const std::uint8_t value = !vram_output && distance<=produced
                                           ? window[(produced-distance)&0xfffu]
                                           : rd8(c,source_address);
            output.put(value);
            window[produced & 0xfffu] = value;
            ++produced;
        }
    }
    close_stream(c, input, output_size);
    return cycles;
}

std::uint32_t rl_decode(Cpu* c, std::uint32_t source, std::uint32_t destination,
               CallbackTable callbacks, std::uint32_t callback_arg,
               bool vram_output) {
    InputStream input{c, source, callbacks};
    std::uint32_t header = 0;
    if (!open_stream(c, input, source, destination, callback_arg, header)) return 0;
    const std::uint32_t output_size = header >> 8;
    const std::uint32_t cycles =
        output_size == 0 && oracle_stream_range_valid(input.position, header) ? 1u : 0u;
    OutputStream output{c, destination, vram_output};
    while (output.size < output_size) {
        const std::uint32_t flag = input.get8();
        if ((flag & 0x80u) != 0) {
            const std::uint32_t run = (flag & 0x7fu) + 3;
            const std::uint8_t value = input.get8();
            for (std::uint32_t i = 0; i < run && output.size < output_size; ++i) {
                output.put(value);
            }
        } else {
            const std::uint32_t run = (flag & 0x7fu) + 1;
            for (std::uint32_t i = 0; i < run && output.size < output_size; ++i) {
                output.put(input.get8());
            }
        }
    }
    close_stream(c, input, output_size);
    return cycles;
}

std::uint8_t huffman_symbol(InputStream& input,
                            const std::array<std::uint8_t, 0x200>& tree,
                            std::uint32_t tree_size, unsigned& bits_left,
                            std::uint32_t& bit_word, std::uint8_t root) {
    std::uint32_t position = 0;
    std::uint8_t node = root;
    for (std::uint32_t depth = 0; depth < tree_size; ++depth) {
        if (bits_left == 0) {
            bit_word = input.get32();
            bits_left = 32;
        }
        const unsigned branch = bit_word >> 31;
        bit_word <<= 1;
        --bits_left;

        if (position == 0) ++position;
        else position += ((node & 0x3fu) + 1u) << 1;
        const std::uint32_t child_index = position + branch;
        if (child_index >= tree_size) return 0;
        const bool is_leaf = (node & (branch != 0 ? 0x40u : 0x80u)) != 0;
        node = tree[child_index];
        if (is_leaf) return node;
    }
    return 0;
}

std::uint32_t huffman_decode(Cpu* c, std::uint32_t source, std::uint32_t destination,
                    CallbackTable callbacks, std::uint32_t temp_buffer) {
    InputStream input{c, source, callbacks};
    std::uint32_t header = 0;
    if (!open_stream(c, input, source, destination, temp_buffer, header)) return 0;
    const std::uint32_t output_size = header >> 8;
    const std::uint32_t cycles = oracle_stream_range_valid(input.position, header) ? 1u : 0u;
    const unsigned unit_width = header & 0x0fu;
    const std::uint32_t tree_size_byte = input.get8();
    const std::uint32_t tree_size = (tree_size_byte + 1) * 2;
    std::array<std::uint8_t, 0x200> tree{};
    for (std::uint32_t i = 0; i < tree_size; ++i) {
        tree[i] = input.get8();
        if (callbacks.active() && temp_buffer != 0) swi_wr8(c, temp_buffer + i, tree[i]);
    }
    const std::uint8_t root = tree[0];
    --input.position;

    const std::uint32_t padded_size = (output_size + 3u) & ~3u;
    const std::uint32_t symbols_per_byte = unit_width == 4 ? 2 : 1;
    unsigned bits_left = 0;
    std::uint32_t bit_word = 0;
    for (std::uint32_t output_offset = 0; output_offset < padded_size;
         output_offset += 4) {
        std::uint32_t word = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            std::uint8_t value;
            if (symbols_per_byte == 2) {
                const std::uint8_t low = huffman_symbol(input, tree, tree_size,
                                                        bits_left, bit_word, root);
                const std::uint8_t high = huffman_symbol(input, tree, tree_size,
                                                         bits_left, bit_word, root);
                value = static_cast<std::uint8_t>((low & 0x0fu) |
                                                  ((high & 0x0fu) << 4));
            } else {
                value = huffman_symbol(input, tree, tree_size, bits_left, bit_word, root);
            }
            word |= static_cast<std::uint32_t>(value) << (byte * 8);
        }
        swi_wr32(c, destination + output_offset, word);
    }
    close_stream(c, input, output_size);
    return cycles;
}


void crc16(Cpu* c) {
    std::uint32_t crc = c->r[0] & 0xffffu;
    const std::uint32_t source = c->r[1];
    // BIOS processes complete halfwords; the low bit of the byte count is
    // ignored by the NDS implementation.
    const std::uint32_t size = c->r[2] & ~1u;
    for (std::uint32_t i = 0; i < size; ++i) {
        crc ^= rd8(c, source + i);
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0xa001u : 0u);
        }
    }
    c->r[0] = crc & 0xffffu;
    c->r[3] = size != 0 ? rd16(c, source + size - 2) : 0;
}

void diff8(Cpu* c, bool vram_output) {
    const std::uint32_t source = c->r[0];
    const std::uint32_t destination = c->r[1];
    const std::uint32_t size = rd32(c, source) >> 8;
    OutputStream output{c, destination, vram_output};
    std::uint8_t value = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        value = static_cast<std::uint8_t>(value + rd8(c, source + 4 + i));
        output.put(value);
    }
}

void diff16(Cpu* c) {
    const std::uint32_t source = c->r[0];
    std::uint32_t destination = c->r[1];
    const std::uint32_t byte_size = rd32(c, source) >> 8;
    const std::uint32_t units = byte_size / 2;
    std::uint16_t value = 0;
    for (std::uint32_t i = 0; i < units; ++i) {
        value = static_cast<std::uint16_t>(value + rd16(c, source + 4 + i * 2));
        swi_wr16(c, destination, value);
        destination += 2;
    }
}

// The ARM7 BIOS volume lookup is represented by rounded exponential ranges.
std::uint32_t volume_table_value(std::uint32_t index) {
    if (index >= 724) return 0;
    if (index == 0) return 0;

    double value;
    if (index < 483) {
        value = 0.5 * std::pow(10.0, (static_cast<double>(index) - 0.525) / 200.0);
        return static_cast<std::uint32_t>(std::min<long>(127, std::lround(value)));
    }
    if (index < 603) {
        value = 32.0 * std::pow(10.0,
                                (static_cast<double>(index - 483) + 0.8) / 200.0);
        return static_cast<std::uint32_t>(std::min<long>(126, std::lround(value)));
    }
    if (index < 663) {
        value = 64.0 * std::pow(10.0,
                                (static_cast<double>(index - 603) + 0.408) / 200.0);
        return static_cast<std::uint32_t>(std::min<long>(126, std::lround(value)));
    }
    value = 64.0 * std::exp(0.0115 * (static_cast<double>(index - 663) + 0.245));
    return static_cast<std::uint32_t>(std::min<long>(127, std::lround(value)));
}

} // namespace

std::optional<std::uint32_t> handle_data_swi(Cpu* c, std::uint32_t imm) {
    if (c == nullptr) return std::nullopt;
    const unsigned number=imm;
    SwiTraceScope trace(c,number);
    switch (number) {
    case 0x09: // NDS Div: signed quotient in r0, remainder in r1, |quotient| in r3.
        return divide(c);
    case 0x0b:
        cpu_set(c);
        return 1;
    case 0x0c:
        cpu_fast_set(c);
        return 1;
    case 0x0d:
        c->r[0] = isqrt(c->r[0]);
        return 1;
    case 0x0e:
        crc16(c);
        return 1;
    case 0x0f: {
        const std::uint32_t address = c->id == CpuId::Arm7 ? 0x027ffffau
                                                            : 0x027ffff8u;
        const std::uint32_t mirror = address - 0x00400000u;
        const std::uint32_t probe = rd16(c, address) ^ 0xffffu;
        wr16(c, address, probe);
        // The 4 MB retail RAM mirrors at +4 MB; debug hardware has 8 MB and
        // leaves the corresponding 0x023ffffx halfword unchanged.
        c->r[0] = rd16(c, mirror) != probe ? 1u : 0u;
        return 1;
    }
    case 0x10:
        return bit_unpack(c);
    case 0x11:
        return lz77_decode(c, c->r[0], c->r[1], {}, c->r[2], false);
    case 0x12:
        return lz77_decode(c, c->r[0], c->r[1], load_callbacks(c, c->r[3]), c->r[2],
                           true);
    case 0x13:
        return huffman_decode(c, c->r[0], c->r[1], load_callbacks(c, c->r[3]), c->r[2]);
    case 0x14:
        return rl_decode(c, c->r[0], c->r[1], {}, c->r[2], false);
    case 0x15:
        return rl_decode(c, c->r[0], c->r[1], load_callbacks(c, c->r[3]), c->r[2],
                         true);
    case 0x16:
        if (c->id != CpuId::Arm9) return std::nullopt;
        diff8(c, false);
        return 1;
    case 0x18:
        if (c->id != CpuId::Arm9) return std::nullopt;
        diff16(c);
        return 1;
    case 0x1a: // Raw index: round(32767 * sin(index * pi / 128)).
        if (c->id != CpuId::Arm7) return std::nullopt;
        if (c->r[0] >= 64) return 1;
        c->r[0] = static_cast<std::uint32_t>(std::lround(
            std::sin(static_cast<double>(c->r[0]) * 3.14159265358979323846 / 128.0) *
            32767.0));
        return 1;
    case 0x1b: // 768 index steps span one octave; result is Q16 fractional pitch.
        if (c->id != CpuId::Arm7) return std::nullopt;
        if (c->r[0] >= 768) return 1;
        c->r[0] = static_cast<std::uint32_t>(std::lround(
            (std::exp2(static_cast<double>(c->r[0]) / 768.0) - 1.0) * 65536.0));
        return 1;
    case 0x1c:
        if (c->id != CpuId::Arm7) return std::nullopt;
        if (c->r[0] >= 724) return 1;
        c->r[0] = volume_table_value(c->r[0]);
        return 1;
    // GetBootProcs is not implemented; do not invent BIOS routine addresses.
    default:
        return std::nullopt;
    }
}

} // namespace dsr::detail
