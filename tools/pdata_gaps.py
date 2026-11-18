"""Huecos de codigo sin .pdata y destinos de bl fuera de cualquier funcion.
python tools/pdata_gaps.py xex/basefile.bin xex/functions_pdata.txt xex/pdata_gaps.txt"""
import struct, sys, bisect, os
sys.path.insert(0, os.path.dirname(__file__))
from pe_sections import sections
img = open(sys.argv[1], "rb").read(); B = 0x82000000
fn = sorted((int(a, 16), int(b, 16)) for a, b in (l.split()[:2] for l in open(sys.argv[2])))
st = [f[0] for f in fn]
code = [x for x in sections(img)[2] if x[3] == "code"] if len(sections(img)[2][0]) > 3 else None
w = lambda a: struct.unpack_from(">I", img, a - B)[0]
segs = [(".text", 0x822A0000, 0x822A0000 + 0xDFC054), ("BINK", 0x8309C200, 0x8309C200 + 0x20BB0), ("RADCODE", 0x830BCE00, 0x830BCE00 + 0xAFC)]
out = open(sys.argv[3], "w"); ngap = nw = 0
def inside(a):
    i = bisect.bisect_right(st, a) - 1
    return i >= 0 and a < fn[i][0] + fn[i][1]
for name, s0, s1 in segs:
    a = s0
    while a < s1:
        if inside(a): a += 4; continue
        b = a; nz = 0
        while b < s1 and not inside(b):
            if w(b): nz += 1
            b += 4
        if nz:
            ngap += 1; nw += nz; out.write("%s 0x%08X 0x%X nz=%d\n" % (name, a, b - a, nz))
        a = b
# destinos de bl fuera de funciones
outside = set()
for name, s0, s1 in segs:
    for a in range(s0, s1, 4):
        x = w(a)
        if (x >> 26) == 18 and (x & 3) == 1:
            d = x & 0x3FFFFFC
            if d & 0x2000000: d -= 0x4000000
            t = (a + d) & 0xFFFFFFFF
            if not inside(t) and not (0x8309AD54 <= t < 0x8309C200): outside.add(t)
out.write("# bl a destinos fuera de .pdata: %d\n" % len(outside))
for t in sorted(outside): out.write("bl_target 0x%08X\n" % t)
print("huecos=%d palabras=%d bl_fuera=%d" % (ngap, nw, len(outside)))
