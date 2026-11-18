# Sesión 1 — Extracción y mapeo del XEX (Gears of War: Judgment, X360)

Fecha: 2026-11-14. Sin commits (igual que en GH WoR y NG2).
Método propio (XenonRecomp parcheado + runtime propio de GH/NG2). **No se usa código de ports ajenos**
(ni el repo OverkillLabs3/gears-judgement-recomp basado en ReXGlue, ni COGBox-0.4.8.zip que hay en la carpeta del juego).
Plan por sesiones: 1 XEX → 2 TOML + C++ → 3 runtime → 4 GPU → 5 audio/entrada/vídeo (Bink) → 6 TU → 7 lanzador.

## Carpetas

- `G:\gears of war judgment\` — originales (ISO, `.7z`, `Update\TU_16L62H6_…`). **Solo lectura.**
- `G:\Gears of war Judgmanet pc\` — todo el trabajo:
  `tools\` (copiadas de NG2 y adaptadas), `xex\`, `config\gowj.toml`, `tu\`, `ref\` (tablas de Xenia, BSD), `rea\`, `logs\`.

## Qué se hizo

1. **ISO → default.xex** (`tools/xdvdfs.py`). **XGD3**, partición en `0x2080000`. 2.371 entradas (`xex/iso_filelist.txt`).
   - `xex/default.xex`: 22.855.680 bytes, SHA1 `426c5d27ff73d3ff2af9d404adb49e9a3db18bf1`.
   - Datos UE3 en `GearGame/CookedXbox360/*.xxx` (paquetes cocinados). También `AvatarAssetPack`, `AvatarAwards`, `$SystemUpdate/`.
2. **XEX → imagen** (`tools/xex2.py`): AES retail + compresión **basic**.
   - `xex/basefile.bin`: 23.756.800 bytes (imagen 0x16B0000), SHA1 `b5d68072bf1942290bd3c6b3f664539d630dfafd`. Offset = VA − 0x82000000.
3. **Secciones** → `xex/pe_sections.txt`. **Imports** → `xex/imports_named.txt`: 311 funciones + 324 variables, todas con nombre.
4. **Funciones** (`tools/analyze.py`) → `xex/functions_pdata.txt`: **43.348** en `.pdata`.
5. **Huecos sin .pdata** (`tools/pdata_gaps.py`, nuevo) → `xex/pdata_gaps.txt`: 6.720 huecos (~219.000 palabras) y
   **2.516 destinos de `bl` fuera de cualquier función de `.pdata`** (en NG2 hicieron falta solo 174 manuales).
   Son sobre todo funciones hoja sin `.pdata` (p. ej. 0x822A2740, un inicializador de 0xD8 bytes). Trabajo principal de la sesión 2.
6. **Tablas de saltos**: XenonAnalyse (el compilado en NG2) → 81; `tools/jt_finder.py` (adaptado, `JT_ROOT`) → 30 más
   (13 BYTE, 14 SHRT, 3 ABS). Total `config/gowj_switch_tables_all.toml`: **111**. Hay 1.584 `bctr`: la mayoría son llamadas
   virtuales de UE3 (`lwz` de vtable + `mtctr`), no switches.
7. **TOML borrador**: `config/gowj.toml`.
8. **Actualización del título aplicada** con las herramientas propias de NG2 (sesión 26):
   - `tools/stfs_extract.py` → `tu/pkg/000b0000_tu_…/` (LIVE, tipo 0xB0000, 108 entradas): `default.xexp` (1,8 MB),
     `GearGame/Patches/ScriptPatch_Engine.bin`, `ScriptPatch_GearGame.bin` (+ `_us`), varios `TU__*.ini` y textos por idioma.
   - Versión **0x3 → 0x403**. Cabeceras 0x4000 → 0x4000 (1 registro LZX). Imagen 0x16B0000 → **0x16F0000**.
   - Imagen: 30 bloques, 894 registros (38 ceros, 183 copias, 673 LZX).
   - Pasos: `xexp_records.py tu/default.xexp tu/work` (1.ª vez falla al no haber new_hdr; deja hdr_records.bin) →
     `lzxdelta hdr_records.bin 0x8000 base_hdr.bin 0x4000 new_hdr.bin` (base_hdr = primeros 0x4000 del default.xex) →
     `xexp_records.py` otra vez → `lzxdelta img_records.bin 0x8000 ../../xex/basefile.bin 0x16F0000 tu_basefile.bin`.
   - Resultado `tu/work/tu_basefile.bin` (24.051.712 bytes): entry 0x82DBFFD8 (`mflr r12`), 43.385 funciones `.pdata`.
   - Los `ScriptPatch_*.bin` son parches de UnrealScript que el juego carga del paquete TU: el runtime tendrá que montar el TU como contenido.

## REA

No soporta PowerPC ni STFS (ver `rea/LEEME.md`). Se usa `tools/ppcdasm.py` (capstone). Útil a partir de la sesión 3.

## Hallazgos

| Dato | Disco (v3) | TU (v0x403) |
|---|---|---|
| Title ID / media ID | 4D530A26 / 3528321A | igual |
| Imagen | 0x16B0000 | 0x16F0000 |
| **Entry point** | **0x82DBD420** | 0x82DBFFD8 |
| Motor | Unreal Engine 3 | |
| Vídeo | **Bink** (secciones BINK/RADCODE, como GH) | |

### Secciones (disco)

| Sección | VA | Tamaño | Tipo |
|---|---|---|---|
| .rdata | 0x82000600 | 0x23E1C4 | RO |
| .pdata | 0x8223E800 | 0x54AA0 | RO |
| BINKCONS / RADCONST | 0x82293400 / 0x82296200 | | RO |
| **.text** | **0x822A0000** | **0xDFC054** | código |
| BINK | 0x8309C200 | 0x20BB0 | código |
| RADCODE | 0x830BCE00 | 0xAFC | código |
| BINKBSS / .data | 0x830C0000 / 0x830C5C00 | 0x5B1C / 0x4BA5A0 | RW |
| .idata | 0x83590000 | 0x588 | RW |
| .reloc | 0x835A0200 | 0x1737D0 | RO |

Thunks de import en 0x8309AD54…0x8309C1FF (final de `.text`).

### Direcciones para XenonRecomp (ya en `config/gowj.toml`)

| | Disco | TU |
|---|---|---|
| savegprlr_14 / restgprlr_14 | 0x82D9F360 / 0x82D9F3B0 | 0x82DA1FC0 / 0x82DA2010 |
| savefpr_14 / restfpr_14 | 0x82D9FC20 / 0x82D9FC6C | 0x82DA2880 / 0x82DA28CC |
| savevmx_14 / restvmx_14 | 0x82D9FD20 / 0x82D9FFB8 | 0x82DA2980 / 0x82DA2C18 |
| savevmx_64 / restvmx_64 | 0x82D9FDB4 / 0x82DA004C | 0x82DA2A14 / 0x82DA2CAC |
| longjmp | 0x82DA1ED0 (len 0x314) | 0x82DA4A80 |
| setjmp | 0x82DA21F0 (sin .pdata, 0x2CC) | 0x82DA4DA0 |

- setjmp igual que en NG2: `lis r4,0x8358; lwz r0,-0x3B4(r4)` → gancho opcional en **0x8357FC4C**, `bnectr`, `mflr r0; mfcr r4; stfd f14,0(r3)`…
- vmx_64 = vmx_14 + 0x94, como en GH y NG2.

## Herramientas nuevas

- `tools/find_jmp.py` (candidatos a setjmp/longjmp), `tools/pdata_gaps.py` (huecos y destinos de bl fuera de `.pdata`).
- `tools/jt_finder.py` ahora toma la raíz de `JT_ROOT` (por defecto esta carpeta).
- Ojo: con `python -I` los scripts que importan `pe_sections` fallan; ejecutarlos con `python` normal (son nuestros).

## Próximos pasos (sesión 2)

1. Copiar el XenonRecomp parcheado de NG2 (`G:\ninja gaiden 2\Recomp\XenonRecomp` + `tools/patch_recompiler_ng2.py`) y compilarlo.
2. Generar `functions` para los ~2.516 destinos sin `.pdata` (adaptar `make_functions.py` / `gap_functions.py` de NG2:
   inicio = destino de bl, fin = siguiente inicio conocido o `blr` final) y repetir con el log de XenonRecomp.
3. XenonRecomp con `config/gowj.toml` → `ppc/` y conseguir que el C++ compile (CMake + Ninja + clang-cl).
4. Decidir si la build principal es la del disco (como NG2) o directamente la de la TU (la TU trae parches de script y
   correcciones; en NG2 se hizo exe aparte NG2TU.exe).
