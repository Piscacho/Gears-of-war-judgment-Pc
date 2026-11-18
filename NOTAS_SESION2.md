# Sesión 2 (larga) — XenonRecomp, runtime de NG2 portado, título, menús y partida en marcha

Fecha: 2026-11-14. Sin commits. Método propio (sin código de ports ajenos).

## Resultado

- **C++ generado completo** (`ppc/`, 350 trozos, ~256 MB, **88.628 funciones**), 0 switches con error, 0 instrucciones sin reconocer.
- **GOWJ.exe** (`project/build/`, 66 MB) con el runtime de NG2 adaptado (`runtime/`; copia intacta en `runtime_ng2_orig/`).
- Llega a: vídeos Bink de logos → **PRESS START** → **menú principal** → Campaign → Judgment → New → **Select Difficulty** →
  pantalla de carga (Baird) → **partida en marcha** (la escena 3D se dibuja en la superficie HDR: Baird con el Lancer en tercera persona).
- Memoria estable en partida (~330 MB). ~20 fps en partida (17 resolves por CPU por cuadro).

## Arreglos del final de la sesión (GPU)

- **Texturas en el vertex shader**: la tabla de SRV (t1-t48) y el sampler s0 ahora son visibles para todos los shaders, y se enlazan
  las texturas que usa el VS además de las del PS (`texMask2D/3D/Cube`). Antes 4 pares VS/PS de UE3 fallaban al crear el PSO
  (`hr=80004005/80070057`, RT 87) y esos draws se omitían. Ahora 0 PSO fallidos.
- **Formato 54** (`k_2_10_10_10_AS_16_16_16_16`, mismos bits que 7): se sube como R10G10B10A2 y, si hay un resolve por GPU 8888
  de esa dirección, se enlaza directamente (`GOWJ_NO54AS8888=1` = antes). También 50 (8888_AS_16) y 51-53 (DXT_AS_16).
  `HostRep` ya no trata 54 como de 3 componentes. La escena (`0B2C0000`, resolve fmt 7 desde la superficie 14000500) llega al tonemap.

## Problema abierto (siguiente sesión)

**Sigue la imagen final negra en partida** (tras los dos arreglos de arriba). Cadena del final de cuadro (GOWJ_DRAWLOG=t270):
escena HDR (EDRAM b720 p1280, RGBA16F) → resolve fmt 7 a `0B2C0000` → tonemap (lee tf1=54@0B2C0000, tf2=6@0C110000 /
22@0B9F0000, tf3=18@02693000) escribe 8888 en EDRAM 720 → resolve a `0B9F0000` → PS 0BA717F87EEF07F8 copia al frontbuffer `003B0000`.
La superficie 8888 de EDRAM 720 sale negra: revisar el draw del tonemap (PS 64E4BB659DD2D065 y los de 24 draws n=1 con PS 50C7E959),
el contenido real que ve en tf1 (¿se sube bien el 2_10_10_10_FLOAT como 2_10_10_10 UNORM? la superficie es formato 3 = 2_10_10_10_FLOAT,
el resolve fmt 7 debería convertir de float7e3 a unorm; comprobar `PackResolve` para superficie 3), y los colores raros del personaje
en la escena HDR (cian/rayas: probablemente normales o shaders de piel).

**Hipótesis principal (comprobar primero):** el resolve a `0B2C0000` se hace con `color0=000202D0` = superficie **formato 2**
(2_10_10_10 UNORM) en la base 720, pero la escena se dibujó en la misma base como **formato 3** (2_10_10_10_FLOAT, que el runtime
emula con RGBA16F). `GetEdramRt` crea recursos distintos por formato, así que el resolve lee una superficie 2 vacía → negro.
UE3 en 360 resuelve la escena 7e3 "en bruto" con una vista 2_10_10_10 y el tonemap la lee como fmt 54 (bits crudos) y la
decodifica en el shader. Arreglo: alias 2↔3 en la misma base (como el 12→3 de NG2 sesión 25): al resolver/leer formato 2 sobre
una superficie 3, codificar cada canal RGBA16F a float 7e3 (3 bits de exponente, 7 de mantisa) + alfa de 2 bits y empaquetar
los bits como fmt 7.

**Imagen final negra en partida.** La escena HDR (EDRAM b720 p1280, RGBA16F, máx. 81) tiene contenido (con colores raros: cian y rayas),
pero las superficies 8888 finales (b0 p1280 / b720 p1280) salen negras → falla el posproceso/tonemapping de UE3.
Pistas:
- `PSO fallido hr=80004005 (prim 4, formato RT 87, mask=3)` para 4 pares VS/PS (9D857238DBBA5ADA, 1AAB47E69ECEF7C7, 57F822FEACD214FC);
  la capa de depuración no dio mensajes (revisar que `DrainDebugMessages` se llame).
- 17 resolves por CPU por cuadro (formatos 26/28/29/31 desde superficies 5/7), con sesgo de exponente -5: revisar la conversión.
- `resolve OMITIDO`: src=4 (profundidad) a fmt 6 sobre superficie formato 3.
- Texto de la pantalla de carga/"desclasificar" corrupto (no es la caché: el hash completo de k_8 no lo arregló).
- Herramientas: `GOWJ_DUMPEDRAMAT=<s>` vuelca todas las superficies EDRAM (ahora también HDR: `_hdr.bmp`, con tonemap simple);
  `tools/window_shot.py` captura la ventana real (los DUMP del guion no funcionan si no hay frontbuffer resuelto por GPU).
- Guion de entrada: `scripts/campaign4.txt` (START 24 s, A 34 s, A cada 7 s hasta Select Difficulty, A cada 8 s desde 100 s).

## Cambios a XenonRecomp (`tools/patch_recompiler_gowj.py`, idempotente, sobre el parche de NG2)

- `mulchw/mulchw.` (PPC405) fuera de la tabla: en Xenon esa codificación es `vsldoi128` (36 en Bink).
- `vnor128`, `vpkuhus128` (= vpkuhus) y `vpkuwus/vpkuwus128` (nuevo, min_epu32 + packus_epi32).
- `tools/build_msvc.bat` con rutas de este proyecto.

## Config

- Switches `config/gowj_switch_tables_all.toml` = XenonAnalyse (81) + jt_finder (30) + jt_nobound (3, Bink: lis/slwi/addi en cualquier orden).
  Corregido a mano: 0x823B5D30 r4 → r5 (`tools/jt_check_regs.py`).
- `functions` (`tools/make_functions.py`): 9 huecos con switch (`gap_functions.py` → `config/gowj_gap_functions.txt`) + setjmp +
  10 hojas sin .pdata a las que se llega por punteros (thunks de GMalloc 0x82E38080…, `lisaddi_functions.py`/`ptr_functions.py`).
- `tools/run_recomp.py` ahora aparta también trozos viejos cuyas funciones están todas en otros (fallo de enlace por duplicados).

## Arreglos del runtime (sobre el de NG2)

1. Partición XDVDFS detectada (XGD3 = 0x2080000; NG2 era XGD2).
2. **Memoria virtual como la consola**: sin MEM_LARGE_PAGES (0x20000000) → páginas de 4 KB en 0x00100000-0x3FFF0000; con ellas, 64 KB en
   0x40000000+. El asignador de UE3 (sub_82989EB8) trata todo puntero 0x4xxxxxxx como de su pool: un bloque grande en 0x4... se
   reasignaba sin copiar → tabla del TOC vacía → bucle infinito en sub_822B48E8.
3. `NtQueryVirtualMemory` con la reserva real (`GuestQueryRegion`; montones con `std::map`).
4. **SLIST**: `InterlockedPopEntrySList` / `InterlockedFlushSList` reales (eran stubs a 0): los pools de PhysX (PxcCache) nunca
   reutilizaban bloques → 1,7 GB en uso → `appError "OOM"` → congelado.
5. Hash completo de texturas k_8 ≤ 4 MB (`GOWJ_NOK8FULLHASH=1` = antes).
6. Ganchos (`hooks_gowj.cpp`): `appError` (sub_82321758) registra el mensaje fatal; `GOWJ_MAPCHECK` (ciclos en TMap); `GOWJ_REALLOCLOG`.
7. Diagnóstico nuevo: `GOWJ_WATCHADDR/WATCHLEN/WATCHSTATIC/WATCHFILTER` (vigilancia de escrituras por protección de páginas, con valor y
   RIP host → `llvm-symbolizer`), `GOWJ_MEMSTATS`, `GOWJ_MEMTRACE=<s>` (pila de invitado por la cadena r1), `GOWJ_TEXLIST=<s>`,
   `[mem] virtual4k` en el resumen por minuto.
8. Variables de entorno renombradas `NG2_` → `GOWJ_`; ganchos/fibras/direcciones de NG2 retirados (fibras: UE3 no las usa).

## Direcciones útiles

| | |
|---|---|
| GMalloc | 0x83523438 (proxy con lock sub_829D5EC8; interno Realloc sub_82989EB8) |
| TArray::Realloc | sub_822B1D10 |
| appError | sub_82321758 (r4 = mensaje ancho) |
| Mapa del TOC | 0x8355CF48 |

## Comandos

```
python tools/run_recomp.py logs/recompN.log
tools\build_game.bat
python tools/run_game.py logs/runN.log 260 GOWJ_INPUTSCRIPT=../scripts/campaign4.txt [GOWJ_DUMPEDRAMAT=200]
```
