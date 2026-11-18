# Sesión 8 — Rendimiento y gráficos

Fecha: 2026-11-15. Sin commits.

## Resultado

- **Campaña (Old Town): de 8,5 fps a 30 fps** (el límite del propio juego) casi todo el tiempo; Survival a 21-30 fps.
- Por cuadro: **0 resolves por CPU** (antes 33) y **0 volcados GPU→memoria** (antes 1-11, cada uno con espera a la GPU).
- **Cuñas negras de sombra arregladas** (mapas de sombra leídos de memoria vieja). Con eso Baird ya no sale cobrizo en las
  zonas de cobertura.
- Medición: el hilo principal del juego pasaba el 98 % esperando; el cuello era el hilo de comandos de la GPU
  (`GOWJ_PROFILE=<s>:<n> GOWJ_PROFILEALL=1` + `python tools/profagg.py logs/runN.log`; `profagg.py` ya apunta a GOWJ.exe).

## Cambios (`runtime/gpu_d3d12.cpp`, `gpu_cp.cpp`)

1. **Escena HDR a 7e3 por GPU**: el resolve "en bruto" de la superficie 3 (RGBA16F) a fmt 7 que lee el tonemap como fmt 54 se
   hacía en la CPU (0,9 M píxeles + espera, 54 % del hilo). Ahora un shader (`mode.y == 3`, `F7e3`) escribe los bits 7e3 en una
   textura R10G10B10A2 que se enlaza para fmt 7/54. `GOWJ_CPU7E3=1` = antes. Las estadísticas `[7e3]` por píxel ahora solo con
   `GOWJ_7E3STATS=1`.
2. **Posproceso por GPU** (bloom, DOF, luminancia): superficies 16_16_16_16 (SNORM×32, sesgo +5) o 16_16_FLOAT / 16_16_16_16_FLOAT
   resueltas a 26 / 32 / 31 (y 2_10_10_10_FLOAT → 7 con MSAA, muestra 0) con conversión genérica (`mode.y == 4`: valor/outScale ×
   2^sesgo). Texturas 26 (R16G16B16A16_UNORM), 31/28 (R16G16_FLOAT) enlazadas directamente o como región derivada; volcado a
   memoria si hace falta. `GOWJ_CPUPOST=1` = antes.
3. **LUT de color 3D en la GPU**: el resolve 16×512 se copia capa a capa (filas 32z..32z+15) a una textura 3D B8G8R8A8; ya no se
   vuelca a memoria a mitad de cuadro. `GOWJ_NOGPULUT=1` = antes.
4. **Constantes por draw con memcpy**: `g_gpuConstShadow` (registros 0x4000-0x4FFF en orden de host, actualizada en
   `WriteRegister`) en vez de 2×1024 lecturas con cambio de endian por draw. `GOWJ_NOCONSTSHADOW=1` = antes;
   `GOWJ_CONSTCHECK=1` compara con los registros (comprobado: sin diferencias).
5. Camino caliente sin trabajo de diagnóstico: `CmdTrace` (vsnprintf + std::string por draw) solo con `GOWJ_CMDTRACE=1`
   (o los modos GOWJ_FLUSHEACH/FLUSHRESOLVE); `getenv` fuera de los bucles; búfer de índices reutilizado y un bucle por caso
   de endian.
6. **Sombras**: un resolve de profundidad por GPU (p. ej. 256×224 en el origen) leído como k_24_8 más grande (1120×688, el atlas)
   no se enlazaba y se leía la memoria del juego, que no tenía el resolve → basura = "en sombra" → cuñas negras. Ahora se hace
   una copia derivada R32_FLOAT en la GPU (`g_derivedDepth`). `GOWJ_NODERIVEDDEPTH=1` = antes.

## Guiones

- `scripts/campaign8.txt`: campaña larga con capturas cada 12 s. `scripts/campaign8b.txt`: pulsa A durante la intro para
  saltar las cinemáticas ("TO SKIP") y llega antes a las zonas de combate.

## Pendiente

- Algunos personajes (Paduk/Cole) siguen con tonos cobrizos fuertes en ciertas luces; sin referencia no está claro si es
  correcto (el cielo de Halvo Bay es rojizo). Revisar con capturas de la consola si las hay.
- Picos de ~50-90 ms en momentos de combate y carga; texturas k_8 grandes se siguen hasheando enteras en cada uso.
- `fetch op 19`, avisos XAM sin implementar, prueba larga a mano con el mando (`run\jugar.bat`).
