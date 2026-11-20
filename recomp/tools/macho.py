"""Minimal i386 Mach-O reader with the same interface as pe.PE (sections, imports, bytes by VA), for Aspyr's Homeworld
Remastered Mac executables (MH_EXECUTE, not position independent, symbols present).

Differences from a PE image, and how they map onto what the recompiler expects:
  - imports: the symbol-pointer sections (__la_symbol_ptr, __nl_symbol_ptr, __got) play the role of the IAT. Code
    calls an import through a 6-byte stub in __symbol_stub (jmp *ptr); the stubs are lifted like any function and
    their indirect jump becomes rt_import, so `imports` maps each pointer slot to 'library!symbol'.
  - binds: other data that dyld fills with an imported symbol's address (vtables of std::exception etc. inside
    typeinfo objects, CFString class pointers in __cfstring): [(va, 'library!symbol', addend)]. The runtime writes
    the guest address of its implementation there at load time.
  - sections: (name, va, vsize, fileoff, filesize, flags) where name is 'segment,section' and flags uses the PE
    meanings the tools test: 0x20000000 executable, 0x40 initialised data, 0x80 zero fill.
  - symbols: {va: name} for every defined function symbol, and func_starts from LC_FUNCTION_STARTS.
"""
import struct

MH_MAGIC = 0xfeedface
LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB, LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB = 0x1, 0x2, 0xb, 0xc, 0x80000018, 0x8000001f
LC_DYLD_INFO, LC_DYLD_INFO_ONLY, LC_MAIN, LC_FUNCTION_STARTS, LC_UNIXTHREAD = 0x22, 0x80000022, 0x80000028, 0x26, 0x5
S_ZEROFILL, S_NON_LAZY, S_LAZY, S_STUBS, S_MOD_INIT, S_GB_ZEROFILL, S_TLV_ZEROFILL, S_TLV_VARS = 1, 6, 7, 8, 9, 0xc, 0x12, 0x13
INDIRECT_LOCAL, INDIRECT_ABS = 0x80000000, 0x40000000


def is_macho(path):
    with open(path, 'rb') as f: return f.read(4) == struct.pack('<I', MH_MAGIC)


def uleb(d, o):
    v = s = 0
    while True:
        b = d[o]; o += 1; v |= (b & 0x7f) << s; s += 7
        if b < 0x80: return v, o


def sleb(d, o):
    v = s = 0
    while True:
        b = d[o]; o += 1; v |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            if b & 0x40: v -= 1 << s
            return v, o


class MachO:
    kind = 'macho'

    def __init__(self, path):
        d = self.data = open(path, 'rb').read()
        magic, cpu, sub, ftype, ncmds, sizeofcmds, self.flags = struct.unpack_from('<7I', d, 0)
        if magic != MH_MAGIC or cpu != 7: raise ValueError('%s: not an i386 Mach-O' % path)
        self.is_dylib = ftype == 6
        self.base = None; self.entry = None; self.segments = []; self.sections = []; self.raw_sections = []
        self.dylibs = []; symtab = dysym = dyld = fstarts = None
        o = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', d, o)
            if cmd == LC_SEGMENT:
                seg = d[o + 8:o + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize, maxp, initp, nsects, sflags = struct.unpack_from('<8I', d, o + 24)
                self.segments.append((seg, vmaddr, vmsize, fileoff, filesize, initp))
                if seg == '__TEXT' and self.base is None: self.base = vmaddr
                for k in range(nsects):
                    so = o + 56 + 68 * k
                    sect = d[so:so + 16].rstrip(b'\0').decode()
                    addr, ssize, off, align, reloff, nreloc, fl, r1, r2 = struct.unpack_from('<9I', d, so + 32)
                    typ = fl & 0xff
                    zero = typ in (S_ZEROFILL, S_GB_ZEROFILL, S_TLV_ZEROFILL)
                    code = bool(fl & 0x80000400)                      # S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
                    pfl = (0x20000000 if code else 0) | (0x80 if zero else 0x40)
                    self.raw_sections.append((seg, sect, addr, ssize, off, fl, r1, r2))
                    self.sections.append((seg + ',' + sect, addr, ssize, 0 if zero else off, 0 if zero else ssize, pfl))
            elif cmd == LC_SYMTAB: symtab = struct.unpack_from('<4I', d, o + 8)
            elif cmd == LC_DYSYMTAB: dysym = struct.unpack_from('<18I', d, o + 8)
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB):
                no = struct.unpack_from('<I', d, o + 8)[0]
                p = d[o + no:o + size].split(b'\0')[0].decode()
                self.dylibs.append(p)
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY): dyld = struct.unpack_from('<10I', d, o + 8)
            elif cmd == LC_MAIN: self.entry_off = struct.unpack_from('<Q', d, o + 8)[0]
            elif cmd == LC_UNIXTHREAD: self.entry = struct.unpack_from('<I', d, o + 16 + 10 * 4)[0]   # i386 thread state: eip
            elif cmd == LC_FUNCTION_STARTS: fstarts = struct.unpack_from('<II', d, o + 8)
            o += size
        if self.entry is None and hasattr(self, 'entry_off'): self.entry = self.base + self.entry_off
        self.libname = lambda ordinal: self.short(self.dylibs[ordinal - 1]) if 0 < ordinal <= len(self.dylibs) else \
            {0: 'self', -1: 'main', -2: 'flat'}.get(ordinal, '?')
        # symbols
        self.symbols = {}; self.undef = []; self.defined = {}
        syms = []
        if symtab:
            symoff, nsyms, stroff, strsize = symtab
            for k in range(nsyms):
                strx, typ, sect, desc, val = struct.unpack_from('<IBBHI', d, symoff + 12 * k)
                e = d.index(b'\0', stroff + strx); name = d[stroff + strx:e].decode('latin1')
                syms.append((name, typ, sect, desc, val))
                if typ & 0xe0: continue                                # debugging (stab) entries
                if typ & 0x0e == 0xe and sect:                          # N_SECT: defined here
                    self.defined.setdefault(name, val)
                    s = self.raw_sections[sect - 1]
                    if s[5] & 0x80000400: self.symbols.setdefault(val, name)
        self.syms = syms
        # indirect symbol table -> import pointer slots
        self.imports = {}; self.stubs = {}; self.mod_init = []; self.tlv = None; self.self_slots = {}
        ind = []
        if dysym:
            indoff, nind = dysym[12], dysym[13]
            ind = list(struct.unpack_from('<%dI' % nind, d, indoff)) if nind else []
        for seg, sect, addr, ssize, off, fl, r1, r2 in self.raw_sections:
            typ = fl & 0xff
            if typ in (S_NON_LAZY, S_LAZY):
                for k in range(ssize // 4):
                    si = ind[r1 + k]
                    if si & (INDIRECT_LOCAL | INDIRECT_ABS): continue  # a local pointer: the file already holds its value
                    name, styp, ssect, desc, val = syms[si]
                    lib = self.libname(self.ordinal(desc, styp, ssect))
                    if lib == 'self' and name in self.defined:          # a weak definition the executable itself wins
                        self.self_slots[addr + 4 * k] = self.defined[name]; continue
                    self.imports[addr + 4 * k] = lib + '!' + name
            elif typ == S_STUBS and r2:
                for k in range(ssize // r2):
                    si = ind[r1 + k]
                    if si & (INDIRECT_LOCAL | INDIRECT_ABS): continue
                    self.stubs[addr + r2 * k] = syms[si][0]
            elif typ == S_MOD_INIT:
                self.mod_init = [v for v in struct.unpack_from('<%dI' % (ssize // 4), d, off)]
            elif typ == S_TLV_VARS:
                self.tlv = (addr, ssize)
        # dyld binds (the pointer slots above appear here too; `binds` keeps the others)
        self.binds = []; self.weak_binds = []; self.lazy_binds = []; self.rebases = []
        if dyld:
            rb_off, rb_size, b_off, b_size, w_off, w_size, l_off, l_size, e_off, e_size = dyld
            self.rebases = self._rebase(rb_off, rb_size)
            self.binds = self._bind(b_off, b_size, False)
            self.weak_binds = self._bind(w_off, w_size, False, weak=True)
            self.lazy_binds = self._bind(l_off, l_size, True)
        # data that dyld points at a symbol: an import (the runtime supplies it) or a definition in this image
        self.fixups = {}                                                # va -> value written at load time
        self.data_binds = []
        for va, nm, add in self.binds + self.weak_binds:
            if va in self.imports or va in self.self_slots: continue
            sym = nm.split('!', 1)[1]
            if sym in self.defined: self.fixups[va] = (self.defined[sym] + add) & 0xffffffff
            else: self.data_binds.append((va, ('c++' if nm.startswith('weak!') else nm.split('!')[0]) + '!' + sym, add))
        for va, nm, add in self.weak_binds:                             # weak pointer slots nothing here defines
            if va in self.imports or va in self.self_slots: continue
        for slot, nm in list(self.imports.items()):
            if nm.startswith('weak!'): self.imports[slot] = 'c++!' + nm[5:]
        self.fixups.update(self.self_slots)
        # stubs of imports the executable defines itself become direct jumps (code_range patches them)
        self.stub_jumps = {}
        for seg, sect, addr, ssize, off, fl, r1, r2 in self.raw_sections:
            if fl & 0xff == S_STUBS and r2 == 6:
                for k in range(ssize // 6):
                    b = d[off + 6 * k:off + 6 * k + 6]
                    if b[:2] == b'\xff\x25':
                        slot = struct.unpack_from('<I', b, 2)[0]
                        if slot in self.self_slots: self.stub_jumps[addr + 6 * k] = self.self_slots[slot]
        self.func_starts = []
        if fstarts:
            fo, fs = fstarts; o = fo; a = self.base
            while o < fo + fs:
                v, o = uleb(d, o)
                if not v: break
                a += v; self.func_starts.append(a)
        self.dirs = [(0, 0)] * 16                                       # PE data directories: none (exports via symbols)

    @staticmethod
    def short(p):
        n = p.rsplit('/', 1)[-1]
        if n.startswith('lib'):
            n = n[3:]
        for suf in ('.dylib',):
            if n.endswith(suf): n = n[:-len(suf)]
        n = n.split('.')[0]
        return n.replace('X86', '').lower() if n.lower().startswith(('miles', 'bink')) else n

    @staticmethod
    def ordinal(desc, typ, sect):
        o = (desc >> 8) & 0xff
        return o - 256 if o >= 0xfe else o

    def seg_va(self, k):
        return self.segments[k][1]

    def _rebase(self, off, size):
        d = self.data; o = off; end = off + size; out = []; seg = 0; addr = 0; typ = 1
        while o < end:
            b = d[o]; o += 1; op, imm = b & 0xf0, b & 0x0f
            if op == 0x00: break
            elif op == 0x10: typ = imm
            elif op == 0x20: seg = imm; v, o = uleb(d, o); addr = self.seg_va(seg) + v
            elif op == 0x30: v, o = uleb(d, o); addr += v
            elif op == 0x40: addr += imm * 4
            elif op == 0x50:
                for _ in range(imm): out.append(addr); addr += 4
            elif op == 0x60:
                n, o = uleb(d, o)
                for _ in range(n): out.append(addr); addr += 4
            elif op == 0x70: out.append(addr); v, o = uleb(d, o); addr += 4 + v
            elif op == 0x80:
                n, o = uleb(d, o); sk, o = uleb(d, o)
                for _ in range(n): out.append(addr); addr += 4 + sk
            else: raise ValueError('rebase opcode %x' % op)
        return out

    def _bind(self, off, size, lazy, weak=False):
        d = self.data; o = off; end = off + size; out = []
        ordinal = 0; name = ''; addend = 0; addr = 0; flags = 0
        while o < end:
            b = d[o]; o += 1; op, imm = b & 0xf0, b & 0x0f
            if op == 0x00:
                if lazy: continue
                break
            elif op == 0x10: ordinal = imm
            elif op == 0x20: ordinal, o = uleb(d, o)
            elif op == 0x30: ordinal = (imm | 0xf0) - 256 if imm else 0
            elif op == 0x40:
                flags = imm; e = d.index(b'\0', o); name = d[o:e].decode('latin1'); o = e + 1
            elif op == 0x50: pass                                       # type (pointer)
            elif op == 0x60: addend, o = sleb(d, o)
            elif op == 0x70: seg = imm; v, o = uleb(d, o); addr = self.seg_va(seg) + v
            elif op == 0x80: v, o = uleb(d, o); addr = (addr + v) & 0xffffffff
            elif op == 0x90: out.append((addr, self._bname(ordinal, name, weak), addend)); addr += 4
            elif op == 0xa0: out.append((addr, self._bname(ordinal, name, weak), addend)); v, o = uleb(d, o); addr = (addr + 4 + v) & 0xffffffff
            elif op == 0xb0: out.append((addr, self._bname(ordinal, name, weak), addend)); addr += 4 + imm * 4
            elif op == 0xc0:
                n, o = uleb(d, o); sk, o = uleb(d, o)
                for _ in range(n): out.append((addr, self._bname(ordinal, name, weak), addend)); addr += 4 + sk
            else: raise ValueError('bind opcode %x' % op)
        return out

    def _bname(self, ordinal, name, weak):
        return ('weak' if weak else self.libname(ordinal)) + '!' + name

    # ---- pe.PE interface ----
    def relocs(self): return list(self.rebases)

    def off(self, va):
        for seg, sva, vs, fo, fs, prot in self.segments:
            if sva <= va < sva + vs: return va - sva + fo if va - sva < fs else None
        return None

    def cstr(self, va):
        o = self.off(va); return self.data[o:self.data.index(b'\0', o)].decode('latin1')

    def section(self, name):
        for s in self.sections:
            if s[0] == name or s[0].split(',')[1] == name: return s

    def bytes_at(self, va, n):
        o = self.off(va)
        if o is None: return b''
        for seg, sva, vs, fo, fs, prot in self.segments:
            if sva <= va < sva + vs: return self.data[o:min(o + n, fo + fs)]
        return b''

    def u32(self, va):
        b = self.bytes_at(va, 4); return struct.unpack('<I', b)[0] if len(b) == 4 else None


def code_range(m):
    """(lo, hi, bytes): every executable section (__text, __symbol_stub, __stub_helper), gaps filled with int3."""
    ex = [s for s in m.sections if s[5] & 0x20000000]
    lo, hi = min(s[1] for s in ex), max(s[1] + s[2] for s in ex)
    buf = bytearray(b'\xcc' * (hi - lo))
    for s in ex:
        b = m.bytes_at(s[1], s[2]); buf[s[1] - lo:s[1] - lo + len(b)] = b
    for va, t in m.stub_jumps.items():                                  # jmp *slot -> jmp rel32; nop
        buf[va - lo:va - lo + 6] = b'\xe9' + struct.pack('<i', t - (va + 5)) + b'\x90'
    return lo, hi, bytes(buf)


if __name__ == '__main__':
    import sys, collections
    m = MachO(sys.argv[1])
    print('base 0x%x entry 0x%x dylib %s' % (m.base, m.entry, m.is_dylib))
    for s in m.sections: print('  %-28s 0x%08x %9d flags %x' % s[:3] + (s[5],) if False else '  %-28s 0x%08x %9d  %x' % (s[0], s[1], s[2], s[5]))
    print('imports (pointer slots): %d  stubs: %d  data binds: %d  weak binds: %d  lazy binds: %d  rebases: %d' % (
        len(m.imports), len(m.stubs), len(m.data_binds), len(m.weak_binds), len(m.lazy_binds), len(m.rebases)))
    print('self-resolved slots: %d  stub jumps: %d  fixups: %d' % (len(m.self_slots), len(m.stub_jumps), len(m.fixups)))
    print('function symbols: %d  function starts: %d  mod_init: %d  tlv: %s' % (len(m.symbols), len(m.func_starts), len(m.mod_init), m.tlv))
    print('libraries:', collections.Counter(v.split('!')[0] for v in m.imports.values()).most_common())
    print('data binds by library:', collections.Counter(b[1].split('!')[0] for b in m.data_binds).most_common())
    stubbed = set(m.stubs.values()); lazyset = {m.imports.get(a, '') for a, _, _ in m.lazy_binds}
    print('lo/hi code:', ['0x%x' % v for v in code_range(m)[:2]])


# ---- C++ exception tables: compact unwind (__unwind_info) and LSDAs (__gcc_except_tab) ----
def read_enc(d, o, enc, base_va, sect_va, sect_off):
    """A DW_EH_PE value at file offset o; returns (value, new offset). pcrel is relative to the field's address."""
    if enc == 0xff: return None, o
    fmt, app = enc & 0x0f, enc & 0x70
    if fmt == 0x01: v, o2 = uleb(d, o)
    elif fmt == 0x09: v, o2 = sleb(d, o)
    elif fmt in (0x02, 0x0a): v = struct.unpack_from('<H' if fmt == 2 else '<h', d, o)[0]; o2 = o + 2
    elif fmt in (0x00, 0x03, 0x0b): v = struct.unpack_from('<i' if fmt == 0x0b else '<I', d, o)[0]; o2 = o + 4
    elif fmt in (0x04, 0x0c): v = struct.unpack_from('<q' if fmt == 0x0c else '<Q', d, o)[0]; o2 = o + 8
    else: raise ValueError('pointer encoding %x' % enc)
    if app == 0x10: v += sect_va + (o - sect_off)                   # pcrel
    return v & 0xffffffff, o2


def unwind_entries(m):
    """[(function start va, encoding, lsda va or 0)] from __unwind_info, sorted."""
    sec = {s[1]: s for s in m.raw_sections}
    ui = sec.get('__unwind_info')
    if not ui: return []
    d = m.data; base = ui[4]
    ver, ceo, cec, peo, pec, ieo, iec = struct.unpack_from('<7I', d, base)
    common = list(struct.unpack_from('<%dI' % cec, d, base + ceo)) if cec else []
    hdr = m.base                                                    # offsets are from the mach header (= __TEXT start)
    out = []; lsda = {}
    for k in range(iec):
        fo, slo, lio = struct.unpack_from('<3I', d, base + ieo + 12 * k)
        if k + 1 < iec:
            nfo, _, nlio = struct.unpack_from('<3I', d, base + ieo + 12 * (k + 1))
            for j in range((nlio - lio) // 8):
                f2, lo = struct.unpack_from('<2I', d, base + lio + 8 * j); lsda[hdr + f2] = hdr + lo
        if not slo: continue
        p = base + slo; kind = struct.unpack_from('<I', d, p)[0]
        if kind == 2:
            eo, ec = struct.unpack_from('<HH', d, p + 4)
            for j in range(ec):
                f2, enc = struct.unpack_from('<2I', d, p + eo + 8 * j); out.append((hdr + f2, enc))
        elif kind == 3:
            eo, ec, xo, xc = struct.unpack_from('<4H', d, p + 4)
            local = list(struct.unpack_from('<%dI' % xc, d, p + xo)) if xc else []
            for j in range(ec):
                e = struct.unpack_from('<I', d, p + eo + 4 * j)[0]
                ix = e >> 24; enc = common[ix] if ix < cec else local[ix - cec]
                out.append((hdr + fo + (e & 0xffffff), enc))
    out.sort()
    return [(f, enc, lsda.get(f, 0)) for f, enc in out]


def landing_pads(m, func, lsda_va):
    """Landing pad addresses of one function, from its LSDA call-site table."""
    sec = [s for s in m.raw_sections if s[1] == '__gcc_except_tab']
    if not sec or not lsda_va: return []
    _, _, sva, ssz, soff = sec[0][:5]
    d = m.data; o = soff + (lsda_va - sva)
    lpenc = d[o]; o += 1
    lpstart, o = read_enc(d, o, lpenc, m.base, sva, soff)
    if lpstart is None: lpstart = func
    tenc = d[o]; o += 1
    if tenc != 0xff: _, o = uleb(d, o)
    csenc = d[o]; o += 1
    cslen, o = uleb(d, o); end = o + cslen; pads = []
    while o < end:
        st, o = read_enc(d, o, csenc & 0x0f, m.base, sva, soff)
        ln, o = read_enc(d, o, csenc & 0x0f, m.base, sva, soff)
        lp, o = read_enc(d, o, csenc & 0x0f, m.base, sva, soff)
        act, o = uleb(d, o)
        if lp: pads.append(lpstart + lp)
    return sorted(set(pads))
