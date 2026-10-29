// ROM-backed integration test: this links the generated guest builder, rather
// than a host-side model of its fixed-point arithmetic. The input image is
// private and is never copied into the source tree.
#include "dsr/cpu.h"
#include "dsr/machine.h"
#include "dsr/rt.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
constexpr std::uint32_t kBase = 0x02000000;
constexpr std::uint32_t kBuilder = 0x020c28b0;
constexpr std::uint32_t kEntry = 0x023d0000;
constexpr std::uint32_t kStack = 0x023e0000;
constexpr std::uint32_t kMatrix = 0x023f0000;
bool completed = false;

void invoke_builder(dsr::Cpu* cpu) {
    cpu->r[0] = 2048; // sin(fov/2), Q12
    cpu->r[1] = 3547; // cos(fov/2), Q12
    cpu->r[2] = 0x1555; // 4:3 aspect, Q12
    cpu->r[3] = 0x1000; // near
    cpu->r[13] = kStack;
    cpu->m->write32(dsr::CpuId::Arm9, kStack, 0x64000); // far
    cpu->m->write32(dsr::CpuId::Arm9, kStack + 4, 0x1000); // scale
    cpu->m->write32(dsr::CpuId::Arm9, kStack + 8, kMatrix);
    dsr::dsr_call(cpu, kBuilder);
    completed = true;
}

bool run(const std::vector<std::uint8_t>& image, bool full,
         std::array<std::int32_t, 16>& matrix) {
    dsr::GuestFn wrapper = invoke_builder;
    const auto& generated = dsr::gen_table_arm9();
    std::vector<dsr::FnEntry> entries(generated.entries, generated.entries + generated.count);
    entries.push_back({kEntry, 4, dsr::kModuleArm9Main, 0, 0, &wrapper, "projection_test_entry"});
    dsr::MachineConfig config;
    config.direct_boot = false;
    config.arm9_table = {entries.data(), entries.size()};
    config.widescreen_camera = full;
    dsr::Machine machine(config);
    for (std::size_t i = 0; i < image.size(); ++i)
        machine.write8(dsr::CpuId::Arm9, kBase + static_cast<std::uint32_t>(i), image[i]);
    completed = false;
    machine.start_cpu(dsr::CpuId::Arm9, kEntry);
    machine.run_ticks(100000);
    if (!completed) {
        std::fprintf(stderr, "guest projection builder did not return\n");
        return false;
    }
    for (std::size_t i = 0; i < matrix.size(); ++i)
        matrix[i] = static_cast<std::int32_t>(machine.read32(dsr::CpuId::Arm9, kMatrix + 4 * i));
    return true;
}
} // namespace

int main() {
    const char* path = std::getenv("DQ9_TEST_ARM9_IMAGE");
    if (!path || !*path) {
        std::fprintf(stderr, "SKIP: set DQ9_TEST_ARM9_IMAGE to the decompressed EU ARM9 image\n");
        return 77;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open DQ9_TEST_ARM9_IMAGE: %s\n", path);
        return 1;
    }
    const std::vector<std::uint8_t> image{std::istreambuf_iterator<char>(input), {}};
    const auto& table = dsr::gen_table_arm9();
    const dsr::FnEntry* builder = nullptr;
    for (std::size_t i = 0; i < table.count; ++i)
        if (table.entries[i].addr == kBuilder && table.entries[i].module == dsr::kModuleArm9Main)
            builder = &table.entries[i];
    if (!builder || image.size() > kEntry - kBase ||
        image.size() < kBuilder - kBase + builder->size ||
        dsr::fnv1a64(image.data() + (kBuilder - kBase), builder->size) != builder->hash) {
        std::fprintf(stderr, "ARM9 image does not match generated perspective builder\n");
        return 1;
    }
    std::array<std::int32_t, 16> picture{}, full{};
    if (!run(image, false, picture) || !run(image, true, full)) return 1;
    // Picture multiplies clip X by 3/4; Full widens the guest projection and
    // leaves clip X alone. Allow four Q12 units for the two integer divisions.
    const std::int64_t delta = 3LL * picture[0] - 4LL * full[0];
    if (picture[0] <= 0 || full[0] <= 0 || picture[5] <= 0 ||
        delta < -16 || delta > 16) {
        std::fprintf(stderr, "horizontal FOV differs: picture m00=%d full m00=%d\n", picture[0], full[0]);
        return 1;
    }
    for (std::size_t i = 1; i < picture.size(); ++i) {
        if (picture[i] != full[i]) {
            std::fprintf(stderr, "projection element %zu changed: %d versus %d\n", i, picture[i], full[i]);
            return 1;
        }
    }
    return 0;
}
