#include "dsr/gpu3d.h"
#include "dsr/gpu2d.h"

#include "band_pool.h"
#include "capture_policy.h"
#include "cpu_features.h"
#include "texture.h"
#include "gpu3d_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define DSR_X86_SPANS 1
#if defined(__GNUC__) || defined(__clang__)
// PE has no ELF IFUNC resolver. Inline the identical SSE2 arithmetic into
// explicit default/AVX2 entry points and select one after CPU initialization.
#define DSR_WINDOWS_SPAN_DISPATCH 1
#define DSR_SPAN_TARGETS __attribute__((always_inline))
#else
// MSVC's baseline x64 target supports SSE2; never emit unsupported AVX2 here.
#define DSR_SPAN_TARGETS
#endif
#endif
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__ELF__)
#include <immintrin.h>
#define DSR_X86_SPANS 1
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
// IFUNC resolvers run before sanitizer initialization; retain plain SSE2 spans.
#define DSR_SPAN_TARGETS
#else
#define DSR_SPAN_TARGETS __attribute__((target_clones("avx2", "default")))
#endif
#endif

namespace dsr {
namespace {

using Matrix = std::array<std::int32_t, 16>;
constexpr std::size_t kVertexLimit = 6144;
constexpr std::size_t kPolygonLimit = 2048;
constexpr std::size_t kScreenWidth = 256;
constexpr std::size_t kScreenHeight = 192;
constexpr std::size_t kScreenPixels = kScreenWidth * kScreenHeight;
constexpr int kRenderBands = 8;
constexpr int kBandHeight = static_cast<int>(kScreenHeight) / kRenderBands;
constexpr std::uint32_t kIoBase = 0x04000320U;
constexpr std::uint32_t kIoEnd = 0x040006a4U;
constexpr std::uint32_t kFifoCapacity = 256;
constexpr std::uint32_t kPipeCapacity = 4;
constexpr std::int32_t kOne = 1 << 12;

std::int32_t fixed_from_int16(std::uint32_t value) {
    // VTX_16, VTX_XY/XZ/YZ, POS_TEST and BOX_TEST coordinates are signed Q4.12.
    return static_cast<std::int16_t>(value & 0xffffU);
}

std::int32_t signed10(std::uint32_t value) {
    const auto bits = static_cast<std::int32_t>(value & 0x3ffU);
    return (bits & 0x200) != 0 ? bits - 1024 : bits;
}

std::int32_t saturate_i32(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}
Matrix identity_matrix() {
    Matrix result{};
    result[0] = kOne;
    result[5] = kOne;
    result[10] = kOne;
    result[15] = kOne;
    return result;
}

Matrix multiply_matrix(const Matrix& left, const Matrix& right) {
    Matrix result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            std::uint64_t sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(left[static_cast<std::size_t>(row * 4 + k)]) *
                    right[static_cast<std::size_t>(k * 4 + column)]);
            }
            result[static_cast<std::size_t>(row * 4 + column)] = static_cast<std::int32_t>(sum >> 12);
        }
    }
    return result;
}

std::array<std::int32_t, 4> transform_vector(const Matrix& matrix,
                                              const std::array<std::int32_t, 4>& vector) {
    std::array<std::int32_t, 4> result{};
    for (int column = 0; column < 4; ++column) {
        std::int64_t sum = 0;
        for (int row = 0; row < 4; ++row) {
            sum += static_cast<std::int64_t>(vector[static_cast<std::size_t>(row)]) *
                   matrix[static_cast<std::size_t>(row * 4 + column)];
        }
        result[static_cast<std::size_t>(column)] = saturate_i32(sum >> 12);
    }
    return result;
}

std::array<float, 4> transform_test_vector(const Matrix& matrix,
                                         const std::array<float, 4>& vector) {
    std::array<float, 4> result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row)
            result[column] += vector[row] *
                (static_cast<float>(matrix[row * 4U + column]) / 4096.0F);
    }
    return result;
}

Matrix upper3x3(const Matrix& matrix) {
    auto result = identity_matrix();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result[static_cast<std::size_t>(row * 4 + column)] =
                matrix[static_cast<std::size_t>(row * 4 + column)];
        }
    }
    return result;
}

std::uint8_t color5(std::uint16_t color, int shift) {
    return static_cast<std::uint8_t>((color >> shift) & 31U);
}

std::uint8_t expand6(std::uint8_t value) {
    return static_cast<std::uint8_t>(value == 0 ? 0 : value * 2U + 1U);
}

constexpr std::uint32_t kDepthEqualTolerance = 255U;

std::uint32_t depth15_to24(std::uint16_t depth) {
    return (static_cast<std::uint32_t>(depth & 0x7fffU) << 9) | 0x1ffU;
}

std::uint32_t pack_pixel(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t alpha) {
    return static_cast<std::uint32_t>(r & 63U) |
           (static_cast<std::uint32_t>(g & 63U) << 6) |
           (static_cast<std::uint32_t>(b & 63U) << 12) |
           (static_cast<std::uint32_t>(alpha & 31U) << 24);
}



using Vertex = Gpu3DVertex;
using Polygon = Gpu3DPolygon;
struct ClipVertex {
    std::array<float, 4> clip{};
    float u = 0.0F;
    float v = 0.0F;
    std::array<float, 3> color{};
};

struct ScreenVertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float inv_w = 0.0F;
    float u_over_w = 0.0F;
    float v_over_w = 0.0F;
    std::array<float, 3> color_over_w{};
};

struct FramePixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t alpha = 0;
    std::uint32_t depth = 0x00ffffffU;
    std::uint8_t polygon_id = 0;
    bool fog = false;
    std::uint8_t translucent_polygon_id = 0xffU;
    bool is_translucent_polygon = false;
    bool stencil = false;
    std::uint8_t facing = 0; // 0 = nothing drawn yet, 1 = front-facing polygon, 2 = back-facing
};
static_assert(std::is_trivially_copyable_v<FramePixel>);

struct Target {
    FramePixel* fb;
    std::uint32_t* out;
    int scale;
    int width;
    int height;
    int band_height;
};

struct DirectPacket {
    std::uint8_t code = 0;
    std::uint8_t count = 0;
    std::array<std::uint32_t, 32> args{};
};

struct FifoWord {
    std::uint32_t value = 0;
    bool direct_command = false;
};

std::uint8_t command_parameter_count(std::uint8_t code) {
    switch (code) {
    case 0x10: case 0x12: case 0x13: case 0x14:
    case 0x20: case 0x21: case 0x22: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a:
    case 0x2b: case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x40: case 0x50: case 0x60: case 0x72:
        return 1;
    case 0x11: case 0x15: case 0x41:
        return 0;
    case 0x16: case 0x18:
        return 16;
    case 0x17: case 0x19:
        return 12;
    case 0x1a:
        return 9;
    case 0x1b: case 0x1c: case 0x70:
        return 3;
    case 0x23: case 0x71:
        return 2;
    case 0x34:
        return 32;
    default:
        return 0;
    }
}

std::int64_t command_cost(std::uint8_t code) {
    switch (code) {
    case 0x16: case 0x18: return 18;
    case 0x17: case 0x19: return 15;
    case 0x1a: return 12;
    case 0x1b: case 0x1c: return 8;
    case 0x21: return 3;
    case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: return 8;
    case 0x40: case 0x50: case 0x60: return 4;
    case 0x70: case 0x71: case 0x72: return 12;
    default: return 2;
    }
}

} // namespace

struct Gpu3D::Impl {
    std::array<std::uint8_t, kIoEnd - kIoBase> io{};
    std::uint32_t disp3d = 0;
    gpu3d_detail::TextureSource texture_source{};
    gpu3d_detail::DecodedTextureCache decoded_textures;

    Matrix projection = identity_matrix();
    Matrix position = identity_matrix();
    Matrix directional = identity_matrix();
    Matrix texture_matrix = identity_matrix();
    std::array<Matrix, 32> position_stack{};
    std::array<Matrix, 32> directional_stack{};
    Matrix projection_saved = identity_matrix();
    Matrix texture_saved = identity_matrix();
    std::uint8_t matrix_mode = 0;
    std::uint8_t stack_pointer = 0;
    std::uint8_t projection_stack_pointer = 0;
    std::uint8_t texture_stack_pointer = 0;
    bool stack_error = false;

    std::array<std::array<std::int32_t, 3>, 4> light_direction{};
    std::array<std::array<std::int32_t, 3>, 4> light_half_vector{};
    std::array<std::array<std::uint8_t, 3>, 4> light_color{};
    std::uint16_t vertex_color = 0x7fffU;
    std::uint32_t polygon_attr = 31U << 16;
    std::uint32_t pending_polygon_attr = 31U << 16;
    std::uint32_t texture_param = 0;
    std::uint32_t palette_param = 0;
    std::uint32_t material_diffuse_ambient = 0;
    std::uint32_t material_specular_emission = 0;
    std::array<std::uint8_t, 128> shininess{};
    std::array<std::int32_t, 3> normal{};
    std::array<std::int32_t, 3> current_position{};
    std::int32_t texcoord_s = 0;
    std::int32_t texcoord_t = 0;
    std::int64_t transformed_texcoord_s = 0;
    std::int64_t transformed_texcoord_t = 0;

    std::array<Vertex, kVertexLimit> vertices{};
    std::array<Polygon, kPolygonLimit> polygons{};
    std::size_t vertex_count = 0;
    std::size_t polygon_count = 0;
    std::array<std::uint16_t, 4> primitive_vertices{};
    std::uint32_t primitive_vertex_count = 0;
    std::uint8_t primitive_mode = 0;
    bool in_vertex_list = false;
    bool vertex_overflow = false;
    bool polygon_overflow = false;

    std::array<Vertex, kVertexLimit> render_vertices{};
    std::array<Polygon, kPolygonLimit> render_polygons{};
    std::size_t render_vertex_count = 0;
    std::size_t render_polygon_count = 0;
    std::array<std::uint16_t, kPolygonLimit> render_opaque_order{};
    std::array<std::uint16_t, kPolygonLimit> render_translucent_order{};
    std::size_t render_opaque_count = 0;
    std::size_t render_translucent_count = 0;
    std::uint64_t render_generation = 0;
    std::uint8_t swap_mode = 0;
    std::uint8_t pending_swap_mode = 0;
    std::uint8_t geometry_swap_mode = 0;
    bool render_vertex_overflow = false;
    bool render_polygon_overflow = false;
    bool swap_pending = false;
    // Busy flags cover submitted commands, including ones still waiting in the
    // FIFO. DQ9 reads BOX_TEST immediately after submission to cull scene objects.
    std::size_t pending_test_commands = 0;
    std::size_t pending_stack_commands = 0;
    std::uint32_t fifo_write_command_word = 0;
    std::uint8_t fifo_write_parameters = 0;

    std::array<FifoWord, kFifoCapacity> fifo{};
    std::size_t fifo_head = 0;
    std::size_t fifo_size = 0;
    std::array<FifoWord, kPipeCapacity> pipe{};
    std::size_t pipe_head = 0;
    std::size_t pipe_size = 0;
    std::uint32_t command_word = 0;
    std::uint8_t command_byte = 4;
    bool have_command_word = false;
    bool have_pending_command = false;
    std::uint8_t pending_code = 0;
    std::uint8_t pending_needed = 0;
    std::uint8_t pending_received = 0;
    std::array<std::uint32_t, 32> pending_args{};
    std::int64_t pending_cycles = 0;
    // Direct ports are a separate command stream, not packed-FIFO arguments.
    // Suspend an incomplete packed command while a complete direct packet runs.
    DirectPacket suspended_packed{};
    bool have_suspended_packed = false;
    std::array<DirectPacket, 0x73> direct_staging{};
    std::array<bool, 0x73> direct_ready{};
    bool have_ready_direct_staging = false;
    std::array<std::uint8_t, 0x73> direct_counts{};
    std::array<DirectPacket, kFifoCapacity> direct_queue{};
    std::size_t direct_head = 0;
    std::size_t direct_size = 0;
    std::uint32_t irq_mode = 0;
    bool box_test_result = false;
    std::array<std::int32_t, 4> position_result{};
    std::array<std::int16_t, 3> vector_result{};

    std::array<FramePixel, kScreenPixels> framebuffer{};
    std::array<std::uint32_t, kScreenPixels> output{};
    Target tgt_{framebuffer.data(), output.data(), 1,
                static_cast<int>(kScreenWidth), static_cast<int>(kScreenHeight), kBandHeight};
    int output_scale = 1;
    int rendered_scale = 1;
    std::vector<FramePixel> hires_fb;
    std::vector<std::uint32_t> hires_out;
    // Capture and its per-pixel scratch are native-only; the hi-res pass always
    // instantiates Capture=false. Polygon IDs, depth, stencil and fog used by
    // both passes live in each target's FramePixel storage.
    bool capture_opaque_only = false;
    bool opaque_only_valid = false;
    std::array<std::uint8_t, kScreenPixels> opaque_only{};
    std::array<std::uint8_t, kScreenPixels> opaque_only_dilated{};
    struct CaptureData {
        std::array<std::uint8_t, kScreenPixels> causes{};
#ifdef DSR_GPU_CAPTURE_IDS
        std::array<std::uint8_t, kScreenPixels> polygon_ids{};
#endif
        std::array<bool, kScreenPixels> partial_seen{};
        std::array<std::array<std::uint32_t, kPolygonLimit>, kRenderBands> polygon_touches{};
        std::array<std::uint32_t, kPolygonLimit> polygon_totals{};
    };
    std::unique_ptr<CaptureData> capture_data;
    std::unique_ptr<BandPool> pool = std::make_unique<BandPool>();

    Impl() { reset(); }

    std::uint32_t get32(std::uint32_t addr) const {
        if (addr < kIoBase || addr + 3U >= kIoEnd) return 0;
        const auto offset = static_cast<std::size_t>(addr - kIoBase);
        return static_cast<std::uint32_t>(io[offset]) |
               (static_cast<std::uint32_t>(io[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(io[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(io[offset + 3]) << 24);
    }

    void put32(std::uint32_t addr, std::uint32_t value) {
        if (addr < kIoBase || addr + 3U >= kIoEnd) return;
        const auto offset = static_cast<std::size_t>(addr - kIoBase);
        io[offset] = static_cast<std::uint8_t>(value);
        io[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        io[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        io[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    }

    std::uint32_t raw_io_read(std::uint32_t addr, int width) const {
        if (width != 8 && width != 16 && width != 32) return 0;
        const auto aligned = addr & ~3U;
        const auto word = get32(aligned);
        const auto shift = (addr & 3U) * 8U;
        if (width == 8) return (word >> shift) & 0xffU;
        if (width == 16) return (word >> (shift & 16U)) & 0xffffU;
        return word;
    }

    void raw_io_write(std::uint32_t addr, std::uint32_t value, int width) {
        if (width != 8 && width != 16 && width != 32) return;
        const auto aligned = addr & ~3U;
        auto word = get32(aligned);
        const auto shift = (addr & 3U) * 8U;
        if (width == 8) {
            word = (word & ~(0xffU << shift)) | ((value & 0xffU) << shift);
        } else if (width == 16) {
            const auto half_shift = shift & 16U;
            word = (word & ~(0xffffU << half_shift)) | ((value & 0xffffU) << half_shift);
        } else {
            word = value;
        }
        put32(aligned, word);
    }

    std::uint32_t queue_count() const {
        return static_cast<std::uint32_t>(fifo_size + pipe_size);
    }

    bool below_half() const { return queue_count() < 128U; }

    bool irq_condition() const {
        if (irq_mode == 1U) return below_half();
        if (irq_mode == 2U) return queue_count() == 0U;
        return false;
    }

    void notify_fifo_change(bool old_below, bool old_irq, const std::function<void()>& callback) const {
        if (callback && (old_below != below_half() || old_irq != irq_condition())) callback();
    }

    void refill_pipe() {
        while (pipe_size < kPipeCapacity && fifo_size != 0) {
            const auto tail = (pipe_head + pipe_size) % kPipeCapacity;
            pipe[tail] = fifo[fifo_head];
            fifo_head = (fifo_head + 1U) % kFifoCapacity;
            --fifo_size;
            ++pipe_size;
        }
    }

    bool pop_word(FifoWord& value) {
        refill_pipe();
        if (pipe_size == 0) return false;
        value = pipe[pipe_head];
        pipe_head = (pipe_head + 1U) % kPipeCapacity;
        --pipe_size;
        refill_pipe();
        return true;
    }

    bool push_fifo_word(std::uint32_t value, bool direct_command = false) {
        if (fifo_size == kFifoCapacity) return false;
        const auto tail = (fifo_head + fifo_size) % kFifoCapacity;
        fifo[tail] = {value, direct_command};
        ++fifo_size;
        return true;
    }

    bool enqueue_packet(const DirectPacket& packet) {
        const auto words = static_cast<std::size_t>(packet.count) + 1U;
        if (fifo_size + words > kFifoCapacity) return false;
        if (!push_fifo_word(packet.code, true)) return false;
        for (std::size_t i = 0; i < packet.count; ++i) {
            if (!push_fifo_word(packet.args[i])) return false;
        }
        return true;
    }

    bool queue_direct_packet(const DirectPacket& packet) {
        if (direct_size == 0U && enqueue_packet(packet)) {
            account_queued_command(packet.code);
            return true;
        }
        if (direct_size == direct_queue.size()) return false;
        const auto tail = (direct_head + direct_size) % direct_queue.size();
        direct_queue[tail] = packet;
        ++direct_size;
        account_queued_command(packet.code);
        return true;
    }

    void flush_direct_queue() {
        while (direct_size != 0) {
            const auto& packet = direct_queue[direct_head];
            if (!enqueue_packet(packet)) break;
            direct_head = (direct_head + 1U) % direct_queue.size();
            --direct_size;
        }
        // A complete packet can still be held at its direct port when both
        // queues were full. Retry it as capacity returns, rather than leaving
        // its parameter count (and GXSTAT test-busy) set forever.
        if (!have_ready_direct_staging) return;
        have_ready_direct_staging = false;
        for (std::size_t code = 0; code < direct_ready.size(); ++code) {
            if (!direct_ready[code]) continue;
            if (!queue_direct_packet(direct_staging[code])) {
                have_ready_direct_staging = true;
                continue;
            }
            direct_counts[code] = 0;
            direct_ready[code] = false;
            direct_staging[code] = {};
        }
    }

    void account_queued_command(std::uint8_t code) {
        if (code >= 0x70U && code <= 0x72U) ++pending_test_commands;
        if (code == 0x11U || code == 0x12U) ++pending_stack_commands;
    }

    void account_fifo_word(std::uint32_t value) {
        // Track the packed command stream at ingress; parameter words must not
        // be interpreted as command bytes, even when they contain 0x70/0x11.
        if (fifo_write_parameters != 0U) {
            if (--fifo_write_parameters != 0U) return;
            fifo_write_command_word >>= 8;
        } else {
            fifo_write_command_word = value;
        }
        while (fifo_write_command_word != 0U) {
            const auto code = static_cast<std::uint8_t>(fifo_write_command_word);
            if (code == 0U) {
                fifo_write_command_word = 0;
                return;
            }
            account_queued_command(code);
            fifo_write_parameters = command_parameter_count(code);
            if (fifo_write_parameters != 0U) return;
            fifo_write_command_word >>= 8;
        }
    }

    std::uint32_t gxstat() const {
        std::uint32_t status = 0;
        if (pending_test_commands != 0U || direct_counts[0x70] != 0U ||
            direct_counts[0x71] != 0U || direct_counts[0x72] != 0U) status |= 1U;
        if (box_test_result) status |= 1U << 1;
        status |= static_cast<std::uint32_t>(stack_pointer & 31U) << 8;
        status |= static_cast<std::uint32_t>(projection_stack_pointer & 1U) << 13;
        if (pending_stack_commands != 0U || direct_ready[0x12]) status |= 1U << 14;
        if (stack_error) status |= 1U << 15;
        status |= (queue_count() & 0x1ffU) << 16;
        if (below_half()) status |= 1U << 25;
        if (queue_count() == 0U) status |= 1U << 26;
        if (have_pending_command || have_command_word || queue_count() != 0U || swap_pending) status |= 1U << 27;
        status |= (irq_mode & 3U) << 30;
        return status;
    }

    std::uint32_t ram_count() const {
        return static_cast<std::uint32_t>(std::min<std::size_t>(polygon_count, 0xfffU)) |
               (static_cast<std::uint32_t>(std::min<std::size_t>(vertex_count, 0x1fffU)) << 16);
    }

    std::uint32_t result_register(std::uint32_t addr) const {
        if (addr >= 0x04000620U && addr <= 0x0400062cU) {
            const auto index = static_cast<std::size_t>((addr - 0x04000620U) / 4U);
            return static_cast<std::uint32_t>(position_result[index]);
        }
        if (addr == 0x04000630U) {
            return static_cast<std::uint16_t>(vector_result[0]) |
                   (static_cast<std::uint32_t>(static_cast<std::uint16_t>(vector_result[1])) << 16);
        }
        if (addr == 0x04000634U) return static_cast<std::uint16_t>(vector_result[2]);
        if (addr >= 0x04000640U && addr <= 0x0400067cU) {
            const auto index = static_cast<std::size_t>((addr - 0x04000640U) / 4U);
            const auto row = index & ~3U;
            const auto column = index & 3U;
            std::uint64_t sum = 0;
            for (std::size_t k = 0; k < 4; ++k)
                sum += static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(position[row + k]) * projection[k * 4U + column]);
            return static_cast<std::uint32_t>(sum >> 12);
        }
        if (addr >= 0x04000680U && addr <= 0x040006a0U) {
            const auto position_index = static_cast<std::size_t>((addr - 0x04000680U) / 4U);
            const auto row = position_index / 3U;
            const auto column = position_index % 3U;
            return static_cast<std::uint32_t>(directional[row * 4U + column]);
        }
        return 0;
    }

    std::uint32_t io_read(std::uint32_t addr, int width) const {
        if (width != 8 && width != 16 && width != 32) return 0;
        if (addr >= 0x04000060U && addr <= 0x04000063U) {
            const auto shift = (addr & 3U) * 8U;
            const auto value = disp3d;
            if (width == 8) return (value >> shift) & 0xffU;
            if (width == 16) return (value >> (shift & 16U)) & 0xffffU;
            return value;
        }
        const auto aligned = addr & ~3U;
        if (aligned == 0x04000600U) {
            const auto word = gxstat();
            const auto shift = (addr & 3U) * 8U;
            if (width == 8) return (word >> shift) & 0xffU;
            if (width == 16) return (word >> (shift & 16U)) & 0xffffU;
            return word;
        }
        if (aligned == 0x04000604U) {
            const auto word = ram_count();
            const auto shift = (addr & 3U) * 8U;
            if (width == 8) return (word >> shift) & 0xffU;
            if (width == 16) return (word >> (shift & 16U)) & 0xffffU;
            return word;
        }
        if (aligned >= 0x04000620U && aligned <= 0x040006a0U) {
            const auto word = result_register(aligned);
            const auto shift = (addr & 3U) * 8U;
            if (width == 8) return (word >> shift) & 0xffU;
            if (width == 16) return (word >> (shift & 16U)) & 0xffffU;
            return word;
        }
        if (addr >= kIoBase && addr < kIoEnd) return raw_io_read(addr, width);
        return 0;
    }


    void reset() {
        io.fill(0);
        disp3d = 0;
        decoded_textures.clear();
        projection = identity_matrix();
        position = identity_matrix();
        directional = identity_matrix();
        texture_matrix = identity_matrix();
        position_stack.fill(identity_matrix());
        directional_stack.fill(identity_matrix());
        projection_saved = identity_matrix();
        texture_saved = identity_matrix();
        stack_pointer = 0;
        projection_stack_pointer = 0;
        texture_stack_pointer = 0;
        stack_error = false;
        light_direction = {};
        light_half_vector = {};
        light_color = {};
        vertex_color = 0x7fffU;
        polygon_attr = 31U << 16;
        pending_polygon_attr = polygon_attr;
        texture_param = 0;
        palette_param = 0;
        material_diffuse_ambient = 0;
        material_specular_emission = 0;
        shininess.fill(0);
        normal = {};
        current_position = {};
        texcoord_s = 0;
        texcoord_t = 0;
        transformed_texcoord_s = 0;
        transformed_texcoord_t = 0;
        vertices.fill({});
        polygons.fill({});
        vertex_count = 0;
        polygon_count = 0;
        primitive_vertices = {};
        primitive_vertex_count = 0;
        primitive_mode = 0;
        in_vertex_list = false;
        vertex_overflow = false;
        polygon_overflow = false;
        render_vertex_count = 0;
        render_polygon_count = 0;
        render_opaque_count = 0;
        render_translucent_count = 0;
        render_generation = 0;
        swap_mode = 0;
        pending_swap_mode = 0;
        geometry_swap_mode = 0;
        render_vertex_overflow = false;
        render_polygon_overflow = false;
        swap_pending = false;
        pending_test_commands = 0;
        pending_stack_commands = 0;
        fifo_write_command_word = 0;
        fifo_write_parameters = 0;
        fifo.fill({});
        fifo_head = 0;
        fifo_size = 0;
        pipe.fill({});
        pipe_head = 0;
        pipe_size = 0;
        command_word = 0;
        command_byte = 4;
        have_command_word = false;
        have_pending_command = false;
        pending_code = 0;
        pending_needed = 0;
        pending_received = 0;
        pending_args.fill(0);
        pending_cycles = 0;
        have_suspended_packed = false;
        direct_staging.fill({});
        direct_ready.fill(false);
        have_ready_direct_staging = false;
        direct_counts.fill(0);
        direct_queue.fill({});
        direct_head = 0;
        direct_size = 0;
        irq_mode = 0;
        box_test_result = false;
        position_result = {};
        vector_result = {};
        framebuffer.fill({});
        output.fill(0);
        std::fill(hires_fb.begin(), hires_fb.end(), FramePixel{});
        std::fill(hires_out.begin(), hires_out.end(), 0);
        rendered_scale = 1;
        opaque_only_valid = false;
        put32(0x04000350U, 0);
        put32(0x04000354U, 0x7fffU);
        put32(0x04000580U, 0xbfff0000U);
    }
    void write_direct_port(std::uint8_t code, std::uint32_t value) {
        if (code >= direct_staging.size()) return;
        const auto needed = command_parameter_count(code);
        if (needed == 0U) {
            DirectPacket packet{};
            packet.code = code;
            queue_direct_packet(packet);
            return;
        }
        auto& state = direct_staging[code];
        if (direct_ready[code]) return;
        state.code = code;
        if (direct_counts[code] < state.args.size()) {
            state.args[direct_counts[code]++] = value;
        }
        if (direct_counts[code] == needed) {
            state.count = needed;
            direct_ready[code] = true;
            if (queue_direct_packet(state)) {
                direct_counts[code] = 0;
                direct_ready[code] = false;
                state = {};
            } else {
                have_ready_direct_staging = true;
            }
        }
    }

    void write_io(std::uint32_t addr, std::uint32_t value, int width,
                  const std::function<void()>& callback) {
        if (width != 8 && width != 16 && width != 32) return;
        const bool old_below = below_half();
        const bool old_irq = irq_condition();
        if (addr >= 0x04000060U && addr <= 0x04000063U) {
            const auto shift = (addr & 3U) * 8U;
            const auto status_mask = (1U << 12) | (1U << 13);
            std::uint32_t merged = disp3d;
            std::uint32_t acknowledge = 0;
            if (width == 8) {
                merged = (merged & ~(0xffU << shift)) | ((value & 0xffU) << shift);
                acknowledge = ((value & 0xffU) << shift) & status_mask;
            } else if (width == 16) {
                const auto half_shift = shift & 16U;
                merged = (merged & ~(0xffffU << half_shift)) | ((value & 0xffffU) << half_shift);
                acknowledge = ((value & 0xffffU) << half_shift) & status_mask;
            } else {
                merged = value;
                acknowledge = value & status_mask;
            }
            const auto flags = (disp3d & status_mask) & ~acknowledge;
            disp3d = (merged & ~status_mask) | flags;
            notify_fifo_change(old_below, old_irq, callback);
            return;
        }
        // GXFIFO is mirrored across its 64-byte register window. This also
        // allows STMIA bursts to feed consecutive words into the same FIFO.
        if (addr >= 0x04000400U && addr < 0x04000440U) {
            const auto word = width == 32 ? value : value & (width == 8 ? 0xffU : 0xffffU);
            if (push_fifo_word(word)) account_fifo_word(word);
            notify_fifo_change(old_below, old_irq, callback);
            return;
        }
        if (addr >= 0x04000440U && addr <= 0x040005cbU) {
            const auto code = static_cast<std::uint8_t>(0x10U + ((addr - 0x04000440U) >> 2));
            write_direct_port(code, value);
            notify_fifo_change(old_below, old_irq, callback);
            return;
        }
        if ((addr & ~3U) == 0x04000600U) {
            if (width == 32) {
                irq_mode = (value >> 30) & 3U;
                if ((value & (1U << 15)) != 0U) {
                    stack_error = false;
                    projection_stack_pointer = 0U;
                }
                if ((value & (1U << 29)) != 0U) {
                    fifo_head = 0;
                    fifo_size = 0;
                    pipe_head = 0;
                    pipe_size = 0;
                    command_word = 0;
                    command_byte = 4;
                    have_command_word = false;
                    have_pending_command = false;
                    pending_cycles = 0;
                    have_suspended_packed = false;
                    direct_staging.fill({});
                    direct_ready.fill(false);
                    have_ready_direct_staging = false;
                    direct_counts.fill(0);
                    direct_head = 0;
                    direct_size = 0;
                    pending_test_commands = 0;
                    pending_stack_commands = 0;
                    fifo_write_command_word = 0;
                    fifo_write_parameters = 0;
                }
            } else {
                const auto current = irq_mode << 30;
                const auto shift = (addr & 3U) * 8U;
                std::uint32_t merged = current;
                if (width == 8) merged = (current & ~(0xffU << shift)) | ((value & 0xffU) << shift);
                else if (width == 16) {
                    const auto half_shift = shift & 16U;
                    merged = (current & ~(0xffffU << half_shift)) | ((value & 0xffffU) << half_shift);
                }
                irq_mode = (merged >> 30) & 3U;
                if ((merged & (1U << 15)) != 0U) {
                    stack_error = false;
                    projection_stack_pointer = 0U;
                }
            }
            notify_fifo_change(old_below, old_irq, callback);
            return;
        }
        if ((addr & ~3U) == 0x04000604U) return;
        if (addr >= 0x04000620U && addr <= 0x040006a3U) return;
        if (addr >= kIoBase && addr < kIoEnd) {
            raw_io_write(addr, value, width);
            notify_fifo_change(old_below, old_irq, callback);
        }
    }


    void set_matrix_for_mode(const Matrix& matrix) {
        if (matrix_mode == 0U) {
            projection = matrix;
        } else if (matrix_mode == 1U) {
            position = matrix;
        } else if (matrix_mode == 2U) {
            position = matrix;
            directional = matrix;
        } else {
            texture_matrix = matrix;
        }
    }

    void multiply_current(const Matrix& matrix) {
        if (matrix_mode == 0U) {
            projection = multiply_matrix(matrix, projection);
        } else if (matrix_mode == 1U) {
            position = multiply_matrix(matrix, position);
        } else if (matrix_mode == 2U) {
            position = multiply_matrix(matrix, position);
            directional = multiply_matrix(matrix, directional);
        } else {
            texture_matrix = multiply_matrix(matrix, texture_matrix);
        }
    }

    Matrix matrix_from_args(const std::array<std::uint32_t, 32>& args,
                            std::uint8_t code) const {
        Matrix result = identity_matrix();
        if (code == 0x16U || code == 0x18U) {
            for (std::size_t i = 0; i < 16; ++i) result[i] = static_cast<std::int32_t>(args[i]);
        } else if (code == 0x17U || code == 0x19U) {
            result = {};
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    result[row * 4U + column] = static_cast<std::int32_t>(args[row * 3U + column]);
                }
            }
            result[15] = kOne;
        } else if (code == 0x1aU) {
            result = identity_matrix();
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) result[i * 4U + j] = static_cast<std::int32_t>(args[i * 3U + j]);
            }
        }
        return result;
    }

    void load_matrix(std::uint8_t code, const std::array<std::uint32_t, 32>& args) {
        auto matrix = matrix_from_args(args, code);
        if (code == 0x16U || code == 0x17U) set_matrix_for_mode(matrix);
        else multiply_current(matrix);
    }

    void apply_scale_translate(bool translate, const std::array<std::uint32_t, 32>& args) {
        auto matrix = identity_matrix();
        if (translate) {
            matrix[12] = static_cast<std::int32_t>(args[0]);
            matrix[13] = static_cast<std::int32_t>(args[1]);
            matrix[14] = static_cast<std::int32_t>(args[2]);
        } else {
            matrix[0] = static_cast<std::int32_t>(args[0]);
            matrix[5] = static_cast<std::int32_t>(args[1]);
            matrix[10] = static_cast<std::int32_t>(args[2]);
        }
        if (!translate && matrix_mode == 2U) {
            position = multiply_matrix(matrix, position);
        } else {
            multiply_current(matrix);
        }
    }

    void set_material_vertex_color() {
        if ((material_diffuse_ambient & (1U << 15)) != 0U) {
            vertex_color = static_cast<std::uint16_t>(material_diffuse_ambient & 0x7fffU);
        }
    }

    static std::int32_t fixed_dot(const std::array<std::int32_t, 3>& left,
                                  const std::array<std::int32_t, 3>& right) {
        const std::int64_t sum = static_cast<std::int64_t>(left[0]) * right[0] +
                                 static_cast<std::int64_t>(left[1]) * right[1] +
                                 static_cast<std::int64_t>(left[2]) * right[2];
        return saturate_i32(sum >> 12);
    }

    std::array<std::int32_t, 3> transformed_normal() const {
        const std::array<std::int32_t, 4> input{
            normal[0] * 8, normal[1] * 8, normal[2] * 8, 0};
        const auto transformed = transform_vector(directional, input);
        return {transformed[0], transformed[1], transformed[2]};
    }

    void light_vertex() {
        const auto n = transformed_normal();
        const auto diffuse = static_cast<std::uint16_t>(material_diffuse_ambient & 0x7fffU);
        const auto ambient = static_cast<std::uint16_t>((material_diffuse_ambient >> 16) & 0x7fffU);
        const auto emission = static_cast<std::uint16_t>((material_specular_emission >> 16) & 0x7fffU);
        const auto specular = static_cast<std::uint16_t>(material_specular_emission & 0x7fffU);
        std::array<std::int32_t, 3> out{
            color5(emission, 0), color5(emission, 5), color5(emission, 10)};
        for (std::size_t light = 0; light < light_direction.size(); ++light) {
            if ((polygon_attr & (1U << light)) == 0U) continue;
            const auto fixed_diffuse = std::max(0, -fixed_dot(light_direction[light], n));
            const auto& half = light_half_vector[light];
            const std::array<std::int32_t, 3> negative_half{-half[0], -half[1], -half[2]};
            const auto half_dot = fixed_dot(negative_half, n);
            std::int32_t fixed_shininess = 0;
            if (half_dot > 0) {
                const auto squared = static_cast<std::int64_t>(half_dot) * half_dot;
                fixed_shininess = saturate_i32(2 * (squared >> 12) - kOne);
            }
            fixed_shininess = std::clamp(fixed_shininess, 0, 4095);
            if ((material_specular_emission & (1U << 15)) != 0U)
                fixed_shininess = static_cast<std::int32_t>(
                    shininess[static_cast<std::size_t>(fixed_shininess >> 5)]) << 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto diffuse_component =
                    (static_cast<std::int64_t>(color5(diffuse, static_cast<int>(channel * 5U))) *
                     light_color[light][channel] * fixed_diffuse) >> 17;
                const auto specular_component =
                    (static_cast<std::int64_t>(color5(specular, static_cast<int>(channel * 5U))) *
                     light_color[light][channel] * fixed_shininess) >> 17;
                const auto ambient_component =
                    (color5(ambient, static_cast<int>(channel * 5U)) * light_color[light][channel]) >> 5;
                out[channel] += static_cast<std::int32_t>(
                    diffuse_component + specular_component + ambient_component);
            }
        }
        vertex_color = static_cast<std::uint16_t>(
            std::clamp(out[0], 0, 31) | (std::clamp(out[1], 0, 31) << 5) |
            (std::clamp(out[2], 0, 31) << 10));
    }

    void set_light_direction(std::uint32_t value) {
        const auto index = static_cast<std::size_t>(value >> 30);
        const std::array<std::int32_t, 4> input{
            signed10(value) * 8,
            signed10(value >> 10) * 8,
            signed10(value >> 20) * 8,
            0};
        const auto transformed = transform_vector(directional, input);
        auto& direction = light_direction[index];
        direction = {transformed[0], transformed[1], transformed[2]};
        auto& half = light_half_vector[index];
        half = {direction[0], direction[1], direction[2] - kOne};
        // Reference gfx3d.cpp:999-1009 shifts sqrt(dot) left by 6 then right by 6 again,
        // so the divisor is plain sqrt(dot) while each component is scaled by 64.
        const auto half_length = static_cast<std::int32_t>(
            std::sqrt(static_cast<double>(std::max(0, fixed_dot(half, half)))));
        if (half_length == 0) return;
        for (auto& component : half) {
            component = saturate_i32((static_cast<std::int64_t>(component) * 64) /
                                     half_length);
        }
    }



    void emit_polygon(const std::uint16_t* indices, std::uint8_t count) {
        if (polygon_count >= kPolygonLimit) {
            polygon_overflow = true;
            disp3d |= 1U << 13;
            return;
        }
        Polygon polygon{};
        polygon.count = count;
        for (std::size_t i = 0; i < count; ++i) polygon.index[i] = indices[i];
        polygon.attr = polygon_attr;
        polygon.texture = texture_param;
        polygon.palette = palette_param;
        polygon.viewport = get32(0x04000580U);
        polygons[polygon_count++] = polygon;
    }

    void assemble_vertex(std::uint16_t index) {
        if (!in_vertex_list) return;
        const auto topology = static_cast<std::uint8_t>((primitive_mode >> 1) & 1U);
        const auto primitive_type = static_cast<std::uint8_t>(primitive_mode & 1U);
        if (topology == 0U) {
            const auto required = primitive_type == 0U ? 3U : 4U;
            primitive_vertices[primitive_vertex_count % required] = index;
            ++primitive_vertex_count;
            if (primitive_vertex_count % required == 0U) {
                emit_polygon(primitive_vertices.data(), static_cast<std::uint8_t>(required));
            }
            return;
        }
        if (primitive_type == 0U) {
            if (primitive_vertex_count < 3U) {
                primitive_vertices[primitive_vertex_count++] = index;
                if (primitive_vertex_count == 3U) emit_polygon(primitive_vertices.data(), 3);
                return;
            }
            std::array<std::uint16_t, 3> triangle{
                primitive_vertices[1], primitive_vertices[2], index};
            if ((primitive_vertex_count & 1U) != 0U) std::swap(triangle[0], triangle[1]);
            emit_polygon(triangle.data(), 3);
            primitive_vertices[0] = primitive_vertices[1];
            primitive_vertices[1] = primitive_vertices[2];
            primitive_vertices[2] = index;
            ++primitive_vertex_count;
            return;
        }
        if (primitive_vertex_count < 4U) {
            primitive_vertices[primitive_vertex_count++] = index;
            if (primitive_vertex_count == 4U) {
                const std::array<std::uint16_t, 4> quad{
                    primitive_vertices[0], primitive_vertices[1], primitive_vertices[3], primitive_vertices[2]};
                emit_polygon(quad.data(), 4);
            }
            return;
        }
        if (primitive_vertex_count == 4U) {
            primitive_vertices[0] = primitive_vertices[2];
            primitive_vertices[1] = primitive_vertices[3];
            primitive_vertices[2] = index;
        } else if ((primitive_vertex_count & 1U) == 0U) {
            primitive_vertices[2] = index;
        } else {
            const std::array<std::uint16_t, 4> quad{
                primitive_vertices[0], primitive_vertices[1], index, primitive_vertices[2]};
            emit_polygon(quad.data(), 4);
            primitive_vertices[0] = primitive_vertices[2];
            primitive_vertices[1] = index;
        }
        ++primitive_vertex_count;
    }

    std::array<std::int64_t, 4> texcoord_transform() const {
        const auto mode = (texture_param >> 30) & 3U;
        if (mode == 0U) return {texcoord_s, texcoord_t, 0, kOne};
        std::array<std::int64_t, 4> result{};
        for (std::size_t column = 0; column < 2; ++column) {
            std::int64_t sum;
            if (mode == 1U) {
                sum = static_cast<std::int64_t>(texture_matrix[column]) * (texcoord_s / 256) +
                      static_cast<std::int64_t>(texture_matrix[4U + column]) * (texcoord_t / 256) +
                      texture_matrix[8U + column] + texture_matrix[12U + column];
                result[column] = static_cast<std::int64_t>(static_cast<std::int32_t>(sum >> 12)) * 256;
            } else {
                const auto source = mode == 2U
                    ? std::array<std::int32_t, 3>{normal[0] * 8, normal[1] * 8, normal[2] * 8}
                    : current_position;
                sum = static_cast<std::int64_t>(texture_matrix[column]) * source[0] +
                      static_cast<std::int64_t>(texture_matrix[4U + column]) * source[1] +
                      static_cast<std::int64_t>(texture_matrix[8U + column]) * source[2] +
                      static_cast<std::int64_t>(column == 0 ? texcoord_s / 256 : texcoord_t / 256) *
                          (1LL << 24);
                result[column] = static_cast<std::int64_t>(static_cast<std::int32_t>(sum >> 24)) * 256;
            }
        }
        return result;
    }

    void submit_vertex() {
        if (!in_vertex_list) return;
        if (vertex_count >= kVertexLimit) {
            vertex_overflow = true;
            disp3d |= 1U << 13;
            return;
        }
        const std::array<std::int32_t, 4> position_vector{
            current_position[0], current_position[1], current_position[2], kOne};
        const auto view = transform_vector(position, position_vector);
        const auto clip = transform_vector(projection, view);
        const auto mode = (texture_param >> 30) & 3U;
        const auto uv = mode == 3U ? texcoord_transform()
            : std::array<std::int64_t, 4>{transformed_texcoord_s, transformed_texcoord_t, 0, kOne};
        Vertex vertex{};
        for (std::size_t i = 0; i < 4; ++i) vertex.clip[i] = static_cast<float>(clip[i]) / 4096.0F;
        vertex.u = static_cast<float>(uv[0]) / 4096.0F;
        vertex.v = static_cast<float>(uv[1]) / 4096.0F;
        vertex.color = {static_cast<float>(expand6(color5(vertex_color, 0))),
                        static_cast<float>(expand6(color5(vertex_color, 5))),
                        static_cast<float>(expand6(color5(vertex_color, 10)))};
        vertices[vertex_count] = vertex;
        const auto index = static_cast<std::uint16_t>(vertex_count);
        ++vertex_count;
        assemble_vertex(index);
    }

    void vertex_xy(std::uint32_t value, int x_component, int y_component) {
        current_position[static_cast<std::size_t>(x_component)] = fixed_from_int16(value);
        current_position[static_cast<std::size_t>(y_component)] = fixed_from_int16(value >> 16);
        submit_vertex();
    }

    void store_position_result(std::uint32_t xy, std::uint32_t z) {
        current_position = {fixed_from_int16(xy), fixed_from_int16(xy >> 16), fixed_from_int16(z)};
        const std::array<float, 4> input{
            current_position[0] / 4096.0F, current_position[1] / 4096.0F,
            current_position[2] / 4096.0F, 1.0F};
        const auto view = transform_test_vector(position, input);
        const auto clip = transform_test_vector(projection, view);
        for (std::size_t i = 0; i < position_result.size(); ++i)
            position_result[i] = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(static_cast<std::int64_t>(clip[i] * 4096.0F)));
    }

    void store_vector_result(std::uint32_t value) {
        const std::array<float, 4> input{
            signed10(value) / 512.0F, signed10(value >> 10) / 512.0F,
            signed10(value >> 20) / 512.0F, 0.0F};
        const auto result = transform_test_vector(directional, input);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto raw = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(result[i] * 4096.0F)) & 0x1fffU;
            vector_result[i] = static_cast<std::int16_t>(
                (raw & 0x1000U) != 0U ? static_cast<std::int32_t>(raw) - 0x2000
                                     : static_cast<std::int32_t>(raw));
        }
    }

    bool box_intersects_view(std::uint32_t p0, std::uint32_t p1, std::uint32_t p2) const {
        const std::array<std::uint16_t, 3> origin{
            static_cast<std::uint16_t>(p0), static_cast<std::uint16_t>(p0 >> 16),
            static_cast<std::uint16_t>(p1)};
        const std::array<std::uint16_t, 3> extent{
            static_cast<std::uint16_t>(p1 >> 16), static_cast<std::uint16_t>(p2),
            static_cast<std::uint16_t>(p2 >> 16)};
        std::array<ClipVertex, 8> corners{};
        static constexpr std::array<std::array<std::uint8_t, 3>, 8> corner_bits{{
            {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
            {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}}}};
        for (unsigned corner = 0; corner < corners.size(); ++corner) {
            std::array<float, 4> input{};
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto raw = static_cast<std::uint16_t>(
                    origin[axis] + (corner_bits[corner][axis] != 0U ? extent[axis] : 0U));
                input[axis] = static_cast<std::int16_t>(raw) / 4096.0F;
            }
            input[3] = 1.0F;
            const auto view = transform_test_vector(position, input);
            corners[corner].clip = transform_test_vector(projection, view);
        }

        static constexpr std::array<std::array<std::size_t, 4>, 6> faces{{
            {{7, 6, 5, 4}}, {{0, 1, 2, 3}}, {{0, 3, 7, 4}},
            {{6, 2, 1, 5}}, {{3, 2, 6, 7}}, {{0, 4, 5, 1}}}};
        for (const auto& face : faces) {
            std::array<ClipVertex, 16> input{};
            std::size_t count = face.size();
            for (std::size_t i = 0; i < count; ++i) input[i] = corners[face[i]];
            for (int plane = 0; plane < 6 && count != 0U; ++plane) {
                std::array<ClipVertex, 16> next{};
                std::size_t next_count = 0;
                auto previous = input[count - 1U];
                auto previous_distance = plane_distance(previous, plane);
                bool previous_inside = previous_distance >= 0.0F;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto current = input[i];
                    const auto current_distance = plane_distance(current, plane);
                    const bool current_inside = current_distance >= 0.0F;
                    if (current_inside != previous_inside) {
                        const auto divisor = previous_distance - current_distance;
                        if (std::abs(divisor) > 1.0e-20F && next_count < next.size())
                            next[next_count++] =
                                interpolate_clip(previous, current, previous_distance / divisor);
                    }
                    if (current_inside && next_count < next.size()) next[next_count++] = current;
                    previous = current;
                    previous_distance = current_distance;
                    previous_inside = current_inside;
                }
                input = next;
                count = next_count;
            }
            if (count >= 3U) return true;
        }
        return false;
    }

    void execute(std::uint8_t code, const std::array<std::uint32_t, 32>& args) {
        const auto value = args[0];
        switch (code) {
        case 0x10:
            matrix_mode = static_cast<std::uint8_t>(value & 3U);
            break;
        case 0x11:
            if (matrix_mode == 0U) {
                if (projection_stack_pointer != 0U) stack_error = true;
                projection_saved = projection;
                projection_stack_pointer ^= 1U;
            } else if (matrix_mode == 3U) {
                if (texture_stack_pointer != 0U) stack_error = true;
                texture_saved = texture_matrix;
                texture_stack_pointer ^= 1U;
            } else {
                position_stack[stack_pointer & 31U] = position;
                directional_stack[stack_pointer & 31U] = directional;
                stack_pointer = static_cast<std::uint8_t>((stack_pointer + 1U) & 63U);
                if (stack_pointer >= 32U) stack_error = true;
            }
            break;
        case 0x12: {
            if (matrix_mode == 0U) {
                projection_stack_pointer ^= 1U;
                if (projection_stack_pointer != 0U) stack_error = true;
                projection = projection_saved;
            } else if (matrix_mode == 3U) {
                texture_stack_pointer ^= 1U;
                if (texture_stack_pointer != 0U) stack_error = true;
                texture_matrix = texture_saved;
            } else {
                stack_pointer = static_cast<std::uint8_t>((stack_pointer - (value & 63U)) & 63U);
                if (stack_pointer >= 32U) stack_error = true;
                position = position_stack[stack_pointer & 31U];
                directional = directional_stack[stack_pointer & 31U];
            }
            break;
        }
        case 0x13:
        case 0x14: {
            const auto index = static_cast<std::uint8_t>(value & 31U);
            if (matrix_mode != 0U && matrix_mode != 3U && index == 31U) {
                stack_error = true;
            }
            if (matrix_mode == 0U) {
                if (code == 0x13U) {
                    projection_saved = projection;
                } else {
                    projection = projection_saved;
                }
            } else if (matrix_mode == 3U) {
                if (code == 0x13U) {
                    texture_saved = texture_matrix;
                } else {
                    texture_matrix = texture_saved;
                }
            } else if (code == 0x13U) {
                position_stack[index] = position;
                directional_stack[index] = directional;
            } else {
                position = position_stack[index];
                directional = directional_stack[index];
            }
            break;
        }
        case 0x15:
            set_matrix_for_mode(identity_matrix());
            break;
        case 0x16: case 0x17:
            load_matrix(code, args);
            break;
        case 0x18: case 0x19: case 0x1a:
            load_matrix(code, args);
            break;
        case 0x1b:
            apply_scale_translate(false, args);
            break;
        case 0x1c:
            apply_scale_translate(true, args);
            break;
        case 0x20:
            vertex_color = static_cast<std::uint16_t>(value & 0x7fffU);
            break;
        case 0x21:
            normal = {signed10(value), signed10(value >> 10), signed10(value >> 20)};
            light_vertex();
            if (((texture_param >> 30) & 3U) == 2U) {
                const auto uv = texcoord_transform();
                transformed_texcoord_s = uv[0];
                transformed_texcoord_t = uv[1];
            }
            break;
        case 0x22: {
            texcoord_s = static_cast<std::int16_t>(value & 0xffffU) * 256;
            texcoord_t = static_cast<std::int16_t>(value >> 16) * 256;
            const auto mode = (texture_param >> 30) & 3U;
            if (mode == 0U || mode == 1U) {
                const auto uv = texcoord_transform();
                transformed_texcoord_s = uv[0];
                transformed_texcoord_t = uv[1];
            }
            break;
        }
        case 0x23:
            current_position[0] = fixed_from_int16(args[0]);
            current_position[1] = fixed_from_int16(args[0] >> 16);
            current_position[2] = fixed_from_int16(args[1]);
            submit_vertex();
            break;
        case 0x24:
            current_position[0] = signed10(value) * 64;
            current_position[1] = signed10(value >> 10) * 64;
            current_position[2] = signed10(value >> 20) * 64;
            submit_vertex();
            break;
        case 0x25:
            vertex_xy(value, 0, 1);
            break;
        case 0x26:
            vertex_xy(value, 0, 2);
            break;
        case 0x27:
            vertex_xy(value, 1, 2);
            break;
        case 0x28:
            current_position[0] = static_cast<std::int16_t>(current_position[0] + signed10(value));
            current_position[1] = static_cast<std::int16_t>(current_position[1] + signed10(value >> 10));
            current_position[2] = static_cast<std::int16_t>(current_position[2] + signed10(value >> 20));
            submit_vertex();
            break;
        case 0x29:
            pending_polygon_attr = value;
            if (!in_vertex_list) polygon_attr = value;
            break;
        case 0x2a:
            texture_param = value;
            break;
        case 0x2b:
            palette_param = value;
            break;
        case 0x30:
            material_diffuse_ambient = value;
            set_material_vertex_color();
            break;
        case 0x31:
            material_specular_emission = value;
            break;
        case 0x32:
            set_light_direction(value);
            break;
        case 0x33: {
            const auto light = static_cast<std::size_t>(value >> 30);
            light_color[light] = {color5(static_cast<std::uint16_t>(value), 0),
                                  color5(static_cast<std::uint16_t>(value), 5),
                                  color5(static_cast<std::uint16_t>(value), 10)};
            break;
        }
        case 0x34:
            for (std::size_t i = 0; i < 32; ++i) {
                const auto word = args[i];
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    shininess[i * 4U + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
                }
            }
            break;
        case 0x40:
            in_vertex_list = true;
            primitive_mode = static_cast<std::uint8_t>(value & 3U);
            primitive_vertex_count = 0;
            polygon_attr = pending_polygon_attr;
            break;
        case 0x41:
            in_vertex_list = false;
            primitive_vertex_count = 0;
            break;
        case 0x50:
            in_vertex_list = false;
            primitive_vertex_count = 0;
            pending_swap_mode = value & 3U;
            swap_pending = true;
            break;
        case 0x60:
            put32(0x04000580U, value);
            break;
        case 0x70:
            box_test_result = box_intersects_view(args[0], args[1], args[2]);
            break;
        case 0x71:
            box_test_result = false;
            store_position_result(args[0], args[1]);
            break;
        case 0x72:
            store_vector_result(value);
            break;
        default:
            break;
        }
    }

    bool next_command(std::uint8_t& code) {
        while (true) {
            if (have_command_word && command_byte < 4U) {
                code = static_cast<std::uint8_t>((command_word >> (command_byte * 8U)) & 0xffU);
                ++command_byte;
                if (code == 0U) {
                    have_command_word = false;
                    command_byte = 4;
                    continue;
                }
                return true;
            }
            FifoWord word{};
            if (!pop_word(word)) return false;
            if (word.direct_command) {
                command_word = word.value;
                command_byte = 1;
                have_command_word = true;
                code = static_cast<std::uint8_t>(word.value);
                return true;
            }
            command_word = word.value;
            command_byte = 0;
            have_command_word = true;
        }
    }

    bool gather_pending_args() {
        while (pending_received < pending_needed) {
            FifoWord word{};
            if (!pop_word(word)) return false;
            if (word.direct_command) {
                suspended_packed.code = pending_code;
                suspended_packed.count = pending_received;
                std::copy_n(pending_args.begin(), pending_received, suspended_packed.args.begin());
                have_suspended_packed = true;
                pending_code = static_cast<std::uint8_t>(word.value);
                pending_needed = command_parameter_count(pending_code);
                pending_received = 0;
                pending_args.fill(0);
            } else {
                pending_args[pending_received++] = word.value;
            }
        }
        return true;
    }


    void run(std::int64_t ticks, const std::function<void()>& callback) {
        if (ticks <= 0) return;
        const bool old_below = below_half();
        const bool old_irq = irq_condition();
        flush_direct_queue();
        while (ticks > 0 && !swap_pending) {
            if (!have_pending_command) {
                std::uint8_t code = 0;
                if (!next_command(code)) break;
                have_pending_command = true;
                pending_code = code;
                pending_needed = command_parameter_count(code);
                pending_received = 0;
                pending_args.fill(0);
                pending_cycles = 0;
            }
            if (!gather_pending_args()) break;
            if (pending_cycles == 0) pending_cycles = command_cost(pending_code);
            if (ticks < pending_cycles) {
                pending_cycles -= ticks;
                ticks = 0;
                break;
            }
            ticks -= pending_cycles;
            pending_cycles = 0;
            execute(pending_code, pending_args);
            if (pending_code >= 0x70U && pending_code <= 0x72U) --pending_test_commands;
            if (pending_code == 0x11U || pending_code == 0x12U) --pending_stack_commands;
            if (have_suspended_packed) {
                pending_code = suspended_packed.code;
                pending_needed = command_parameter_count(pending_code);
                pending_received = suspended_packed.count;
                std::copy_n(suspended_packed.args.begin(), pending_received, pending_args.begin());
                have_suspended_packed = false;
            } else {
                have_pending_command = false;
                pending_needed = 0;
                pending_received = 0;
            }
            flush_direct_queue();
        }
        notify_fifo_change(old_below, old_irq, callback);
    }

    float plane_distance(const ClipVertex& vertex, int plane) const {
        const auto x = vertex.clip[0];
        const auto y = vertex.clip[1];
        const auto z = vertex.clip[2];
        const auto w = vertex.clip[3];
        switch (plane) {
        case 0: return x + w;
        case 1: return w - x;
        case 2: return y + w;
        case 3: return w - y;
        case 4: return z + w;
        default: return w - z;
        }
    }

    static ClipVertex interpolate_clip(const ClipVertex& a, const ClipVertex& b, float t) {
        ClipVertex result{};
        for (std::size_t i = 0; i < 4; ++i) result.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
        result.u = a.u + (b.u - a.u) * t;
        result.v = a.v + (b.v - a.v) * t;
        for (std::size_t i = 0; i < 3; ++i) result.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
        return result;
    }

    std::size_t clip_polygon(const Polygon& polygon, std::array<ClipVertex, 16>& output_vertices) const {
        std::array<ClipVertex, 16> input{};
        std::size_t count = polygon.count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& source = render_vertices[polygon.index[i]];
            input[i] = {source.clip, source.u, source.v, source.color};
        }
        if ((polygon.attr & (1U << 12)) == 0U) {
            bool any_beyond_far = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (plane_distance(input[i], 5) < 0.0F) any_beyond_far = true;
            }
            if (any_beyond_far) return 0;
        }
        const int last_plane = (polygon.attr & (1U << 12)) != 0U ? 6 : 5;
        for (int plane = 0; plane < last_plane; ++plane) {
            std::array<ClipVertex, 16> next{};
            std::size_t next_count = 0;
            if (count == 0) return 0;
            auto previous = input[count - 1U];
            auto previous_distance = plane_distance(previous, plane);
            bool previous_inside = previous_distance >= 0.0F;
            for (std::size_t i = 0; i < count; ++i) {
                const auto current = input[i];
                const auto current_distance = plane_distance(current, plane);
                const bool current_inside = current_distance >= 0.0F;
                if (current_inside != previous_inside) {
                    const auto divisor = previous_distance - current_distance;
                    if (std::abs(divisor) > 1.0e-20F && next_count < next.size()) {
                        next[next_count++] = interpolate_clip(previous, current, previous_distance / divisor);
                    }
                }
                if (current_inside && next_count < next.size()) next[next_count++] = current;
                previous = current;
                previous_distance = current_distance;
                previous_inside = current_inside;
            }
            input = next;
            count = next_count;
        }
        output_vertices = input;
        return count;
    }

    ScreenVertex to_screen(const ClipVertex& vertex, std::uint32_t viewport) const {
        ScreenVertex result{};
        const auto w = vertex.clip[3];
        if (std::abs(w) < 1.0e-20F) return result;
        const auto inv_w = 1.0F / w;
        const float x1 = static_cast<float>(viewport & 0xffU);
        const float y1 = static_cast<float>((viewport >> 8) & 0xffU);
        // Viewport endpoints are inclusive; retain the byte-wrapped difference
        // before adding one so a full-width viewport spans all 256 pixels.
        const float width = static_cast<float>(static_cast<std::uint8_t>(((viewport >> 16) & 0xffU) - (viewport & 0xffU))) + 1.0F;
        const float height = static_cast<float>(static_cast<std::uint8_t>(((viewport >> 24) & 0xffU) - ((viewport >> 8) & 0xffU))) + 1.0F;
        const auto ndc_x = vertex.clip[0] * inv_w;
        const auto ndc_y = vertex.clip[1] * inv_w;
        const float y_base = y1 > 191.0F ? y1 - 255.0F : y1;
        result.x = x1 + (ndc_x + 1.0F) * width * 0.5F;
        result.y = 192.0F - (y_base + (ndc_y + 1.0F) * height * 0.5F);
        result.x *= static_cast<float>(tgt_.scale);
        result.y *= static_cast<float>(tgt_.scale);
        result.z = vertex.clip[2] * inv_w;
        result.inv_w = inv_w;
        result.u_over_w = vertex.u * inv_w;
        result.v_over_w = vertex.v * inv_w;
        for (std::size_t i = 0; i < 3; ++i) result.color_over_w[i] = vertex.color[i] * inv_w;
        return result;
    }

    static float edge_function(const ScreenVertex& a, const ScreenVertex& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    }

    std::uint16_t edge_color(std::uint8_t id) const {
        const auto color_index = static_cast<std::uint32_t>(id >> 3);
        const auto addr = 0x04000330U + color_index * 2U;
        const auto aligned = addr & ~3U;
        const auto raw = get32(aligned);
        return static_cast<std::uint16_t>(raw >> ((addr & 2U) * 8U));
    }

    template<bool Capture = false, bool Scaled = false>
    void clear_framebuffer(int y_begin, int y_end) {
        static_assert(!Capture || !Scaled, "debug capture is native-only");
        if constexpr (Capture) {
            std::fill(opaque_only.begin() + y_begin * tgt_.width,
                      opaque_only.begin() + y_end * tgt_.width, 1);
            std::fill(capture_data->causes.begin() + y_begin * tgt_.width,
                      capture_data->causes.begin() + y_end * tgt_.width, 0);
            std::fill(capture_data->partial_seen.begin() + y_begin * tgt_.width,
                      capture_data->partial_seen.begin() + y_end * tgt_.width, false);
            for (int band = y_begin / tgt_.band_height; band < (y_end + tgt_.band_height - 1) / tgt_.band_height; ++band)
                capture_data->polygon_touches[band].fill(0);
        }
        const auto clear_color = get32(0x04000350U);
        const auto clear_depth = depth15_to24(
            static_cast<std::uint16_t>(get32(0x04000354U) & 0x7fffU));
        const auto id = static_cast<std::uint8_t>((clear_color >> 24) & 63U);
        const bool fog = (clear_color & (1U << 15)) != 0U;
        const auto alpha = static_cast<std::uint8_t>((clear_color >> 16) & 31U);
        const auto red = expand6(color5(static_cast<std::uint16_t>(clear_color), 0));
        const auto green = expand6(color5(static_cast<std::uint16_t>(clear_color), 5));
        const auto blue = expand6(color5(static_cast<std::uint16_t>(clear_color), 10));
        const bool bitmap = (disp3d & (1U << 14)) != 0U;
        if (!bitmap) {
            const FramePixel pixel{red, green, blue, alpha, clear_depth, id, fog};
            auto* destination = tgt_.fb + y_begin * tgt_.width;
            const auto* end = tgt_.fb + y_end * tgt_.width;
            // Copy padding too so the compiler can use full-width stores instead
            // of separate colour, depth and flag stores for each cleared pixel.
            for (; destination != end; ++destination)
                std::memcpy(destination, &pixel, sizeof(pixel));
            return;
        }
        const auto* color_slot = texture_source.texture_slot(2);
        const auto* depth_slot = texture_source.texture_slot(3);
        const auto scroll = (get32(0x04000354U) >> 16) & 0xffffU;
        const auto x_offset = scroll & 0xffU;
        const auto y_offset = (scroll >> 8) & 0xffU;
        for (std::size_t y = static_cast<std::size_t>(y_begin);
             y < static_cast<std::size_t>(y_end); ++y) {
            const auto source_y = Scaled ? y / tgt_.scale : y;
            for (std::size_t x = 0; x < static_cast<std::size_t>(tgt_.width); ++x) {
                auto pixel = FramePixel{red, green, blue, alpha, clear_depth, id, fog};
                if (bitmap) {
                    // Rear-plane VRAM stays at native resolution. Specializing
                    // this division away preserves the native clear-image path.
                    const auto source_x = Scaled ? x / tgt_.scale : x;
                    const auto bitmap_index = (((source_y + y_offset) & 0xffU) * 256U +
                                               ((source_x + x_offset) & 0xffU)) * 2U;
                    // Unmapped texture slots read zero independently, including the rear plane.
                    const auto packed_color = color_slot == nullptr
                        ? texture_source.texture_half(0x40000U + bitmap_index)
                        : static_cast<std::uint16_t>(color_slot[bitmap_index] |
                            (static_cast<std::uint16_t>(color_slot[bitmap_index + 1U]) << 8U));
                    const auto packed_depth = depth_slot == nullptr
                        ? texture_source.texture_half(0x60000U + bitmap_index)
                        : static_cast<std::uint16_t>(depth_slot[bitmap_index] |
                            (static_cast<std::uint16_t>(depth_slot[bitmap_index + 1U]) << 8U));
                    pixel.r = expand6(color5(packed_color, 0));
                    pixel.g = expand6(color5(packed_color, 5));
                    pixel.b = expand6(color5(packed_color, 10));
                    pixel.alpha = (packed_color & 0x8000U) != 0U ? 31U : 0U;
                    pixel.depth = depth15_to24(static_cast<std::uint16_t>(packed_depth & 0x7fffU));
                    pixel.fog = (packed_depth & 0x8000U) != 0U;
                }
                tgt_.fb[y * tgt_.width + x] = pixel;
            }
        }
    }

    struct PolygonShading {
        std::uint32_t mode = 0U;
        std::uint8_t alpha_attr = 0U;
        bool textured = false;
        bool highlight = false;
        int texture_width = 8;
        int texture_height = 8;
        bool repeat_x = false;
        bool flip_x = false;
        bool repeat_y = false;
        bool flip_y = false;
        std::uint32_t texture_address = 0;
        std::uint32_t palette_address = 0;
        bool color_zero_transparent = false;
        const gpu3d_detail::Texel* decoded = nullptr;
        struct CachedTexel {
            std::uint32_t key = 0xffffffffU;
            gpu3d_detail::Texel value{};
        };
        // Include the sub-byte texel index in the key: neighboring indexed
        // texels can share a byte but must never share a decoded result.
        std::array<CachedTexel, 256> texels{};
    };
    std::array<PolygonShading, kRenderBands> band_shading{};

    void prepare_frame_textures() {
        if (texture_source.generation == nullptr) return;
        decoded_textures.begin_frame(texture_source.generation(texture_source.context));
        if ((disp3d & 1U) == 0U) return;
        for (std::size_t i = 0; i < render_polygon_count; ++i) {
            const auto& polygon = render_polygons[i];
            decoded_textures.acquire(texture_source, polygon.texture, polygon.palette);
        }
    }

    void prepare_polygon_shading(const Polygon& polygon, PolygonShading& shading) const {
        shading.mode = (polygon.attr >> 4) & 3U;
        shading.alpha_attr = static_cast<std::uint8_t>((polygon.attr >> 16) & 31U);
        shading.textured = (disp3d & 1U) != 0U && ((polygon.texture >> 26) & 7U) != 0U;
        shading.highlight = shading.mode == 2U && (disp3d & (1U << 1U)) != 0U;
        shading.decoded = nullptr;
        if (shading.textured) {
            shading.texture_width = static_cast<int>(8U << ((polygon.texture >> 20) & 7U));
            shading.texture_height = static_cast<int>(8U << ((polygon.texture >> 23) & 7U));
            shading.repeat_x = (polygon.texture & (1U << 16U)) != 0U;
            shading.repeat_y = (polygon.texture & (1U << 17U)) != 0U;
            shading.flip_x = (polygon.texture & (1U << 18U)) != 0U;
            shading.flip_y = (polygon.texture & (1U << 19U)) != 0U;
            shading.texture_address = (polygon.texture & 0xffffU) * 8U;
            shading.palette_address = (polygon.palette & 0x1fffU) *
                (((polygon.texture >> 26U) & 7U) == 2U ? 8U : 16U);
            shading.color_zero_transparent = (polygon.texture & (1U << 29U)) != 0U;
            shading.decoded = texture_source.generation == nullptr ? nullptr :
                decoded_textures.find(polygon.texture, polygon.palette);
            // Capacity misses and unversioned sources retain the polygon cache.
            if (shading.decoded == nullptr)
                for (auto& texel : shading.texels) texel.key = 0xffffffffU;
        }
    }

    // DS texture dimensions are powers of two, so unsigned masks implement modulo exactly.
    static int wrapped_coordinate(int value, int size, bool repeat, bool flip) {
        if (!repeat) return std::clamp(value, 0, size - 1);
        const std::uint32_t extent = static_cast<std::uint32_t>(size);
        const std::uint32_t period = extent * (flip ? 2U : 1U);
        std::uint32_t result = static_cast<std::uint32_t>(value) & (period - 1U);
        if (flip && result >= extent) result = period - 1U - result;
        return static_cast<int>(result);
    }
    template<int Format>
    gpu3d_detail::Texel decode_prepared_texel(const Polygon& polygon,
                                             const PolygonShading& shading,
                                             int x, int y) const {
        if constexpr (Format == -1) {
            return gpu3d_detail::decode_texel(texture_source, polygon.texture, polygon.palette, x, y);
        } else {
            const auto pixel = static_cast<std::uint32_t>(y * shading.texture_width + x);
            std::uint16_t color;
            if constexpr (Format == 7) {
                color = texture_source.texture_half(shading.texture_address + pixel * 2U);
                if ((color & 0x8000U) == 0U) return {};
            } else {
                unsigned index;
                if constexpr (Format == 2) {
                    const auto byte = texture_source.texture_byte(shading.texture_address + pixel / 4U);
                    index = (byte >> ((pixel & 3U) * 2U)) & 3U;
                } else if constexpr (Format == 3) {
                    const auto byte = texture_source.texture_byte(shading.texture_address + pixel / 2U);
                    index = (byte >> ((pixel & 1U) * 4U)) & 15U;
                } else {
                    index = texture_source.texture_byte(shading.texture_address + pixel);
                }
                if (index == 0U && shading.color_zero_transparent) return {};
                color = texture_source.palette_half(shading.palette_address + index * 2U);
            }
            return {expand6(color5(color, 0)), expand6(color5(color, 5)),
                    expand6(color5(color, 10)), 31};
        }
    }

    template<int Format = -1>
    gpu3d_detail::Texel sample_texture(const Polygon& polygon, float u, float v,
                                       PolygonShading& shading) const {
        // Generic sampling is compiled for the baseline ISA, where float floor
        // needs a long roundtrip. Specialized AVX2 spans keep their hardware
        // floor instruction rather than paying for integer correction there.
        const int iu = Format == -1 ? gpu3d_detail::floor_texcoord(u) : static_cast<int>(std::floor(u));
        const int iv = Format == -1 ? gpu3d_detail::floor_texcoord(v) : static_cast<int>(std::floor(v));
        const int x = wrapped_coordinate(iu, shading.texture_width, shading.repeat_x, shading.flip_x);
        const int y = wrapped_coordinate(iv, shading.texture_height, shading.repeat_y, shading.flip_y);
        const auto pixel = static_cast<std::uint32_t>(y * shading.texture_width + x);
        if (shading.decoded != nullptr) return shading.decoded[pixel];
        // The base and format are fixed for this polygon. A linear texel
        // address uniquely identifies direct, indexed and compressed samples.
        auto& entry = shading.texels[(pixel ^ (pixel >> 8U)) & 255U];
        if (entry.key != pixel) {
            entry.value = decode_prepared_texel<Format>(polygon, shading, x, y);
            entry.key = pixel;
        }
        return entry.value;
    }

    template<int Maximum>
    static std::uint8_t rounded_color(float value) {
        if (value <= 0.0F) return 0;
        if (value >= static_cast<float>(Maximum)) return Maximum;
        const int integer = static_cast<int>(value);
        // Adding 0.5f can round a float just below a half up to that half.
        // Compare the original fraction instead to match lround exactly.
        return static_cast<std::uint8_t>(integer + (value - static_cast<float>(integer) >= 0.5F));
    }

    static std::uint8_t clamp6(float value) { return rounded_color<63>(value); }
    static std::uint8_t clamp5(float value) { return rounded_color<31>(value); }

    std::array<std::uint8_t, 3> toon_color(std::uint8_t index) const {
        const auto addr = 0x04000380U + static_cast<std::uint32_t>(index) * 2U;
        const auto value = get32(addr & ~3U);
        const auto color = static_cast<std::uint16_t>(value >> ((addr & 2U) * 8U));
        return {expand6(color5(color, 0)), expand6(color5(color, 5)), expand6(color5(color, 10))};
    }

    struct ShadedPixel {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        std::uint8_t alpha = 0;
    };

    // -1 retains all modes; 0 is opaque untextured modulation; 2/3/4/7
    // specialize the common opaque indexed/direct modulation spans.
    template<int Format = -1, bool Rounded = false>
    ShadedPixel shade_pixel(const Polygon& polygon, PolygonShading& shading,
                            const std::array<float, 3>& vertex_color, float u, float v) const {
        const std::array<std::uint8_t, 3> vertex = [&] {
            if constexpr (Rounded) {
                return std::array<std::uint8_t, 3>{
                    static_cast<std::uint8_t>(vertex_color[0]),
                    static_cast<std::uint8_t>(vertex_color[1]),
                    static_cast<std::uint8_t>(vertex_color[2])};
            } else {
                return std::array<std::uint8_t, 3>{
                    clamp6(vertex_color[0]), clamp6(vertex_color[1]), clamp6(vertex_color[2])};
            }
        }();
        if constexpr (Format != -1) {
            if constexpr (Format == 0) return {vertex[0], vertex[1], vertex[2], 31};
            else {
                const auto texel = sample_texture<Format>(polygon, u, v, shading);
                return {
                    static_cast<std::uint8_t>(((texel.r + 1U) * (vertex[0] + 1U) - 1U) / 64U),
                    static_cast<std::uint8_t>(((texel.g + 1U) * (vertex[1] + 1U) - 1U) / 64U),
                    static_cast<std::uint8_t>(((texel.b + 1U) * (vertex[2] + 1U) - 1U) / 64U),
                    texel.a};
            }
        }
        const auto mode = shading.mode;
        const auto alpha_attr = shading.alpha_attr;
        const bool textured = shading.textured;
        const auto texel = textured ? sample_texture(polygon, u, v, shading)
                                    : gpu3d_detail::Texel{0, 0, 0, 31};
        const auto toon = mode == 2U
            ? toon_color(static_cast<std::uint8_t>(vertex[0] >> 1U)) : vertex;
        const bool highlight = shading.highlight;
        std::array<std::uint8_t, 3> shade = mode == 2U && !highlight ? toon : vertex;
        std::uint8_t alpha = alpha_attr == 0U ? 31U : alpha_attr;
        if (mode != 3U) {
            const std::array<std::uint8_t, 3> texture_color = textured
                ? std::array<std::uint8_t, 3>{texel.r, texel.g, texel.b}
                : std::array<std::uint8_t, 3>{63U, 63U, 63U};
            if (mode == 0U || mode == 2U) {
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto modulation_color = highlight ? vertex[0] : shade[i];
                    const auto modulated = ((static_cast<unsigned>(texture_color[i]) + 1U) *
                                            (static_cast<unsigned>(modulation_color) + 1U) - 1U) / 64U;
                    shade[i] = static_cast<std::uint8_t>(
                        highlight ? std::min<unsigned>(63U, modulated + toon[i]) : modulated);
                }
            } else if (mode == 1U && textured) {
                const auto texture_alpha = static_cast<unsigned>(texel.a);
                for (std::size_t i = 0; i < 3; ++i) {
                    shade[i] = (texture_alpha == 0U || texture_alpha == 31U)
                        ? (texture_alpha == 0U ? vertex[i] : texture_color[i])
                        : static_cast<std::uint8_t>(
                            (static_cast<unsigned>(texture_color[i]) * texture_alpha +
                             static_cast<unsigned>(vertex[i]) * (31U - texture_alpha)) >> 5U);
                }
            }
            if (textured && mode != 1U) {
                const auto alpha6 = ((static_cast<unsigned>(expand6(texel.a)) + 1U) *
                                     (static_cast<unsigned>(expand6(alpha)) + 1U) - 1U) / 64U;
                alpha = static_cast<std::uint8_t>(std::min(alpha6 / 2U, 31U));
            }
        }
        return {shade[0], shade[1], shade[2], alpha};
    }

    void apply_fog(FramePixel& pixel, std::uint32_t depth) const {
        if ((disp3d & (1U << 7)) == 0U) return;
        const auto fog_shift = (disp3d >> 8) & 15U;
        const auto step = 0x400U >> fog_shift;
        const auto offset = get32(0x0400035cU) & 0x7fffU;
        const auto depth_value = depth >> 9;
        const auto fog_density = [this](std::uint32_t index) {
            const auto address = 0x04000360U + index;
            const auto packed = get32(address & ~3U);
            return static_cast<std::int32_t>((packed >> ((address & 3U) * 8U)) & 0x7fU);
        };
        std::int32_t density = 0;
        if (step == 0U) {
            density = fog_density(depth_value <= offset ? 0U : 31U);
        } else {
            const auto relative_depth = depth_value > offset ? depth_value - offset : 0U;
            if (relative_depth <= step) {
                density = fog_density(0U);
            } else if (relative_depth >= step * 32U) {
                density = fog_density(31U);
            } else {
                const auto segment = relative_depth / step;
                const auto fraction = relative_depth - segment * step;
                const auto lower = fog_density(segment - 1U);
                const auto upper = fog_density(segment);
                density = (lower * static_cast<std::int32_t>(step - fraction) +
                           upper * static_cast<std::int32_t>(fraction)) /
                          static_cast<std::int32_t>(step);
            }
        }
        const auto density_weight = static_cast<std::uint32_t>(density == 127 ? 128 : density);
        const auto fog_color = get32(0x04000358U);
        const auto fog_alpha = static_cast<std::uint8_t>((fog_color >> 16) & 31U);
        if ((disp3d & (1U << 6)) == 0U) {
            const auto fog_r = expand6(color5(static_cast<std::uint16_t>(fog_color), 0));
            const auto fog_g = expand6(color5(static_cast<std::uint16_t>(fog_color), 5));
            const auto fog_b = expand6(color5(static_cast<std::uint16_t>(fog_color), 10));
            pixel.r = static_cast<std::uint8_t>((fog_r * density_weight + pixel.r * (128U - density_weight)) / 128U);
            pixel.g = static_cast<std::uint8_t>((fog_g * density_weight + pixel.g * (128U - density_weight)) / 128U);
            pixel.b = static_cast<std::uint8_t>((fog_b * density_weight + pixel.b * (128U - density_weight)) / 128U);
        }
        pixel.alpha = static_cast<std::uint8_t>(std::min<unsigned>(31U,
            (fog_alpha * density_weight + pixel.alpha * (128U - density_weight)) / 128U));
    }

    template<bool Opaque = false>
    void blend_pixel(FramePixel& pixel, const ShadedPixel& source, std::uint8_t polygon_id,
                     bool is_translucent_polygon, bool fog_enabled, std::uint32_t depth,
                     std::uint32_t attr, bool front_facing) {
        if constexpr (Opaque) {
            // These span formats only produce alpha 0/31 in modulation mode.
            // Alpha 31 cannot fail the five-bit alpha reference or blend, and
            // alpha 0 must leave every destination field untouched.
            if (source.alpha != 0U) {
                pixel.r = source.r;
                pixel.g = source.g;
                pixel.b = source.b;
                pixel.alpha = 31U;
                pixel.facing = front_facing ? 1U : 2U;
                pixel.polygon_id = polygon_id;
                pixel.is_translucent_polygon = is_translucent_polygon;
                pixel.fog = fog_enabled;
                pixel.depth = depth;
            }
            return;
        }
        const auto shading_mode = (attr >> 4) & 3U;
        if (shading_mode == 3U) {
            if (polygon_id == 0U) return;
            if (!pixel.stencil || pixel.polygon_id == polygon_id) return;
            pixel.stencil = false;
        }
        if (source.alpha == 0U) return;
        const auto alpha_ref = static_cast<std::uint8_t>(get32(0x04000340U) & 31U);
        if ((disp3d & (1U << 2)) != 0U && source.alpha < alpha_ref) return;
        const bool translucent = source.alpha < 31U;
        if (translucent && pixel.translucent_polygon_id == polygon_id) {
            return;
        }
        const bool blend = translucent && (disp3d & (1U << 3)) != 0U && pixel.alpha != 0U;
        if (blend) {
            const auto alpha = static_cast<unsigned>(source.alpha);
            const auto inverse = 31U - alpha;
            pixel.r = static_cast<std::uint8_t>((source.r * (alpha + 1U) + pixel.r * inverse) / 32U);
            pixel.g = static_cast<std::uint8_t>((source.g * (alpha + 1U) + pixel.g * inverse) / 32U);
            pixel.b = static_cast<std::uint8_t>((source.b * (alpha + 1U) + pixel.b * inverse) / 32U);
            pixel.alpha = std::max(pixel.alpha, source.alpha);
        } else {
            pixel.r = source.r;
            pixel.g = source.g;
            pixel.b = source.b;
            pixel.alpha = source.alpha;
        }
        pixel.facing = front_facing ? 1U : 2U;
        if (translucent) {
            pixel.translucent_polygon_id = polygon_id;
        } else {
            pixel.polygon_id = polygon_id;
            pixel.is_translucent_polygon = is_translucent_polygon;
        }
        pixel.fog = translucent ? (pixel.fog && fog_enabled) : fog_enabled;
        if (!translucent || (attr & (1U << 11)) != 0U) pixel.depth = depth;
    }

    template<bool Capture>
    void raster_triangle(const Polygon& polygon, PolygonShading& shading,
                         bool is_translucent_polygon,
                         const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                         int y_begin, int y_end) {
        const auto area = edge_function(a, b, c.x, c.y);
        if (std::abs(area) < 1.0e-12F) return;
        const bool front = area < 0.0F;
        const bool surface_enabled = front ? (polygon.attr & (1U << 7)) != 0U
                                           : (polygon.attr & (1U << 6)) != 0U;
        if (!surface_enabled) return;
        const auto min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const auto max_x = std::min(tgt_.width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const auto min_y = std::max(y_begin, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const auto max_y = std::min(y_end - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (max_x < min_x || max_y < min_y) return;
        const auto alpha_attr = static_cast<std::uint8_t>((polygon.attr >> 16) & 31U);
        const bool wireframe = alpha_attr == 0U;
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const auto e0 = edge_function(b, c, px, py);
                const auto e1 = edge_function(c, a, px, py);
                const auto e2 = edge_function(a, b, px, py);
                float w0 = e0 / area;
                float w1 = e1 / area;
                float w2 = e2 / area;
                if (w0 < -1.0e-6F || w1 < -1.0e-6F || w2 < -1.0e-6F) continue;
                const auto edge_weight = std::min({w0, w1, w2});
                if (wireframe && edge_weight > 0.02F) continue;
                const auto inv_w = w0 * a.inv_w + w1 * b.inv_w + w2 * c.inv_w;
                if (std::abs(inv_w) < 1.0e-20F) continue;
                const auto inv = 1.0F / inv_w;
                const auto u = (w0 * a.u_over_w + w1 * b.u_over_w + w2 * c.u_over_w) * inv;
                const auto v = (w0 * a.v_over_w + w1 * b.v_over_w + w2 * c.v_over_w) * inv;
                std::array<float, 3> vertex_color{};
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    vertex_color[channel] = (w0 * a.color_over_w[channel] +
                        w1 * b.color_over_w[channel] + w2 * c.color_over_w[channel]) * inv;
                }
                const auto ndc_z = w0 * a.z + w1 * b.z + w2 * c.z;
                const auto z_depth = static_cast<std::uint32_t>(std::floor(
                    std::clamp((ndc_z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F)) << 2;
                const auto depth = (swap_mode & 2U) != 0U
                    ? static_cast<std::uint32_t>(std::clamp(
                        static_cast<int>(std::floor(inv * 4096.0F)), 0, 0x00ffffff))
                    : z_depth;
                write_fragment<-1, Capture>(polygon, shading, is_translucent_polygon, front, x, y,
                               vertex_color, u, v, depth);
            }
        }
    }

    // Depth test, shading and blending for one fragment (shared by both rasterizers).
    template<int Format = -1, bool Capture = false, bool Rounded = false>
    void write_fragment(const Polygon& polygon, PolygonShading& shading,
                        bool is_translucent_polygon, bool front_facing,
                        int x, int y, const std::array<float, 3>& vertex_color, float u, float v,
                        std::uint32_t depth) {
        const auto polygon_id = static_cast<std::uint8_t>((polygon.attr >> 24) & 63U);
        const auto pixel_index = static_cast<std::size_t>(y) * tgt_.width + static_cast<std::size_t>(x);
        if constexpr (Capture) capture_fragment(polygon, shading, pixel_index, u, v);
        auto& destination = tgt_.fb[pixel_index];
        const bool depth_equal = (polygon.attr & (1U << 14)) != 0U;
        const auto depth_delta = depth > destination.depth
            ? depth - destination.depth : destination.depth - depth;
        // A front-facing fragment over an opaque back-facing one passes on equal depth.
        const bool less_equal = front_facing && destination.facing == 2U && destination.alpha == 31U;
        const bool depth_fail = depth_equal ? depth_delta > kDepthEqualTolerance
            : less_equal ? depth > destination.depth : depth >= destination.depth;
        if (depth_fail) {
            if (((polygon.attr >> 4) & 3U) == 3U && polygon_id == 0U)
                destination.stencil = true;
            return;
        }
        // Texture decoding and colour shading cannot affect a depth-rejected fragment.
        // Keep the shadow-mask stencil update above, but shade only visible fragments.
        const auto shaded = shade_pixel<Format, Rounded>(polygon, shading, vertex_color, u, v);
        blend_pixel<(Format != -1)>(destination, shaded, polygon_id,
                    is_translucent_polygon, (polygon.attr & (1U << 15)) != 0U,
                    depth, polygon.attr, front_facing);
    }

    // Scanline polygon fill. Vertices live on a 28.4 fixed-point grid; each edge
    // tracks its exact intersection with every scanline (integer DDA with a
    // ceiling convention), attributes advance per scanline along the edge, and a
    // span interpolates linearly between its left and right edge values, one step
    // per pixel from the first covered pixel. Perspective correction divides the
    // interpolated u/w, v/w and colour/w by the interpolated 1/w per pixel.
    struct RasterVertex {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::array<float, 7> attribute{}; // 1/w, z (0..1), u/w, v/w, r/w, g/w, b/w
    };

    enum class PolygonShape { Empty, Dot, Filled, Wire };
    struct PreparedPolygon {
        PolygonShape shape = PolygonShape::Empty;
        std::size_t count = 0;
        int y_begin = 0;
        int y_end = 0;
        bool front_facing = false;
        std::array<ScreenVertex, 16> screen{};
        std::array<RasterVertex, 10> raster{};
        ClipVertex dot{};
    };
    // Created only for multithreaded rendering, then reused without per-frame
    // allocation. Inline rendering needs just one caller-owned scratch polygon.
    std::unique_ptr<std::array<PreparedPolygon, kPolygonLimit>> band_polygons;

    struct RasterEdge {
        std::int64_t x = 0;
        std::int64_t x_step = 0;
        std::int64_t numerator = 0;
        std::int64_t denominator = 1;
        std::int64_t error = 0;
        int y = 0;
        int height = 0;
        std::array<float, 7> value{};
        std::array<float, 7> step{};
    };

    static bool floor_div_mod(std::int64_t numerator, std::int64_t denominator,
                              std::int64_t& quotient, std::int64_t& remainder) {
        if (denominator <= 0) return false;
        quotient = numerator / denominator;
        remainder = numerator % denominator;
        if (remainder < 0) {
            --quotient;
            remainder += denominator;
        }
        return true;
    }

    static int ceil_fixed(std::int64_t value) {
        std::int64_t quotient = 0;
        std::int64_t remainder = 0;
        floor_div_mod(value + 15, 16, quotient, remainder);
        return static_cast<int>(quotient);
    }

    RasterVertex to_raster_vertex(const ClipVertex& vertex, std::uint32_t viewport) const {
        const float w = vertex.clip[3] != 0.0F ? vertex.clip[3] : 0.00000001F;
        const float x1 = static_cast<float>(viewport & 0xffU);
        const float y1 = static_cast<float>((viewport >> 8) & 0xffU);
        const float width = static_cast<float>(static_cast<std::uint8_t>(
            ((viewport >> 16) & 0xffU) - (viewport & 0xffU))) + 1.0F;
        const float height = static_cast<float>(static_cast<std::uint8_t>(
            ((viewport >> 24) & 0xffU) - ((viewport >> 8) & 0xffU))) + 1.0F;
        const float x01 = (vertex.clip[0] + w) / (2.0F * w);
        const float y01 = (vertex.clip[1] + w) / (2.0F * w);
        const float z01 = (vertex.clip[2] + w) / (2.0F * w);
        const float screen_x = x01 * width + x1;
        const float screen_y = 192.0F - (y01 * height + (y1 > 191.0F ? y1 - 255.0F : y1));
        constexpr float kLimit = 1.0e6F;
        RasterVertex result;
        // Quantize after scaling: the DDA still uses 16 subpixels per output
        // pixel, so coverage and attribute steps share the scaled row units.
        result.x = static_cast<std::int32_t>(std::clamp(16.0F * (screen_x * tgt_.scale), -kLimit, kLimit));
        result.y = static_cast<std::int32_t>(std::clamp(16.0F * (screen_y * tgt_.scale), -kLimit, kLimit));
        result.attribute = {1.0F / w, z01, vertex.u / w, vertex.v / w,
                            vertex.color[0] / w, vertex.color[1] / w, vertex.color[2] / w};
        return result;
    }

    static bool make_edge(const RasterVertex& top, const RasterVertex& bottom, RasterEdge& edge) {
        edge = {};
        edge.y = ceil_fixed(top.y);
        edge.height = ceil_fixed(bottom.y) - edge.y;
        edge.x = ceil_fixed(top.x);
        const std::int64_t width = ceil_fixed(bottom.x) - edge.x;
        if (edge.height != 0 || width != 0) {
            std::int64_t delta_y = static_cast<std::int64_t>(bottom.y) - top.y;
            const std::int64_t delta_x = static_cast<std::int64_t>(bottom.x) - top.x;
            if (delta_y != 0) {
                const std::int64_t initial = delta_x * 16 * edge.y - delta_x * top.y +
                                             delta_y * top.x - 1 + delta_y * 16;
                if (!floor_div_mod(initial, delta_y * 16, edge.x, edge.error)) return false;
                if (!floor_div_mod(delta_x * 16, delta_y * 16, edge.x_step, edge.numerator)) return false;
                edge.denominator = delta_y * 16;
            } else {
                edge.x_step = width;
                edge.numerator = 0;
                edge.error = 0;
                edge.denominator = 1;
                delta_y = 1;
            }
            const float y_prestep = static_cast<float>(static_cast<std::int64_t>(edge.y) * 16 - top.y) / 16.0F;
            const float per_line = 1.0F / (static_cast<float>(delta_y) / 16.0F);
            for (std::size_t i = 0; i < edge.value.size(); ++i) {
                const float slope = per_line * (bottom.attribute[i] - top.attribute[i]);
                edge.value[i] = top.attribute[i] + y_prestep * slope;
                edge.step[i] = slope;
            }
        } else {
            edge.x_step = 1;
            edge.value = top.attribute;
        }
        return true;
    }

    static void step_edge(RasterEdge& edge) {
        edge.x += edge.x_step;
        ++edge.y;
        --edge.height;
        for (std::size_t i = 0; i < edge.value.size(); ++i) edge.value[i] += edge.step[i];
        edge.error += edge.numerator;
        if (edge.error >= edge.denominator) {
            ++edge.x;
            edge.error -= edge.denominator;
        }
    }

    // Diagnose raw causes, then permit only a single partial fragment over
    // opaque history. Shadow/wire and ambiguous multilayer pixels stay software.
    void capture_fragment(const Polygon& polygon, PolygonShading& shading,
                          std::size_t index, float u, float v) {
        std::uint8_t causes = 0;
        if (shading.alpha_attr == 0U) causes |= 8U;
        else if (shading.alpha_attr != 31U) causes |= 1U;
        if (shading.mode == 3U) causes |= 4U;
        std::uint8_t alpha = shading.alpha_attr == 0U ? 31U : shading.alpha_attr;
        if (shading.textured) {
            const auto texel_alpha = sample_texture(polygon, u, v, shading).a;
            if (texel_alpha != 0U && texel_alpha != 31U)
                causes |= shading.mode == 1U && shading.alpha_attr == 31U ? 16U : 2U;
            if (shading.mode != 1U && shading.mode != 3U) {
                const auto alpha6 = ((static_cast<unsigned>(expand6(texel_alpha)) + 1U) *
                    (static_cast<unsigned>(expand6(alpha)) + 1U) - 1U) / 64U;
                alpha = static_cast<std::uint8_t>(std::min(alpha6 / 2U, 31U));
            }
        }
        capture_data->causes[index] |= causes;
        if ((causes & 3U) != 0U) {
            const auto polygon_index = static_cast<std::size_t>(&polygon - render_polygons.data());
            ++capture_data->polygon_touches[index / (tgt_.width * tgt_.band_height)][polygon_index];
        }
        if ((disp3d & 4U) != 0U && alpha < (get32(0x04000340U) & 31U) &&
            (causes & (4U | 8U)) == 0U) return;
        const auto& destination = tgt_.fb[index];
        if (!gpu3d_detail::capture_single_layer(alpha, polygon.attr, disp3d,
                destination.alpha, destination.fog, capture_data->partial_seen[index]))
            opaque_only[index] = 0;
    }

#if DSR_X86_SPANS
    // Four fragments share exact DIVPS perspective correction and colour/depth
    // conversion. Attribute lanes still advance by the original three/four
    // sequential additions: value + delta * lane would change rounding.
    template<int Format>
    DSR_SPAN_TARGETS
    void draw_span_batch(const Polygon& polygon, PolygonShading& shading,
                         bool is_translucent_polygon, bool front_facing,
                         int& x, int y, int& width, std::array<float, 7>& value,
                         const std::array<float, 7>& delta) {
        __m128 low = _mm_loadu_ps(value.data());
        __m128 high = _mm_setr_ps(value[4], value[5], value[6], 0.0F);
        const __m128 low_step = _mm_loadu_ps(delta.data());
        const __m128 high_step = _mm_setr_ps(delta[4], delta[5], delta[6], 0.0F);
        const __m128 zero = _mm_setzero_ps();
        const __m128 one = _mm_set1_ps(1.0F);
        const __m128 half = _mm_set1_ps(0.5F);
        const __m128 color_max = _mm_set1_ps(63.0F);
        const auto polygon_id = static_cast<std::uint8_t>((polygon.attr >> 24U) & 63U);
        const bool depth_equal = (polygon.attr & (1U << 14U)) != 0U;
        const bool shadow_mask = shading.mode == 3U && polygon_id == 0U;
        const bool fog_enabled = (polygon.attr & (1U << 15U)) != 0U;
        const bool w_depth = (swap_mode & 2U) != 0U;
        while (width >= 4) {
            __m128 a0 = low, a1 = _mm_add_ps(a0, low_step);
            __m128 a2 = _mm_add_ps(a1, low_step), a3 = _mm_add_ps(a2, low_step);
            __m128 b0 = high, b1 = _mm_add_ps(b0, high_step);
            __m128 b2 = _mm_add_ps(b1, high_step), b3 = _mm_add_ps(b2, high_step);
            // Save the sequential state before transposing attributes to pixels.
            if (width > 4) {
                low = _mm_add_ps(a3, low_step);
                high = _mm_add_ps(b3, high_step);
            }
            _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
            const __m128 w = _mm_div_ps(one, a0);
            __m128i depths;
            if (w_depth) {
                const auto scaled = _mm_mul_ps(w, _mm_set1_ps(4096.0F));
                depths = _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(scaled, zero),
                                                    _mm_set1_ps(16777215.0F)));
            } else {
                const auto z = _mm_min_ps(_mm_max_ps(a1, zero), one);
                depths = _mm_slli_epi32(_mm_cvttps_epi32(
                    _mm_mul_ps(z, _mm_set1_ps(4194303.0F))), 2);
            }
            auto* destination = tgt_.fb + y * tgt_.width + x;
            const __m128i old_depths = _mm_setr_epi32(
                destination[0].depth, destination[1].depth,
                destination[2].depth, destination[3].depth);
            __m128i failed;
            if (depth_equal) {
                const auto difference = _mm_sub_epi32(depths, old_depths);
                const auto sign = _mm_srai_epi32(difference, 31);
                const auto absolute = _mm_sub_epi32(_mm_xor_si128(difference, sign), sign);
                failed = _mm_cmpgt_epi32(absolute, _mm_set1_epi32(kDepthEqualTolerance));
            } else {
                failed = _mm_or_si128(_mm_cmpgt_epi32(depths, old_depths),
                                      _mm_cmpeq_epi32(depths, old_depths));
            }
            unsigned active = static_cast<unsigned>(~_mm_movemask_ps(_mm_castsi128_ps(failed))) & 15U;
            alignas(16) std::array<std::uint32_t, 4> depth{};
            _mm_store_si128(reinterpret_cast<__m128i*>(depth.data()), depths);
            if (front_facing && !depth_equal) {
                unsigned equal = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_castsi128_ps(_mm_cmpeq_epi32(depths, old_depths))));
                for (unsigned lane = 0; equal != 0U; ++lane, equal >>= 1U)
                    if ((equal & 1U) != 0U && destination[lane].facing == 2U &&
                        destination[lane].alpha == 31U) active |= 1U << lane;
            }
            if (shadow_mask) {
                for (unsigned lane = 0; lane < 4; ++lane)
                    if ((active & (1U << lane)) == 0U) destination[lane].stencil = true;
            }
            if (active != 0U) {
                _MM_TRANSPOSE4_PS(b0, b1, b2, b3);
                const auto rounded = [&](const __m128 color) {
                    const auto scaled = _mm_add_ps(_mm_mul_ps(color, w), half);
                    return _mm_cvtepi32_ps(_mm_cvttps_epi32(
                        _mm_min_ps(_mm_max_ps(scaled, zero), color_max)));
                };
                alignas(16) std::array<float, 4> red{}, green{}, blue{}, u{}, v{};
                _mm_store_ps(red.data(), rounded(b0));
                _mm_store_ps(green.data(), rounded(b1));
                _mm_store_ps(blue.data(), rounded(b2));
                if constexpr (Format != 0) {
                    _mm_store_ps(u.data(), _mm_mul_ps(a2, w));
                    _mm_store_ps(v.data(), _mm_mul_ps(a3, w));
                }
                for (unsigned lane = 0; lane < 4; ++lane) {
                    if ((active & (1U << lane)) == 0U) continue;
                    const auto shaded = shade_pixel<Format, true>(
                        polygon, shading, {red[lane], green[lane], blue[lane]}, u[lane], v[lane]);
                    blend_pixel<(Format != -1)>(destination[lane], shaded, polygon_id,
                        is_translucent_polygon, fog_enabled, depth[lane], polygon.attr, front_facing);
                }
            }
            width -= 4;
            x += 4;
        }
        if (width != 0) {
            _mm_storeu_ps(value.data(), low);
            alignas(16) std::array<float, 4> color{};
            _mm_store_ps(color.data(), high);
            for (unsigned channel = 0; channel < 3; ++channel) value[4 + channel] = color[channel];
        }
    }

#if DSR_WINDOWS_SPAN_DISPATCH
    template<int Format>
    static void span_default(Impl& self, const Polygon& polygon, PolygonShading& shading,
                             bool translucent, bool front, int& x, int y, int& width,
                             std::array<float, 7>& value, const std::array<float, 7>& delta) {
        self.draw_span_batch<Format>(polygon, shading, translucent, front, x, y, width, value, delta);
    }

    template<int Format>
    __attribute__((target("avx2"), noinline))
    static void span_avx2(Impl& self, const Polygon& polygon, PolygonShading& shading,
                          bool translucent, bool front, int& x, int y, int& width,
                          std::array<float, 7>& value, const std::array<float, 7>& delta) {
        self.draw_span_batch<Format>(polygon, shading, translucent, front, x, y, width, value, delta);
    }

    template<int Format>
    void dispatch_span_batch(const Polygon& polygon, PolygonShading& shading,
                              bool translucent, bool front, int& x, int y, int& width,
                              std::array<float, 7>& value, const std::array<float, 7>& delta) {
        // Function-local initialization is synchronized across render bands.
        // The CPU check includes OS support for preserving the AVX registers.
        static const auto function = [] {
            return gpu3d_detail::host_supports_avx2() ? &span_avx2<Format> : &span_default<Format>;
        }();
        function(*this, polygon, shading, translucent, front, x, y, width, value, delta);
    }
#endif
#endif

    template<int Format, bool Capture, bool Batch = true>
    void draw_span_specialized(const Polygon& polygon, PolygonShading& shading,
                   bool is_translucent_polygon, bool front_facing,
                   const RasterEdge& left, const RasterEdge& right) {
        const int y = left.y;
        if (y < 0 || y >= tgt_.height) return;
        int x = static_cast<int>(left.x);
        int width = static_cast<int>(right.x) - x;
        if (width <= 0) return;
        std::array<float, 7> value = left.value;
        const float inverse_width = 1.0F / static_cast<float>(width);
        std::array<float, 7> delta{};
        for (std::size_t i = 0; i < delta.size(); ++i)
            delta[i] = (right.value[i] - left.value[i]) * inverse_width;
        if (x < 0) {
            const float skipped = static_cast<float>(-x);
            for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i] * skipped;
            width += x;
            x = 0;
        }
        if (x + width > tgt_.width) width = tgt_.width - x;
        if (width <= 0) return;
#if DSR_X86_SPANS
        if constexpr (Batch && !Capture) {
            if (width >= 4) {
#if DSR_WINDOWS_SPAN_DISPATCH
                dispatch_span_batch<Format>(polygon, shading, is_translucent_polygon, front_facing,
                                           x, y, width, value, delta);
#else
                draw_span_batch<Format>(polygon, shading, is_translucent_polygon, front_facing,
                                       x, y, width, value, delta);
#endif
            }
        }
#endif
        for (; width > 0; --width, ++x) {
            const float w = 1.0F / value[0];
            std::array<float, 3> color{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float scaled = value[4 + channel] * w + 0.5F;
                color[channel] = static_cast<float>(std::clamp(static_cast<int>(std::max(scaled, 0.0F)), 0, 63));
            }
            const auto depth = (swap_mode & 2U) != 0U
                ? static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(std::floor(w * 4096.0F)), 0, 0x00ffffff))
                : static_cast<std::uint32_t>(std::floor(
                    std::clamp(value[1], 0.0F, 1.0F) * 4194303.0F)) << 2;
            write_fragment<Format, Capture, true>(polygon, shading, is_translucent_polygon, front_facing,
                           x, y, color, value[2] * w, value[3] * w, depth);
            if (width > 1) {
                for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i];
            }
        }
    }

    template<bool Capture = false, bool Batch = true>
    void draw_span(const Polygon& polygon, PolygonShading& shading,
                   bool is_translucent_polygon, bool front_facing,
                   const RasterEdge& left, const RasterEdge& right, int y_begin, int y_end) {
        if (left.y < 0 || left.y >= tgt_.height ||
            left.y < y_begin || left.y >= y_end) return;
        if (shading.mode == 0U && shading.alpha_attr == 31U) {
            if (!shading.textured)
                return draw_span_specialized<0, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
            switch ((polygon.texture >> 26U) & 7U) {
            case 2: return draw_span_specialized<2, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
            case 3: return draw_span_specialized<3, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
            case 4: return draw_span_specialized<4, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
            case 7: return draw_span_specialized<7, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
            default: break;
            }
        }
        draw_span_specialized<-1, Capture, Batch>(polygon, shading, is_translucent_polygon, front_facing, left, right);
    }

    bool prepare_raster_polygon(const Polygon& polygon,
                                const std::array<ClipVertex, 16>& clipped,
                                PreparedPolygon& prepared) const {
        const auto count = prepared.count;
        auto& verts = prepared.raster;
        if (count < 3U || count > verts.size()) return false;
        for (std::size_t i = 0; i < count; ++i) verts[i] = to_raster_vertex(clipped[i], polygon.viewport);
        const auto begin = verts.begin();
        const auto end = begin + static_cast<std::ptrdiff_t>(count);
        const std::size_t last = count - 1U;
        double facing = static_cast<double>(verts[0].y + verts[last].y) * (verts[0].x - verts[last].x) +
                        static_cast<double>(verts[1].y + verts[0].y) * (verts[1].x - verts[0].x) +
                        static_cast<double>(verts[2].y + verts[1].y) * (verts[2].x - verts[1].x);
        for (std::size_t j = 2; j < last; ++j)
            facing += static_cast<double>(verts[j + 1U].y + verts[j].y) * (verts[j + 1U].x - verts[j].x);
        const bool back_facing = facing < 0.0;
        if (((polygon.attr & (1U << (back_facing ? 6 : 7)))) == 0U) return false;
        prepared.front_facing = !back_facing;
        // The edge walker needs clockwise vertices starting at the topmost (then
        // leftmost) one: reverse front-facing polygons, then rotate.
        if (!back_facing) std::reverse(begin, end);
        for (std::size_t guard = 0; guard < count; ++guard) {
            bool above = false;
            for (std::size_t i = 1; i < count; ++i) above = above || verts[0].y > verts[i].y;
            if (!above) break;
            std::rotate(begin, begin + 1, end);
        }
        for (std::size_t guard = 0; guard < count &&
             verts[0].y == verts[1].y && verts[0].x > verts[1].x; ++guard)
            std::rotate(begin, begin + 1, end);
        return true;
    }

    template<bool Capture>
    void raster_polygon(const Polygon& polygon, PolygonShading& shading,
                        bool is_translucent_polygon, const PreparedPolygon& prepared,
                        int y_begin, int y_end) {
        const auto& verts = prepared.raster;
        const auto count = prepared.count;
        const bool back_facing = !prepared.front_facing;
        std::size_t left_vertex = count;
        std::size_t right_vertex = 0;
        RasterEdge left;
        RasterEdge right;
        bool step_left = true;
        bool step_right = true;
        for (std::size_t guard = 0; guard < 2U * count; ++guard) {
            if (step_left && !make_edge(verts[left_vertex == count ? 0 : left_vertex],
                                        verts[left_vertex - 1U], left)) return;
            if (step_right && !make_edge(verts[right_vertex], verts[right_vertex + 1U], right)) return;
            step_left = step_right = false;
            for (int rows = std::min(left.height, right.height); rows > 0; --rows) {
                // Walk all preceding rows by the original additions: multiplying
                // the DDA step by skipped rows changes floating-point rounding.
                if (left.y >= y_begin && left.y < y_end)
                    draw_span<Capture>(polygon, shading, is_translucent_polygon, !back_facing,
                              left, right, y_begin, y_end);
                step_edge(left);
                step_edge(right);
            }
            if (right.height == 0) {
                step_right = true;
                ++right_vertex;
            }
            if (left.height == 0) {
                step_left = true;
                --left_vertex;
            }
            if (left_vertex <= right_vertex + 1U) break;
        }
    }
    template<bool Capture>
    void raster_dot(const Polygon& polygon, PolygonShading& shading,
                    bool is_translucent_polygon, const ClipVertex& vertex,
                    int y_begin, int y_end) {
        const auto screen = to_screen(vertex, polygon.viewport);
        const auto x = static_cast<int>(std::floor(screen.x));
        const auto y = static_cast<int>(std::floor(screen.y));
        if (x < 0 || x >= tgt_.width || y < 0 || y >= tgt_.height || y < y_begin || y >= y_end) return;
        const auto polygon_id = static_cast<std::uint8_t>((polygon.attr >> 24) & 63U);
        const auto shaded = shade_pixel(polygon, shading, vertex.color, vertex.u, vertex.v);
        auto alpha = shaded.alpha;
        if (((polygon.attr >> 16) & 31U) == 0U) alpha = 31U;
        const auto depth = (swap_mode & 2U) != 0U
            ? static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::floor(vertex.clip[3] * 4096.0F)), 0, 0x00ffffff))
            : static_cast<std::uint32_t>(std::floor(
                std::clamp((screen.z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F)) << 2;
        const auto index = static_cast<std::size_t>(y) * tgt_.width + static_cast<std::size_t>(x);
        if constexpr (Capture) capture_fragment(polygon, shading, index, vertex.u, vertex.v);
        auto& destination = tgt_.fb[index];
        const auto depth_delta = depth > destination.depth
            ? depth - destination.depth : destination.depth - depth;
        if ((polygon.attr & (1U << 14)) != 0U
                ? depth_delta > kDepthEqualTolerance : depth >= destination.depth) {
            if (((polygon.attr >> 4) & 3U) == 3U && polygon_id == 0U)
                destination.stencil = true;
            return;
        }
        blend_pixel(destination, {shaded.r, shaded.g, shaded.b, alpha}, polygon_id,
                    is_translucent_polygon, (polygon.attr & (1U << 15)) != 0U,
                    depth, polygon.attr, true);
    }
    // Translucent: partial polygon alpha, or an A3I5/A5I3 texture unless decal/shadow mode.
    bool polygon_is_translucent(const Polygon& polygon) const {
        const auto alpha = (polygon.attr >> 16) & 31U;
        if (alpha != 0U && alpha != 31U) return true;
        const auto format = (polygon.texture >> 26) & 7U;
        const auto mode = (polygon.attr >> 4) & 3U;
        return (format == 1U || format == 6U) && mode != 1U && mode != 3U;
    }

    // {maximum, minimum} of the vertices' normalized screen y (0 = top) before clipping.
    std::array<float, 2> polygon_y_range(const Polygon& polygon) const {
        float low = 0.0F;
        float high = 0.0F;
        for (std::size_t i = 0; i < polygon.count; ++i) {
            const auto& vertex = render_vertices[polygon.index[i]];
            const float w = vertex.clip[3] != 0.0F ? vertex.clip[3] : 0.00000001F;
            const float y = 1.0F - (vertex.clip[1] + w) / (2.0F * w);
            low = i == 0 ? y : std::min(low, y);
            high = i == 0 ? y : std::max(high, y);
        }
        return {high, low};
    }

    void prepare_polygon(const Polygon& polygon, PreparedPolygon& prepared) const {
        prepared.shape = PolygonShape::Empty;
        prepared.y_begin = prepared.y_end = 0;
        std::array<ClipVertex, 16> clipped{};
        const auto count = clip_polygon(polygon, clipped);
        prepared.count = count;
        if (count < 3U) return;
        auto& screen = prepared.screen;
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = std::numeric_limits<float>::max();
        float max_y = std::numeric_limits<float>::lowest();
        for (std::size_t i = 0; i < count; ++i) {
            screen[i] = to_screen(clipped[i], polygon.viewport);
            min_x = std::min(min_x, screen[i].x);
            max_x = std::max(max_x, screen[i].x);
            min_y = std::min(min_y, screen[i].y);
            max_y = std::max(max_y, screen[i].y);
        }
        if (max_x - min_x < 1.0F && max_y - min_y < 1.0F) {
            if ((polygon.attr & (1U << 13)) != 0U) {
                const auto one_dot_limit = get32(0x04000610U) & 0x7fffU;
                bool visible = false;
                for (std::size_t i = 0; i < polygon.count; ++i) {
                    const auto& vertex = render_vertices[polygon.index[i]];
                    if (vertex.clip[3] * 8.0F <= static_cast<float>(one_dot_limit)) {
                        visible = true;
                        break;
                    }
                }
                if (!visible) return;
            }
            const auto& first = render_vertices[polygon.index[0]];
            prepared.dot = {first.clip, first.u, first.v, first.color};
            // The dot path uses the original first vertex, not a clipped one.
            const int y = static_cast<int>(std::floor(to_screen(prepared.dot, polygon.viewport).y));
            if (y < 0 || y >= tgt_.height) return;
            prepared.y_begin = y;
            prepared.y_end = y + 1;
            prepared.shape = PolygonShape::Dot;
            return;
        }
        if (((polygon.attr >> 16) & 31U) != 0U) {
            if (!prepare_raster_polygon(polygon, clipped, prepared)) return;
            int first_row = tgt_.height;
            int end_row = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const int row = ceil_fixed(prepared.raster[i].y);
                first_row = std::min(first_row, row);
                end_row = std::max(end_row, row);
            }
            prepared.y_begin = std::max(0, first_row);
            prepared.y_end = std::min(tgt_.height, end_row);
            prepared.shape = PolygonShape::Filled;
        } else {
            // Wire triangles have inclusive floor/ceil bounds, unlike the
            // filled polygon edge walk's half-open fixed-point row interval.
            prepared.y_begin = std::max(0, static_cast<int>(std::floor(min_y)));
            prepared.y_end = std::min(tgt_.height,
                                     static_cast<int>(std::ceil(max_y)) + 1);
            prepared.shape = PolygonShape::Wire;
        }
    }

    template<bool Capture>
    void draw_prepared_polygon(const Polygon& polygon, bool is_translucent_polygon,
                               const PreparedPolygon& prepared, PolygonShading& shading,
                               int y_begin, int y_end) {
        // Reject nonoverlapping bands before resetting the polygon texel cache.
        if (prepared.y_begin >= y_end || prepared.y_end <= y_begin) return;
        prepare_polygon_shading(polygon, shading);
        if (prepared.shape == PolygonShape::Dot) {
            raster_dot<Capture>(polygon, shading, is_translucent_polygon, prepared.dot, y_begin, y_end);
        } else if (prepared.shape == PolygonShape::Filled) {
            raster_polygon<Capture>(polygon, shading, is_translucent_polygon, prepared, y_begin, y_end);
        } else if (prepared.shape == PolygonShape::Wire) {
            for (std::size_t i = 1; i + 1U < prepared.count; ++i) {
                raster_triangle<Capture>(polygon, shading, is_translucent_polygon,
                                prepared.screen[0], prepared.screen[i], prepared.screen[i + 1U],
                                y_begin, y_end);
            }
        }
    }

    void dilate_opaque_only() {
        capture_data->polygon_totals.fill(0);
        for (const auto& band : capture_data->polygon_touches)
            for (std::size_t i = 0; i < render_polygon_count; ++i)
                capture_data->polygon_totals[i] += band[i];
        // Serial after the band barrier: read raw rows, write a separate result,
        // so diagonal neighbours and band borders dilate exactly once.
        for (int y = 0; y < tgt_.height; ++y) {
            for (int x = 0; x < tgt_.width; ++x) {
                std::uint8_t eligible = 1;
                for (int row = std::max(0, y - 1);
                     row <= std::min(tgt_.height - 1, y + 1); ++row)
                    for (int column = std::max(0, x - 1);
                         column <= std::min(tgt_.width - 1, x + 1); ++column)
                        eligible &= opaque_only[row * tgt_.width + column];
                opaque_only_dilated[y * tgt_.width + x] = eligible;
            }
        }
        opaque_only_valid = true;
    }

    void apply_edges() {
        if ((disp3d & (1U << 5)) == 0U) return;
        const auto clear_color = get32(0x04000350U);
        const auto clear_id = static_cast<std::uint8_t>((clear_color >> 24) & 63U);
        const auto clear_depth = depth15_to24(
            static_cast<std::uint16_t>(get32(0x04000354U) & 0x7fffU));
        const auto edge_alpha = static_cast<std::uint8_t>((disp3d & (1U << 4)) != 0U ? 16U : 31U);
        const auto edge_alpha_plus_one = static_cast<unsigned>(edge_alpha) + 1U;
        const auto inverse_alpha = 32U - edge_alpha_plus_one;
        for (std::size_t y = 0; y < static_cast<std::size_t>(tgt_.height); ++y) {
            for (std::size_t x = 0; x < static_cast<std::size_t>(tgt_.width); ++x) {
                const auto index = y * tgt_.width + x;
                auto& pixel = tgt_.fb[index];
                if (pixel.is_translucent_polygon) continue;
                const bool is_clear_edge = pixel.polygon_id != clear_id && pixel.depth < clear_depth;
                bool marked = false;
                std::uint8_t edge_id = pixel.polygon_id;
                const auto mark_from_neighbor = [&](std::size_t neighbor_index) {
                    const auto& neighbor = tgt_.fb[neighbor_index];
                    if (pixel.polygon_id == neighbor.polygon_id || pixel.depth < neighbor.depth)
                        return false;
                    edge_id = neighbor.polygon_id;
                    return true;
                };
                if (x + 1U == static_cast<std::size_t>(tgt_.width)) marked = is_clear_edge;
                else marked = mark_from_neighbor(index + 1U);
                if (!marked) {
                    if (y + 1U == static_cast<std::size_t>(tgt_.height)) marked = is_clear_edge;
                    else marked = mark_from_neighbor(index + tgt_.width);
                }
                if (!marked) {
                    if (x == 0U) marked = is_clear_edge;
                    else marked = mark_from_neighbor(index - 1U);
                }
                if (!marked) {
                    if (y == 0U) marked = is_clear_edge;
                    else marked = mark_from_neighbor(index - tgt_.width);
                }
                if (!marked) continue;
                const auto color = edge_color(edge_id);
                const std::array<std::uint8_t, 3> edge_rgb{
                    expand6(color5(color, 0)), expand6(color5(color, 5)), expand6(color5(color, 10))};
                if (edge_alpha == 31U || pixel.alpha == 0U) {
                    pixel.r = edge_rgb[0];
                    pixel.g = edge_rgb[1];
                    pixel.b = edge_rgb[2];
                } else {
                    pixel.r = static_cast<std::uint8_t>((edge_rgb[0] * edge_alpha_plus_one +
                        pixel.r * inverse_alpha) >> 5);
                    pixel.g = static_cast<std::uint8_t>((edge_rgb[1] * edge_alpha_plus_one +
                        pixel.g * inverse_alpha) >> 5);
                    pixel.b = static_cast<std::uint8_t>((edge_rgb[2] * edge_alpha_plus_one +
                        pixel.b * inverse_alpha) >> 5);
                }
                pixel.alpha = std::max(pixel.alpha, edge_alpha);
            }
        }
    }

    template<bool Capture>
    void pack_rows(int y_begin, int y_end) {
        const auto first = static_cast<std::size_t>(y_begin) * tgt_.width;
        const auto end = static_cast<std::size_t>(y_end) * tgt_.width;
        for (auto i = first; i < end; ++i) {
            auto& pixel = tgt_.fb[i];
            if (pixel.fog) apply_fog(pixel, pixel.depth);
            tgt_.out[i] = pack_pixel(pixel.r, pixel.g, pixel.b, pixel.alpha);
#ifdef DSR_GPU_CAPTURE_IDS
            if constexpr (Capture) capture_data->polygon_ids[i] = pixel.polygon_id;
#endif
        }
    }

    template<bool Capture>
    void render_frame() {
        prepare_frame_textures();
        std::array<std::array<float, 2>, kPolygonLimit> y_range{}; // {max, min} per polygon
        render_opaque_count = 0;
        render_translucent_count = 0;
        for (std::size_t i = 0; i < render_polygon_count; ++i) {
            const auto& polygon = render_polygons[i];
            y_range[i] = polygon_y_range(polygon);
            if (polygon_is_translucent(polygon))
                render_translucent_order[render_translucent_count++] = static_cast<std::uint16_t>(i);
            else
                render_opaque_order[render_opaque_count++] = static_cast<std::uint16_t>(i);
        }
        // Opaque polygons are always drawn in ascending screen-y order; translucent
        // ones too unless SWAP_BUFFERS selected manual sorting. Ties keep submission order.
        const auto by_y = [&y_range](std::uint16_t left, std::uint16_t right) {
            if (y_range[left][0] != y_range[right][0]) return y_range[left][0] < y_range[right][0];
            if (y_range[left][1] != y_range[right][1]) return y_range[left][1] < y_range[right][1];
            return left < right;
        };
        std::sort(render_opaque_order.begin(),
                  render_opaque_order.begin() + static_cast<std::ptrdiff_t>(render_opaque_count), by_y);
        if ((swap_mode & 1U) == 0U)
            std::sort(render_translucent_order.begin(),
                      render_translucent_order.begin() + static_cast<std::ptrdiff_t>(render_translucent_count), by_y);
        render_target<Capture>();
        if (output_scale > 1) {
            // Sorting, texture-cache preparation and optional capture happen
            // exactly once above. This presentation pass only reads geometry,
            // IO and the pinned decoded textures; it cannot advance GX state.
            const auto native = tgt_;
            struct RestoreTarget {
                Target& target;
                Target saved;
                ~RestoreTarget() { target = saved; }
            } restore{tgt_, native};
            const int width = static_cast<int>(kScreenWidth) * output_scale;
            const int height = static_cast<int>(kScreenHeight) * output_scale;
            tgt_ = {hires_fb.data(), hires_out.data(), output_scale, width, height, height / kRenderBands};
            rendered_scale = 1;
            render_target<false, true>();
            rendered_scale = output_scale;
        }
    }

    template<bool Capture, bool Scaled = false>
    void render_target() {
        if (pool->worker_count() <= 1) {
            // Inline means one full-frame pass, not eight serialized bands.
            // No pool submission, worker, or band preparation storage is needed.
            clear_framebuffer<Capture, Scaled>(0, tgt_.height);
            PreparedPolygon prepared;
            auto& shading = band_shading[0];
            const auto draw = [this, &prepared, &shading](std::uint16_t index, bool translucent) {
                const auto& polygon = render_polygons[index];
                prepare_polygon(polygon, prepared);
                draw_prepared_polygon<Capture>(polygon, translucent, prepared, shading,
                                      0, tgt_.height);
            };
            for (std::size_t i = 0; i < render_opaque_count; ++i) draw(render_opaque_order[i], false);
            for (std::size_t i = 0; i < render_translucent_count; ++i) draw(render_translucent_order[i], true);
            if constexpr (Capture) dilate_opaque_only();
            apply_edges();
            pack_rows<Capture>(0, tgt_.height);
            return;
        }
        if (render_polygon_count != 0U && !band_polygons)
            band_polygons = std::make_unique<std::array<PreparedPolygon, kPolygonLimit>>();
        // Clip, project, cull and order vertices once. The prepared row bounds
        // are from the actual raster path, not the preclip sorting y range.
        // prepare_polygon only reads frozen geometry/IO and writes its own PreparedPolygon, so
        // large polygon lists are prepared in contiguous chunks on the pool (the serial loop used
        // to sit on the critical path before the band pass). Small lists skip the extra barrier.
        constexpr std::size_t kParallelPrepareMin = 48;
        if (render_polygon_count >= kParallelPrepareMin) {
            pool->run(kRenderBands, [this](int band) {
                const std::size_t first = render_polygon_count * static_cast<std::size_t>(band) / kRenderBands;
                const std::size_t last = render_polygon_count * static_cast<std::size_t>(band + 1) / kRenderBands;
                for (std::size_t i = first; i < last; ++i)
                    prepare_polygon(render_polygons[i], (*band_polygons)[i]);
            });
        } else {
            for (std::size_t i = 0; i < render_polygon_count; ++i)
                prepare_polygon(render_polygons[i], (*band_polygons)[i]);
        }
        pool->run(kRenderBands, [this](int band) {
            const int y_begin = band * tgt_.band_height;
            const int y_end = y_begin + tgt_.band_height;
            clear_framebuffer<Capture, Scaled>(y_begin, y_end);
            auto& shading = band_shading[static_cast<std::size_t>(band)];
            for (std::size_t i = 0; i < render_opaque_count; ++i) {
                const auto index = render_opaque_order[i];
                draw_prepared_polygon<Capture>(render_polygons[index], false, (*band_polygons)[index],
                                      shading, y_begin, y_end);
            }
            for (std::size_t i = 0; i < render_translucent_count; ++i) {
                const auto index = render_translucent_order[i];
                draw_prepared_polygon<Capture>(render_polygons[index], true, (*band_polygons)[index],
                                      shading, y_begin, y_end);
            }
        });
        // Edge marking reads neighbouring rows, including across band boundaries.
        // Both blocking runs are barriers; IO, VRAM and geometry remain frozen.
        if constexpr (Capture) dilate_opaque_only();
        apply_edges();
        pool->run(kRenderBands, [this](int band) {
            pack_rows<Capture>(band * tgt_.band_height, (band + 1) * tgt_.band_height);
        });
    }

    void vblank_start() {
        if (swap_pending) {
            // The pending parameters select the mode of the following
            // geometry frame, not the list just completed before the swap.
            swap_mode = geometry_swap_mode;
            geometry_swap_mode = pending_swap_mode;
            render_vertex_count = vertex_count;
            render_polygon_count = polygon_count;
            render_vertex_overflow = vertex_overflow;
            render_polygon_overflow = polygon_overflow;
            ++render_generation;
            std::copy_n(vertices.begin(), render_vertex_count, render_vertices.begin());
            std::copy_n(polygons.begin(), render_polygon_count, render_polygons.begin());
            vertex_count = 0;
            polygon_count = 0;
            vertex_overflow = false;
            polygon_overflow = false;
            swap_pending = false;
        }
        // Geometry swap/flush is separate from rasterization: the guest can
        // upload textures during the 3D VBlank interval before render_start().
    }
};

std::array<std::int32_t, 3>
gpu3d_detail::Gpu3DTestAccess::light_half_vector(Gpu3D& gpu, unsigned index) {
    return gpu.impl_->light_half_vector[index];
}

std::array<std::int64_t, 2>
gpu3d_detail::Gpu3DTestAccess::transformed_texcoord(Gpu3D& gpu) {
    const auto mode = (gpu.impl_->texture_param >> 30) & 3U;
    const auto coordinates = mode == 3U ? gpu.impl_->texcoord_transform()
        : std::array<std::int64_t, 4>{
            gpu.impl_->transformed_texcoord_s, gpu.impl_->transformed_texcoord_t, 0, kOne};
    return {coordinates[0], coordinates[1]};
}

std::uint8_t gpu3d_detail::Gpu3DTestAccess::rounded_color(float value, bool six_bit) {
    return six_bit ? Gpu3D::Impl::clamp6(value) : Gpu3D::Impl::clamp5(value);
}

std::array<gpu3d_detail::Texel, 64> gpu3d_detail::Gpu3DTestAccess::sample_texels(
    Gpu3D& gpu, const Gpu3DPolygon& polygon,
    const std::array<std::array<float, 2>, 64>& coordinates) {
    if (gpu.impl_->texture_source.generation != nullptr) {
        gpu.impl_->decoded_textures.begin_frame(
            gpu.impl_->texture_source.generation(gpu.impl_->texture_source.context));
        gpu.impl_->decoded_textures.acquire(gpu.impl_->texture_source,
                                            polygon.texture, polygon.palette);
    }
    Gpu3D::Impl::PolygonShading shading;
    gpu.impl_->prepare_polygon_shading(polygon, shading);
    std::array<Texel, 64> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto u = coordinates[i][0], v = coordinates[i][1];
        switch ((polygon.texture >> 26U) & 7U) {
        case 2: result[i] = gpu.impl_->sample_texture<2>(polygon, u, v, shading); break;
        case 3: result[i] = gpu.impl_->sample_texture<3>(polygon, u, v, shading); break;
        case 4: result[i] = gpu.impl_->sample_texture<4>(polygon, u, v, shading); break;
        case 7: result[i] = gpu.impl_->sample_texture<7>(polygon, u, v, shading); break;
        default: result[i] = gpu.impl_->sample_texture(polygon, u, v, shading); break;
        }
    }
    return result;
}

std::array<std::uint32_t, 256> gpu3d_detail::Gpu3DTestAccess::draw_span(
    Gpu3D& gpu, const Gpu3DPolygon& polygon, int x_left, int x_right,
    const std::array<float, 7>& left, const std::array<float, 7>& right) {
    gpu.impl_->clear_framebuffer(0, static_cast<int>(kScreenHeight));
    Gpu3D::Impl::PolygonShading shading;
    gpu.impl_->prepare_polygon_shading(polygon, shading);
    Gpu3D::Impl::RasterEdge a{}, b{};
    a.x = x_left; b.x = x_right;
    a.value = left; b.value = right;
    gpu.impl_->draw_span(polygon, shading, false, true, a, b, 0, static_cast<int>(kScreenHeight));
    std::array<std::uint32_t, 256> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& pixel = gpu.impl_->framebuffer[i];
        result[i] = pack_pixel(pixel.r, pixel.g, pixel.b, pixel.alpha);
    }
    return result;
}

std::array<std::uint32_t, 4> gpu3d_detail::Gpu3DTestAccess::clear_pixel(Gpu3D& gpu, unsigned index) {
    gpu.impl_->clear_framebuffer(0, static_cast<int>(kScreenHeight));
    return framebuffer_pixel(gpu, index);
}

std::array<std::uint32_t, 4> gpu3d_detail::Gpu3DTestAccess::framebuffer_pixel(
    const Gpu3D& gpu, unsigned index) {
    const auto& pixel = gpu.impl_->framebuffer[index];
    return {pack_pixel(pixel.r, pixel.g, pixel.b, pixel.alpha), pixel.depth,
        pixel.polygon_id | (static_cast<std::uint32_t>(pixel.translucent_polygon_id) << 8U),
        static_cast<std::uint32_t>(pixel.fog) | (static_cast<std::uint32_t>(pixel.stencil) << 1U) |
        (static_cast<std::uint32_t>(pixel.is_translucent_polygon) << 2U) |
        (static_cast<std::uint32_t>(pixel.facing) << 3U)};
}

std::array<std::uint8_t, 4> gpu3d_detail::Gpu3DTestAccess::shade_pixel(
    Gpu3D& gpu, const Gpu3DPolygon& polygon, const std::array<float, 3>& color, float u, float v) {
    Gpu3D::Impl::PolygonShading shading;
    gpu.impl_->prepare_polygon_shading(polygon, shading);
    const auto pixel = gpu.impl_->shade_pixel(polygon, shading, color, u, v);
    return {pixel.r, pixel.g, pixel.b, pixel.alpha};
}

gpu3d_detail::Gpu3DTestAccess::SpanState gpu3d_detail::Gpu3DTestAccess::draw_span_state(
    Gpu3D& gpu, const Gpu3DPolygon& polygon, int x_left, int x_right,
    const std::array<float, 7>& left, const std::array<float, 7>& right,
    const SpanState& initial, bool front_facing, bool translucent_polygon, bool w_depth, bool batch) {
    for (std::size_t i = 0; i < initial.size(); ++i) {
        auto& pixel = gpu.impl_->framebuffer[i];
        const auto& state = initial[i];
        pixel.r = state[0] & 63U;
        pixel.g = (state[0] >> 6U) & 63U;
        pixel.b = (state[0] >> 12U) & 63U;
        pixel.alpha = (state[0] >> 24U) & 31U;
        pixel.depth = state[1];
        pixel.polygon_id = state[2] & 63U;
        pixel.translucent_polygon_id = (state[2] >> 8U) & 255U;
        pixel.fog = (state[3] & 1U) != 0U;
        pixel.stencil = (state[3] & 2U) != 0U;
        pixel.is_translucent_polygon = (state[3] & 4U) != 0U;
        pixel.facing = (state[3] >> 3U) & 3U;
    }
    gpu.impl_->swap_mode = w_depth ? 2U : 0U;
    Gpu3D::Impl::PolygonShading shading;
    gpu.impl_->prepare_polygon_shading(polygon, shading);
    Gpu3D::Impl::RasterEdge a{}, b{};
    a.x = x_left; b.x = x_right;
    a.value = left; b.value = right;
    if (batch) gpu.impl_->draw_span<false, true>(
        polygon, shading, translucent_polygon, front_facing, a, b, 0, static_cast<int>(kScreenHeight));
    else gpu.impl_->draw_span<false, false>(
        polygon, shading, translucent_polygon, front_facing, a, b, 0, static_cast<int>(kScreenHeight));
    SpanState result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = framebuffer_pixel(gpu, i);
    return result;
}

Gpu3D::Gpu3D() : impl_(new Impl()) {}
Gpu3D::~Gpu3D() {
    shutdown_render_workers();
    delete impl_;
}

void Gpu3D::reset() { impl_->reset(); }
void Gpu3D::set_render_threads(int threads) {
    // Configuration is machine-thread-only, between completed render calls.
    // Join the old workers before installing the new configuration.
    impl_->pool.reset();
    impl_->pool = std::make_unique<BandPool>(threads);
}
void Gpu3D::shutdown_render_workers() {
    if (impl_->pool) impl_->pool->shutdown();
}
void Gpu3D::set_capture_opaque_only(bool enabled) {
    if (enabled && !impl_->capture_data) impl_->capture_data = std::make_unique<Impl::CaptureData>();
    if (impl_->capture_opaque_only == enabled) return;
    impl_->capture_opaque_only = enabled;
    impl_->opaque_only_valid = false;
}
BandPool& Gpu3D::render_pool() { return *impl_->pool; }
void Gpu3D::set_texture_source(const void* context,
                               const std::uint8_t* (*texture_lookup)(const void*, int),
                               const std::uint8_t* (*palette_lookup)(const void*, int),
                               std::uint8_t (*texture_read)(const void*, std::uint32_t),
                               std::uint8_t (*palette_read)(const void*, std::uint32_t),
                               std::uint64_t (*generation)(const void*)) {
    impl_->decoded_textures.clear();
    impl_->texture_source = {context, texture_lookup, palette_lookup, texture_read, palette_read,
                            generation};
}


std::uint32_t Gpu3D::io_read(std::uint32_t addr, int width) { return impl_->io_read(addr, width); }

void Gpu3D::io_write(std::uint32_t addr, std::uint32_t value, int width) {
    impl_->write_io(addr, value, width, on_fifo_state_change);
}

void Gpu3D::run(std::int64_t ticks) { impl_->run(ticks, on_fifo_state_change); }

bool Gpu3D::fifo_below_half() const { return impl_->below_half(); }

bool Gpu3D::irq_condition() const { return impl_->irq_condition(); }

void Gpu3D::vblank_start() { impl_->vblank_start(); }
void Gpu3D::render_start() {
    if (impl_->capture_opaque_only) impl_->render_frame<true>();
    else impl_->render_frame<false>();
    if (on_render_complete) on_render_complete();
}

const std::uint32_t* Gpu3D::line(int y) const {
    if (y < 0 || y >= static_cast<int>(kScreenHeight)) return nullptr;
    return impl_->output.data() + static_cast<std::size_t>(y) * kScreenWidth;
}

void Gpu3D::set_output_scale(int n) {
    n = std::clamp(n, 1, 4);
    if (n == impl_->output_scale) return;
    if (n > 1) {
        const auto pixels = kScreenPixels * static_cast<std::size_t>(n * n);
        impl_->hires_fb.resize(pixels);
        impl_->hires_out.resize(pixels);
    }
    impl_->output_scale = n;
    impl_->rendered_scale = 1;
}

int Gpu3D::output_scale() const { return impl_->output_scale; }

const std::uint32_t* Gpu3D::scaled_line(int y) const {
    const int scale = impl_->output_scale;
    if (scale == 1 || impl_->rendered_scale != scale ||
        y < 0 || y >= static_cast<int>(kScreenHeight) * scale) return nullptr;
    return impl_->hires_out.data() + static_cast<std::size_t>(y) * kScreenWidth * scale;
}

Gpu3DGeometryView Gpu3D::geometry_view() const {
    return {
        impl_->render_vertices.data(),
        impl_->render_vertex_count,
        impl_->render_polygons.data(),
        impl_->render_polygon_count,
        impl_->render_generation,
        impl_->swap_mode,
        impl_->render_vertex_overflow,
        impl_->render_polygon_overflow,
    };
}

Gpu3DDrawOrderView Gpu3D::draw_order_view() const {
    return {impl_->render_opaque_order.data(), impl_->render_opaque_count,
            impl_->render_translucent_order.data(), impl_->render_translucent_count};
}

Gpu3DRasterView Gpu3D::raster_view() const {
    return {
        impl_->disp3d,
        impl_->get32(0x04000580U),
        impl_->get32(0x04000340U) & 31U,
        impl_->get32(0x04000350U),
        impl_->get32(0x04000354U),
        impl_->get32(0x04000358U),
        impl_->get32(0x0400035cU),
        impl_->io.data() + (0x04000380U - kIoBase),
        64U,
        impl_->io.data() + (0x04000330U - kIoBase),
        16U,
        impl_->io.data() + (0x04000360U - kIoBase),
        impl_->output.data(),
        impl_->output.size(),
        impl_->opaque_only_valid ? impl_->opaque_only_dilated.data() : nullptr,
        impl_->opaque_only_valid ? impl_->capture_data->causes.data() : nullptr,
        impl_->opaque_only_valid ? impl_->capture_data->polygon_totals.data() : nullptr,
#ifdef DSR_GPU_CAPTURE_IDS
        impl_->opaque_only_valid ? impl_->capture_data->polygon_ids.data() : nullptr,
#else
        nullptr,
#endif
    };
}

Gpu3DTextureView Gpu3D::texture_view(const Gpu2D& gpu2d) const {
    Gpu3DTextureView view{};
    const auto mapping = gpu2d.frame_view();
    view.mapping_generation = mapping.mapping_generation;
    for (std::size_t i = 0; i < view.textures.size(); ++i) {
        const auto* bytes = gpu2d.texture_slot(static_cast<int>(i));
        if (!bytes) continue;
        auto& slot = view.textures[i];
        slot.mapped = true;
        slot.bytes = bytes;
        slot.physical_bank = mapping.texture_slots[i].physical_bank;
        slot.bank_offset = mapping.texture_slots[i].bank_offset;
        slot.byte_count = 0x20000U;
    }
    for (std::size_t i = 0; i < view.palettes.size(); ++i) {
        const auto* bytes = gpu2d.texpal_slot(static_cast<int>(i));
        if (!bytes) continue;
        auto& slot = view.palettes[i];
        slot.mapped = true;
        slot.bytes = bytes;
        slot.physical_bank = mapping.texture_palette_slots[i].physical_bank;
        slot.bank_offset = mapping.texture_palette_slots[i].bank_offset;
        slot.byte_count = 0x4000U;
    }
    return view;
}
} // namespace dsr
