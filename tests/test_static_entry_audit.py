from pathlib import Path
import json
import shutil
import struct
import subprocess
import tempfile
import unittest

from recomp.emitter import _prepare_entries, emit_manifest
from recomp.dsd_manifest import _module as dsd_module
from tools.arm7_analyze import ModuleState, _manifest as arm7_manifest
from tools.static_entry_audit import (
    apply_entries, audit_calls, audit_pointers, generated_entries, load_call_targets,
    load_modules, matching_code, pc_coverage, plausible, config_pointer_modules,
)
from tools.oracle.install_call_targets import instrument, main as install_call_targets

# Hand-built ARM/Thumb ISA sequences, pointer tables, and metadata. All addresses
# below are invented synthetic values, not a title's executable layout or ABI.


class StaticEntryAuditTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def module(self, name="main", module_id=-1, base=0x08000000, words=None, functions=None, data=None):
        image = bytearray(0x80)
        for offset, value in (words or {0: 0xE12FFF1E}).items():
            struct.pack_into("<I", image, offset, value)
        path = self.root / (name + ".bin")
        path.write_bytes(image)
        return {"name": name, "module_id": module_id, "base": base, "image": str(path),
                "functions": functions or [{"addr": base, "size": 4, "thumb": False, "name": "entry"}],
                "labels": [], "calls": [], "data_ranges": data or [], "code_ranges": [[base, base + len(image)]]}

    def manifest(self, *modules):
        return {"cpu": "arm9", "arch": "v5te", "modules": list(modules)}

    def test_generated_entry_parser_keeps_module_and_instruction_state(self):
        entries = generated_entries([
            '{0x09000000u, 8u, 3, 0u, 0x0ull, &left, "x"},',
            '{0x09000000u, 8u, 4, 1u, 0x0ull, &right, "y"},',
            '{0x08000000u, 4u, -1, 0u, 0x0ull, &main, "z"},',
            '{0x09010000u, 8u, 9, 0u, 0x0ull, nullptr, "unimplemented"},',
            '{0x09020000u, 8u, 10, 0u, 0x0ull, nullptr, 0u, &region},',
        ])
        self.assertEqual(entries, {(3, 0x09000000, False), (4, 0x09000000, True), (-1, 0x08000000, False)})

    def test_ambiguous_overlay_pointers_are_ranked_without_inventing_provenance(self):
        base, target = 0x09000000, 0x09000020
        source = self.module(words={0: 0xE12FFF1E, 0x10: target},
                             data=[[0x08000010, 0x08000014]])
        words = {0: 0xE12FFF1E, 0x20: 0xE92D4000, 0x24: 0xE8BD8000}
        left = self.module("left", 3, base, words=words)
        right = self.module("right", 4, base, words=words)
        report = audit_pointers(load_modules(self.manifest(source, left, right)), {(3, target, False)})
        self.assertEqual(report["counts"]["ambiguous_overlay_source"], 1)
        self.assertEqual(report["candidates"], [])
        candidate, = report["ambiguous_overlay_candidates"]
        self.assertEqual((candidate["addr"], candidate["thumb"]), (target, False))
        self.assertEqual([(module["module_id"], module["already_entry"], module["plausible_missing_entry"])
                          for module in candidate["possible_modules"]], [(3, True, False), (4, False, True)])

    def test_load_relocation_resolves_overlay_pointer_and_ignores_stale_address(self):
        target = 0x09000020
        source = self.module(words={0: 0xE12FFF1E, 0x10: target},
                             data=[[0x08000010, 0x08000014]])
        words = {0: 0xE12FFF1E, 0x20: 0xE92D4000, 0x24: 0xE8BD8000}
        left = self.module("left", 3, 0x09000000, words=words)
        right = self.module("right", 4, 0x09000000, words=words)
        modules = load_modules(self.manifest(source, left, right))
        references = {"main": {0x08000010: (target, {4})}}
        report = audit_pointers(modules, {(3, target, False)}, pointer_modules=references)
        candidate, = report["candidates"]
        self.assertTrue(candidate["accepted"])
        self.assertEqual(candidate["module_id"], 4)
        references["main"][0x08000010] = (target + 4, {4})
        stale = audit_pointers(modules, set(), pointer_modules=references)
        self.assertEqual(stale["counts"]["ambiguous_overlay_source"], 1)

    def test_pointer_relocation_parser_preserves_module_sets_and_ignores_calls(self):
        config = self.root / "config.yaml"
        config.write_text("main_module:\n  relocations: refs.txt\n")
        (self.root / "refs.txt").write_text(
            "from:0x08000010 kind:load to:0x09000021 module:overlays(3,4)\n"
            "from:0x08000014 kind:arm_call to:0x09000020 module:overlay(3)\n")
        self.assertEqual(config_pointer_modules(config),
                         {"main": {0x08000010: (0x09000021, {3, 4})}})

    def test_data_pointer_recovers_unowned_prologue_without_config_edits(self):
        base = 0x08000000
        module = self.module(words={0: 0xE12FFF1E, 0x10: base + 0x20,
            0x20: 0xE92D4000, 0x24: 0xE3A00007, 0x28: 0xE8BD8000}, data=[[base + 0x10, base + 0x14]])
        manifest = self.manifest(module)
        report = audit_pointers(load_modules(manifest), {(-1, base, False)})
        accepted = [record for record in report["candidates"] if record["accepted"]]
        self.assertEqual([(record["addr"], record["thumb"]) for record in accepted], [(base + 0x20, False)])
        self.assertEqual(apply_entries(manifest, accepted), [(-1, base + 0x20, False)])
        self.assertEqual(module["functions"][-1]["size"], 12)
        self.assertEqual(apply_entries(manifest, accepted), [])

    def test_untyped_instruction_word_does_not_seed_code(self):
        base = 0x08000000
        module = self.module(words={0: 0xE12FFF1E, 0x10: base + 0x20,
            0x20: 0xE92D4000, 0x24: 0xE8BD8000})
        report = audit_pointers(load_modules(self.manifest(module)), {(-1, base, False)})
        self.assertFalse(any(record["accepted"] for record in report["candidates"]))
        self.assertEqual(report["counts"]["source_not_data_or_literal"], 1)

    def test_literal_pointer_is_recognized_even_without_data_symbol(self):
        base = 0x08000000
        module = self.module(words={0: 0xE59F0008, 4: 0xE12FFF1E, 0x10: base + 0x20,
            0x20: 0xE92D4000, 0x24: 0xE8BD8000},
            functions=[{"addr": base, "size": 0x14, "thumb": False, "name": "loader"}])
        report = audit_pointers(load_modules(self.manifest(module)), {(-1, base, False)})
        record = next(record for record in report["candidates"] if record["addr"] == base + 0x20)
        self.assertTrue(record["accepted"])
        self.assertEqual(record["sources"][0]["evidence"], "literal")

    def test_rejects_data_mode_mismatch_and_misalignment(self):
        base = 0x08000000
        module = self.module(words={0: 0xE92D4000, 4: 0xE8BD8000}, data=[[base + 0x20, base + 0x24]])
        loaded = load_modules(self.manifest(module))[0]
        self.assertEqual(plausible(loaded, base + 0x20, False)[1], "target_data")
        self.assertEqual(plausible(loaded, base, True)[1], "mode_mismatch")
        self.assertEqual(plausible(loaded, base + 2, False)[1], "misaligned_target")

    def test_rejects_second_half_of_thumb_instruction(self):
        base = 0x08000000
        module = self.module(functions=[{"addr": base, "size": 8, "thumb": True, "name": "thumb"}])
        image = bytearray(0x80)
        struct.pack_into("<HHHH", image, 0, 0xF000, 0xF800, 0x4770, 0x4770)
        Path(module["image"]).write_bytes(image)
        loaded = load_modules(self.manifest(module))[0]
        self.assertEqual(plausible(loaded, base + 2, True, observed=True)[1], "inside_instruction")

    def test_shared_overlay_addresses_require_live_provenance(self):
        base = 0x09000000
        main = self.module("main", -1, 0x08000000, words={0: 0xE12FFF1E, 0x10: base},
                           data=[[0x08000010, 0x08000014]])
        left = self.module("ov003", 3, base, words={0: 0xE92D4000, 4: 0xE8BD8000})
        right = self.module("ov004", 4, base, words={0: 0xE92D4010, 4: 0xE8BD8010})
        modules = load_modules(self.manifest(main, left, right))
        report = audit_pointers(modules, set())
        self.assertEqual(report["counts"]["ambiguous_overlay_source"], 1)
        self.assertFalse(any(record["addr"] == base for record in report["candidates"]))
        call = {"cpu": 0, "addr": base, "thumb": False, "sample": modules[2].image[:32], "source": 0x08000000, "frame": 1}
        observed = audit_calls(modules, {(4, base, False)}, [call], 0)
        self.assertEqual(observed["targets"][0]["modules"], ["ov004"])
        self.assertEqual(observed["coverage_percent"], 100)

    def test_short_function_fingerprint_excludes_following_mutable_data(self):
        module = load_modules(self.manifest(self.module()))[0]
        sample = module.image[:4] + bytes([0xCC]) * 28
        self.assertTrue(matching_code(module, module.base, False, sample))
        self.assertFalse(matching_code(module, module.base, False, bytes([0xCC]) * 32))

    def test_call_parser_and_unresolved_targets_are_not_counted_as_covered(self):
        sample = "00" * 32
        line = f"0 08000000 a {sample} 08000004 42"
        calls = load_call_targets([line, line])
        self.assertEqual(len(calls), 1)
        with self.assertRaises(ValueError):
            load_call_targets(["0 08000000 a 00 08000004 42"])
        with self.assertRaises(ValueError):
            load_call_targets(["0 08000000 bad " + sample + " 08000004 42"])
        observed = audit_calls(load_modules(self.manifest(self.module())), set(), calls, 0)
        self.assertEqual(observed["unresolved_targets"], 1)
        self.assertEqual(observed["coverage_percent"], 0)

    def test_observed_frameless_entry_uses_manifest_entry_points(self):
        base = 0x08000000
        module = self.module(words={0: 0xE12FFF1E, 4: 0xE2800007, 8: 0xE12FFF1E},
            functions=[{"addr": base, "size": 12, "thumb": False, "name": "owner"}])
        manifest = self.manifest(module)
        modules = load_modules(manifest)
        call = {"cpu": 0, "addr": base + 4, "thumb": False,
                "sample": modules[0].image[4:36], "source": base + 0x40, "frame": 1}
        observed = audit_calls(modules, {(-1, base, False)}, [call], 0)
        holes = observed["targets"][0]["holes"]
        self.assertTrue(holes[0]["accepted"])
        self.assertEqual(apply_entries(manifest, holes), [(-1, base + 4, False)])
        self.assertEqual(module["entry_points"], [{"addr": base + 4, "thumb": False}])
        entries, *_ = _prepare_entries(manifest, None)
        self.assertEqual([(entry.address, entry.thumb) for entry in entries], [(base, False), (base + 4, False)])
        output = self.root / "generated"
        stats = emit_manifest(manifest, output)
        self.assertEqual(stats["extra_entries"], 1)
        self.assertEqual(stats["undefined_sites"], 0)
        self.assertIn((-1, base + 4, False), generated_entries((output / "fn_table.cpp").read_text().splitlines()))

    def test_manifest_entry_validation_rejects_wrong_state_and_data(self):
        base = 0x08000000
        module = self.module(functions=[{"addr": base, "size": 12, "thumb": False, "name": "owner"}])
        for point in ({"addr": base + 2, "thumb": False}, {"addr": base + 4, "thumb": True}):
            module["entry_points"] = [point]
            with self.assertRaises(ValueError):
                _prepare_entries(self.manifest(module), None)
        module["entry_points"] = [{"addr": base + 4, "thumb": False}]
        module["data_ranges"] = [[base + 4, base + 8]]
        with self.assertRaises(ValueError):
            _prepare_entries(self.manifest(module), None)

    def test_declared_entry_deduplication_preserves_both_instruction_states(self):
        base = 0x08000000
        module = self.module(base=base - 2, functions=[
            {"addr": base - 2, "size": 8, "thumb": True, "name": "thumb_owner"},
            {"addr": base, "size": 4, "thumb": False, "name": "arm_entry"}])
        module["entry_points"] = [{"addr": base, "thumb": True}]
        entries, *_ = _prepare_entries(self.manifest(module), None)
        self.assertEqual({(entry.address, entry.thumb) for entry in entries},
                         {(base - 2, True), (base, False), (base, True)})

    def test_pc_range_coverage_is_separate_from_entry_coverage(self):
        modules = load_modules(self.manifest(self.module()))
        report = pc_coverage(modules, ["8000000 a", "8000002 a", "8000004 a", "8000000 a"])
        self.assertEqual(report, {"executed_pcs": 3, "range_covered": 2, "outside_ranges": 1})

    def test_dsd_manifest_exports_only_executable_delink_ranges(self):
        sections = self.root / "sections.txt"
        sections.write_text(
            "    .text start:0x08000000 end:0x08000020 kind:code\n"
            "    .rodata start:0x08000020 end:0x08000040 kind:data\n"
            "\nunit.cpp:\n    .text start:0x08000080 end:0x08000090 kind:code\n")
        module = dsd_module(self.root, {"object": "image.bin", "delinks": sections.name},
                            -1, "main", 0x08000000)
        self.assertEqual(module["code_ranges"], [[0x08000000, 0x08000020]])

    def test_arm7_manifest_exports_loaded_code_ranges_without_bss(self):
        module = ModuleState(name="arm7", module_id=-5, base=0x09000000, image=bytes(0x40),
                             loaded_size=0x30, executable_ranges=[(0, 0x10), (0x20, 0x40), (0x30, 0x40)])
        manifest = arm7_manifest([module], {"arm7": self.root / "arm7.bin"})
        self.assertEqual(manifest["modules"][0]["code_ranges"],
                         [[0x09000000, 0x09000010], [0x09000020, 0x09000030]])

    @staticmethod
    def invented_oracle_source():
        return "#include <synthetic>\n\ttrace_step();\n\t\tstep_arm();\n\tstep_thumb();\n"

    @staticmethod
    def invented_oracle_anchors():
        return {"include": r"^#include <synthetic>$", "trace": r"^\ttrace_step\(\);$",
                "arm": r"^\t\tstep_arm\(\);$", "thumb": r"^\tstep_thumb\(\);$"}

    def test_installer_is_idempotent_and_rejects_missing_anchors(self):
        source = self.invented_oracle_source()
        anchors = self.invented_oracle_anchors()
        patched = instrument(source, anchors)
        self.assertEqual(instrument(patched, anchors), patched)
        self.assertEqual(patched.count("dq9_oracle::record_call"), 2)
        self.assertIn("\ttrace_step();\n\tconst bool oracle_is_call", patched)
        self.assertIn("\t\tstep_arm();\n\t\tif (oracle_is_call)", patched)
        self.assertIn("\tstep_thumb();\n\tif (oracle_is_call)", patched)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            instrument("unsupported oracle source", anchors)

    def test_installer_rejects_ambiguous_invalid_and_overlapping_anchors(self):
        source = self.invented_oracle_source()
        anchors = self.invented_oracle_anchors()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            instrument(source + "\tstep_thumb();\n", anchors)
        for patterns in ({}, {**anchors, "extra": "anything"}, {**anchors, "arm": 3},
                         {**anchors, "arm": ""}, {**anchors, "arm": "["},
                         {**anchors, "arm": "^"}, {**anchors, "arm": anchors["thumb"]}):
            with self.subTest(patterns=patterns), self.assertRaises(ValueError):
                instrument(source, patterns)

    def test_installer_preserves_line_endings_in_matched_anchors(self):
        source = self.invented_oracle_source()
        anchors = {key: pattern + "\n" for key, pattern in self.invented_oracle_anchors().items()}
        patched = instrument(source, anchors)
        self.assertIn("\ttrace_step();\n\tconst bool oracle_is_call", patched)
        self.assertIn("oracle_call_source = ARMPROC.instruct_adr;\n\t\tstep_arm();", patched)
        self.assertTrue(patched.endswith("\n"))

    def test_installer_rejects_a_different_installed_header(self):
        source = instrument(self.invented_oracle_source(), self.invented_oracle_anchors())
        with self.assertRaisesRegex(ValueError, "different header path"):
            instrument(source, self.invented_oracle_anchors(), self.root / "other.h")

    def test_installer_cli_reads_local_json_patterns(self):
        source = self.root / "synthetic.cpp"
        anchors = self.root / "anchors.json"
        source.write_text(self.invented_oracle_source())
        anchors.write_text(json.dumps(self.invented_oracle_anchors()))
        self.assertEqual(install_call_targets(["--source", str(source), "--anchors", str(anchors)]), 0)
        self.assertEqual(source.read_text().count("dq9_oracle::record_call"), 2)

    def test_installer_cli_leaves_source_unchanged_on_anchor_error(self):
        source = self.root / "synthetic.cpp"
        anchors = self.root / "anchors.json"
        original = self.invented_oracle_source()
        source.write_text(original)
        anchors.write_text(json.dumps({**self.invented_oracle_anchors(), "arm": "missing"}))
        with self.assertRaisesRegex(ValueError, "exactly one"):
            install_call_targets(["--source", str(source), "--anchors", str(anchors)])
        self.assertEqual(source.read_text(), original)

    def test_oracle_call_classifier_excludes_returns_and_untaken_calls(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        header = Path(__file__).resolve().parents[1] / "tools/oracle/call_targets.h"
        source = self.root / "oracle-hook.cpp"
        source.write_text(r'''#include <cstdint>
#include <cassert>
using u8 = std::uint8_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
struct Status { struct { unsigned T = 0; } bits; };
struct armcpu_t { u32 instruction = 0, instruct_adr = 0x08000000, next_instruction = 0; u32 R[16]{}; Status CPSR; };
#define CODE(op) (((op) >> 25) & 7)
#define TEST_COND(cond, inst, cpsr) ((cond) == 14 || (cond) == 15)
constexpr int MMU_AT_DEBUG = 0;
u8 live_byte = 0;
u8 _MMU_read08(int, int, u32) { return live_byte; }
''' + f'#include "{header.as_posix()}"\n' + r'''int main(int argc, char** argv) {
    assert(argc == 2);
    setenv("DS_ORACLE_CALL_TARGETS", argv[1], 1);
    armcpu_t s;
    s.instruction = 0xeb000000; assert(dq9_oracle::is_call(s));
    s.instruction = 0x0b000000; assert(!dq9_oracle::is_call(s));
    s.instruction = 0xe12fff1e; assert(!dq9_oracle::is_call(s));
    s.instruction = 0xe12fff13; s.R[14] = s.instruct_adr + 4; assert(dq9_oracle::is_call(s));
    s.R[14] = 0; assert(!dq9_oracle::is_call(s));
    s.instruction = 0xe12fff33; assert(dq9_oracle::is_call(s));
    s.CPSR.bits.T = 1; s.instruction = 0xf801; assert(dq9_oracle::is_call(s));
    s.instruction = 0x4770; assert(!dq9_oracle::is_call(s));
    s.instruction = 0x4718; s.R[14] = (s.instruct_adr + 2) | 1; assert(dq9_oracle::is_call(s));
    s.R[14] = 0; assert(!dq9_oracle::is_call(s));
    s.CPSR.bits.T = 0; s.next_instruction = 0x08000010;
    dq9_oracle::record_call(0, s, 0x08000000, 42);
    dq9_oracle::record_call(0, s, 0x08000004, 43);
    live_byte = 1;
    dq9_oracle::record_call(0, s, 0x08000008, 44);
}''')
        executable = self.root / "oracle-hook"
        compiled = subprocess.run([compiler, "-std=c++17", str(source), "-o", str(executable)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        trace = self.root / "call_targets.txt"
        run = subprocess.run([str(executable), str(trace)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        rows = load_call_targets(trace.read_text().splitlines())
        self.assertEqual([(row["cpu"], row["addr"], row["thumb"], row["frame"]) for row in rows],
                         [(0, 0x08000010, False, 42), (0, 0x08000010, False, 44)])
        self.assertEqual([row["sample"] for row in rows], [bytes(32), bytes([1]) * 32])

    def test_declared_arm_and_thumb_entries_match_unicorn(self):
        from tests.recomp_tests import test_differential as differential
        if differential._IMPORT_ERROR is not None or not shutil.which("g++"):
            self.skipTest("compiled differential dependencies unavailable")
        base = differential.CODE_BASE
        image = bytearray(0x80)
        # Roots return before their callback tails. Only the manifest additions
        # make those tails independently callable in the generated FnTable.
        struct.pack_into("<IIIII", image, 0, 0xE3A00003, 0xE12FFF1E,
                         0xE2800007, 0xE5860000, 0xE12FFF1E)
        struct.pack_into("<HHHHH", image, 0x40, 0x2003, 0x4770, 0x3007, 0x6030, 0x4770)
        module = self.module(base=base, functions=[
            {"addr": base, "size": 20, "thumb": False, "name": "arm_owner"},
            {"addr": base + 0x40, "size": 10, "thumb": True, "name": "thumb_owner"}])
        Path(module["image"]).write_bytes(image)
        manifest = self.manifest(module)
        apply_entries(manifest, [
            {"accepted": True, "module_id": -1, "addr": base + 8, "thumb": False, "size": 12},
            {"accepted": True, "module_id": -1, "addr": base + 0x44, "thumb": True, "size": 6}])
        generated = self.root / "differential-generated"
        stats = emit_manifest(manifest, generated)
        self.assertEqual(stats["extra_entries"], 2)
        self.assertEqual(stats["undefined_sites"], 0)
        (self.root / "runner.cpp").write_text(differential._RUNNER_SOURCE)
        (self.root / "code_image.h").write_text("#pragma once\n#include <cstdint>\n"
            + f"constexpr std::uint32_t kCodeBase = {base:#x}u;\n"
            + "constexpr std::uint8_t kCodeImage[] = {" + ",".join(str(value) for value in image) + "};\n")
        executable = self.root / "differential-runner"
        command = [shutil.which("g++"), "-std=c++20", "-O2", "-DDSR_RET_CHECK",
            "-I", str(Path(__file__).resolve().parents[1] / "runtime/include"), "-I", str(generated),
            *[str(path) for path in sorted(generated.glob("chunk_*.cpp"))], str(generated / "fn_table.cpp"),
            str(self.root / "runner.cpp"), "-o", str(executable)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        harness = differential.RecompilerDifferentialTests()
        harness.executable, harness.image = executable, bytes(image)
        cases = []
        for thumb, address in ((False, base + 8), (True, base + 0x44)):
            for value in (0, 1, 0x7fffffff, 0xffffffff):
                regs = [0] * 16
                regs[0], regs[6], regs[13], regs[14] = value, differential.DATA_BASE, differential.DATA_BASE + 128, differential.RETURN_ADDRESS
                spec = differential.FunctionSpec("audit_callback", "audit_entry", thumb, b"", terminal=True)
                cases.append(differential.TestState(0, spec, value, regs, (0, 0, 1, 0, 0), bytes(differential.MEMORY_SIZE), address))
        actual = harness._native_batch(cases)
        uc, returned = harness._new_unicorn()
        try:
            for case, native in zip(cases, actual):
                expected = harness._run_unicorn(uc, returned, case)
                self.assertEqual(native.regs, expected.regs)
                self.assertEqual(native.flags, expected.flags)
                self.assertEqual(native.memory, expected.memory)
                self.assertEqual(expected.transfer_target, differential.RETURN_ADDRESS)
        finally:
            close = getattr(uc, "close", None)
            if close:
                close()


if __name__ == "__main__":
    unittest.main()
