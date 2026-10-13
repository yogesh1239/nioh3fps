"""S0.5 generator: probe_targets.csv, sites.csv and README.md under docs/s05.

Run from anywhere:  python -I tools/s05/gen.py
Static and read-only on Nioh3.exe.unpacked.exe; deterministic (all sets sorted).
"""
import sys, os, re, csv, struct, bisect, collections
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import relib
from capstone import x86 as cx

EXE = r'G:\SteamLibrary\steamapps\common\Nioh3\Nioh3.exe.unpacked.exe'
OUT = os.path.join(ROOT, 'docs', 's05')
SITES_IN = os.path.join(ROOT, 'tools', 'review', 'rev-sites', 'sites_classified.csv')
G = 0x4759910          # clock global
GETFPS, GETTS = 0x46594, 0x46484
MAIN_UPD = 0x39e4e4    # P2 function (F9)
P2 = 0x39e526

N = relib.Img(EXE)
TEXT = N.sec('.text')
R64 = {'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'}

def h(x): return f'{x:#x}'

# ---------------------------------------------------------------- .pdata chains
PD = {}  # begin -> (end, unwind)
a, n = N.sections['.pdata']
for off in range(a, a + n - 11, 12):
    b, e, u = struct.unpack_from('<III', N.mem, off)
    if b == 0: continue
    PD[b] = (e, u)

def primary(begin):
    """Follow UNW_FLAG_CHAININFO to the primary function entry."""
    seen = 0
    while begin in PD and seen < 32:
        u = PD[begin][1]
        flags = N.mem[u] >> 3
        if not flags & 4: return begin
        cnt = N.mem[u + 2]
        c = u + 4 + 2 * ((cnt + 1) & ~1)
        begin = N.u32(c); seen += 1
    return begin

FRAGS = collections.defaultdict(list)   # primary -> chained fragment begins
for b in sorted(PD):
    pb = primary(b)
    if pb != b: FRAGS[pb].append(b)

def leaf_start(r):
    """Start of a leaf function not covered by .pdata (walk back to padding/ret)."""
    p = r
    while p > TEXT[0] and N.mem[p - 1] not in (0xcc, 0xc3):
        p -= 1
        if r - p > 0x400: return None
    return p

# leaf functions without .pdata named in the findings (F6, F7c); 0x46484 follows a tail jmp, not padding
KNOWN_LEAF = [0x2a0a44, 0x42dde4, 0x434df4, 0x46398, 0x46484, 0x46594, 0x26f4768, 0x26f4780, 0xe3a750]

def containing(r):
    f = N.func_of(r)
    if f: return primary(f[0])
    for k in sorted(KNOWN_LEAF, reverse=True):
        if k <= r < fn_end(k): return k
    return leaf_start(r)

def fn_end(f):
    if f in PD: return PD[f][0]
    for ins in N.md.disasm(N.mem[f:f + 0x400], f):
        if ins.mnemonic in ('ret', 'int3') or (ins.mnemonic == 'jmp' and ins.operands[0].type == cx.X86_OP_IMM):
            return ins.address + ins.size
    return f + 0x40

# ---------------------------------------------------------------- relocations
RELOC = set()
a, n = N.sections['.reloc']
p = a
while p < a + n - 8:
    va, sz = struct.unpack_from('<II', N.mem, p)
    if sz < 8: break
    for i in range(p + 8, p + sz, 2):
        ent = struct.unpack_from('<H', N.mem, i)[0]
        if ent >> 12 == 10:
            r = va + (ent & 0xfff)
            RELOC.update(range(r, r + 8))
    p += sz

# ---------------------------------------------------------------- F7b inline +0x614 reads
def inline_614():
    out = {}
    for ia, f, t in N.xrefs(G):
        i0 = next(N.md.disasm(N.mem[ia:ia + 16], ia))
        if i0.mnemonic != 'mov': continue
        reg = i0.op_str.split(',')[0]
        if reg not in R64: continue
        for k, ins in enumerate(N.md.disasm(N.mem[ia + i0.size:ia + 400], ia + i0.size)):
            if k >= 40: break
            m = re.search(r'\[(\w+) \+ 0x614\]', ins.op_str)
            if m and m.group(1) == reg:
                out[ins.address] = f'{ins.mnemonic} {ins.op_str}'
            if ins.mnemonic in ('ret', 'call', 'jmp', 'int3'): break
            if ins.op_str.startswith(reg + ',') and ins.mnemonic not in ('test', 'cmp'): break
    for acc in (GETFPS, GETTS):          # the accessors themselves are retaddr_hist rows
        for s in [s for s in out if containing(s) == acc]: del out[s]
    return dict(sorted(out.items()))

# ---------------------------------------------------------------- F9 dt consumers of 0x39e4e4
def dt_consumers():
    """Direct calls in 0x39e4e4 whose argument xmm regs are loaded from xmm6/xmm7 (dt)."""
    res, pend = [], set()
    for ins in N.md.disasm(N.mem[MAIN_UPD:fn_end(MAIN_UPD)], MAIN_UPD):
        if ins.address < 0x39e543: continue          # dt not final before 0x39e543
        m = re.match(r'(xmm[0-3]), (xmm[67])$', ins.op_str)
        if ins.mnemonic in ('movaps', 'movss') and m: pend.add(f'{m.group(1)}={m.group(2)}')
        if ins.mnemonic == 'call':
            if pend and ins.operands[0].type == cx.X86_OP_IMM:
                res.append((ins.address, ins.operands[0].imm, ' '.join(sorted(pend))))
            pend = set()
    return res

# ---------------------------------------------------------------- MinHook 1.3.3 trampoline model
def is_pad(r, n): return all(b in (0x00, 0x90, 0xcc) for b in N.mem[r:r + n])

def minhook(f):
    """Simulate CreateTrampolineFunction. Returns (ok, copied_len, prologue_insns, note)."""
    pos, jmp_dest, insns, note = 0, 0, [], []
    finished = False
    while not finished:
        p = f + pos
        try: ins = next(N.md.disasm(N.mem[p:p + 15], p, 1))
        except StopIteration: return False, pos, insns, 'undecodable byte'
        b = ins.bytes
        insns.append(ins)
        if b[0] in (0xc4, 0xc5, 0x62): return False, pos, insns, 'VEX/EVEX prologue (hde64 cannot decode)'
        length_changes = False
        op = ins.mnemonic
        if op == 'call' and b[0] == 0xe8: length_changes = True
        elif op == 'jmp' and b[0] in (0xeb, 0xe9):
            d = ins.operands[0].imm
            if f <= d < f + 5: jmp_dest = max(jmp_dest, d)
            else: length_changes = True; finished = p >= jmp_dest
        elif op.startswith('j') or op.startswith('loop') or op in ('jrcxz', 'jecxz'):
            if b[0] in (0xe0, 0xe1, 0xe2, 0xe3): return False, pos, insns, 'LOOP/JRCXZ in prologue'
            d = ins.operands[0].imm
            if f <= d < f + 5: jmp_dest = max(jmp_dest, d)
            else: length_changes = True
        elif op in ('ret', 'retf'): finished = p >= jmp_dest
        elif any(o.type == cx.X86_OP_MEM and o.mem.base == cx.X86_REG_RIP for o in ins.operands):
            if op == 'jmp': finished = p >= jmp_dest
        elif op == 'int3': return False, pos, insns, 'int3 in prologue'
        if p < jmp_dest and length_changes: return False, pos, insns, 'internal branch over resized insn'
        pos += ins.size
        if pos >= 5: finished = True
    if pos < 5:
        if is_pad(f + pos, 5 - pos): note.append(f'function is {pos} B; padding follows')
        elif pos >= 2 and is_pad(f - 5, 5): note.append(f'function is {pos} B; MinHook patches above (short jmp)')
        else: return False, pos, insns, f'function is {pos} B and no padding for MinHook'
    return True, pos, insns, '; '.join(note)

# ---------------------------------------------------------------- whole-image branch-into scan
def branch_into(fns):
    """{fn: [(src, kind)]} for any reference to fn+1..fn+4: rel8, rel32/rip disp32 (decoded), RVA32 and VA64 values."""
    tgt = {f + k: f for f in fns for k in range(1, 5)}
    hits = collections.defaultdict(set)
    # rel32 / rip-relative (verified by decoding)
    for p in N.xrefs_raw(sorted(tgt), slack=(0, 1, 2, 4)):
        ins = N.insn_at(p) if N.func_of(p) else None
        cands = [ins] if ins is not None else []
        if ins is None:  # code outside .pdata: try every resync point
            for back in range(1, 12):
                for i in N.md.disasm(N.mem[p - back:p + 16], p - back, 1):
                    if i.address + i.size > p: cands.append(i)
        for i in cands:
            t = N.ref_target(i)
            if t in tgt:
                hits[tgt[t]].add((i.address, f'{i.mnemonic} {i.op_str}' + ('' if ins is not None else ' (non-.pdata code, unverified)')))
    # rel8 near the function
    for f in fns:
        for p in range(f - 0x82, f + 0x86):
            if N.mem[p] in (0xeb, 0xe0, 0xe1, 0xe2, 0xe3) or 0x70 <= N.mem[p] <= 0x7f:
                d = p + 2 + struct.unpack_from('<b', N.mem, p + 1)[0]
                if f < d < f + 5:
                    cf = N.func_of(p)
                    ins = N.insn_at(p, cf) if cf else None
                    if ins is not None and ins.address == p:
                        hits[f].add((p, f'{ins.mnemonic} {ins.op_str}'))
                    elif cf is None and not is_pad(p, 2):
                        hits[f].add((p, f'rel8 {N.mem[p]:02x} (non-.pdata code, unverified)'))
    # RVA32 (jump tables) and VA64 (pointers) anywhere in the image
    import numpy as np
    arr = N.arr
    for sn in ('.text', '.rdata', '.data', '_RDATA'):
        s0, s1 = N.sec(sn)
        s1 = min(s1, len(arr)) & ~3
        d32 = arr[s0:s1].view('<u4')
        for k in np.nonzero(np.isin(d32, np.array(sorted(tgt), dtype=np.uint32)))[0].tolist():
            r = s0 + 4 * k; v = int(d32[k])
            if sn == '.text' and N.func_of(r):
                ins = N.insn_at(r)
                if ins is not None and ins.address <= r < ins.address + ins.size and (N.ref_target(ins) is not None or ins.address != r):
                    continue  # bytes belong to a decoded instruction, not a table
            hits[tgt[v]].add((r, f'RVA32 value {v:#x} in {sn}'))
        s1q = s1 & ~7
        d64 = arr[s0:s1q].view('<u8')
        va = np.array(sorted(N.base + t for t in tgt), dtype=np.uint64)
        for k in np.nonzero(np.isin(d64, va))[0].tolist():
            r = s0 + 8 * k; v = int(d64[k]) - N.base
            hits[tgt[v]].add((r, f'VA64 pointer to {v:#x} in {sn}'))
    return {f: sorted(v) for f, v in hits.items()}

# ---------------------------------------------------------------- callers and vtable refs
def callers_of(fns):
    fset = set(fns)
    out = collections.defaultdict(set)
    for p in N.xrefs_raw(sorted(fset), slack=(0,)):
        f = N.func_of(p)
        if not f: continue
        ins = N.insn_at(p, f)
        if ins is None or ins.mnemonic not in ('call', 'jmp'): continue
        t = N.ref_target(ins)
        if t in fset: out[t].add((ins.address, primary(f[0]), ins.mnemonic))
    return {k: sorted(v) for k, v in out.items()}

def vtable_refs(fns):
    import numpy as np
    s0, s1 = N.sec('.rdata'); s1 &= ~7
    d64 = N.arr[s0:s1].view('<u8')
    va = {N.base + f: f for f in fns}
    out = collections.defaultdict(list)
    for k in np.nonzero(np.isin(d64, np.array(sorted(va), dtype=np.uint64)))[0].tolist():
        out[va[int(d64[k])]].append(s0 + 8 * k)
    return out

# ---------------------------------------------------------------- AOB
def aob_at(start, minlen=16, maxlen=96):
    toks, pos = [], start
    while pos - start < maxlen:
        try: ins = next(N.md.disasm(N.mem[pos:pos + 15], pos, 1))
        except StopIteration: break
        wild = set()
        if ins.operands and ins.operands[0].type == cx.X86_OP_IMM and ins.size >= 5 and \
                (ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j')):
            wild |= set(range(ins.size - 4, ins.size))
        if ins.disp_offset and any(o.type == cx.X86_OP_MEM and o.mem.base == cx.X86_REG_RIP for o in ins.operands):
            wild |= set(range(ins.disp_offset, ins.disp_offset + 4))
        wild |= {i for i in range(ins.size) if pos + i in RELOC}
        toks += ['??' if i in wild else f'{x:02X}' for i, x in enumerate(ins.bytes)]
        pos += ins.size
        if len(toks) >= minlen:
            pat = ' '.join(toks)
            m = aob_find(pat)
            if m == [start]: return pat, len(toks)
    return '', 0

def aob_for(f):
    """(pattern, offset): at the function start if unique, else at the first instruction inside it that is."""
    pat, _ = aob_at(f, maxlen=128)
    if pat: return pat, 0
    for b in [f] + FRAGS.get(f, []):   # primary range, then its chained fragments (same function)
        for ins in N.md.disasm(N.mem[b:fn_end(b)], b):
            if ins.address == f: continue
            pat, _ = aob_at(ins.address, maxlen=64)
            if pat: return pat, ins.address - f
    return '', 0

def aob_find(pat, limit=2):
    toks = pat.split()
    runs, cur, cs = [], [], 0
    for i, t in enumerate(toks + ['??']):
        if t == '??':
            if cur: runs.append((cs, bytes(int(x, 16) for x in cur)))
            cur = []
        else:
            if not cur: cs = i
            cur.append(t)
    off, key = max(runs, key=lambda r: len(r[1]))
    rx = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in toks), re.S)
    out, mem = [], N.mem
    i = mem.find(key)
    while i != -1:
        s = i - off
        if s >= 0 and rx.match(mem, s): out.append(s)
        if len(out) >= limit: break
        i = mem.find(key, i + 1)
    return out

# ================================================================ build target list
targets = collections.OrderedDict()   # fn -> dict
def add(fn, kind, name, src, note=''):
    t = targets.setdefault(fn, dict(kind=kind, names=[], srcs=[], notes=[]))
    if kind == 'retaddr_hist': t['kind'] = kind
    if name not in t['names']: t['names'].append(name)
    if src not in t['srcs']: t['srcs'].append(src)
    if note and note not in t['notes']: t['notes'].append(note)

sites = []  # (site, containing, chain_override, class, evidence)

# F7b
INL = inline_614()
for s, txt in INL.items():
    f = containing(s)
    add(f, 'entry_count', f'inline614_{f:x}', 'F7b', f'reads +0x614 at {s:#x}')
    sites.append((s, f, None, 'unknown', f'F7b inline +0x614 read: {txt}'))

# F7c raw speed readers
F7C = [0x42dde4, 0x434df4, 0x10f7de3, 0x26f4768, 0x26f4780, 0x2a0a44]
for r in F7C:
    f = containing(r)
    add(f, 'entry_count', f'speedreader_{f:x}', 'F7c', f'raw +0x624/+0x628 reader ({r:#x})')
    end = fn_end(f)
    rd = [i for i in N.md.disasm(N.mem[f:end], f) if re.search(r'\+ 0x62[48]\]', i.op_str)]
    for i in rd:
        if abs(i.address - r) < 0x40 or r == f:
            sites.append((i.address, f, None, 'unknown', f'F7c raw speed read: {i.mnemonic} {i.op_str}'))
    if not rd:  # 0x26f4768/0x26f4780 read other clock fields (+0x58/+0x5c), not +0x624/+0x628
        i = next(i for i in N.md.disasm(N.mem[f:end], f) if re.search(r'\[(?!rip)r\w+ \+ 0x[0-9a-f]+\]', i.op_str))
        targets[f]['notes'][-1] += f'; NOTE reads {i.op_str.split(", ")[1]} of the clock object, not +0x624/+0x628'
        sites.append((i.address, f, None, 'unknown', f'F7c listed reader, but reads clock {i.op_str.split(", ")[1]} (not +0x624/+0x628): {i.mnemonic} {i.op_str}'))
add(0x42dc6c, 'entry_count', 'speedreader_caller_42dc6c', 'F7c', 'sole caller (0x42dcc8) of leaf 0x42dde4')

# F7d
for r in (0x8c8e54, 0x5ab644):
    add(containing(r), 'entry_count', f'f7d_consumer_{r:x}', 'F7d', 'receives the 60/fps field built at 0x5ad7b8/0x5ad825')
for s in (0x5ad7b8, 0x5ad825):
    i = N.insn_at(s)
    sites.append((s, 0x5ad5f8, '0x5ad5f8<-0x5acf56(0x5acee8 task)', 'unknown', f'F7d 60/fps field: {i.mnemonic} {i.op_str}'))

# 0x46398 variant and F7e copy
add(0x46398, 'entry_count', 'timestep_variant_46398', 'F6/F7', 'no direct callers (F6)')
i = N.insn_at(0x46398, (0x46398, fn_end(0x46398)))
sites.append((0x46398, 0x46398, None, 'unknown', f'F6 variant 0x46398: {i.mnemonic} {i.op_str}'))
f = containing(0x5adc64)
add(f, 'entry_count', f'clock_copy_{f:x}', 'F7e', 'copies +0x614..+0x638 at 0x5adc64')
sites.append((0x5adc64, f, None, 'unknown', 'F7e clock copy into another clock object'))

# F9
sites.append((P2, MAIN_UPD, '0x5ac664->0x5ac9ec->0x564f40->0x564f60->0x565198->0x5651d0->0x39e4e4', 'scale',
              'F9/F9c/G1: dt=60/S.fps ignores speed; drives camera (0x8bee50->0x565ddc->0x567070); P2 patch'))
DTC = dt_consumers()
for cs, tg, regs in DTC:
    f = containing(tg)
    add(f, 'entry_count', f'dt_consumer_{tg:x}', 'F9', f'called at {cs:#x} with {regs}')
    sites.append((cs, MAIN_UPD, '0x5651d0->0x39e4e4', 'unknown', f'F9 dt consumer call {tg:#x} ({regs}; xmm7=dt, xmm6=dt or 0 when paused)'))

# F9d
add(0x20dda0c, 'entry_count', 'camera_path2_20dda0c', 'F9d', 'second camera path; entry count only')
add(0x567070, 'entry_count', 'camera_mgr_567070', 'F9d', 'camera manager update; entry count only')
for cs, cf, mn in callers_of([0x567070]).get(0x567070, []):
    sites.append((cs, cf, None, 'unknown', f'F9/F9d call into camera manager 0x567070 from {cf:#x}'))

# F9e
for s in (0x5acb36, 0x271590d, 0x271827a):
    f = containing(s)
    i = N.insn_at(s)
    add(f, 'entry_count', f'own60fps_{f:x}', 'F9e', f'own 60/S.fps at {s:#x}')
    sites.append((s, f, None, 'unknown', f'F9e own 60/S.fps: {i.mnemonic} {i.op_str}'))

# legacy tick, accessors
add(0x5a6bb0, 'entry_count', 'legacy_tick_5a6bb0', 'F5/S4.2', 'counts legacy 60 Hz ticks (S4.2)')
add(GETFPS, 'retaddr_hist', 'GetFps', 'F6/F7', 'return-address histogram (129 callers)')
add(GETTS, 'retaddr_hist', 'GetTimeStep', 'F6/F7', 'return-address histogram (65 callers)')

# sites_classified.csv
for row in csv.DictReader(open(SITES_IN, newline='')):
    s, fn = int(row['site'], 16), int(row['func'], 16)
    ev = f"{'GetFps' if row['kind'] == 'FPS' else 'GetTimeStep'} call site; provisional category={row['category']}; speed_scaled={row['speed_scaled']} (rev-sites)"
    if row['note']: ev += f"; {row['note']}"
    sites.append((s, containing(fn), None, 'unknown', ev))

# ================================================================ hookability
FNS = sorted(targets)
# positive control: a known jcc rel8 (0x5ad6b9 -> 0x5ad6c5) and call rel32 (0x5ad630 -> 0x5ace5c) must be reported
_pc = branch_into([0x5ad6c5 - 2, 0x5ace5c - 3])
assert 0x5ad6b9 in [x for x, _ in _pc.get(0x5ad6c5 - 2, [])] and 0x5ad630 in [x for x, _ in _pc.get(0x5ace5c - 3, [])]
BI = branch_into(FNS)
HK = {f: minhook(f) for f in FNS}

def hookable(f, hk, bi):
    ok, ln, insns, note = hk
    return ok and not bi

CALLERS = callers_of(FNS)
VT = vtable_refs(FNS)
fallback = {}
extra = sorted({c for f in FNS if not hookable(f, HK[f], BI.get(f)) for _, c, _ in CALLERS.get(f, [])} - set(FNS))
if extra:
    BI.update(branch_into(extra))
    for c in extra: HK[c] = minhook(c)
for f in FNS:
    if hookable(f, HK[f], BI.get(f)): continue
    cands = sorted({c for _, c, _ in CALLERS.get(f, [])})
    good = [c for c in cands if hookable(c, HK[c], BI.get(c))]
    fallback[f] = (good[0] if good else None, cands)

# ================================================================ write probe_targets.csv
os.makedirs(OUT, exist_ok=True)
rows = []
for f in FNS:
    t = targets[f]
    ok, ln, insns, mnote = HK[f]
    bi = BI.get(f, [])
    hk = ok and not bi
    notes = list(t['notes'])
    if mnote: notes.append(mnote)
    notes.append('prologue: ' + '; '.join(f'{i.mnemonic} {i.op_str}' for i in insns))
    if f not in PD: notes.append('leaf (no .pdata entry)')
    for src, why in bi: notes.append(f'branch into fn+{1}..4 from {src:#x}: {why}')
    hook = f
    if not hk:
        fb, cands = fallback[f]
        if fb is not None:
            hook = fb; notes.append(f'fallback: hookable caller {fb:#x}' + (f' (of {len(cands)})' if len(cands) > 1 else ''))
        else:
            hook = f
            vt = VT.get(f, [])
            notes.append('NO hookable direct caller' + (f'; vtable slots {[h(v) for v in vt]}: swap slot pointer instead' if vt else '; needs mid-function/manual hook'))
    pro = ''.join(i.bytes.hex() for i in HK[hook][2]).upper()   # exact bytes at hook_rva (MinHook-copied insns)
    assert bytes.fromhex(pro) == N.mem[hook:hook + len(pro) // 2]
    pat, aoff = aob_for(hook)
    if not pat: notes.append('no unique AOB found')
    elif aoff: notes.append('AOB anchored inside the function (function start is not unique: template twins)')
    rows.append(dict(rva=h(f), kind=t['kind'], name='|'.join(t['names']), source_finding='|'.join(t['srcs']),
                     prologue_bytes_hex=pro, hookable='yes' if hk else 'no', hook_rva=h(hook), aob=pat, aob_offset=h(aoff),
                     notes='; '.join(notes)))
# independent re-check: every AOB is >= 16 B and matches exactly once, at hook_rva (+ anchor offset)
for r in rows:
    want = int(r['hook_rva'], 16) + int(r['aob_offset'], 16)
    assert len(r['aob'].split()) >= 16 and aob_find(r['aob'], limit=3) == [want], r['rva']
COLS = ['rva', 'kind', 'name', 'source_finding', 'prologue_bytes_hex', 'hookable', 'hook_rva', 'aob', 'aob_offset', 'notes']
with open(os.path.join(OUT, 'probe_targets.csv'), 'w', newline='') as fo:
    w = csv.DictWriter(fo, COLS, lineterminator='\n'); w.writeheader(); w.writerows(rows)

# ================================================================ write sites.csv
SC = callers_of(sorted({c for _, c, _, _, _ in sites if c}))
def chain(fn):
    cs = sorted({c for _, c, _ in SC.get(fn, [])})
    if not cs:
        vt = VT.get(fn) or vtable_refs([fn]).get(fn)
        return f'{fn:#x}<-(no direct callers' + (f'; vtable {h(vt[0])})' if vt else ')')
    s = ','.join(h(c) for c in cs[:3]) + (f'(+{len(cs) - 3} more)' if len(cs) > 3 else '')
    return f'{fn:#x}<-{s}'
seen, srows = set(), []
for s, fn, ch, cls, ev in sorted(sites, key=lambda x: x[0]):
    if s in seen:
        for r in srows:
            if r['site_rva'] == h(s) and ev not in r['evidence']: r['evidence'] += ' | ' + ev
        continue
    seen.add(s)
    srows.append(dict(site_rva=h(s), containing_fn=h(fn), caller_chain=ch or chain(fn), **{'class': cls}, evidence=ev, hits_per_s=''))
SCOLS = ['site_rva', 'containing_fn', 'caller_chain', 'class', 'evidence', 'hits_per_s']
with open(os.path.join(OUT, 'sites.csv'), 'w', newline='') as fo:
    w = csv.DictWriter(fo, SCOLS, lineterminator='\n'); w.writeheader(); w.writerows(srows)

# ================================================================ README
cnt = collections.Counter(r['source_finding'].split('|')[0] for r in rows)
unh = [r for r in rows if r['hookable'] == 'no']
src_cnt = collections.Counter()
for _, _, _, _, ev in sites: src_cnt[ev.split(' ')[0].split(';')[0]] += 1
L = []
L.append('# S0.5 probe target list\n')
L.append('Generated by `python -I tools/s05/gen.py` (static, read-only on `Nioh3.exe.unpacked.exe`; deterministic). '
         'Do not edit the CSVs by hand except `sites.csv` columns `class`, `evidence`, `hits_per_s` after S1.\n')
L.append('## How\n')
L.append('- **F7b inline +0x614 reads**: every `mov reg,[rip+0x4759910]` (clock global) followed within 40 insns by `[reg+0x614]` '
         f'before `reg` is redefined or control leaves. {len(INL) + 2} reads total; the 2 inside GetFps/GetTimeStep are covered by the '
         f'retaddr_hist rows, leaving {len(INL)} inline reads in {len({containing(s) for s in INL})} functions.')
L.append('- **F9 consumers**: direct calls in `0x39e4e4` after `0x39e543` whose xmm0-3 args are loaded from xmm6/xmm7 (dt) since the '
         f'previous call: {len(DTC)} calls.')
L.append('- **Containing function**: `.pdata` entry, following `UNW_FLAG_CHAININFO` to the primary entry; leaf functions without '
         '`.pdata` start after the preceding `ret`/int3 padding.')
L.append('- **Hookable** = (a) a model of MinHook 1.3.3 `CreateTrampolineFunction` accepts the prologue (whole instructions >= 5 B; '
         'rip-relative, call/jmp/jcc rel are relocated; VEX, LOOP/JRCXZ, int3 rejected; < 5 B functions need padding after or above) and '
         '(b) nothing references fn+1..fn+4: rel32 and rip-relative disp32 (whole .text, verified by decoding), rel8 within +-0x82, '
         'RVA32 values (jump tables) and VA64 pointers in .text/.rdata/.data/_RDATA.')
L.append('- **hook_rva**: the function itself, or the first hookable direct caller (by RVA) when not hookable.')
L.append('- **aob / aob_offset**: rule: `aob` matches exactly once in the whole mapped image, at `hook_rva + aob_offset` '
         '(hook = match - aob_offset). Built from whole instructions, >= 16 B, starting at `hook_rva` (`aob_offset` = `0x0`) '
         'unless that start is not unique, then at the first unique instruction inside the function or its chained fragments. '
         'rel32 branch targets, rip-relative disp32 and base-relocated bytes are `??`. Asserted on every run.')
L.append('- **prologue_bytes_hex**: the exact bytes at `hook_rva` covering the instructions MinHook relocates (>= 5 B); asserted against the image.')
L.append('- **sites.csv**: prefilled from `tools/review/rev-sites/sites_classified.csv` plus the new sites below; `class=unknown` '
         'except P2 `0x39e526` (`scale`, proven statically in F9/F9c/G1). `caller_chain` is the documented chain where one exists, '
         'else up to 3 direct callers of the containing function. `hits_per_s` is filled from S1 logs.\n')
L.append('## Counts\n')
L.append(f'- probe_targets.csv: {len(rows)} rows ({sum(r["kind"] == "entry_count" for r in rows)} entry_count, '
         f'{sum(r["kind"] == "retaddr_hist" for r in rows)} retaddr_hist); hookable {len(rows) - len(unh)}, unhookable {len(unh)}.')
L.append('- rows by first source finding: ' + ', '.join(f'{k} {v}' for k, v in sorted(cnt.items())))
L.append(f'- sites.csv: {len(srows)} sites (' + ', '.join(f'{k} {v}' for k, v in sorted(src_cnt.items())) + ').\n')
L.append('## Unhookable targets and fallbacks\n')
if not unh: L.append('None.')
else:
    L.append('|target|name|reason|fallback hook_rva|')
    L.append('|-|-|-|-|')
    for r in unh:
        f = int(r['rva'], 16)
        ok, ln, insns, mnote = HK[f]
        why = mnote if not ok else '; '.join(f'{s:#x} {w}' for s, w in BI.get(f, []))
        fb = r['hook_rva'] if r['hook_rva'] != r['rva'] else ('none: ' + ('vtable slot swap' if VT.get(f) else 'manual'))
        L.append(f"|{r['rva']}|{r['name']}|{why}|{fb}|")
L.append('')
anch = [r for r in rows if r['aob_offset'] != '0x0']
L.append('## Notes\n')
L.append(f'- {len(anch)} targets have a non-unique function start (template twins); their AOB is anchored inside the function and '
         '`aob_offset` holds the anchor offset: ' + ', '.join(f"{r['rva']} ({r['aob_offset']})" for r in anch) + '.')
L.append('- F7c lists `0x26f4768`/`0x26f4780` as raw speed readers, but both read clock `+0x58`/`+0x5c` (int, -1 if the clock is null), '
         'not `+0x624`/`+0x628`. They stay as targets; S2 should re-check what those fields are.')
L.append('- The `[0x4745348]` manager named in F9 is not called directly with dt from `0x39e4e4` (only `byte [mgr+0x143d]=0` there); '
         'how it is reached (via one of the consumers or elsewhere) was not traced here.')
L.append('- Branch-into scan positive control (asserted on every run): it reports the known jcc `0x5ad6b9` and call `0x5ad630`; across the targets it found no rel8/rel32 '
         'references into fn+1..4. RVA32 values are matched raw (jump tables cannot be told from data), so a hit would be conservative.')
L.append('')
open(os.path.join(OUT, 'README.md'), 'w', newline='\n').write('\n'.join(L) + '\n')
print(f'targets {len(rows)} unhookable {len(unh)} sites {len(srows)} inline {len(INL)} dtc {len(DTC)}')
