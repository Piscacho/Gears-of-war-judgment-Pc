"""Sesion 2 (GoW Judgment): cambios a XenonRecomp sobre el parche de NG2. Idempotente.
- mulchw/mulchw. (PPC405) fuera de la tabla: en Xenon esa codificacion es vsldoi128 (Bink).
- vnor128, vpkuhus128 (= vpkuhus) y vpkuwus/vpkuwus128 (nuevo)."""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
d = os.path.join(root, "XenonRecomp", "thirdparty", "disasm", "ppc-dis.c")
t = open(d).read()
for s in ('{ "mulchw",\tXRC(4,168,0)', '{ "mulchw.",\tXRC(4,168,1)'):
    if s in t and "//" + s not in t: t = t.replace(s, "//" + s)
open(d, "w").write(t)
r = os.path.join(root, "XenonRecomp", "XenonRecomp", "recompiler.cpp")
t = open(r).read()
if "case PPC_INST_VNOR128:" not in t:
    t = t.replace("    case PPC_INST_VNOR:\n", "    case PPC_INST_VNOR:\n    case PPC_INST_VNOR128:\n", 1)
if "case PPC_INST_VPKUHUS128:" not in t:
    t = t.replace("    case PPC_INST_VPKUHUS:\n", "    case PPC_INST_VPKUHUS:\n    case PPC_INST_VPKUHUS128:\n", 1)
if "case PPC_INST_VPKUWUS128:" not in t:
    t = t.replace("    case PPC_INST_VPKUHUS:\n", "    case PPC_INST_VPKUWUS:\n    case PPC_INST_VPKUWUS128:\n"
        '        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi32(simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi32(0xFFFF)), simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi32(0xFFFF))));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));\n'
        "        break;\n\n    case PPC_INST_VPKUHUS:\n", 1)
open(r, "w").write(t)
print("ok")
