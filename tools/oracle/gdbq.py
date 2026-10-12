#!/usr/bin/env python3
"""Run patched DeSmuME under its GDB stub and emit JSON at CPU breakpoints.

On startup/probe failure, report the bounded tail of combined emulator output.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import signal
import tempfile


LOG_TAIL_BYTES = 8192


def cleanup_process(process, kill_group=os.killpg, timeout: float = 5.0) -> None:
    """Stop the emulator process group, escalating only while its leader is alive."""
    if process.poll() is not None:
        return
    try:
        kill_group(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=timeout)
        return
    except subprocess.TimeoutExpired:
        pass
    if process.poll() is None:
        try:
            kill_group(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def log_tail(handle, limit: int = LOG_TAIL_BYTES) -> str:
    handle.flush()
    handle.seek(0, os.SEEK_END)
    size = handle.tell()
    handle.seek(max(0, size - limit))
    return handle.read(limit).decode("utf-8", "replace")

DEFAULT_BINARY = Path("reference/desmume-trace/build/cli-gdb/cli/desmume-cli")


def integer(text: str) -> int:
    return int(text, 0)


def parse_memory(text: str) -> tuple[int, int]:
    try:
        address_text, length_text = text.split(":", 1)
        address, length = integer(address_text), integer(length_text)
    except (ValueError, TypeError) as exc:
        raise argparse.ArgumentTypeError("expected ADDR:LENGTH, e.g. synthetic 0x08000000:16") from exc
    if address < 0 or length < 1 or length > 1024:
        raise argparse.ArgumentTypeError("memory address must be nonnegative and length 1..1024")
    return address, length


def checksum(payload: bytes) -> int:
    return sum(payload) & 0xff


def packet(payload: bytes) -> bytes:
    return b"$" + payload + b"#" + f"{checksum(payload):02x}".encode()

class Remote:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.sock.settimeout(120)

    def receive(self) -> bytes:
        while self.sock.recv(1) != b"$":
            pass
        body = self.receive_body()
        self.sock.sendall(b"+")
        return body

    def receive_body(self) -> bytes:
        body = bytearray()
        checksum = 0
        while True:
            byte = self.sock.recv(1)
            if not byte:
                raise RuntimeError("GDB connection closed while receiving packet")
            if byte == b"#":
                break
            body.extend(byte)
            checksum = (checksum + byte[0]) & 0xff
        check = self.sock.recv(2)
        if len(check) != 2:
            raise RuntimeError("truncated GDB packet checksum")
        try:
            valid = int(check, 16) == checksum
        except ValueError as exc:
            raise RuntimeError(f"invalid GDB checksum {check!r}") from exc
        if not valid:
            self.sock.sendall(b"-")
            return self.receive()
        return bytes(body)

    def read_ack(self) -> bytes:
        while True:
            byte = self.sock.recv(1)
            if byte != b"$":
                return byte
            # Some stub states send an unsolicited stop packet before the ack.
            self.receive_body()
            self.sock.sendall(b"+")

    def command(self, text: str) -> bytes:
        self.sock.sendall(packet(text.encode()))
        try:
            ack = self.read_ack()
        except socket.timeout as exc:
            raise RuntimeError(f"timed out waiting for acknowledgement of {text!r}") from exc
        if ack != b"+":
            raise RuntimeError(f"GDB did not acknowledge {text!r}: {ack!r}")
        try:
            return self.receive()
        except socket.timeout as exc:
            raise RuntimeError(f"timed out waiting for reply to {text!r}") from exc

    def continue_execution(self) -> bytes:
        self.sock.sendall(packet(b"c"))
        try:
            ack = self.read_ack()
        except socket.timeout as exc:
            raise RuntimeError("timed out waiting for continue acknowledgement") from exc
        if ack != b"+":
            raise RuntimeError(f"GDB did not acknowledge continue: {ack!r}")
        try:
            return self.receive()
        except socket.timeout as exc:
            raise RuntimeError("timed out waiting for breakpoint stop (increase --timeout)") from exc
    def synchronize_stop(self) -> bytes:
        self.sock.sendall(packet(b"?"))
        ack = self.sock.recv(1)
        if ack == b"+":
            return self.receive()
        if ack != b"-":
            raise RuntimeError(f"unexpected GDB acknowledgement: {ack!r}")
        # The running stub NACKs commands; interrupt out of band and wait for its stop packet.
        self.sock.sendall(b"\x03")
        try:
            return self.receive()
        except socket.timeout as exc:
            raise RuntimeError("timed out interrupting the running emulator") from exc




def parse_registers(raw: bytes) -> dict[str, int]:
    # ARM stub's 'g' packet has r0-r15, 8 FP registers, FPS, and CPSR.
    if len(raw) < 16 * 8 + 8:
        raise RuntimeError(f"short ARM register packet ({len(raw)} hex characters)")
    try:
        regs = [int.from_bytes(bytes.fromhex(raw[i * 8:(i + 1) * 8].decode()), "little")
                for i in range(16)]
        cpsr_offset = (16 + 8 * 3 + 1) * 8
        cpsr = int.from_bytes(bytes.fromhex(raw[cpsr_offset:cpsr_offset + 8].decode()), "little")
    except (ValueError, UnicodeDecodeError) as exc:
        raise RuntimeError("malformed ARM register packet") from exc
    return {**{f"r{i}": value for i, value in enumerate(regs)}, "cpsr": cpsr}


def wait_connect(process: subprocess.Popen[bytes], port: int, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"emulator exited before GDB connection (status {process.returncode})")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise RuntimeError(f"timed out connecting to ARM GDB stub on 127.0.0.1:{port}: {last_error}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path, help="Nintendo DS ROM")
    parser.add_argument("--port", type=int, default=3333, help="local GDB stub port (default: 3333)")
    parser.add_argument("--cpu", choices=("arm9", "arm7"), default="arm9")
    parser.add_argument("--break", dest="breakpoints", required=True,
                        help="comma-separated instruction addresses (synthetic example: 0x08000000,0x08000010)")
    parser.add_argument("--watch-write", type=parse_memory, help="write-watch address:length")
    parser.add_argument("--max-hits", type=int, default=1, help="stop after this many hits (default: 1)")
    parser.add_argument("--regs", action="store_true", help="include r0-r15 and CPSR in hit output")
    parser.add_argument("--mem", action="append", type=parse_memory, default=[], metavar="ADDR:LENGTH",
                        help="read a memory range at each hit; repeatable")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="seconds for startup and each breakpoint wait (default: 120)")
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY, help="GDB-enabled DeSmuME CLI")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535 or args.max_hits < 1 or args.timeout <= 0:
        parser.error("port must be 1..65535, max-hits and timeout must be positive")
    if not args.rom.is_file():
        parser.error(f"ROM not found: {args.rom}")
    if not args.binary.is_file():
        parser.error(f"GDB-enabled DeSmuME binary not found: {args.binary}; see tools/oracle/README.md")
    try:
        breakpoints = [integer(item.strip()) for item in args.breakpoints.split(",")]
    except ValueError:
        parser.error("--break expects comma-separated integer addresses")
    if not breakpoints or any(address < 0 or address > 0xffffffff for address in breakpoints):
        parser.error("--break requires at least one 32-bit address")
    # Refuse to attach to an older emulator on the requested port.
    probe = socket.socket()
    try:
        probe.bind(("127.0.0.1", args.port))
    except OSError as exc:
        parser.error(f"GDB port {args.port} is already in use: {exc}")
    finally:
        probe.close()

    command = [str(args.binary.resolve()), "--disable-limiter", "--disable-sound", "--3d-render", "SW",
               "--start-paused", f"--{args.cpu}gdb", str(args.port), str(args.rom.resolve())]
    env = os.environ.copy()
    env.pop("DS_ORACLE_FRAMES", None)
    env.pop("DS_ORACLE_INPUT", None)
    if shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a", *command]
    else:
        env["SDL_VIDEODRIVER"] = "dummy"
    log = tempfile.TemporaryFile()
    try:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
    except OSError as exc:
        print(f"gdbq.py: failed to start emulator: {exc}", file=sys.stderr)
        log.close()
        return 1
    try:
        sock = wait_connect(process, args.port, args.timeout)
        with sock:
            remote = Remote(sock)
            remote.sock.settimeout(args.timeout)
            # Synchronize if the emulator has started running despite --start-paused.
            stopped = remote.synchronize_stop()
            if not stopped:
                raise RuntimeError("GDB stub returned an empty initial stop reply")
            for address in breakpoints:
                reply = remote.command(f"Z0,{address:x},4")
                if reply != b"OK":
                    raise RuntimeError(f"failed to set breakpoint at 0x{address:08x}: {reply.decode(errors='replace')}")
            if args.watch_write:
                address, length = args.watch_write
                reply = remote.command(f"Z2,{address:x},{length:x}")
                if reply != b"OK":
                    raise RuntimeError(f"failed to set write watchpoint at 0x{address:08x}: {reply.decode(errors='replace')}")

            for hit_index in range(args.max_hits):
                stopped = remote.continue_execution().decode("ascii", errors="replace")
                if stopped.startswith(("S", "T")) is False:
                    raise RuntimeError(f"unexpected GDB stop reply: {stopped!r}")
                registers = parse_registers(remote.command("g"))
                hit = registers["r15"]
                if stopped.startswith("T") and "watch:" in stopped:
                    try:
                        hit = int(stopped.split("watch:", 1)[1].split(";", 1)[0][::-1], 16)
                    except ValueError:
                        pass
                event: dict[str, object] = {"hit": hit_index + 1, "address": f"0x{hit:08x}",
                                           "stop": stopped}
                if args.regs:
                    event["registers"] = {name: f"0x{value:08x}" for name, value in registers.items()}
                if args.mem:
                    memory = {}
                    for address, length in args.mem:
                        data = remote.command(f"m{address:x},{length:x}")
                        if len(data) != length * 2 or data.startswith(b"E"):
                            raise RuntimeError(f"failed memory read at 0x{address:08x}: {data.decode(errors='replace')}")
                        memory[f"0x{address:08x}:{length}"] = data.decode("ascii")
                    event["memory"] = memory
                print(json.dumps(event, separators=(",", ":")), flush=True)
                if hit_index + 1 < args.max_hits:
                    remote.sock.settimeout(args.timeout)
    except (OSError, RuntimeError, TimeoutError, socket.timeout) as exc:
        print(f"gdbq.py: {exc}", file=sys.stderr)
        tail = log_tail(log)
        if tail:
            print(f"emulator output (last {LOG_TAIL_BYTES} bytes):\n{tail}", file=sys.stderr)
        return 1
    finally:
        cleanup_process(process)
        log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
