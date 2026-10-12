# @runtime PyGhidra
# Read-only query postScript. Never starts transactions or saves the program.
import json
import re
from ghidra.program.model.listing import CodeUnit
from ghidra.program.model.symbol import SymbolType
from ghidra.app.decompiler import DecompInterface

MARKER = "GHIDRA_QUERY_JSON:"
MAX_TEXT = 40000
MAX_ITEMS = 1000

MAX_RESULT_BYTES = 1024 * 1024

def encoded_size(value):
    return len(json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))

def bounded_batch(responses):
    results = list(responses)
    payload = {"module": module_name(), "results": results, "truncated": False}
    while results and encoded_size(payload) > MAX_RESULT_BYTES:
        results.pop()
        payload["truncated"] = True
    if encoded_size(payload) > MAX_RESULT_BYTES:
        payload["results"] = []
        payload["truncated"] = True
    return payload

def emit(value):
    print(MARKER + json.dumps(value, separators=(",", ":"), ensure_ascii=False))

def module_name():
    name = currentProgram.getName()
    if name == "arm9.bin":
        return "main"
    return name[:-4] if name.endswith(".bin") else name

def parse_addr(value):
    if value.lower().startswith("0x"):
        return toAddr(int(value, 16))
    if len(value) == 8 and re.match(r"^[0-9a-fA-F]{8}$", value):
        return toAddr(int(value, 16))
    return toAddr(int(value, 0))

def function_for(value):
    if re.match(r"^(0[xX][0-9a-fA-F]+|[0-9]+|[0-9a-fA-F]{8})$", value):
        address = parse_addr(value)
        function = currentProgram.getFunctionManager().getFunctionAt(address)
        if function is None:
            function = currentProgram.getFunctionManager().getFunctionContaining(address)
        if function is None:
            raise ValueError("no function at %s" % value)
        return function
    matches = currentProgram.getSymbolTable().getSymbols(value)
    for symbol in matches:
        function = currentProgram.getFunctionManager().getFunctionAt(symbol.getAddress())
        if function is not None:
            return function
    raise ValueError("function not found: %s" % value)

def bounded_text(text):
    text = str(text)
    return text[:MAX_TEXT] + ("\n...[truncated]" if len(text) > MAX_TEXT else "")

def disasm(options):
    start = parse_addr(options["addr"])
    length = min(max(1, int(options.get("length", 0x80))), 4096)
    end = start.add(length - 1)
    iterator = currentProgram.getListing().getInstructions(start, True)
    items = []
    while iterator.hasNext():
        instruction = iterator.next()
        if instruction.getAddress().compareTo(end) > 0:
            break
        items.append({
            "address": str(instruction.getAddress()),
            "length": instruction.getLength(),
            "text": str(instruction),
        })
    return {"module": module_name(), "start": str(start), "length": length, "items": items}

def funcs(options):
    pattern = re.compile(options.get("grep") or ".", re.IGNORECASE)
    limit = min(int(options.get("limit", 100)), MAX_ITEMS)
    result = []
    truncated = False
    iterator = currentProgram.getFunctionManager().getFunctions(True)
    for function in iterator:
        entry = function.getEntryPoint()
        name = function.getName()
        if pattern.search(name):
            if len(result) == limit:
                truncated = True
                break
            result.append({"name": name, "address": str(entry), "size": function.getBody().getNumAddresses(), "thunk": function.isThunk(), "signature": str(function.getSignature())})
    return {"module": module_name(), "items": result, "limit": limit, "truncated": truncated}

def strings(options):
    pattern = re.compile(options.get("grep") or ".", re.IGNORECASE)
    limit = min(int(options.get("limit", 100)), MAX_ITEMS)
    result = []
    truncated = False
    listing = currentProgram.getListing()
    for data in listing.getDefinedData(True):
        if not data.hasStringValue():
            continue
        value = str(data.getValue())
        if pattern.search(value):
            if len(result) == limit:
                truncated = True
                break
            result.append({"address": str(data.getAddress()), "value": bounded_text(value)})
    return {"module": module_name(), "items": result, "limit": limit, "truncated": truncated}

def symbols(options):
    pattern = re.compile(options.get("grep") or ".", re.IGNORECASE)
    result = []
    truncated = False
    iterator = currentProgram.getSymbolTable().getAllSymbols(True)
    for symbol in iterator:
        name = symbol.getName()
        if pattern.search(name):
            if len(result) == MAX_ITEMS:
                truncated = True
                break
            result.append({"name": name, "address": str(symbol.getAddress()), "type": str(symbol.getSymbolType()), "source": str(symbol.getSource())})
    return {"module": module_name(), "items": result, "limit": MAX_ITEMS, "truncated": truncated}

def decompile(target):
    function = function_for(target)
    interface = DecompInterface()
    interface.openProgram(currentProgram)
    result = interface.decompileFunction(function, 60, monitor)
    if not result.decompileCompleted():
        raise RuntimeError("decompilation failed: " + str(result.getErrorMessage()))
    return {"module": module_name(), "function": function.getName(), "address": str(function.getEntryPoint()), "signature": str(function.getSignature()), "c": bounded_text(result.getDecompiledFunction().getC())}

def related(target, callers):
    function = function_for(target)
    result = []
    seen = set()
    truncated = False
    if callers:
        refs = currentProgram.getReferenceManager().getReferencesTo(function.getEntryPoint())
        for ref in refs:
            if not ref.getReferenceType().isCall():
                continue
            address = ref.getFromAddress()
            source_function = currentProgram.getFunctionManager().getFunctionContaining(address)
            key = (str(address), source_function.getName() if source_function else None)
            if key not in seen:
                seen.add(key)
                if len(result) >= MAX_ITEMS:
                    truncated = True
                    break
                result.append({"address": str(address), "function": source_function.getName() if source_function else None, "reference_type": str(ref.getReferenceType())})
    else:
        listing = currentProgram.getListing()
        instructions = listing.getInstructions(function.getBody(), True)
        for instruction in instructions:
            for ref in currentProgram.getReferenceManager().getReferencesFrom(instruction.getAddress()):
                if not ref.getReferenceType().isCall():
                    continue
                target = ref.getToAddress()
                target_function = currentProgram.getFunctionManager().getFunctionContaining(target)
                key = (str(target), target_function.getName() if target_function else None)
                if key not in seen:
                    seen.add(key)
                    if len(result) >= MAX_ITEMS:
                        truncated = True
                        break
                    result.append({"address": str(target), "function": target_function.getName() if target_function else None, "reference_type": str(ref.getReferenceType())})
            if truncated:
                break
    return {"module": module_name(), "function": function.getName(), "address": str(function.getEntryPoint()), "items": result, "truncated": truncated}
def xrefs(options):
    address = parse_addr(options["addr"])
    result = []
    for ref in currentProgram.getReferenceManager().getReferencesTo(address):
        from_address = ref.getFromAddress()
        function = currentProgram.getFunctionManager().getFunctionContaining(from_address)
        result.append({"from": str(from_address), "function": function.getName() if function else None, "type": str(ref.getReferenceType())})
        if len(result) >= MAX_ITEMS:
            break
    return {"module": module_name(), "address": str(address), "items": result, "truncated": len(result) == MAX_ITEMS}

def one(operation, args):
    options = json.loads(args[0]) if args else {}
    if operation == "funcs": return funcs(options)
    if operation == "strings": return strings(options)
    if operation == "symbols": return symbols(options)
    if operation == "decompile": return decompile(options["target"])
    if operation == "callers": return related(options["target"], True)
    if operation == "callees": return related(options["target"], False)
    if operation == "disasm": return disasm(options)
    if operation == "xrefs": return xrefs(options)
    raise ValueError("unknown query " + operation)

args = getScriptArgs()
try:
    operation = args[0]
    if operation == "batch":
        requests = json.loads(args[1])
        if not isinstance(requests, list) or len(requests) > 100:
            raise ValueError("batch requires a list of at most 100 queries")
        responses = []
        for request in requests:
            responses.append(one(request["command"], [json.dumps(request.get("args", {}))]))
        emit(bounded_batch(responses))
    else:
        emit(one(operation, args[1:]))
except Exception as error:
    emit({"error": str(error), "operation": args[0] if args else None})
    raise
