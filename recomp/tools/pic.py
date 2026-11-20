"""Position-independent code patterns of Apple's i386 compilers (Aspyr's Mac executables), shared by analyze.py and
lift.py.

1. Program counter reads: `call L; L: pop reg` loads the address of L into reg. The call is not a function call:
   it pushes L and falls through (lifted as a push), and L becomes the function's "PIC base".

2. Relative jump tables: `mov r, [base + idx*4 + d]; ...; add r, base; jmp r`. `base` holds a PIC base P (directly,
   or reloaded from a spill slot), so the table sits at P + d and each entry is an offset from P. The table's size
   comes from a preceding `cmp idx, N; ja` when there is one; otherwise entries are read while they land inside
   the function.
"""
import bisect
from capstone import x86 as X


class PicInfo:
    def __init__(self, ins_at, lo, hi, code, func_starts):
        """Scan the code once for `call next; pop reg` sites. func_starts: sorted function start addresses."""
        self.ins_at = ins_at; self.starts = sorted(func_starts)
        self.sites = []                                        # (address of the pop = PIC base value, register)
        k = code.find(b'\xe8\x00\x00\x00\x00')
        while k >= 0:
            p = lo + k + 5
            q = ins_at(p)
            if q is not None and q.mnemonic == 'pop' and q.operands[0].type == X.X86_OP_REG:
                self.sites.append((p, q.reg_name(q.operands[0].reg)))
            k = code.find(b'\xe8\x00\x00\x00\x00', k + 1)
        self.sites.sort(); self.site_addrs = [s for s, _ in self.sites]
        self.call_next = {p - 5 for p, _ in self.sites}

    def func_range(self, va):
        k = bisect.bisect_right(self.starts, va) - 1
        if k < 0: return None
        return self.starts[k], self.starts[k + 1] if k + 1 < len(self.starts) else va + 0x100000

    def bases(self, va):
        """PIC base values defined in the function that contains va."""
        r = self.func_range(va)
        if not r: return []
        a = bisect.bisect_left(self.site_addrs, r[0]); b = bisect.bisect_left(self.site_addrs, r[1])
        return [p for p in self.site_addrs[a:b]]

    def jumptable(self, i, chain, u32):
        """i: a `jmp reg`; chain: the instructions just before it, in execution order. Returns (table, targets) or
        None."""
        if i.mnemonic != 'jmp' or not i.operands or i.operands[0].type != X.X86_OP_REG: return None
        r = i.reg_name(i.operands[0].reg); base = load = None; k = len(chain) - 1
        while k >= 0:                                          # the add that forms the target: add r, base
            q = chain[k]
            if q.mnemonic == 'add' and len(q.operands) == 2 and q.operands[0].type == X.X86_OP_REG and \
                    q.reg_name(q.operands[0].reg) == r and q.operands[1].type == X.X86_OP_REG:
                base = q.reg_name(q.operands[1].reg); break
            if q.mnemonic == 'add' and len(q.operands) == 2 and q.operands[0].type == X.X86_OP_REG and \
                    q.reg_name(q.operands[0].reg) == r and q.operands[1].type == X.X86_OP_MEM:
                m = q.operands[1]                          # add r, [r + idx*4 + d]: r holds the base, load and add in one
                if m.mem.scale == 4 and m.mem.index and m.mem.base and q.reg_name(m.mem.base) == r and not m.mem.segment:
                    base = r; load = q
                break
            if writes(q, r): return None
            k -= 1
        if base is None: return None
        k -= 1
        while load is None and k >= 0:                         # the load: mov r, [base + idx*4 + d]
            q = chain[k]
            if q.mnemonic == 'mov' and q.operands[0].type == X.X86_OP_REG and q.reg_name(q.operands[0].reg) == r:
                m = q.operands[1]
                if m.type == X.X86_OP_MEM and m.mem.scale == 4 and m.mem.index and m.mem.base and not m.mem.segment:
                    load = q
                break
            if writes(q, r): return None
            k -= 1
        if load is None: return None
        m = load.operands[1].mem; idx = load.reg_name(m.index); disp = m.disp
        n = None
        for q in reversed(chain[:k]):                          # bound: cmp idx, N (+ ja) before the load
            if q.mnemonic == 'cmp' and q.operands[0].type == X.X86_OP_REG and q.reg_name(q.operands[0].reg) == idx \
                    and q.operands[1].type == X.X86_OP_IMM:
                n = (q.operands[1].imm & 0xffffffff) + 1; break
            if writes(q, idx) and q.mnemonic != 'mov': break
        rng = self.func_range(i.address)
        best = None
        for p in self.bases(i.address):                        # the base whose table lands inside this function
            table = (p + disp) & 0xffffffff; tg = []
            for e in range(n if n and n <= 4096 else 1024):
                v = u32(table + 4 * e)
                if v is None: break
                t = (v + p) & 0xffffffff
                if not (rng[0] <= t < rng[1]):
                    if n: tg = None
                    break
                tg.append(t)
            if tg and (best is None or len(tg) > len(best[1])): best = (table, tg)
        return best


def writes(q, reg):
    """Does q write register reg (32-bit name)? Conservative: any explicit register operand 0 of a writing op."""
    if not q.operands: return q.mnemonic in ('cdq', 'cwde') and reg in ('eax', 'edx')
    o = q.operands[0]
    if o.type != X.X86_OP_REG: return q.mnemonic in ('call',) and reg in ('eax', 'ecx', 'edx')
    if q.mnemonic in ('cmp', 'test', 'push'): return False
    n = q.reg_name(o.reg)
    return n == reg or n in SUB.get(reg, ())


SUB = {'eax': ('ax', 'al', 'ah'), 'ebx': ('bx', 'bl', 'bh'), 'ecx': ('cx', 'cl', 'ch'), 'edx': ('dx', 'dl', 'dh'),
       'esi': ('si',), 'edi': ('di',), 'ebp': ('bp',), 'esp': ('sp',)}
