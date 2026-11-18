"""Switches sin cmplwi (lis/addi r12 ; slwi r0,rI,2 ; lwzx ; mtctr ; bctr ; tabla inline).
Numero de entradas = hasta la primera etiqueta (donde empieza el codigo). python -I jt_nobound.py basefile.bin BCTR... > salida.toml"""
import struct, sys
img = open(sys.argv[1], "rb").read(); B = 0x82000000
w = lambda a: struct.unpack_from(">I", img, a - B)[0]
for s in sys.argv[2:]:
    bctr = int(s, 16)
    win = [w(bctr - k) for k in (20, 16, 12)]   # GoW: lis/addi/slwi en cualquier orden (Bink: lis, slwi, addi)
    lis = [x for x in win if x >> 26 == 15][0]; addi = [x for x in win if x >> 26 == 14][0]; slwi = [x for x in win if x >> 26 == 21][0]
    lo = addi & 0xFFFF; lo -= 0x10000 if lo & 0x8000 else 0
    table = ((lis & 0xFFFF) << 16) + lo & 0xFFFFFFFF
    assert table == bctr + 4, (s, hex(table))
    idx = (slwi >> 21) & 31
    labels = []; end = 0xFFFFFFFF; a = table
    while a < end:
        l = w(a); assert bctr < l < bctr + 0x10000, (s, hex(a), hex(l))
        labels.append(l); end = min(end, l); a += 4
    print("[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x0\nlabels = [\n%s]\n" % (bctr - 20, idx, "".join("    0x%08X,\n" % l for l in labels)))
    sys.stderr.write("0x%08X r%d %d entradas\n" % (bctr - 20, idx, len(labels)))
