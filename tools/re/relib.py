"""Reusable static RE helpers for x64 PE (Katana engine games).
Usage: import relib; im = relib.Img(path)
"""
import sys, os, bisect, struct, re as _re, pickle
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone import x86 as cx

class Img:
    def __init__(self, path, cache=True):
        self.path = path
        cp = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          os.path.basename(path) + '.cache.pkl')
        if cache and os.path.exists(cp) and os.path.getmtime(cp) > os.path.getmtime(path):
            d = pickle.load(open(cp, 'rb'))
            self.__dict__.update(d)
        else:
            self._load()
            if cache:
                pickle.dump({k: v for k, v in self.__dict__.items() if k not in ('md',)}, open(cp, 'wb'))
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        self.arr = np.frombuffer(self.mem, dtype=np.uint8)

    def _load(self):
        pe = pefile.PE(self.path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                                               pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytes(pe.get_memory_mapped_image())
        self.sections = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in pe.sections}
        self.imports = {}
        for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in e.imports:
                if i.name:
                    self.imports[i.address - self.base] = i.name.decode()
        self.funcs = []
        for f in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', []):
            self.funcs.append((f.struct.BeginAddress, f.struct.EndAddress))
        self.funcs.sort()
        self.fstarts = [a for a, b in self.funcs]
        pe.close()

    # ---- basics
    def sec(self, name):
        a, n = self.sections[name]; return a, a + n
    def u32(self, r): return struct.unpack_from('<I', self.mem, r)[0]
    def i32(self, r): return struct.unpack_from('<i', self.mem, r)[0]
    def u64(self, r): return struct.unpack_from('<Q', self.mem, r)[0]
    def f32(self, r): return struct.unpack_from('<f', self.mem, r)[0]
    def f64(self, r): return struct.unpack_from('<d', self.mem, r)[0]
    def cstr(self, r, n=200):
        e = self.mem.find(b'\0', r, r + n); return self.mem[r:e].decode('latin1')
    def imp(self, name):
        return [r for r, n in self.imports.items() if n == name]

    # ---- strings
    def find_str(self, s, exact=True, secs=('.rdata', '.data')):
        """RVAs of string s (ascii). exact: preceded and followed by NUL."""
        b = s.encode() if isinstance(s, str) else s
        out = []
        for sn in secs:
            a, e = self.sec(sn)
            i = self.mem.find(b, a, e)
            while i != -1:
                if not exact or (self.mem[i - 1] == 0 and self.mem[i + len(b)] == 0):
                    out.append(i)
                i = self.mem.find(b, i + 1, e)
        return out

    def find_bytes(self, b, secs=('.rdata', '.data', '.text')):
        out = []
        for sn in secs:
            a, e = self.sec(sn)
            i = self.mem.find(b, a, e)
            while i != -1:
                out.append(i); i = self.mem.find(b, i + 1, e)
        return out

    # ---- xrefs
    def xrefs_raw(self, targets, slack=(0, 1, 2, 4)):
        """positions p of disp32 such that p+4+disp+k == target (k = trailing imm size)."""
        a, e = self.sec('.text')
        n = e - a - 4
        d = np.ndarray(shape=(n,), dtype='<i4', buffer=self.mem, offset=a, strides=(1,))
        tg = np.array(sorted(set(targets)), dtype=np.int64)
        hits = set()
        CH = 1 << 23
        for c in range(0, n, CH):
            dd = d[c:c + CH].astype(np.int64)
            pos = np.arange(a + c, a + c + len(dd), dtype=np.int64)
            t = pos + 4 + dd
            for k in slack:
                m = np.isin(t + k, tg)
                if m.any():
                    hits.update(pos[m].tolist())
        return sorted(hits)

    def xrefs(self, target, slack=(0, 1, 2, 4)):
        """List of (insn_rva, func_start, insn_text) referencing target via rip-relative / rel32."""
        out = []
        for p in self.xrefs_raw([target] if isinstance(target, int) else target, slack):
            f = self.func_of(p)
            ins = self.insn_at(p, f)
            if ins is None:
                continue
            tgt = self.ref_target(ins)
            tl = [target] if isinstance(target, int) else target
            if tgt in tl:
                out.append((ins.address, f[0] if f else None, f'{ins.mnemonic} {ins.op_str}'))
        return out

    def ref_target(self, ins):
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            if len(ins.operands) == 1 and ins.operands[0].type == cx.X86_OP_IMM:
                return ins.operands[0].imm
        for o in ins.operands:
            if o.type == cx.X86_OP_MEM and o.mem.base == cx.X86_REG_RIP:
                return ins.address + ins.size + o.mem.disp
        return None

    # ---- functions
    def func_of(self, r):
        i = bisect.bisect_right(self.fstarts, r) - 1
        if i >= 0 and self.funcs[i][0] <= r < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def insn_at(self, p, f=None):
        """Instruction containing byte p, decoding linearly from function start."""
        f = f or self.func_of(p)
        start = f[0] if f else max(p - 64, 0)
        end = (f[1] if f else p + 16)
        for ins in self.md.disasm(self.mem[start:end], start):
            if ins.address <= p < ins.address + ins.size:
                return ins
            if ins.address > p:
                break
        if not f:
            return None
        # fallback: try local resync
        for back in range(1, 16):
            for ins in self.md.disasm(self.mem[p - back:p + 16], p - back, 1):
                if ins.address + ins.size > p:
                    return ins
        return None

    def annotate(self, ins):
        t = self.ref_target(ins)
        if t is None:
            return ''
        if t in self.imports:
            return f'  ; [{self.imports[t]}]'
        s = ''
        a, e = self.sec('.rdata')
        if a <= t < e:
            c = self.cstr(t, 60)
            if len(c) >= 3 and all(32 <= ord(ch) < 127 for ch in c):
                s = f'  ; "{c}"'
            else:
                s = f'  ; rdata {t:#x} f32={self.f32(t):.7g} f64={self.f64(t):.7g}'
        elif t in self.fstarts:
            s = f'  ; sub_{t:x}'
        else:
            s = f'  ; {t:#x}'
        return s

    def dis(self, start, end=None, maxn=400, out=None):
        if end is None:
            f = self.func_of(start); end = f[1] if f else start + 0x200
        lines = []
        n = 0
        for ins in self.md.disasm(self.mem[start:end], start):
            lines.append(f'{ins.address:08x}: {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}{self.annotate(ins)}')
            n += 1
            if n >= maxn: break
        txt = '\n'.join(lines)
        if out: open(out, 'w').write(txt)
        return txt

    def callers(self, fstart):
        return [x for x in self.xrefs(fstart, slack=(0,)) if x[2].startswith(('call', 'jmp'))]

    # ---- AOB
    def aob(self, start, n=48):
        """Signature from start: wildcard rip-relative disp32 and rel32 branch targets."""
        out = []
        p = start
        for ins in self.md.disasm(self.mem[start:start + n + 16], start):
            if p - start >= n: break
            b = ins.bytes.hex(' ').upper().split()
            wild = set()
            if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
                if ins.operands and ins.operands[0].type == cx.X86_OP_IMM and ins.size >= 5:
                    wild = set(range(ins.size - 4, ins.size))
            dof = ins.disp_offset
            for o in ins.operands:
                if o.type == cx.X86_OP_MEM and o.mem.base == cx.X86_REG_RIP and dof:
                    wild |= set(range(dof, dof + 4))
            out += ['??' if i in wild else x for i, x in enumerate(b)]
            p += ins.size
        return ' '.join(out)

    def aob_search(self, pat, secs=('.text',), limit=20):
        rx = b''.join(b'.' if t == '??' else _re.escape(bytes([int(t, 16)])) for t in pat.split())
        rxc = _re.compile(rx, _re.S)
        out = []
        for sn in secs:
            a, e = self.sec(sn)
            for m in rxc.finditer(self.mem, a, e):
                out.append(m.start())
                if len(out) >= limit: return out
        return out
