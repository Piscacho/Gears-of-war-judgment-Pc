"""Reescribe la lista functions de config/gowj.toml = config/gowj_gap_functions.txt + entradas manuales.
python -I tools/make_functions.py"""
import re, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANUAL = {
    0x82DA21F0: 0x2CC,  # setjmp
    # sesion 2: hojas sin .pdata a las que se llega por punteros (tools/lisaddi_functions.py, tools/ptr_functions.py)
    0x82E38080: 0x20, 0x82E380A0: 0x30, 0x82E380D0: 0x28, 0x82E380F8: 0x24, 0x82E38120: 0x1C,   # thunks de GMalloc (0x834D5588) tras el switch de 0x82E37F08
    0x82D9D930: 0xC, 0x82DA1BE8: 0xC, 0x82D9D4FC: 0x28, 0x82DA8F78: 0x18, 0x82DABBE4: 0x18,
    # sesion 7: memset del CRT (sin .pdata). XenonRecomp cortaba la funcion en el primer 'bdzlr-' (BO=26): las colas de 2-3 bytes
    # no se escribian (SkelControlIndex con basura -> caida en sub_826A8DF8)
    0x82D9F940: 0xA0,
}
cfg = os.path.join(root, "config", "gowj.toml")
gap = dict((int(a, 16), int(s, 16)) for a, s in re.findall(r"address = 0x(\w+), size = 0x(\w+)", open(os.path.join(root, "config", "gowj_gap_functions.txt")).read()))
gap.update(MANUAL)
# una funcion manual dentro de una de hueco estimada: la de hueco se recorta (comprobado que no deja fuera etiquetas de switch)
items = sorted(gap.items())
for i in range(len(items) - 1):
    (a, s_), (b, _) = items[i], items[i + 1]
    if a + s_ > b and b in MANUAL and a not in MANUAL: gap[a] = b - a
fs = sorted(gap.items())
bad = [(hex(a), hex(b)) for (a, s), (b, _) in zip(fs, fs[1:]) if a + s > b]
assert not bad, bad
t = open(cfg).read()
i = t.find("functions = [")
t = (t[:i] if i >= 0 else t.rstrip() + "\n") + "functions = [\n" + ",\n".join("{ address = 0x%08X, size = 0x%X }" % x for x in fs) + "\n]\n"
open(cfg, "w").write(t)
print(len(fs), "funciones")
