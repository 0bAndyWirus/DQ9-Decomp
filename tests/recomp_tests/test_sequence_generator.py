"""Structural checks keep the synthetic corpus complete and reproducible."""
import unittest

from tests.recomp_tests import test_differential as h
from tests.recomp_tests.sequence_generator import make_specs


@unittest.skipIf(h._IMPORT_ERROR is not None, str(h._IMPORT_ERROR))
class SequenceGeneratorTests(unittest.TestCase):
    def test_seed_is_reproducible_and_changes_random_sequences(self):
        first = make_specs(123, 8)
        self.assertEqual(first, make_specs(123, 8))
        other = make_specs(124, 8)
        random_first = [s.instruction for s in first if "_random_" in s.name]
        random_other = [s.instruction for s in other if "_random_" in s.name]
        self.assertEqual(len(random_first), 16)
        self.assertNotEqual(random_first, random_other)
        self.assertEqual(len({s.name for s in first}), len(first))

    def test_required_instruction_families_decode(self):
        names = set()
        conditions = set()
        for spec in h._make_specs() + make_specs():
            decoder = h.Decoder(spec.code(), 0, "v5te", spec.thumb)
            cursor = 0
            # Include all single instructions; stop at the first control-flow
            # transfer so embedded data in existing fixtures is not decoded.
            while cursor < len(spec.code()):
                insn = decoder.decode(cursor)
                if insn is None:
                    break
                name = insn.mnemonic.rstrip("s") if insn.update_flags else insn.mnemonic
                names.add(name)
                conditions.add(insn.condition)
                cursor += insn.size
                if name in {"b", "bl", "blx", "bx"}:
                    break
        self.assertTrue({"mul", "mla", "umull", "smlal", "smulbb", "smulbt",
                         "smultb", "smultt", "smlabb", "smlabt", "smlatb", "smlatt",
                         "smlawb", "smlawt", "smulwb", "smulwt", "qadd", "qsub",
                         "qdadd", "qdsub", "clz", "ldrd", "strd", "swp", "swpb",
                         "adr", "push", "pop", "bl", "blx", "bx"} <= names,
                        sorted(names))
        self.assertTrue(set(range(15)) <= conditions)

    def test_default_synthetic_corpus_fits_code_page(self):
        specs = h._make_specs()[:-2] + make_specs()
        # Include worst-case alignment before every function.
        self.assertLess(sum(len(s.code()) + 3 for s in specs), h.PAGE_SIZE - 24)
