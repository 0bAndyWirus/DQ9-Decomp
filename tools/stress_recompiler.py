#!/usr/bin/env python3
"""Execute seeded synthetic ARMv5TE/Thumb-1 code natively and in Unicorn.

Run from the repository root with .venv/bin/python. No ROM or assembler needed.
The default corpus combines 384 random arithmetic and 32 random memory sequences with focused
instruction forms, each tested with 64 edge/random register and NZCVQ states.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tests.recomp_tests.sequence_generator import SEQUENCE_SEED, make_specs
from tests.recomp_tests.test_differential import RecompilerDifferentialTests, _IMPORT_ERROR


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=SEQUENCE_SEED)
    parser.add_argument("--sequences", type=int, default=192, help="random sequences per instruction set")
    parser.add_argument("--states", type=int, default=64, help="initial states per specimen (minimum 64)")
    args = parser.parse_args(argv)
    if not 1 <= args.sequences <= 256 or args.states < 64:
        parser.error("--sequences must be 1..256 (synthetic code page limit); --states must be >=64")
    if _IMPORT_ERROR is not None:
        parser.exit(2, f"differential dependencies unavailable: {_IMPORT_ERROR}\n")
    specs = make_specs(args.seed, args.sequences)
    print(f"sequence seed=0x{args.seed:x}; stress specimens={len(specs)}; "
          f"states/specimen={args.states}", flush=True)
    RecompilerDifferentialTests.sequence_seed = args.seed
    RecompilerDifferentialTests.sequence_count = args.sequences
    RecompilerDifferentialTests.states_per_function = args.states
    RecompilerDifferentialTests.include_rom_forms = False
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RecompilerDifferentialTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
