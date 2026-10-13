"""Nioh 3 clock monitor - READ-ONLY (ReadProcessMemory only, no writes, no injection).

Usage: python -I n3mon.py <out.csv> [seconds]
Waits for Nioh3.exe, polls the clock object as fast as possible and logs every
change. Press F9 in-game to drop a numbered marker into the log.
"""
import csv, ctypes, struct, sys, time
from ctypes import wintypes as wt

K32 = ctypes.WinDLL('kernel32', use_last_error=True)
U32 = ctypes.WinDLL('user32')
PROCESS_VM_READ, PROCESS_QUERY_LIMITED = 0x10, 0x1000
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10

# RVAs from static analysis of Nioh3.exe.unpacked.exe
RVA_G = 0x4759910          # qword ptr -> clock/app object
RVA_SCENE = 0x45ce800      # qword ptr -> scene manager (cutscene check fn 0x123aa20)
RVA_PATCH = 0x5ad6a2       # planned patch site
AOB_PATCH = bytes.fromhex('F30F109728060000F30F119724060000')

class PE32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_wchar * 260)]

class ME32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('GlblcntUsage', wt.DWORD), ('ProccntUsage', wt.DWORD),
                ('modBaseAddr', ctypes.c_void_p), ('modBaseSize', wt.DWORD), ('hModule', wt.HMODULE),
                ('szModule', ctypes.c_wchar * 256), ('szExePath', ctypes.c_wchar * 260)]

K32.CreateToolhelp32Snapshot.restype = wt.HANDLE
K32.OpenProcess.restype = wt.HANDLE
K32.CloseHandle.argtypes = [wt.HANDLE]
INVALID_HANDLE = wt.HANDLE(-1).value
K32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
for f in (K32.Process32FirstW, K32.Process32NextW):
    f.argtypes = [wt.HANDLE, ctypes.POINTER(PE32)]
for f in (K32.Module32FirstW, K32.Module32NextW):
    f.argtypes = [wt.HANDLE, ctypes.POINTER(ME32)]

def find_pid(name):
    snap = K32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (None, INVALID_HANDLE):
        return None
    e = PE32(); e.dwSize = ctypes.sizeof(e); ok = K32.Process32FirstW(snap, ctypes.byref(e))
    try:
        while ok:
            if e.szExeFile.lower() == name.lower():
                return e.th32ProcessID
            ok = K32.Process32NextW(snap, ctypes.byref(e))
    finally:
        K32.CloseHandle(snap)

def module_base(pid, name):
    snap = K32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap in (None, INVALID_HANDLE):   # fails briefly while the process is starting
        return None, None
    e = ME32(); e.dwSize = ctypes.sizeof(e); ok = K32.Module32FirstW(snap, ctypes.byref(e))
    try:
        while ok:
            if e.szModule.lower() == name.lower():
                return e.modBaseAddr, e.modBaseSize
            ok = K32.Module32NextW(snap, ctypes.byref(e))
    finally:
        K32.CloseHandle(snap)
    return None, None

class Proc:
    def __init__(self, pid):
        self.h = K32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED, False, pid)
        if not self.h:
            raise OSError(f'OpenProcess failed: {ctypes.get_last_error()}')
        self.buf = ctypes.create_string_buffer(0x10000); self.n = ctypes.c_size_t()
    def read(self, addr, size):
        if not addr or not K32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), self.buf, size, ctypes.byref(self.n)):
            return None
        return self.buf.raw[:self.n.value]
    def q(self, addr):
        b = self.read(addr, 8); return struct.unpack('<Q', b)[0] if b else 0

def check_aob(p, base):
    """Find the patch AOB in live .text; report its RVA(s)."""
    hdr = p.read(base, 0x1000); e = struct.unpack_from('<I', hdr, 0x3c)[0]
    nsec = struct.unpack_from('<H', hdr, e + 6)[0]; opt = struct.unpack_from('<H', hdr, e + 20)[0]
    hits = []
    for i in range(nsec):
        s = e + 24 + opt + i * 40
        if hdr[s:s + 5] != b'.text':
            continue
        vs, va = struct.unpack_from('<II', hdr, s + 8)
        for off in range(0, vs, 0x10000 - 64):
            chunk = p.read(base + va + off, min(0x10000, vs - off))
            if not chunk:
                continue
            i2 = chunk.find(AOB_PATCH)
            while i2 != -1:
                rva = va + off + i2
                if rva not in hits:
                    hits.append(rva)
                i2 = chunk.find(AOB_PATCH, i2 + 1)
    return hits

def main():
    out = sys.argv[1]; dur = float(sys.argv[2]) if len(sys.argv) > 2 else 900
    print('waiting for Nioh3.exe ...', flush=True)
    while not (pid := find_pid('Nioh3.exe')):
        time.sleep(1)
    base = None
    while not base:
        base, _ = module_base(pid, 'Nioh3.exe'); time.sleep(0.5)
    p = Proc(pid)
    while not p.q(base + RVA_G):          # object exists once the game is up
        time.sleep(0.5)
    hits = check_aob(p, base)
    print(f'pid={pid} base={base:#x} obj={p.q(base + RVA_G):#x}')
    print(f'patch AOB hits (RVA): {[hex(h) for h in hits]}  expected [{RVA_PATCH:#x}]', flush=True)

    qpf = ctypes.c_int64(); K32.QueryPerformanceFrequency(ctypes.byref(qpf)); f = qpf.value
    c = ctypes.c_int64()
    def now():
        K32.QueryPerformanceCounter(ctypes.byref(c)); return c.value / f

    cols = ['t', 'marker', 'fps', 'fps_req618', 'f61c', 'f620', 'speed_eff', 'speed_req', 'acc60',
            'pause58b', 'pause58c', 'f634', 'f638', 'f64b', 'scene_idx', 'scene_s40', 'scene_f11b', 'obj']
    t0 = now(); marker = 0; f9_prev = False; last = None; last_m = 0; rows = 0
    with open(out, 'w', newline='') as fh:
        w = csv.writer(fh); w.writerow(cols)
        while (t := now() - t0) < dur:
            f9 = bool(U32.GetAsyncKeyState(0x78) & 0x8000)
            if f9 and not f9_prev:
                marker += 1; print(f'marker {marker} at {t:.2f}s', flush=True)
            f9_prev = f9
            obj = p.q(base + RVA_G)
            blk = p.read(obj + 0x580, 0xd0) if obj else None
            if blk is None:
                if not find_pid('Nioh3.exe'):
                    break
                continue
            g = lambda fmt, off: struct.unpack_from(fmt, blk, off - 0x580)[0]
            sm = p.q(base + RVA_SCENE); sidx = s40 = s11b = -1
            if sm and (sb := p.read(sm + 0x1b88, 4)):
                sidx = struct.unpack('<I', sb)[0]
                if sidx < 9 and (e := p.read(sm + sidx * 0x308 + 0x40, 0xe0)):
                    s40 = struct.unpack_from('<i', e, 0)[0]; s11b = e[0x11b - 0x40]
            vals = (g('<i', 0x614), g('<i', 0x618), g('<i', 0x61c), g('<i', 0x620),
                    round(g('<f', 0x624), 6), round(g('<f', 0x628), 6), round(g('<f', 0x62c), 6),
                    blk[0x58b - 0x580], blk[0x58c - 0x580], g('<i', 0x634), g('<i', 0x638),
                    blk[0x64b - 0x580], sidx, s40, s11b, hex(obj))
            if vals != last or marker != last_m:
                w.writerow((f'{t:.6f}', marker) + vals); rows += 1; last = vals; last_m = marker
    print(f'done: {rows} rows -> {out}')

if __name__ == '__main__':
    main()
