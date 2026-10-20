"""Cycle-accounting checks using invented, ROM-free ARM/Thumb programs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from recomp.decoder import Decoder
from recomp.emitter import Block, FunctionEmitter, emit_manifest


SYNTHETIC_BASE = 0x08000000
TIMED_SWIS = (
    0x03, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18,
)


class CostModelTests(unittest.TestCase):
    def cost(self, cpu, encoded, thumb=False):
        image = bytes.fromhex(encoded)
        decoder = Decoder(image, SYNTHETIC_BASE,
                          "v5te" if cpu == "arm9" else "v4t", thumb)
        instructions = []
        address = SYNTHETIC_BASE
        while address < SYNTHETIC_BASE + len(image):
            instruction = decoder.decode(address)
            self.assertIsNotNone(instruction)
            instructions.append(instruction)
            address = instruction.next_address
        emitter = object.__new__(FunctionEmitter)
        emitter.cpu = cpu
        return emitter._cost(Block(SYNTHETIC_BASE, instructions))

    def test_arm9_counts_fetches_without_memory_surcharge(self):
        # MOV, LDR, BNE: even an untaken predicate incurs a fetch cost.
        self.assertEqual(self.cost("arm9", "0000a0e1 000091e5 feffff1a"),
                         (3 * 161, 6))
        # PUSH/POP and load/store multiple retain the same per-fetch average.
        self.assertEqual(self.cost("arm9", "03002de9 0300bde8"), (2 * 161, 6))
        self.assertEqual(self.cost("arm9", ""), (161, 6))

    def test_thumb_long_calls_count_both_fetched_halves(self):
        for encoded in ("00f0 00f8", "00f0 00e8"):  # BL and BLX.
            with self.subTest(encoded=encoded):
                self.assertEqual(self.cost("arm9", encoded, thumb=True), (2 * 161, 6))
        self.assertEqual(self.cost("arm9", "0020 7047", thumb=True), (2 * 161, 6))

    def test_arm7_retains_integer_instruction_and_memory_cost(self):
        self.assertEqual(self.cost("arm7", "000091e5 1eff2fe1"), (3, 0))
        self.assertEqual(self.cost("arm7", "000081e5 1eff2fe1"), (3, 0))
        self.assertEqual(self.cost("arm7", "0100b0e8 1eff2fe1"), (3, 0))
        self.assertEqual(self.cost("arm7", "0020 7047", thumb=True), (2, 0))
        self.assertEqual(self.cost("arm7", ""), (1, 0))

    def test_data_and_delay_swis_charge_only_one_base_cycle(self):
        for cpu in ("arm9", "arm7"):
            for number in TIMED_SWIS:
                with self.subTest(cpu=cpu, number=number):
                    arm = (0xEF000000 | (number << 16)).to_bytes(4, "little").hex()
                    thumb = (0xDF00 | number).to_bytes(2, "little").hex()
                    expected = (64, 6) if cpu == "arm9" else (1, 0)
                    self.assertEqual(self.cost(cpu, arm), expected)
                    self.assertEqual(self.cost(cpu, thumb, thumb=True), expected)
        # A conditional SWI that is not executed still charges its base fetch.
        self.assertEqual(self.cost("arm9", "0000091f"), (64, 6))
        # Adding an ordinary instruction does not discard fractional cycles.
        self.assertEqual(self.cost("arm9", "000009ef 1eff2fe1"), (64 + 161, 6))

    def test_other_control_and_unsupported_swis_keep_average(self):
        for number in (0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x17, 0x1A, 0xFF):
            with self.subTest(number=number):
                arm = (0xEF000000 | (number << 16)).to_bytes(4, "little").hex()
                thumb = (0xDF00 | number).to_bytes(2, "little").hex()
                self.assertEqual(self.cost("arm9", arm), (161, 6))
                self.assertEqual(self.cost("arm9", thumb, thumb=True), (161, 6))

    def test_compiled_blocks_carry_fraction_debt_and_failed_predicates(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is required for generated cycle-budget execution")
        with tempfile.TemporaryDirectory(prefix="recomp-cycles-") as temporary:
            root = Path(temporary)
            image = root / "guest.bin"
            # MOV; BX LR, then SWINE Div; BX LR. All bytes are hand assembled.
            image.write_bytes(bytes.fromhex("0000a0e1 1eff2fe1 0000091f 1eff2fe1"))
            generated = root / "generated"
            emit_manifest({
                "cpu": "arm9", "arch": "v5te",
                "modules": [{
                    "name": "main", "module_id": -1, "base": SYNTHETIC_BASE,
                    "image": str(image.resolve()),
                    "functions": [
                        {"addr": SYNTHETIC_BASE, "size": 8, "thumb": False,
                         "name": "cycles"},
                        {"addr": SYNTHETIC_BASE + 8, "size": 8, "thumb": False,
                         "name": "failed_predicate"},
                    ],
                    "labels": [], "data_ranges": [], "calls": [],
                }],
            }, generated)
            runner = root / "runner.cpp"
            runner.write_text(r'''
#include "decls.h"
#include <cstdlib>
namespace dsr {
void dsr_checkpoint(Cpu*, std::uint32_t) { std::abort(); }
void dsr_swi(Cpu*, std::uint32_t, std::uint32_t, bool) { std::abort(); }
}
int main() {
    dsr::Cpu cpu{};
    // Check every incoming fractional state across repeated generated calls.
    for (unsigned initial = 0; initial < 64; ++initial) {
        cpu.budget = 1000;
        cpu.cycle_fraction = initial;
        for (unsigned calls = 1; calls <= 32; ++calls) {
            (*dsr::p_arm9_main_08000000)(&cpu);
            const unsigned numerator = initial + calls * 2 * 161;
            if (cpu.budget != 1000 - static_cast<std::int64_t>(numerator / 64) ||
                cpu.cycle_fraction != numerator % 64) return 1;
        }
    }
    // 64 fetched instructions total exactly 161 cycles, not 32 rounded blocks.
    if (cpu.budget != 839 || cpu.cycle_fraction != 63) return 2;
    cpu.cycle_fraction = 63;
    cpu.budget = 1;
    (*dsr::p_arm9_main_08000000)(&cpu);
    if (cpu.budget != -5 || cpu.cycle_fraction != 1) return 3;
    // SWINE is not taken when Z=1. Its base cycle is charged, but no HLE runs.
    cpu.z = 1;
    cpu.r[0] = 123;
    cpu.budget = 100;
    cpu.cycle_fraction = 0;
    (*dsr::p_arm9_main_08000008)(&cpu);
    if (cpu.budget != 97 || cpu.cycle_fraction != 33 || cpu.r[0] != 123) return 4;
    return 0;
}
''', encoding="utf-8")
            executable = root / "runner"
            repository = Path(__file__).resolve().parents[2]
            subprocess.run([
                compiler, "-std=c++20", "-O2", "-I", str(generated),
                "-I", str(repository / "runtime/include"), str(runner),
                str(generated / "chunk_000.cpp"), "-o", str(executable),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)
