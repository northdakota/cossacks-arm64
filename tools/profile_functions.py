#!/usr/bin/env python3
"""Sums a cossacks-run --profile by function of dmcr.exe.

Usage: profile_functions.py PROFILE.tsv DMCR.ASM [COUNT]

DMCR.ASM is `objdump -d --no-show-raw-insn --x86-asm-syntax=intel dmcr.exe`. Function starts are
the engine's exported names (objdump labels) and every target of a direct `call`; a block belongs
to the closest start at or below it. Blocks of the host (imports, 0xF0000000) and of the DLLs
(AI players, 0x20000000) are summed per area.

As in zeus-poseidon-arm64's tools/profile_report.py: the measurement costs a roughly fixed time per
block executed; it is estimated as the shortest average time among very busy blocks, and taken off.
"""
import bisect
import re
import sys


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    count = int(argv[3]) if len(argv) > 3 else 30
    names, starts = {}, set()
    line_re = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
    label_re = re.compile(r"^([0-9a-f]{8}) <([^>+]+)>:")
    with open(argv[2]) as f:
        for line in f:
            m = label_re.match(line)
            if m:
                a = int(m.group(1), 16)
                names[a] = m.group(2)
                starts.add(a)
                continue
            m = line_re.match(line)
            if m:
                c = re.match(r"call\s+0x([0-9a-f]+)\b", m.group(2))
                if c:
                    starts.add(int(c.group(1), 16))
    starts = sorted(starts)

    blocks = []
    with open(argv[1]) as f:
        for line in f:
            address, time, executions = line.split()[:3]
            blocks.append((int(address, 16), int(time), int(executions)))
    busy = [t / n for _, t, n in blocks if n >= 100000]
    overhead = min(busy) if busy else 0.0
    per, total = {}, 0.0
    for address, time, executions in blocks:
        corrected = max(time - overhead * executions, 0.0)
        total += corrected
        if address >= 0xF0000000:
            key = (0xF0000000, "host: system functions (imports, COM)")
        elif 0x20000000 <= address < 0x30000000:
            key = (0x20000000, "DLLs of the game (AI players)")
        else:
            i = bisect.bisect_right(starts, address) - 1
            start = starts[i] if i >= 0 else address
            name = names.get(start)
            if not name:
                j = max((s for s in names if s <= start), default=None)
                name = f"sub_{start:x}" + (f"  (after {names[j]})" if j is not None else "")
            key = (start, name)
        entry = per.setdefault(key, [0.0, 0])
        entry[0] += corrected
        entry[1] += executions
    ranked = sorted(per.items(), key=lambda kv: -kv[1][0])
    print(f"measured {sum(b[1] for b in blocks) / 1e9:.1f} s, measurement cost {overhead:.0f} ns per block, "
          f"corrected {total / 1e9:.1f} s, {len(ranked)} functions")
    cumulated = 0.0
    for rank, ((start, name), (time, executions)) in enumerate(ranked[:count], 1):
        cumulated += time
        print(f"{rank:3d}  {start:08x}  {100 * time / total:5.1f} %  total {100 * cumulated / total:5.1f} %  "
              f"{executions:>11d} blocks  {name[:110]}")


if __name__ == "__main__":
    main(sys.argv)
