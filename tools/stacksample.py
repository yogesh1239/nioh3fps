"""Sample one Windows x64 thread; read-only except for suspend/resume.

Usage: python -I tools/stacksample.py <exe> <tid> [seconds=10] [hz=2000] [--out file.txt]
Stacks are approximate: executable pointers preceded by call instructions,
not an unwind. Cached code and the module list assume no module reloads.
"""

import argparse
from bisect import bisect_right
from collections import Counter
import ctypes
from ctypes import wintypes as wt
from dataclasses import dataclass, field
import math
from pathlib import Path
import struct
import sys
import time

# -I excludes the script directory. Import only the adjacent helper module,
# and avoid creating __pycache__ files in this read-only diagnostic workflow.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
if sys.platform != "win32":
    raise SystemExit("stacksample requires Windows and 64-bit x86 Python")
from n3mon import (K32, ME32, INVALID_HANDLE, TH32CS_SNAPMODULE,
                   PROCESS_VM_READ, PROCESS_QUERY_LIMITED, find_pid, module_base)

THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040  # SUSPEND_RESUME, GET_CONTEXT, QUERY_INFORMATION
CONTEXT_CONTROL_INTEGER = 0x00100003
CONTEXT_SIZE = 1232
CONTEXT_FLAGS_OFFSET, RSP_OFFSET, RIP_OFFSET = 48, 152, 248
STACK_SIZE = 16 * 1024
DWORD_FAILURE = 0xFFFFFFFF
SYSTEM_MODULES = {"ntdll.dll", "kernelbase.dll", "kernel32.dll"}
WAIT_MODULES = {"ntdll.dll", "kernelbase.dll"}
WAIT_WORDS = ("wait", "delay", "sleep", "yield", "alertable", "signalandwait")


def bind(name, result, *arguments):
    function = getattr(K32, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


bind("CreateToolhelp32Snapshot", wt.HANDLE, wt.DWORD, wt.DWORD)
bind("OpenProcess", wt.HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
bind("OpenThread", wt.HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
bind("GetProcessIdOfThread", wt.DWORD, wt.HANDLE)
bind("GetCurrentProcessId", wt.DWORD)
bind("SuspendThread", wt.DWORD, wt.HANDLE)
bind("ResumeThread", wt.DWORD, wt.HANDLE)
bind("GetThreadContext", wt.BOOL, wt.HANDLE, ctypes.c_void_p)
bind("ReadProcessMemory", wt.BOOL, wt.HANDLE, ctypes.c_void_p,
     ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
bind("QueryPerformanceCounter", wt.BOOL, ctypes.POINTER(ctypes.c_int64))
bind("QueryPerformanceFrequency", wt.BOOL, ctypes.POINTER(ctypes.c_int64))


def winerror(operation):
    return ctypes.WinError(ctypes.get_last_error(), operation)


class Memory:
    def __init__(self, handle):
        self.handle = handle

    def read(self, address, size):
        buffer = ctypes.create_string_buffer(size)
        copied = ctypes.c_size_t()
        K32.ReadProcessMemory(self.handle, address, buffer, size, ctypes.byref(copied))
        # ERROR_PARTIAL_COPY can still supply useful bytes at a region boundary.
        return buffer.raw[:copied.value]

    def exact(self, address, size):
        data = self.read(address, size)
        if len(data) != size:
            raise ValueError(f"cannot read {size} bytes at {address:#x}")
        return data


@dataclass
class Module:
    base: int
    size: int
    name: str
    executable: list = field(default_factory=list)
    export_rvas: list = field(default_factory=list)
    export_names: list = field(default_factory=list)

    def nearest_export(self, address):
        index = bisect_right(self.export_rvas, address - self.base) - 1
        if index < 0:
            return None
        return self.export_names[index], address - self.base - self.export_rvas[index]


def enumerate_modules(pid):
    # A loader update can produce ERROR_BAD_LENGTH; retry a few times.
    for _ in range(8):
        snapshot = K32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
        if snapshot not in (None, INVALID_HANDLE):
            break
        if ctypes.get_last_error() != 24:
            raise winerror("CreateToolhelp32Snapshot(modules)")
        time.sleep(0.01)
    else:
        raise winerror("CreateToolhelp32Snapshot(modules)")
    modules = []
    try:
        entry = ME32()
        entry.dwSize = ctypes.sizeof(entry)
        if not K32.Module32FirstW(snapshot, ctypes.byref(entry)):
            raise winerror("Module32FirstW")
        while True:
            modules.append(Module(entry.modBaseAddr, entry.modBaseSize, entry.szModule))
            if not K32.Module32NextW(snapshot, ctypes.byref(entry)):
                if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise winerror("Module32NextW")
                break
    finally:
        K32.CloseHandle(snapshot)
    return sorted(modules, key=lambda module: module.base)


def parse_image(memory, module):
    """Read PE sections and named, non-forwarded exports from the live image."""
    def image_read(rva, size):
        if rva < 0 or size < 0 or rva + size > module.size:
            raise ValueError("PE data outside module")
        return memory.exact(module.base + rva, size)

    dos = image_read(0, 64)
    if dos[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe_offset = struct.unpack_from("<I", dos, 60)[0]
    header = image_read(pe_offset, 24)
    if header[:4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    section_count = struct.unpack_from("<H", header, 6)[0]
    optional_size = struct.unpack_from("<H", header, 20)[0]
    optional = image_read(pe_offset + 24, optional_size)
    if len(optional) < 112 or struct.unpack_from("<H", optional)[0] != 0x20B:
        raise ValueError("not a PE32+ image")
    sections = image_read(pe_offset + 24 + optional_size, section_count * 40)
    for offset in range(0, len(sections), 40):
        virtual_size, rva, raw_size = struct.unpack_from("<III", sections, offset + 8)
        characteristics = struct.unpack_from("<I", sections, offset + 36)[0]
        if characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            end = min(module.size, rva + max(virtual_size, raw_size))
            if rva < end:
                module.executable.append((module.base + rva, module.base + end))

    if module.name.lower() not in SYSTEM_MODULES:
        return
    if struct.unpack_from("<I", optional, 108)[0] == 0 or len(optional) < 120:
        return
    export_rva, export_size = struct.unpack_from("<II", optional, 112)
    if not export_rva or not export_size:
        return
    directory = image_read(export_rva, 40)
    function_count, name_count, functions_rva, names_rva, ordinals_rva = (
        struct.unpack_from("<IIIII", directory, 20))
    functions = image_read(functions_rva, function_count * 4)
    names = image_read(names_rva, name_count * 4)
    ordinals = image_read(ordinals_rva, name_count * 2)
    exports = {}
    for index in range(name_count):
        ordinal = struct.unpack_from("<H", ordinals, index * 2)[0]
        if ordinal >= function_count:
            continue
        rva = struct.unpack_from("<I", functions, ordinal * 4)[0]
        if not rva or export_rva <= rva < export_rva + export_size:
            continue  # Forwarders are strings, not function addresses.
        address = module.base + rva
        if not any(start <= address < end for start, end in module.executable):
            continue
        name_rva = struct.unpack_from("<I", names, index * 4)[0]
        raw_name = image_read(name_rva, min(512, module.size - name_rva))
        if b"\0" not in raw_name:
            continue
        name = raw_name.split(b"\0", 1)[0].decode("ascii", "replace")
        # Prefer Nt over Zw at aliased syscall entries; otherwise be deterministic.
        old = exports.get(rva)
        if old is None or (not name.startswith("Nt"), name) < (not old.startswith("Nt"), old):
            exports[rva] = name
    module.export_rvas = sorted(exports)
    module.export_names = [exports[rva] for rva in module.export_rvas]


def looks_like_call(data):
    """Recognise E8 rel32 and unprefixed FF /2 encodings ending at data[-1]."""
    if len(data) >= 5 and data[-5] == 0xE8:
        return True
    for length in (2, 3, 6, 7):
        instruction = data[-length:]
        if len(instruction) != length or instruction[0] != 0xFF:
            continue
        modrm = instruction[1]
        if (modrm >> 3) & 7 != 2:
            continue
        mod, rm = modrm >> 6, modrm & 7
        expected = 2
        if mod != 3 and rm == 4:
            if length < 3:
                continue
            expected += 1  # SIB byte
            if mod == 0 and instruction[2] & 7 == 5:
                expected += 4
        elif mod == 0 and rm == 5:
            expected += 4  # RIP-relative disp32
        if mod == 1:
            expected += 1
        elif mod == 2:
            expected += 4
        if length == expected:
            return True
    return False


class Symbols:
    def __init__(self, memory, modules, main):
        self.memory, self.modules, self.main = memory, modules, main
        self.bases = [module.base for module in modules]
        ranges = sorted(section for module in modules for section in module.executable)
        self.code_starts = [start for start, _ in ranges]
        self.code_ends = [end for _, end in ranges]
        self.call_cache = {}
        self.symbol_cache = {}

    def module_at(self, address):
        index = bisect_right(self.bases, address) - 1
        if index >= 0:
            module = self.modules[index]
            if address < module.base + module.size:
                return module
        return None

    def is_code(self, address):
        index = bisect_right(self.code_starts, address) - 1
        return index >= 0 and address < self.code_ends[index]

    def symbol(self, address):
        if address not in self.symbol_cache:
            module = self.module_at(address)
            if module is None:
                value = f"0x{address:X}"
            else:
                export = module.nearest_export(address) if module is not self.main else None
                if export:
                    name, offset = export
                    value = f"{module.name.rsplit('.', 1)[0]}!{name}+0x{offset:X}"
                else:
                    value = f"{module.name}+0x{address - module.base:04X}"
            self.symbol_cache[address] = value
        return self.symbol_cache[address]

    def is_wait(self, address):
        module = self.module_at(address)
        if module is None or module.name.lower() not in WAIT_MODULES:
            return False
        export = module.nearest_export(address)
        return bool(export and any(word in export[0].lower() for word in WAIT_WORDS))

    def frames(self, rip, stack):
        frames = [rip]
        for (address,) in struct.iter_unpack("<Q", stack[:len(stack) // 8 * 8]):
            if not self.is_code(address):
                continue
            if address not in self.call_cache:
                self.call_cache[address] = looks_like_call(self.memory.read(address - 7, 7))
            if self.call_cache[address]:
                frames.append(address)
                if len(frames) == 12:
                    break
        return frames


class Clock:
    def __init__(self):
        self.counter = ctypes.c_int64()
        frequency = ctypes.c_int64()
        if not K32.QueryPerformanceFrequency(ctypes.byref(frequency)):
            raise winerror("QueryPerformanceFrequency")
        self.frequency = frequency.value

    def now(self):
        K32.QueryPerformanceCounter(ctypes.byref(self.counter))
        return self.counter.value

    def until(self, deadline):
        while True:
            remaining = (deadline - self.now()) / self.frequency
            if remaining <= 0:
                return
            if remaining > 0.002:
                time.sleep(remaining - 0.001)
            elif remaining > 0.0002:
                time.sleep(0)  # Yield, then busy-wait for the final 200 us.


class Capture:
    def __init__(self, process, thread, clock):
        self.process, self.thread, self.clock = process, thread, clock
        # Windows CONTEXT has 16-byte alignment, unlike a ctypes byte array.
        self.storage = ctypes.create_string_buffer(CONTEXT_SIZE + 15)
        self.address = (ctypes.addressof(self.storage) + 15) & ~15
        self.context = (ctypes.c_ubyte * CONTEXT_SIZE).from_address(self.address)
        self.stack = ctypes.create_string_buffer(STACK_SIZE)
        self.copied = ctypes.c_size_t()

    def sample(self):
        struct.pack_into("<I", self.context, CONTEXT_FLAGS_OFFSET, CONTEXT_CONTROL_INTEGER)
        self.copied.value = 0
        previous = DWORD_FAILURE
        started = self.clock.now()
        try:
            previous = K32.SuspendThread(self.thread)
            if previous == DWORD_FAILURE:
                raise winerror("SuspendThread")
            if not K32.GetThreadContext(self.thread, self.address):
                raise winerror("GetThreadContext")
            rsp = struct.unpack_from("<Q", self.context, RSP_OFFSET)[0]
            K32.ReadProcessMemory(self.process, rsp, self.stack, STACK_SIZE,
                                 ctypes.byref(self.copied))
        finally:
            if previous != DWORD_FAILURE:
                # Decrement exactly once, preserving any pre-existing suspension.
                if K32.ResumeThread(self.thread) == DWORD_FAILURE:
                    raise winerror("ResumeThread")
        finished = self.clock.now()
        rip = struct.unpack_from("<Q", self.context, RIP_OFFSET)[0]
        return rip, self.stack.raw[:self.copied.value], finished - started


def require_x64(process):
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise ValueError("use 64-bit x86 Python")
    if hasattr(K32, "IsWow64Process2"):
        function = bind("IsWow64Process2", wt.BOOL, wt.HANDLE,
                        ctypes.POINTER(wt.WORD), ctypes.POINTER(wt.WORD))
        machine, native = wt.WORD(), wt.WORD()
        if not function(process, ctypes.byref(machine), ctypes.byref(native)):
            raise winerror("IsWow64Process2")
        if (machine.value or native.value) != 0x8664:
            raise ValueError("target must be an x64 process")
    else:
        function = bind("IsWow64Process", wt.BOOL, wt.HANDLE, ctypes.POINTER(wt.BOOL))
        wow64 = wt.BOOL()
        if not function(process, ctypes.byref(wow64)):
            raise winerror("IsWow64Process")
        if wow64.value:
            raise ValueError("target must be an x64 process")


def report(exe, pid, args, modules, symbols, leaves, stacks, waits, wait_leaves,
           elapsed, suspend_ticks, clock, partial, interrupted):
    total, waiting = sum(leaves.values()), sum(waits.values())
    percent = lambda count, denominator: 100 * count / denominator if denominator else 0.0
    mean_us = suspend_ticks / total / clock.frequency * 1e6 if total else 0.0
    lines = [f"Stack sample: {exe} pid={pid} tid={args.tid}",
             f"Samples: {total} in {elapsed:.3f} s; achieved rate: {total / elapsed:.1f} Hz "
             f"(requested {args.hz:g} Hz)",
             f"Mean suspend duration: {mean_us:.2f} us (SuspendThread through ResumeThread)",
             f"Stack reads shorter than 16 KB: {partial}",
             "Approximate stacks: call-filtered stack scan, at most 12 frames; no unwind info."]
    if interrupted:
        lines.append("Stopped by Ctrl+C; thread resumed.")
    lines.extend(["", f"Loaded modules ({len(modules)}): base / size / name"])
    for module in modules:
        lines.append(f"  0x{module.base:016X}  0x{module.size:08X}  {module.name}")
    for heading, counts, describe in (
        ("Top 40 leaf locations", leaves, symbols.symbol),
        ("Top 40 stacks (first 6 frames, leaf first)", stacks,
         lambda frames: " <- ".join(symbols.symbol(address) for address in frames)),
    ):
        lines.extend(["", heading, "  count    % all  location"])
        for value, count in counts.most_common(40):
            lines.append(f"  {count:5d}  {percent(count, total):7.2f}%  {describe(value)}")
        if not counts:
            lines.append("  (none)")
    lines.extend(["", f"Who waits: {waiting}/{total} samples ({percent(waiting, total):.2f}%)",
                  "  count    % all  % waits  first main-exe frame"])
    for address, count in waits.most_common():
        location = symbols.symbol(address) if address is not None else "[no main-exe frame in first 12 frames]"
        lines.append(f"  {count:5d}  {percent(count, total):7.2f}%  "
                     f"{percent(count, waiting):7.2f}%  {location}")
    if not waits:
        lines.append("  (none)")
    lines.extend(["", "Wait/sleep leaf functions (nearest exports):", "  count  % waits  location"])
    for address, count in wait_leaves.most_common(40):
        lines.append(f"  {count:5d}  {percent(count, waiting):7.2f}%  {symbols.symbol(address)}")
    if not wait_leaves:
        lines.append("  (none)")
    return "\n".join(lines) + "\n"


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", help="executable name, e.g. python.exe")
    parser.add_argument("tid", type=lambda value: int(value, 16) if value.lower().startswith("0x") else int(value),
                        help="thread ID (decimal or 0x hexadecimal)")
    parser.add_argument("seconds", nargs="?", type=positive, default=10.0)
    parser.add_argument("hz", nargs="?", type=positive, default=2000.0)
    parser.add_argument("--out", type=Path, help="also write the summary to this file")
    args = parser.parse_args()
    if not 0 < args.tid <= 0xFFFFFFFF:
        parser.error("tid must be between 1 and 0xFFFFFFFF")
    pid = find_pid(args.exe)
    if pid is None:
        raise ValueError(f"process not found: {args.exe}")
    thread = K32.OpenThread(THREAD_ACCESS, False, args.tid)
    if not thread:
        raise winerror("OpenThread")
    process = None
    try:
        owner = K32.GetProcessIdOfThread(thread)
        if not owner:
            raise winerror("GetProcessIdOfThread")
        # With multiple instances, TID identifies which instance of <exe> to sample.
        # Validate its main module before opening or suspending anything else.
        base, _ = module_base(owner, args.exe)
        if not base:
            raise ValueError(f"thread {args.tid} does not belong to {args.exe}")
        pid = owner
        if pid == K32.GetCurrentProcessId():
            raise ValueError("cannot sample the sampler's own process")
        process = K32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED, False, pid)
        if not process:
            raise winerror("OpenProcess")
        require_x64(process)
        memory = Memory(process)
        modules = enumerate_modules(pid)
        main_module = next((module for module in modules if module.base == base), None)
        if main_module is None:
            raise ValueError("main executable disappeared from the module list")
        for module in modules:
            try:
                parse_image(memory, module)
            except (ValueError, struct.error) as error:
                if module is main_module or module.name.lower() in SYSTEM_MODULES:
                    raise ValueError(f"cannot parse {module.name}: {error}") from error
                print(f"warning: cannot parse {module.name}: {error}", file=sys.stderr)
        symbols = Symbols(memory, modules, main_module)
        clock = Clock()
        capture = Capture(process, thread, clock)
        leaves, stacks, waits, wait_leaves = Counter(), Counter(), Counter(), Counter()
        suspend_ticks = partial = 0
        interrupted = False
        started = clock.now()
        deadline = started + args.seconds * clock.frequency
        period = clock.frequency / args.hz
        next_sample = started
        try:
            while clock.now() < deadline:
                clock.until(min(next_sample, deadline))
                if clock.now() >= deadline:
                    break
                rip, stack, ticks = capture.sample()
                suspend_ticks += ticks
                partial += len(stack) < STACK_SIZE
                frames = symbols.frames(rip, stack)
                leaves[rip] += 1
                stacks[tuple(frames[:6])] += 1
                if symbols.is_wait(rip):
                    first_main = next((address for address in frames
                                       if main_module.base <= address < main_module.base + main_module.size), None)
                    waits[first_main] += 1
                    wait_leaves[rip] += 1
                # Skip missed slots instead of taking a burst of catch-up samples.
                now = clock.now()
                next_sample = started + max(1, math.ceil((now - started) / period)) * period
        except KeyboardInterrupt:
            interrupted = True
        elapsed = (clock.now() - started) / clock.frequency
        summary = report(args.exe, pid, args, modules, symbols, leaves, stacks, waits,
                         wait_leaves, elapsed, suspend_ticks, clock, partial, interrupted)
        print(summary, end="")
        if args.out:
            args.out.write_text(summary, encoding="utf-8")
    finally:
        if process:
            K32.CloseHandle(process)
        K32.CloseHandle(thread)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"stacksample: {error}", file=sys.stderr)
        sys.exit(1)
