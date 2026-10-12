import ast
import json
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]


def load_functions(script_path, names, namespace):
    tree = ast.parse(script_path.read_text(encoding="utf-8"))
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(script_path), "exec"), namespace)
    return namespace


class GhidraScriptPureHelperTests(unittest.TestCase):
    def test_related_callers_are_capped_and_marked_truncated(self):
        script = ROOT / "tools" / "ghidra" / "query_module.py"
        # A plain Python class is needed for the Java-style string conversion.
        class RefType:
            def isCall(self):
                return True

            def __str__(self):
                return "CALL"

        references = [
            SimpleNamespace(getReferenceType=lambda: RefType(), getFromAddress=lambda index=index: "0x%x" % index)
            for index in range(3)
        ]
        program = SimpleNamespace(
            getReferenceManager=lambda: SimpleNamespace(getReferencesTo=lambda _address: references),
            getFunctionManager=lambda: SimpleNamespace(getFunctionContaining=lambda _address: None),
        )
        function = SimpleNamespace(
            getEntryPoint=lambda: "entry", getName=lambda: "target", getBody=lambda: object()
        )
        namespace = {
            "MAX_ITEMS": 2,
            "currentProgram": program,
            "function_for": lambda _target: function,
            "module_name": lambda: "main",
        }
        load_functions(script, {"related"}, namespace)
        result = namespace["related"]("target", True)
        self.assertEqual(len(result["items"]), 2)
        self.assertTrue(result["truncated"])

    def test_batch_byte_cap_keeps_valid_truncation_marker(self):
        script = ROOT / "tools" / "ghidra" / "query_module.py"
        namespace = {"MAX_RESULT_BYTES": 180, "module_name": lambda: "main", "json": json}
        load_functions(script, {"encoded_size", "bounded_batch"}, namespace)
        result = namespace["bounded_batch"]([{"value": "x" * 200}])
        self.assertTrue(result["truncated"])
        self.assertEqual(result["results"], [])
        self.assertLessEqual(namespace["encoded_size"](result), namespace["MAX_RESULT_BYTES"])

    def test_import_failure_samples_are_bounded(self):
        script = ROOT / "tools" / "ghidra" / "import_module.py"
        namespace = {}
        load_functions(script, {"record_sample"}, namespace)
        samples = {"functions": [], "labels": []}
        for index in range(25):
            namespace["record_sample"](samples, "functions", "function_%d" % index)
        self.assertEqual(len(samples["functions"]), 20)
        self.assertEqual(samples["functions"][0], "function_0")

    def test_import_run_counts_malformed_lines_and_failures(self):
        script = ROOT / "tools" / "ghidra" / "import_module.py"
        with tempfile.TemporaryDirectory() as directory:
            symbols = Path(directory) / "symbols.txt"
            symbols.write_text(
                "malformed line\n"
                "bad_function kind:function(arm,size=0x4) addr:0x1000\n"
                "bad_label kind:data addr:0x2000\n",
                encoding="utf-8",
            )
            emitted = []
            program = SimpleNamespace(getName=lambda: "main", getExecutablePath=lambda: "synthetic.bin")
            namespace = {
                "ADDRESS": re.compile(r"^\s*(\S+)\s+kind:(\S+)\s+addr:(0[xX][0-9a-fA-F]+)(?:\s+.*)?$"),
                "FUNCTION": re.compile(r"function\(([^,()]+)(?:,size=(0[xX][0-9a-fA-F]+))?\)"),
                "json": json,
                "time": SimpleNamespace(time=lambda: 1),
                "getScriptArgs": lambda: [json.dumps({"module": "main", "symbols": str(symbols)})],
                "currentProgram": program,
                "add_function": lambda *_args: False,
                "add_label": lambda *_args: (_ for _ in ()).throw(RuntimeError("label failed")),
                "analyzeAll": lambda _program: None,
                "emit": emitted.append,
            }
            load_functions(script, {"run", "record_sample", "read_lines"}, namespace)
            namespace["run"]()
            result = emitted[0]
            self.assertEqual(result["functions_named"], 0)
            self.assertEqual(result["functions_failed"], 1)
            self.assertEqual(result["labels_failed"], 1)
            self.assertEqual(result["symbol_lines_skipped"], 1)
            self.assertTrue(result["incomplete"])
            self.assertEqual(result["failure_samples"]["functions"], ["bad_function"])
            self.assertEqual(result["failure_samples"]["labels"], ["bad_label"])


if __name__ == "__main__":
    unittest.main()
