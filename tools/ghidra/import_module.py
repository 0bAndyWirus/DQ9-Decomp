# @runtime PyGhidra
# Ghidra postScript: apply DSD names, analyze once, and save during import only.
import json
import re
from ghidra.program.model.address import AddressSet
import time
from java.math import BigInteger
from ghidra.program.model.symbol import SourceType

MARKER = "GHIDRA_QUERY_JSON:"
ADDRESS = re.compile(r"^\s*(\S+)\s+kind:(\S+)\s+addr:(0[xX][0-9a-fA-F]+)(?:\s+.*)?$")
FUNCTION = re.compile(r"function\(([^,()]+)(?:,size=(0[xX][0-9a-fA-F]+))?\)")

def read_json(path):
    with open(path, "r", encoding="utf-8") as source:
        return json.load(source)

def read_lines(path):
    with open(path, "r", encoding="utf-8") as source:
        for line in source:
            yield line

def emit(payload):
    print(MARKER + json.dumps(payload, separators=(",", ":")))

def addr(value):
    return toAddr(int(value, 0))

def add_function(address, name, thumb, size):
    start = addr(str(address)) if not isinstance(address, int) else toAddr(address)
    end = start.add(size - 1) if size else start.add(3)
    clearListing(start, end)
    manager = currentProgram.getProgramContext()
    mode = manager.getRegister("TMode")
    if mode is not None:
        manager.setValue(mode, start, end, BigInteger.valueOf(1 if thumb else 0))
    disassemble(start)
    function = currentProgram.getFunctionManager().getFunctionAt(start)
    if function is None:
        function = createFunction(start, name)
    else:
        function.setName(name, SourceType.USER_DEFINED)
    if function is not None and size:
        function.setBody(AddressSet(start, end))
    return function is not None

def add_label(address, name):
    currentProgram.getSymbolTable().createLabel(addr(str(address)), name, currentProgram.getSymbolTable().getGlobalNamespace(), SourceType.USER_DEFINED)

def record_sample(samples, category, name):
    values = samples[category]
    if len(values) < 20:
        values.append(str(name)[:160])

def run():
    started = time.time()
    options = json.loads(getScriptArgs()[0])
    applied = 0
    function_failures = 0
    label_failures = 0
    skipped_lines = 0
    samples = {"functions": [], "labels": []}
    module_data = {}
    if options.get("manifest"):
        manifest = read_json(options["manifest"])
        module_data = next(item for item in manifest["modules"] if item["name"] == options["module"])
    symbols_path = options.get("symbols")
    if symbols_path:
        for line in read_lines(symbols_path):
            if not line.strip():
                continue
            match = ADDRESS.match(line)
            if not match:
                skipped_lines += 1
                continue
            name, kind, address = match.groups()
            if kind.startswith("function"):
                func = FUNCTION.search(kind)
                if not func:
                    function_failures += 1
                    record_sample(samples, "functions", name)
                    continue
                mode, size = func.groups()
                try:
                    success = add_function(int(address, 0), name, mode == "thumb", int(size, 0) if size else 0)
                except Exception:
                    success = False
                if success:
                    applied += 1
                else:
                    function_failures += 1
                    record_sample(samples, "functions", name)
            else:
                try:
                    add_label(address, name)
                except Exception:
                    label_failures += 1
                    record_sample(samples, "labels", name)
    for fn in module_data.get("functions", []):
        try:
            success = add_function(fn["addr"], fn["name"], bool(fn.get("thumb")), fn.get("size", 0))
        except Exception:
            success = False
        if success:
            applied += 1
        else:
            function_failures += 1
            record_sample(samples, "functions", fn.get("name", "<unnamed>"))
    for label in module_data.get("labels", []):
        try:
            add_label(label["addr"], "arm7_label_%08x" % label["addr"])
        except Exception:
            label_failures += 1
            record_sample(samples, "labels", "arm7_label_%08x" % label.get("addr", 0))
    analyzeAll(currentProgram)
    failures = function_failures + label_failures + skipped_lines
    emit({"module": options.get("module", currentProgram.getName()), "image": currentProgram.getExecutablePath(), "functions_named": applied, "functions_missing": function_failures, "functions_failed": function_failures, "labels_failed": label_failures, "symbol_lines_skipped": skipped_lines, "failure_samples": samples, "incomplete": failures > 0, "analysis_seconds": round(time.time() - started, 3)})
run()

