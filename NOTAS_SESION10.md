# Sesión 10 — Partida online, pantalla de título, ratón y teclado, DualSense

Fecha: 2026-11-15. Sin commits.

## Resultado

- **Online (enlace de sistema): la partida ya empieza.** El anfitrión tomaba el nuevo intento del invitado como la conexión vieja
  del grupo (mismo origen 127.0.0.2:1000). En `net_gowj.cpp`, el invitado enlaza cada socket VDP nuevo del mismo puerto en
  `puerto + 100 × generación` (solo los VDP, no la baliza 14001). Survival por enlace de sistema funciona de principio a fin con
  dos instancias (`tools/run_pair.py`, guiones `net_host4` / `net_join4` con márgenes más amplios). `GOWJ_NETHEX=1` vuelca bytes.
- **Pantalla de título (brillo blanco y bloques pixelados) arreglada.** La causa era el signo **gamma** de las texturas de Xenos
  (dword0 de fetch, bits 2-9, valor 3) sin implementar. Ahora `XTexMode.w` lleva los signos y el PS aplica `XTexSign` después de
  cada tfetch: gamma → lineal con la curva PWL de Xenia (`XGammaToLin`), firmado y con sesgo opcionales (`GOWJ_TEXSIGNED=1`).
  `GOWJ_NOTEXGAMMA=1` lo quita. Mi primera curva PWL era demasiado oscura y estropeaba el texto; con la de Xenia, bien.
  También se dejó el recorte de salida 7e3 a 31,875 (como el hardware, `XMisc` en el cbuffer @4704; `GOWJ_PSCLAMP` diagnóstico).
- **Ratón y teclado para jugar** (el usuario no podía mover la cámara):
  - `gpu_window.cpp`: ratón en bruto (WM_INPUT, `RegisterRawInputDevices`), captura con `ClipCursor` y cursor oculto. Se captura
    al activar la ventana o al hacer clic; **Alt** lo suelta (y al perder el foco). `GOWJ_NOMOUSE=1` lo desactiva.
  - `xam.cpp` `MouseToStick`: la velocidad del ratón (ventanas ≥ 10 ms) pasa a stick derecho con un mínimo de 7.500 para saltar
    la zona muerta; `sens_raton` (`GOWJ_MOUSESENS`) e `invertir_y` (`GOWJ_MOUSEINVERT`). Si el ratón se mueve, manda sobre el
    stick derecho físico.
  - Distribución de shooter de PC: WASD moverse, clic izq. = RT (disparar), clic der. = LT (apuntar), Espacio/Mayús = A,
    E = X, R = RB (recargar), Q / rueda = Y (cambiar de arma), F / clic rueda = B (golpe), G = LB (granada), C = L3, X = R3,
    1-4 / flechas = cruceta, Esc / Intro = START, Tab = BACK, Retroceso = B. IJKL sigue moviendo la cámara.
  - Launcher: en Controles, "Sensibilidad del ratón" (0,5 a 3) e "Invertir el eje vertical del ratón", y la tabla actualizada.
- **DualSense**:
  - `dualsense.cpp`: por Bluetooth Windows entrega todos los informes con la longitud máxima (78), así que el informe básico
    0x01 se decodificaba con la distribución de USB. Ahora el bus se decide por `HidP_GetCaps` (InputReportByteLength 64 = USB).
    Solo se abre la colección de mando (página 1, uso 5). Por Bluetooth se pide el informe de calibración (0x05 / 0x02 en el DS4)
    para activar los informes completos 0x31 / 0x11. Pausa de 500 ms antes de reintentar si falla la lectura.
  - `xam.cpp`: los sticks del DualSense se mezclan siempre con los de XInput (gana el eje con más recorrido). Antes solo se usaban
    sin ningún mando XInput, así que un XInput virtual quieto (Steam, DS4Windows) los anulaba.
  - Registro: `[input] HID Sony …`, `mando de PlayStation conectado: DualSense (USB|Bluetooth)` y los dos primeros informes ya
    decodificados. `GOWJ_PADLOG=1` registra cada cambio de botones.
  - Comprobado en este PC: DualSense por USB detectado y leído (sticks en reposo −512, −1281, 256, −769).

## Rendimiento: combate de 10-13 a 22-60 fps

- Causa principal: `buildCb` (constantes por draw) hacía lecturas sobre la memoria del anillo (heap de subida, escritura combinada,
  sin caché): `XTexMode.w |= signo` (añadido esta sesión), medio píxel `+= 0.5`, sesgo de color. ~130.000 lecturas sin caché por
  cuadro. Ahora se construye en un búfer local `thread_local` y se copia de una vez (`GOWJ_CBDIRECT=1` = antes).
- SRV por draw: se copia de golpe un bloque fijo de 48 descriptores (16 blancos 2D, 16 nulos 3D, 16 nulos cubo) con
  `CopyDescriptorsSimple` y solo se crean los de las ranuras que usa el shader (antes 48 `CreateShaderResourceView` por draw).
  `GOWJ_ALLSRV=1` = antes.
- Huella de texturas recorridas enteras (k_8, <= 64 KB) con 4 carriles (`HashDwordsFast`); `GOWJ_TEXHASHSLOW=1` = antes.
- Medido con `campaign8b` y `fps=60`: cuadros lentos de 88 a 45 ms; mínimo 22 fps, normalmente 30-60. Perfil nuevo del hilo
  de la GPU: 64 % ocupado (subidas de texturas que cambian 9 %, esperas a la GPU en `FlushAndWait` 15 %).

## Español latino

- El disco solo trae español latino (`ESM`, con voces dobladas). `XGetLanguage` y `ExGetXConfigSetting(3, 9)` devuelven el idioma
  de `idioma=` en gowj.ini (`GOWJ_IDIOMA`; por defecto `es` = 5). El juego (`sub_8298B3A0`, appGetLanguageExt) elige ESM salvo con
  región España, pero solo acepta idiomas cuyo `GearGame\Xbox360TOC_<idioma>.txt` encuentre listando `..\GearGame\*.txt`.
- **Fallo del sistema de archivos arreglado**: al indexar el ISO, los hijos de un subdirectorio se guardaban bajo su nombre con
  mayúsculas ("GearGame") y se buscaban en minúsculas: ningún subdirectorio del ISO se podía listar. Además el patrón de
  `NtQueryDirectoryFile` solo llega en la primera llamada (ahora se guarda en el handle, como NT).
- Launcher: Audio → "Idioma" (español latino, inglés, francés, alemán, italiano, polaco, ruso, coreano, chino).
- Subtítulos: opción del propio juego (Opciones).

## Ratón (2)

- El usuario: lento casi no mueve, rápido demasiado. Ahora la velocidad del ratón pasa por una raíz (`GOWJ_MOUSECURVE`, 0,5) que
  compensa la zona muerta y la curva del stick del juego; tope a ~2.400 cuentas/s y media de dos ventanas.

## Pendiente de lo que reportó el usuario (sesión 10)

- **Armas que brillan de azul** (todo el arma cian translúcida en partida; en cinemáticas se ve bien) y **neblina que sigue** al
  jugador (posiblemente el resplandor de lo mismo). Descartado: cubemaps (`GOWJ_NOCUBE=1` no lo quita) y signo de texturas.
- **Texto corrupto** (cajas de tutorial con glifos mezclados).
- **Audio con fallos** (falta saber cómo falla).

## Sin probar a mano

- El ratón no se puede inyectar con los guiones: falta que el usuario pruebe cámara, clics y rueda en partida.
- DualSense por Bluetooth (no hay uno por BT aquí).

## Pendiente

1. fps en combate (objetivo 60).
2. Prueba de campaña más larga; texto corrupto intermitente (caché de glifos k_8); brillo azul exagerado en el arma.
3. Ajustar la curva del ratón si el usuario la nota lenta, rápida o con aceleración.

## Tercera tanda (audio, ratón de Gears 3, imagen)

- **Audio: faltaban sonidos y voces.** XAudio lanza (kick, MMIO 0x650) y limpia (0x6A0) los contextos XMA de una voz en un bucle,
  un `stwbrx` de un bit por contexto: varios seguidos del mismo grupo de 32 se pisaban antes de que el hilo del XMA (cada 1 ms) los
  leyera, y esos contextos no se decodificaban. Ganchos en `sub_82C0D7F0` (lanzar la lista de contextos de una voz: entradas de
  0x60 bytes, índice u16 en +0x50) → `XmaKickContext` de cada uno, y `sub_82C0CE58` (crear/limpiar) → `XmaClearContext` de los nuevos.
  Muestras decodificadas en el mismo tramo de campaña: de ~10 M a ~91 M. `GOWJ_XMA_NOHOOK=1` = antes.
- **Ratón nativo de UE3 (copiado del port de Gears 3, sesión 5)**: `sub_827621F0` = InputAxis del cliente del viewport; al llegar
  `XboxTypeS_RightX` (global 0x835516B8) se repite con `MouseX` (0x83551348) / `MouseY` (0x83551350) y el movimiento acumulado
  (×0,25×sensibilidad). Mismas globales que Gears 3 desplazadas −0x2D80 (InputKey Judgment 827620F0 ↔ GoW3 8273B418 con
  `tools/funcmatch.py` de Gears 3). Ya no pasa por el stick (sin zona muerta, curva ni tope). `GOWJ_MOUSESTICK=1` = sistema
  anterior; `GOWJ_MOUSELOG=1` registra. Comprobado que el gancho recibe RightX cada cuadro; falta la prueba a mano.
- **Ini del juego en memoria** (también de Gears 3): `fs.cpp ApplyIniOverrides` reescribe los `Coalesced_*.bin` al indexar el ISO
  (`IsoEntry::mem`). Por defecto `GearGame.GearPlayerInput:bViewAccelEnabled=FALSE` (sin aceleración de vista; `GOWJ_VIEWACCEL=1`
  la deja) y `GOWJ_INI="Sección:Clave=Valor;..."` para probar (p. ej. `SystemSettings:MotionBlur=False`).
- Profundidad `k_24_8_FLOAT` (fmt 23) resuelta por GPU: ahora se enlaza la copia de la GPU como la fmt 22 (antes se leía de memoria
  sin el resolve). Es la que usan las partículas/niebla con fundido por profundidad. `GOWJ_NODEPTH23=1` = antes.
- **Resolves a la misma dirección**: UE3 usa 0B9F0000 para varios resolves por cuadro (escena 1280x720, bloom 640x360/320x180, la
  región de 640x65 detrás de la caja de texto). (1) `ExactRecentResolve`: si el último no coincide pero hay uno de este cuadro con
  el tamaño y formato pedidos, se usa ese (y no se vuelca a memoria). (2) Resolve parcial con el mismo pitch sobre una textura
  completa reciente: la región se copia dentro de la grande en (x0, y0), como en la 360. `GOWJ_NOEXACTRESOLVE=1`,
  `GOWJ_NORESOLVEMERGE=1` = antes.
- FXAA del juego = PS 0BA717F87EEF07F8 (consola: luma en alfa, ramas con predicado); la traducción se revisó paso a paso contra el
  algoritmo y parece correcta. `GOWJ_NOGAMEFXAA=1` lo sustituye por el color sin suavizar (diagnóstico).

### Sigue pendiente

- **Arma cian translúcida**: está ya en la imagen que entra al FXAA (alfa = luma correcta, el arma brilla de verdad en la escena).
  No es el desenfoque de movimiento (con `MotionBlur=False` sigue), ni los cubemaps, ni la luz simple 9787713D. En las cinemáticas
  el arma se ve bien. Siguiente: GOWJ_DRAWLOG en un cuadro con el arma cian y buscar el draw que la pinta (sobre la superficie 14000500).
- **Texto corrupto intermitente**: no depende del FXAA (con el FXAA quitado salió igual en una prueba). En unas partidas sale y en
  otras no. Sospecha: la caché de glifos en memoria reutilizada que la huella de 64 muestras no detecta (probar páginas sucias de
  Gears 3, `texdirty.cpp`, como validez de las copias de la GPU y de las texturas k_8).

## Cuarta tanda: texto corrupto y arma cian (investigación)

Cambios que se quedan (correctos aunque no arreglan el fallo):
- **Páginas sucias con contador por página** (`runtime/texdirty.cpp`, base del port de Gears 3): `TexDirtyArmSig` protege un rango
  (vistas 0xA0000000 y 0xC0000000) y devuelve la suma de contadores de escritura; `TexDirtyValid` dice si sigue igual. Primero se
  protege y luego se marca (antes una escritura entre medias se perdía). El manejador de main.cpp cuenta la escritura y desprotege;
  `TexDirtyTouch` antes de ReadFile/recvfrom en memoria física. Lo usan: texturas > 64 KB y mipmaps (antes los mips no se
  comprobaban) y **la caché de vértices** (sin huella: combate de ~25 a 30-40 fps). `GOWJ_NOTEXDIRTY=1`, `GOWJ_VBHASH=1`
  (huella muestreada), `GOWJ_VBFULLHASH=1`, `GOWJ_VBBOTH=1` (diagnóstico: compara firma y huella completa; 0 diferencias).
  `[texdirty]` en el registro cada 5 s (15-20 mil escrituras capturadas por segundo).
- **Cerrojo por archivo** en `ReadInto` / `NtWriteFile` (posición + lectura atómicas; varios hilos leen el mismo paquete).
  `GOWJ_NOFILELOCK=1` = antes.
- Diagnóstico `[omitido]`: draws saltados porque el shader o el PSO aún se compilan (solo al principio, posproceso).

Lo que se sabe del fallo:
- Texto del HUD/tutoriales hecho de trozos de otras letras, iconos del HUD rotos y **arma del jugador cian translúcida** aparecen
  **juntos** y son fijos durante toda la partida: o salen en las 4 capturas de una prueba o en ninguna. Intermitente entre pruebas.
- No es: el FXAA, el desenfoque de movimiento, los cubemaps, el signo de texturas, la caché de texturas (la fuente k_8 01DF1000
  512x512 en memoria es correcta: `GOWJ_TEXDUMP=01DF1000`), los vértices viejos (firma = huella completa), los draws saltados
  por compilación.
- Hipótesis siguiente: algo se carga mal al cargar el nivel (datos de la fuente: coordenadas de los glifos; malla/esqueleto del
  arma) y se queda así. Comparar la memoria de los objetos UFont entre una prueba buena y una mala; revisar operaciones atómicas
  (lwarx/stwcx) y la carga asíncrona de UE3.

## Quinta tanda: arreglado con el método de Gears 3 (resolución dinámica)

- **Causa del texto roto, el HUD roto y el arma cian**: la **resolución dinámica de UE3**. Con la GPU cargada el juego dibuja la
  escena más pequeña (p. ej. 1280x433), la resuelve y la lee como textura de 1280x720 con las coordenadas escaladas (la región en
  la esquina). Nuestro runtime no lo admitía: se volcaba a memoria y se leían restos de cuadros viejos. Por eso era intermitente
  (dependía de si el juego bajaba la resolución en esa partida) y fijo dentro de la partida.
- Arreglo copiado del port de Gears 3 (sesión 3): `DynResFits(w, h, rw, rh)` en `gpu_d3d12.cpp` — se enlaza la copia de la GPU
  aunque se lea más grande (hasta 1280x736 y 4 veces); en la región derivada (8888 y otros formatos) y en `WriteBackResolvedForDraw`.
  `GOWJ_NODYNRES=1` = antes.
- Comprobado en 3 partidas seguidas (run269-271, 12 capturas): arma normal, texto y HUD bien. Antes salía en ~la mitad.
- Las armas tiradas en el suelo brillan en azul a propósito (resaltado de "recoger").
- Pendiente: el icono del arma en el aviso de "recoger" (A) sale a bloques en una prueba (run270): texturas DXT5 de 256/512
  dibujadas muy pequeñas → probablemente los niveles de mipmap (desplazamientos de los mips). Probar con `GOWJ_NOMIPS=1`.

## Sexta tanda: icono roto del aviso de "recoger"

- Volcado de mips (`GOWJ_TEXDUMP=<dirección>` o `GOWJ_TEXDUMP=M<formato>`, p. ej. `M20` = cualquier DXT5 de 128+ con mips; escribe
  `texdump_*_L<nivel>_*.raw` y una línea `[texdump]` con desplazamiento, pitch y posición empaquetada). Decodificados (DXT5 → imagen),
  los mips salen bien, incluidos los empaquetados: la disposición coincide con la de la 360.
- `mip_min_level` (dword 4 bits 2-5) ahora se respeta (`MostDetailedMip` del SRV; `GOWJ_NOMIPMIN=1` = antes). En la prueba ninguna
  textura lo usó, así que no era la causa del icono.
- El icono roto solo salió en 1 de 5 partidas y no se ha podido reproducir. `[drawtex]` registra ahora también dw4/dw5 y el endian:
  si el usuario lo ve, **F9** (registra los 2 cuadros siguientes con cada draw y sus texturas) + **F12** (captura) y mandar gowj.log.

## Séptima tanda: fps (combate de 20-28 a 44-55 fps)

Medido con `campaign8b` y `fps=60`, mismo tramo de combate:
1. **Constantes por draw**: `buildCb` tenía `if (constCheck) {...} else { 1024 Reg() }` — el `else` era del diagnóstico, no de la
   copia rápida, así que en cada draw se releían los 1024 registros encima de la copia de la sombra (14 % del hilo de la GPU,
   `Load32`). Ahora `else if (noShadow)`. Comprobado con `GOWJ_CONSTCHECK=1`: la sombra coincide (0 diferencias).
2. **Resolves de profundidad por la CPU**: solo había 64 RTV (32 destinos) y cada tamaño de región del atlas de sombras gastaba 2
   para siempre; agotados, cada resolve de profundidad iba por la CPU con `FlushAndWait` (~13.600 esperas en 3 min). Igual con los
   512 RTV de los resolves de color por shader. Ahora los RTV se crean al usarlos en un anillo (OMSetRenderTargets copia el
   descriptor al momento): sin límite. Quedan ~2.500 esperas de memexport (`MemexProcess`).
3. Profundidad: `WriteBackResolvedForDraw` ya no vuelca el color del cuadro anterior cuando hay un resolve de profundidad más
   reciente (`GOWJ_DEPTHWBOLD=1` = antes).
4. Firmas de páginas por **grupos de 16 páginas** (`texdirty.cpp`, contador por grupo): 16 veces menos lecturas al validar
   búferes grandes. Texturas pequeñas también por firma (antes huella completa cada cuadro; `GOWJ_SMALLTEXHASH=1` = antes).
Perfil ahora: hilo de la GPU al 91 % trabajando (driver de NVIDIA ~27 %, análisis de paquetes ~12 %, D3D12DrawImpl ~9 %).

Corrección: el **arma cian** NO quedó arreglada con la resolución dinámica (salió en run275/277/278/281/283; 269 y 271 limpias
por casualidad). El texto sí sigue bien en todas. Descartado también el bloom (sin lecturas de otro tamaño).

## Octava tanda: prueba larga de campaña y campaña cooperativa por enlace de sistema

- **Prueba larga** (`scripts/campaign_long.txt`, generada: intro + ciclo de combate de ~30 s repetido 20 min, captura cada minuto;
  `python tools/run_game.py logs/long1.log 1320 ... GOWJ_INPUTSCRIPT=../scripts/campaign_long.txt`): 22 min sin caídas ni errores,
  0 resolves por CPU, media 57,6 fps (mínimo 25,7). Avanza por el capítulo hasta el tutorial de la recarga activa ("SÚPER RECARGA
  ACTIVA" / "FALLO DE RECARGA ACTIVA ¿Intentarlo de nuevo?"), donde el guion se queda (no sabe acertar el RB a tiempo). Textos en
  español correctos.
- **Campaña cooperativa por enlace de sistema: funciona.** Anfitrión (`scripts/net_coop2.txt`): menú principal → BACK → Opciones de
  grupo → Enlace de sistema → Y (crear grupo) → en el menú principal "TIPO DE GRUPO: INTERCONEXIÓN" → A en CAMPAÑA (entra directo a
  la partida). Invitado (`scripts/net_coopjoin1.txt`): Explorador de interconexión → "Anfitrion 1/10" → se une, ve la partida con
  "Tomar el control", pulsa A y juega su personaje (HUD propio, munición, "TIRO A LA CABEZA"); su menú de pausa muestra
  "2 JUGADORES: Invitado 1, Anfitrion 1" y "Museo de la Gloria Militar: Ciudad Antigua (Normal)".
  `python tools/run_pair.py logs/cA.log logs/cB.log 255 scripts/net_coop2.txt scripts/net_coopjoin1.txt`.
- Una vez (de 3) el invitado murió al arrancar con 0xC0000409 sin escribir nada en el registro; no se repitió. Pendiente si vuelve.
- Con dos instancias a la vez (más carga) el **texto/HUD roto volvió a salir en el anfitrión**: la resolución dinámica no cubre todos
  los casos. Pendiente, junto con el arma cian.

## Pendiente al cerrar la sesión 10

1. Texto/HUD roto con mucha carga y arma cian (intermitentes). Probar a forzar la resolución dinámica fuera (si UE3 tiene un ajuste
   en el ini) o revisar las lecturas escaladas de las texturas de la escena.
2. fps: memexport aún espera a la GPU (~14 veces por segundo); driver ~27 % del hilo de la GPU.
3. Icono del aviso de "recoger" a bloques (pedir captura F9+F12).
4. Prueba con dos PCs reales / VPN; arranque con 0xC0000409 si se repite.

## Novena tanda: de dónde sale el arma cian

- Herramienta nueva **`GOWJ_PIXELPROBE=x,y[;x,y...]`** (con GOWJ_DRAWLOG/DRAWLOGEVERY): tras cada draw del cuadro registrado copia
  esos píxeles de la superficie de pantalla y registra los draws que los cambian (`[probe] draw N VS PS blend: p0(...)`).
  Otras: `GOWJ_PSCZERO=<hash PS>:<c>,<c>` (pone a cero constantes de ese PS), `GOWJ_VSCLOG=<hash VS>:<c>,...` (registra
  constantes del VS).
- Resultado: el arma la pinta un **draw opaco** de la familia de materiales de las armas (PS 59A175CF18471E94 y variantes) con
  valores HDR de (0,35, 6,1, 29,5) a (1,9, 15,9, 31,875) (el tope del 7e3): cian saturado. Esos PS tienen un término de **brillo
  de borde (fresnel) azul**: `c12 = (0, 5, 0, 1)` leído como `c12.xzy` = azul ×5, por `(1 − V.z)³` (V = vector de vista en el
  espacio tangente) y pulsante con el tiempo (`c13.zw` crece cada cuadro; `frc`/`sin`). Con `GOWJ_PSCZERO=59A175CF18471E94:12`
  el cian desaparece por completo.
- En la consola ese brillo debería verse solo en la silueta (resalte de arma); aquí llena el arma entera. `c241` del VS (cámara en
  espacio local, w = 1/2500) parece correcto. No se ha encontrado aún qué parte del cálculo sale mal.
- Opciones: (a) quitar ese brillo (poner a cero el color del borde en esos materiales; las armas se ven normales pero sin el
  resalte); (b) seguir buscando el fallo exacto (comparar con la consola si las armas del suelo brillan y cómo).

## Décima tanda: análisis del brillo cian (sin fallo de traducción encontrado)

- `GOWJ_PSFULLLOG=<hash PS>`: en el cuadro de GOWJ_DRAWLOG registra c0-c31, c240-c255 y los 256 booleanos de cada draw con ese PS.
  En el draw que pinta el arma: c0.x = 8, c3 = (70, 50, 40, 1), c12 = (0, 5, 0, 1), c13.zw = tiempo (crece cada cuadro),
  c15 = (0, 0, 2, 15); booleanos 128 y 131 activos (0x4904 = 9). Algunas constantes (c18, c26, c28, c30, c31) son restos de otros
  draws: solo se leen dentro de ramas `cjmp` que con esos booleanos no se toman o no afectan.
- Pintando términos por separado (`GOWJ_PSOUT`): el exceso está en `r6` (luz/emisivo), no en la iluminación normal. El factor del
  borde no cubre toda el arma (V.z en espacio tangente sale positivo, 0,3-0,6).
- `r15 = (c3.z, c3.y, c3.x) · tex(tf5).z` → (40, 50, 70) = azul. Probadas otras decodificaciones de la componente de la constante en
  MULsc/ADDsc/SUBsc (`GOWJ_SCCOMP=1|2`): rompen toda la escena, así que la de Xenia (por defecto) es la correcta.
- El shader calcula un ángulo que gira con el tiempo (`frc`, `sin`, 2π) y con él desplaza la textura tf5 (DXT1 256x256 con gamma),
  que modula esa luz azul: es un **efecto animado (pulso) del propio material de las armas**. No se ha encontrado ningún fallo de
  traducción; queda por saber si en la consola tiene esa intensidad (comparar con un vídeo/captura de la consola: armas en el suelo y
  en la mano), o si alguna textura (tf5) o su gamma lo amplifica.

## Undécima tanda: arma cian, personajes con tramas y texto/HUD roto = texturas movidas por la GPU (arreglado)

- Referencia: vídeo de la consola (Xbox Series X, retrocompatible) que pasó el usuario: el Lancer es gris, sin brillo azul. El cian
  era un fallo, no el efecto del material (la tanda anterior se equivocaba).
- El usuario juega a **32:9 (5120x1440) y 120 fps**; ahí el texto salía roto SIEMPRE. Con el mismo guion, unas partidas salían bien y
  otras mal (cian + personajes con tramas + texto/HUD roto, juntos y fijos toda la partida).
- Rastreo de la fuente (k_8 512x512 de campo de distancia, PS E258C771B8C118B8; `GOWJ_FONTLOG=1`): en las partidas malas el juego la
  pasa de `01DF2000` a `01DF1000` (UE3 desfragmenta la memoria de texturas). La copia la hace la GPU con un VS de memexport
  (`45DE25B687582BDC`: draws de 64 puntos, 8 vfetch 16_16_16_16 -> 8 eM por punto = 4 KB por draw) que avanza solo con
  **VGT_INDX_OFFSET** (0x2102). En los draws sin búfer de índices (`source == 2`) no se sumaba: todos los draws copiaban los primeros
  4 KB encima de los primeros 4 KB y el resto de la textura movida quedaba con basura. Arreglo: el VS recibe el desplazamiento en
  `XMisc.y` (`r0.x = vid + XMisc.y`; SV_VertexID NO incluye el vértice inicial de DrawInstanced) y los índices generados de los modos
  expandidos lo suman. `GOWJ_NOAUTOOFFSET=1` = antes. (Cambia el HLSL de todos los VS: la primera partida recompila los VS.)
- Segundo fallo que salió al arreglar el primero: cada draw de copia escribe encima de su origen (mueve 4 KB hacia abajo), invalida la
  copia de 256 KB del búfer de vértices y la vuelve a subir: 64 draws por textura llenaban el anillo de 128 MB y los siguientes draws se
  omitían (`g_skipped[3]`). Tras el `FlushAndWait` de cada draw con memexport, si el anillo pasa de la mitad se recicla (anillo, caché de
  vértices y descriptores del frame). `GOWJ_NORINGRECYCLE=1` = antes. Resultado: la fuente movida a `01DF1000` tiene exactamente la
  huella original (`4338257321DD08E9`) y el texto, el HUD y el arma se ven bien a 32:9 y 120 fps.
- Además (por si acaso): los VS con memexport y sus PSO se compilan al momento (antes podían omitirse mientras se compilaban y la
  copia se perdía). `GOWJ_ASYNCMEMEX=1` = antes.
- Arreglos de paso:
  - Resolves por GPU: la validez de la copia de la GPU se comprueba con firmas por página de texdirty (`TexDirtyPageSig/Valid`) en vez de
    `MemHash` (64 muestras en 256 KB: la caché de glifos de Scaleform son texturas k_8 de 4 KB cada 64 KB y la escritura de una vecina
    invalidaba la otra -> se leía memoria vacía). Si la CPU reescribe el destino de un resolve, se olvida (`PruneStaleResolve`).
    `GOWJ_NORESOLVESIG=1` = antes.
  - Panorámico: 1120/560/280 ya no se ensanchan (en Judgment son atlas de luces/sombras; la escena va en 1280). `GOWJ_PANO1120=1` = antes.
- Herramientas nuevas: `GOWJ_FONTLOG=1` (direcciones y huellas de la fuente), `[memexdbg]` con `GOWJ_MEMEXLOG=<VS>` (draws de copia que
  no llegan a MemexProcess y qué contador de omitidos cambió), `GOWJ_DUMPLOG=1` (cada DUMP del guion registra el cuadro, como F9, para PIXELPROBE), `GOWJ_TDWATCH=<fís>,<largo>`
  (escrituras de CPU capturadas, resolves y memexport que caen en ese rango), `GOWJ_NOPANOUI=1`, `[ancho]` (resolves desde superficies
  anchas), `[memex] VS ... (primera vez)`.
- Nota: PIXELPROBE usa coordenadas lógicas (1280x720; en 32:9 se multiplican por el ancho) y como mucho 8 puntos.

## Duodécima tanda (2026-11-16): personajes que vibran, audio que se corta, nitidez

Lo que reportó el usuario tras jugar a 32:9: personajes que "vibran", aparición tardía de texturas (la sangre), audio que parece
cortarse en conversaciones, imagen muy borrosa y 32:9 que no se ve bien.
- **Personajes que vibran = desenfoque de movimiento.** Con `MotionBlur=True` (Xbox360-GearEngine.ini [SystemSettings]) el personaje,
  que va con la cámara, sale emborronado (en la consola sale nítido: falla el búfer de velocidades de los personajes; también a 30 fps,
  no es cosa de los 120). Medida de nitidez del personaje: ~250 con desenfoque, 475-650 sin `MotionBlurSkinning`, ~1270 sin desenfoque.
  Ahora el desenfoque va **apagado por defecto** por el ini en memoria (`SystemSettings:MotionBlur=False`); `GOWJ_MOTIONBLUR=1` o
  `desenfoque_movimiento=1` en gowj.ini (conmutador vuelto a poner en la página Gráficos del launcher) lo activa. El antiguo
  `GOWJ_NOMOTIONBLUR` apuntaba a shaders de NG2. Pendiente: arreglar el búfer de velocidades para poder dejarlo como en la consola.
- **Audio:** sin huecos de WASAPI, sin voces sin datos a mitad de sonido (diagnóstico nuevo `[xma] hueco en mitad del sonido`), sin
  contextos borrados/bloqueados mientras sonaban. La mezcla 5.1 -> estéreo llegaba a picos de 2,7 y el limitador bajaba todo (voces
  incluidas) a un tercio y lo recuperaba en 50 ms: bombeo que suena a cortes con cada disparo. Ahora ganancia 0,6 (`GOWJ_MIXGAIN`) y
  recuperación de 250 ms (`GOWJ_LIMRELEASE`). El registro `[wasapi]` muestra el % de tiempo con el limitador bajando más de 4 dB.
  Pendiente de que el usuario confirme de oído.
- **Nitidez:** el escalado Catmull-Rom lleva ahora enfoque adaptativo al contraste (tipo CAS de AMD) con los 4 vecinos de la imagen del
  juego: `GOWJ_NITIDEZ=<0..1>` (por defecto 0,5), opción "Nitidez" en el launcher (`nitidez=` en gowj.ini).
- **Texturas que aparecen tarde:** en la partida del usuario hubo cientos de draws omitidos por shader/PSO compilándose (era la primera
  partida tras cambiar el HLSL de todos los VS por `XMisc.y`). Con la caché ya llena debería pasar mucho menos; pendiente de confirmar.
- Herramientas: `GOWJ_INIGREP=<texto>` (registra las claves del ini del juego que lo contienen), guion `scripts/campaign8b_seq.txt`
  (DUMP8 = 8 cuadros seguidos `seq_NNN.bmp`).
- Ojo: las capturas `shot_*.bmp` no salen con ventanas de 3840x1080 (probar a 2560x720).

## Decimotercera tanda (2026-11-16): cinemáticas sin audio, vídeos estirados en 32:9, sangre que parpadea

- **Cinemáticas sin audio (arreglado).** Las cinemáticas `_LOC` (Bink 2 con 33-36 pistas: 6 por idioma, elegidas en sub_823968E0:
  INT 0-5, ..., ESN 15-17, ESM 18-20; BinkSetSoundTrack = sub_8309D250, gancho con registro `[bink]` y `GOWJ_BINKTRACKS=` de prueba)
  sonaban en silencio en cualquier idioma (pico 0,000), aunque los logos (mismas 36 pistas) sí sonaban. Al empezar cada vídeo el juego
  llama a XMsgInProcessCall(app 0xFA = XMP, 0x7001B = XMPGetPlaybackController) y no se respondía: leía basura de la pila (como si la
  música la controlara el usuario) y silenciaba la categoría del vídeo. Ahora se responde como Xenia (controla el título, sin bloquear;
  0x7001A también da éxito). `GOWJ_OLDXMP=1` = antes. Con el guion `scripts/cine_intro.txt` y un perfil limpio: pico 0,4-0,8.
- **Vídeos estirados al doble en 32:9 (arreglado).** Los cuadros con el plano Y de un Bink (k_8 1280x704) se presentan a 16:9 con bandas
  (la superficie ancha se encoge al presentar = proporción correcta). `GOWJ_PANOMOVIE=1` = antes. Registro `[pano] presentacion ...`.
  Ojo: las capturas `shot_*.bmp` leen la imagen de origen (2560x720), no lo que se ve en la ventana.
- **Sangre con rayas / que aparece y desaparece (arreglado).** No se aplicaba el desplazamiento de polígonos (PA_SU_SC_MODE_CNTL bits
  11-13, PA_SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET 0x2380-0x2383; además la clave del PSO solo guardaba 4 bits de ese registro): las
  calcomanías peleaban con el suelo (z-fighting). El juego usa desplazamientos de 6e-05 y 8e-05 (unidades 0..1): DepthBias = x 2^24
  (~1000-1340 en D24), pendiente /16. `GOWJ_NOPOLYOFFSET=1` = antes, `GOWJ_POLYOFFSCALE=<k>`.
- Pendiente, sin poder reproducirlo en los guiones: sangre "arcoíris" al destrozar enemigos de cerca y el efecto de daño (tinta) que solo
  cubre la zona 16:9. Pedir al usuario F9 + F12 en ese momento (F9 registra 2 cuadros en gowj.log; F12 guarda shot_9xx.bmp).

## Decimocuarta tanda (2026-11-16, noche): HUD a 32:9, tinta de daño, bordes 16:9, brillo, desenfoque

Capturas del usuario (shot_900-905 en project\build): tinta de daño cortada, sombras raras, pantalla de carga, cinemática, punto de
control. Diagnóstico nuevo: `GOWJ_PIXELPROBE` registra también el GS de cada draw (`gs 2/3` = interfaz comprimida);
`GOWJ_HUDMATLOG=<VS>` registra la matriz c6-c9 y los primeros vértices de ese VS en los cuadros de DRAWLOG/F9 (`[hudmat]`, `[hudvtx]`).

- **HUD estirado al doble en 32:9 (arreglado).** Estrellas, munición, "OBJETIVOS" y el panel de los tutoriales salían ensanchados
  mientras otros textos sí se comprimían. El VS de Scaleform (DA90EC73854FEFE6, matriz 4x4 completa) recibe vértices con **w = 0,1**
  (coordenadas x 0,1) y el GS solo trataba como interfaz los triángulos con w = 1. Ahora: plano = los tres w iguales y positivos (se
  divide por w antes de medir el ancho). Probar "cualquier w" rompía la composición de la escena (w distintos). `GOWJ_PANOWSTRICT=1` =
  antes. El HUD queda en proporción en la zona central 16:9 (no en los bordes del 32:9).
- **Tinta de daño solo en la zona 16:9 (arreglado).** El efecto (w = 1, de -1,14 a 1,14 en varios triángulos) se comprimía como
  interfaz: cortes exactos en x = 550 y 2010 de 2560. Un triángulo con w = 1 que se sale de la pantalla (>1,02) ya no es interfaz. Los
  paneles de Scaleform que entran desde fuera (fondo de "DESPEDAZAR") sí siguen comprimiéndose. `GOWJ_PANOOVERSCAN=1` = antes.
  Comprobado con la pantalla roja de "OBJETIVO FALLIDO": cubre los 2560.
- **Laterales de las pantallas 16:9 (cargas, resultados, vídeos, menús).** En vez de barras negras, el borde de la imagen sigue hacia
  fuera en espejo, cada vez más desenfocado y oscuro (`kSideHlsl`, pase previo al blit, 49 lecturas). Opción "Bordes en vídeos y menús"
  (`bordes=difuminado|negro`, `GOWJ_BORDES=negro`). Las capturas shot/seq no lo muestran (leen la imagen de origen).
- **Brillo.** Con la misma cinemática del motor, el centro de la imagen a 32:9 sale ~12 % más claro que a 16:9 (51 → 58 de media). A
  32:9 sin Hor+ (`GOWJ_NOPANOFOV=1`) coincide con 16:9: no es un fallo de las superficies anchas sino la exposición automática del juego,
  que mide también los laterales (más oscuros). Opción "Brillo" (`brillo=auto|0.8..1.2`, `GOWJ_BRILLO`; gamma final como en el port de
  Gears 3). En automático, a 21:9/32:9 las imágenes de partida usan 0,9 y vídeos/menús 1,0. Registro `[d3d12] brillo x (partida y)`.
  Pendiente: corregir la exposición en su origen (cadena de luminancia en 512x512/256x256 de la superficie 14000500).
- **Desenfoque:** además de MotionBlur=False, `MotionBlurSkinning=0` y **`AllowRadialBlur=False`** (desenfoque radial al correr y en
  explosiones) salvo con `GOWJ_MOTIONBLUR=1` / `desenfoque_movimiento=1`.
- Launcher (Gráficos): Brillo y Bordes nuevos; fps y ventana se quitan de esta página (están en Inicio) porque no cabía.
- Diagnóstico `GOWJ_PANOPITCHMASK` (1 = 1280, 2 = 640, 4 = 320 ensanchadas): quitar alguna rompe la imagen; solo para pruebas.
- Pendiente (sin reproducir): fragmentos negros en el suelo de la Gran Sala (sección 4, shot_905; parecen calcomanías de cristales),
  un enemigo que se ve como mancha sin textura en la escalera (captura del usuario), sangre arcoíris de cerca. Pedir F9 + F12.
- Ojo para pruebas: el usuario puede estar usando el PC (otro juego en pantalla). No capturar la pantalla con ImageGrab ni forzar el
  primer plano; para ver lo que sale por la ventana, simular en Python o leer la imagen de origen.
- Guion `scripts/bright_cmp.txt`: capturas fijas de la cinemática del museo sin saltarla (shot_2015..2065 en run\, misma escena en
  16:9 y 32:9).

## Decimoquinta tanda (2026-11-17, madrugada): tirones

- Medición (campaign8b_seq, 32:9, 120 fps, 260 s): casi todos los vaciados (9.973 de 10.000, `GOWJ_FLUSHSTATS=1` +
  llvm-symbolizer) venían de `MemexProcess`: cada draw de memexport esperaba a la GPU y copiaba 4 MB de registros a la CPU.
- **Copias de texturas de UE3 en la CPU.** El VS 45DE25B687582BDC (desfragmentación de texturas) es un memmove: por punto lee 64 bytes
  (8 x 16_16_16_16 entero) y los escribe con memexport en eA = C6 + C255.xyxx * C255.w * (v - C7.x). `MemexCopyCpu` hace la misma
  cuenta al grabar el draw (sin GPU ni espera). `GOWJ_MEMEXVERIFY=1` hace las dos y compara: 0 bytes distintos en >4.000 copias.
  `GOWJ_NOMEMEXCPU=1` = por la GPU.
- **Memexport por lotes** para el resto: los registros de draws de memexport seguidos se acumulan y se aplican con una sola espera
  antes del primer draw que lee memoria (no cuenta el draw vacío de 512 puntos que UE3 intercala), de un resolve o de presentar.
  Solo se copia la parte usada del bufer. `GOWJ_MEMEXSYNC=1` = una espera por draw. Ojo: un vaciado a mitad de la grabación de un
  draw pierde el estado de la lista (caída en MemexBind): los vaciados van al principio del draw.
- **Bufer de memexport de 87.000 a 262.144 registros**: había copias de 89.600-130.560 registros que se cortaban (datos perdidos).
- Resultado: media 91 -> 109 fps, tirones 81 -> 60, peor 721 -> 417 ms (el del arranque); los que quedan (~150 ms) no tienen esperas
  a la GPU (cargas del juego).
- Pendiente: arma que cambia de luz (naranja/verde) y pared que se pone negra bajo un arco en Old Town (capturas del usuario; parecen
  sombras), F9 se detecta aunque la ventana no tenga el foco (GetAsyncKeyState) y llena el registro.

## Decimosexta tanda (2026-11-17): pendientes

- **F9 solo con la ventana del juego activa** (antes `GetAsyncKeyState` saltaba con F9 en cualquier aplicación y llenaba el registro).
- **Pruebas sin robar el foco:** `GOWJ_NOFOCUS=1` abre la ventana detrás y sin activarla; `tools/run_game.py` lo pone por defecto.
- **F9 sin configurar nada mira la mira:** si no hay `GOWJ_PIXELPROBE`, en los cuadros de F9 se registran los draws que pintan el centro
  de la pantalla y 4 puntos alrededor (`[probe]`). Para un fallo visual: apuntar a él, pulsar F9 y luego F12.
- **Pared negra y cristales negros: hipótesis.** El desplazamiento de polígonos solo lo usa el juego en calcomanías (escala 0,
  desplazamiento 6e-05/8e-05; PS 1B2985F2D81EE636, 37BA3B1AAA21DC39, 06BB97C8BBE3FD3E, 50CFA6B8F3B00CDE, 593BD7ACF5300CDE), no en
  mapas de sombras. Antes esas calcomanías peleaban con la superficie y casi no se veían; ahora se ven enteras, y si una se dibuja mal
  sale negra. Sin reproducir: el perfil de pruebas (`run\userdata`) guarda el avance y cada prueba sigue en otro sitio (ahora el museo);
  incluso `userdata_bak_s6` arranca en el museo. Comparar con `GOWJ_NOPOLYOFFSET=1` en la misma escena.
- Luz del arma que cambia (naranja/verde): sin reproducir.

## Decimoséptima tanda (2026-11-17): tirones restantes

- **Lista de PSO con entradas viejas (arreglado).** `shadercache/psolist.bin` guardaba 3.632 PSO, pero 3.080 eran de versiones
  anteriores del HLSL. Al arrancar se descartaban ("sin shaders iguales") y, como su clave ya figuraba en `g_psoListed`, la versión nueva
  nunca se registraba: cada partida creaba ~3.000 PSO sobre la marcha (al cambiar de zona). Ahora solo cuentan las entradas válidas y la
  lista se reescribe sin las viejas (`[d3d12] lista de PSO limpiada`). Segunda partida: 1.067 de 1.067 precreados en 0,4 s.
- **Contador de tirones (F3)**: solo cuenta cuadros de partida (antes sumaba las pantallas de carga, que van a 20 fps: marcaba 76-81).
- Queda: el primer cuadro tras una carga (~230 ms: 279 shaders de la caché de disco y 427 texturas nuevas a la vez), tapado por la
  transición. Las cifras de fps entre pasadas no son comparables: el perfil de `run\userdata` avanza y cada prueba es en otra zona.

## Decimoctava tanda (2026-11-18): resolución interna y "exposición" a 32:9

- **Resolución interna (`GOWJ_RESINTERNA=1..3`, `resolucion=` en gowj.ini, opción del launcher en Gráficos).** Generaliza el modo
  ancho: las superficies de pantalla (pitch 1280/640/320) se crean k veces más grandes también en vertical (`EdramRt::sy`,
  `DepthRt::sy`, `ResolvedTex::sy`, `ResolvedDepth::sy`) y `SurfS` = ancho del panorámico x k. Se escalan en Y: visor, recortes,
  borrados, resolves de color/profundidad (GPU, copia y CPU), copias derivadas, fusiones de resolves parciales, volcados a memoria (se
  toma una fila de cada sy), capturas (salen a la resolución interna: 5120x1440 a 32:9 2x). Sombreadores: `XTexMode.z` bits 16-23 =
  escala Y para lecturas en texeles; PsParamGen con nueva codificación (bits 0-4 X, 5-9 Y, 10 sin redondear, 11 X-0,5) cuando
  `ScreenScaleX() > 1`. Con 1x todo igual que antes (probado a 32:9).
  Medido: 32:9 2x = 5120x1440 internos a 55-85 fps en combate (1x: 72-88 en la misma zona); 16:9 2x = 2560x1440 a 80-90 fps. Detalle
  por encima de 720p 2-3 veces mayor que un 1x ampliado (bordes y verjas nítidos). HUD y texto correctos.
- **El 32:9 no estaba sobreexpuesto (corrección de la tanda 14).** Con el perfil de pruebas restaurado antes de cada pasada (copia en
  `run\userdata_snap`, misma escena): el centro de la imagen es idéntico a 16:9 (89 frente a 88); la diferencia de media viene de la
  viñeta del juego, que a 16:9 oscurece los bordes de la imagen y a 32:9 los extremos del monitor. Descartados con pruebas: bloom
  (`Bloom=False`), oclusión ambiental (`AmbientOcclusion=False`) y la medición de luz 32x32 (PS A2004ECD52E1C4AF). Se quitó el
  oscurecimiento automático (0,9) y el GS de prueba; brillo 1,0 por defecto (opción manual en el launcher, `GOWJ_BRILLOPANO=<g>` solo
  para la partida panorámica).
- Para comparar entre pasadas: restaurar `run\userdata` desde `run\userdata_snap` antes de cada una (si no, cada prueba sigue en otro sitio).
- Launcher: "Bordes en vídeos y menús" pasa a Avanzado (en Gráficos no cabía con "Resolución interna").
