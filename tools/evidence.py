#!/usr/bin/env python3
"""Evidence for the stage-1 hypotheses on one PE file.

- sections with rights and entropy, writable+executable flags;
- call sites of chosen imports (`call [IAT]`, `jmp [IAT]`, or `mov reg,[IAT]` then `call reg`),
  each with the instructions before it, so the pushed arguments can be read;
- string literals pushed just before LoadLibraryA / GetProcAddress (dynamic DLL loading);
- strings in the file matching a pattern.

Usage: evidence.py FILE.EXE DISASM_CACHE [--calls Name,Name,...] [--strings REGEX] [--context N]
DISASM_CACHE is where the objdump listing is written (outside the game folder).
Run with `python3 -I`.
"""
import os
import re
import struct
import subprocess
import sys


class PE:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        ndirs = struct.unpack_from("<I", d, opt + 92)[0]
        self.dirs = [struct.unpack_from("<2I", d, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        for i in range(nsec):
            s = opt + opt_size + 40 * i
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rsize, rptr = struct.unpack_from("<4I", d, s + 8)
            flags = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append((name, va, vsize, rptr, rsize, flags))

    def offset(self, rva):
        for _, va, vsize, rptr, rsize, _ in self.sections:
            if va <= rva < va + max(vsize, rsize):
                if rva - va >= rsize:
                    return None  # uninitialised data
                return rva - va + rptr
        return None

    def cstr_va(self, va, limit=200):
        o = self.offset(va - self.image_base)
        if o is None:
            return None
        end = self.d.find(b"\0", o, o + limit)
        if end < 0:
            return None
        s = self.d[o:end]
        if len(s) < 2 or any(b < 0x20 or b > 0x7e for b in s):
            return None
        return s.decode("latin-1")

    def iat(self):
        """IAT slot VA -> 'dll!name'."""
        rva = self.dirs[1][0]
        out = {}
        if not rva:
            return out
        o = self.offset(rva)
        while True:
            ilt, _, _, name, first = struct.unpack_from("<5I", self.d, o)
            if not name:
                break
            dll = self.d[self.offset(name):self.d.index(b"\0", self.offset(name))].decode("latin-1")
            t = self.offset(ilt or first)
            i = 0
            while True:
                v = struct.unpack_from("<I", self.d, t + 4 * i)[0]
                if not v:
                    break
                if v & 0x80000000:
                    fn = f"#{v & 0xFFFF}"
                else:
                    no = self.offset(v + 2)
                    fn = self.d[no:self.d.index(b"\0", no)].decode("latin-1")
                out[self.image_base + first + 4 * i] = f"{dll.lower()}!{fn}"
                i += 1
            o += 20
        return out


def entropy(data):
    import math
    if not data:
        return 0.0
    c = [0] * 256
    for b in data:
        c[b] += 1
    n = len(data)
    return -sum(x / n * math.log2(x / n) for x in c if x)


def disasm(path, cache):
    if not os.path.exists(cache):
        with open(cache, "w") as f:
            subprocess.run(["/usr/bin/objdump", "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", path],
                           stdout=f, check=True)
    rx = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
    out = []
    with open(cache) as f:
        for line in f:
            m = rx.match(line)
            if m:
                out.append((int(m.group(1), 16), m.group(2).strip()))
    return out


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    path, cache = argv[1], argv[2]
    calls = argv[argv.index("--calls") + 1].split(",") if "--calls" in argv else []
    pattern = argv[argv.index("--strings") + 1] if "--strings" in argv else None
    ctx = int(argv[argv.index("--context") + 1]) if "--context" in argv else 8
    pe = PE(path)

    print(f"== {os.path.basename(path)}  image base {pe.image_base:#x}")
    for name, va, vsize, rptr, rsize, flags in pe.sections:
        r = "".join(c for c, b in (("R", 0x40000000), ("W", 0x80000000), ("X", 0x20000000)) if flags & b)
        code = " CODE" if flags & 0x20 else ""
        wx = "  <-- W+X" if (flags & 0x80000000 and flags & 0x20000000) else ""
        print(f"   {name:8} va={pe.image_base + va:#010x} vsize={vsize:>9} raw={rsize:>9} "
              f"H={entropy(pe.d[rptr:rptr + rsize]):4.2f} {r}{code}{wx}")

    if calls or "--loads" in argv:
        iat = pe.iat()
        wanted = {va: n for va, n in iat.items() if n.split("!")[1] in calls}
        listing = disasm(path, cache)
        index = {a: i for i, (a, _) in enumerate(listing)}
        reg_holds = {}  # crude: `mov reg, dword ptr [IAT]` seen recently
        hits = {n: [] for n in wanted.values()}
        for i, (addr, ins) in enumerate(listing):
            m = re.search(r"dword ptr \[0x([0-9a-f]+)\]", ins)
            if m:
                slot = int(m.group(1), 16)
                if slot in wanted:
                    if ins.startswith(("call", "jmp")):
                        hits[wanted[slot]].append(i)
                    else:
                        mm = re.match(r"mov\s+(e[a-z]{2}), dword ptr", ins)
                        if mm:
                            reg_holds[mm.group(1)] = (wanted[slot], i)
            mm = re.match(r"call\s+(e[a-z]{2})$", ins)
            if mm and mm.group(1) in reg_holds:
                name, at = reg_holds[mm.group(1)]
                if i - at < 40:
                    hits[name].append(i)
        # `jmp [IAT]` thunks: the real call sites are `call thunk`
        thunks = {listing[i][0]: n for n, idx in hits.items() for i in idx
                  if listing[i][1].startswith("jmp")}
        if thunks:
            for i, (addr, ins) in enumerate(listing):
                m = re.match(r"(?:call|jmp)\s+0x([0-9a-f]+)\b", ins)
                if m and int(m.group(1), 16) in thunks:
                    hits[thunks[int(m.group(1), 16)]].append(i)
        for name in sorted(hits):
            print(f"\n-- {name}: {len(hits[name])} call site(s)")
            for i in hits[name]:
                print(f"   @ {listing[i][0]:#x}")
                for a, ins in listing[max(0, i - ctx):i + 1]:
                    note = ""
                    for imm in re.findall(r"0x([0-9a-f]{6,8})\b", ins):
                        s = pe.cstr_va(int(imm, 16))
                        if s:
                            note += f'   ; "{s}"'
                    print(f"      {a:#x}  {ins}{note}")

    if pattern:
        print(f"\n-- strings /{pattern}/")
        rx = re.compile(pattern.encode("latin-1"), re.I)
        for m in re.finditer(rb"[\x20-\x7e]{4,}", pe.d):
            if rx.search(m.group()):
                print(f"   {m.start():#09x}  {m.group().decode('latin-1')[:160]}")


if __name__ == "__main__":
    main(sys.argv)
