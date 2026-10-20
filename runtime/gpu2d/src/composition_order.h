#pragma once

#include <array>
#include <cstdint>

namespace dsr::gpu2d_detail {

struct CompositionOrder {
    std::array<std::uint8_t, 5> layers{};
    unsigned count = 0;
};

// Back to front, with OBJ above backgrounds of equal priority. A priority of
// four omits OBJ. Registers stay frozen for the whole scanline.
inline CompositionOrder composition_order(std::uint32_t enabled,
        const std::array<std::uint8_t, 4>& priorities, unsigned object_priority) {
    CompositionOrder order;
    for (int priority = 3; priority >= 0; --priority) {
        for (int bg = 3; bg >= 0; --bg) {
            if ((enabled & (1U << bg)) != 0U && priorities[bg] == priority) {
                order.layers[order.count++] = static_cast<std::uint8_t>(bg);
            }
        }
        if (object_priority == static_cast<unsigned>(priority)) {
            order.layers[order.count++] = 4U;
        }
    }
    return order;
}

} // namespace dsr::gpu2d_detail
