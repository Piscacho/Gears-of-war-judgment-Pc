"""Busca candidatos a setjmp/longjmp: stfd/lfd f14,0(r3) y lo que les precede."""
import struct, sys
img = open(sys.argv[1], "rb").read(); base = 0x82000000
fn = [int(l.split()[0], 16) for l in open(sys.argv[2])]
import bisect
def owner(va):
    i = bisect.bisect_right(fn, va) - 1
    return fn[i] if i >= 0 else 0
for pat, name in ((0xD9C30000, "stfd f14,0(r3)"), (0xC9C30000, "lfd f14,0(r3)")):
    for off in range(0x2A0000, 0x10C0000, 4):
        if struct.unpack_from(">I", img, off)[0] == pat:
            va = base + off
            nxt = struct.unpack_from(">I", img, off + 4)[0]
            print("%-16s 0x%08X fn=0x%08X sig=0x%08X" % (name, va, owner(va), nxt))
