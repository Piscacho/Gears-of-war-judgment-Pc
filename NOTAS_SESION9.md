# Sesión 9 — Launcher, 32:9, límite de fps y enlace de sistema P2P

Fecha: 2026-11-15 (de 1:37 a ~3:30). Sin commits.

## Resultado

- **Launcher nuevo** (`runtime/launcher_gears.cpp`), portado del de Dead Space: Extraction (mismo diseño: barra lateral, ajustes
  rápidos, filas `◀ valor ▶`, conmutadores SÍ/NO, campos de texto, GUARDAR / JUGAR) con estética Gears (acero y rojo carmesí).
  Páginas: Inicio, Gráficos, Controles (tabla de mando y teclado), Audio, En línea, Avanzado (ISO, partidas, DLC, registro).
  Imágenes opcionales en `launcher\` junto al exe: `portada.(jpg|png|bmp)` y `logo.png` (puesta una captura propia en 32:9 en
  `project\build\launcher\portada.png`). Sustituye a `launcher_ui.cpp` (el de NG2) en CMake. Capturas: `tools/launcher_shot.py`.
  Se quitaron grano y desenfoque de movimiento (apuntaban a shaders de NG2).
- **Arte del launcher (SteamGridDB, juego 5257339)** en `projectuild\launcher\` con `CREDITOS.txt`: `heroe_arte.jpg` (cabecera,
  hero 14446), `heroe_baird.jpg` y `heroe_escuadron.jpg` (fondos por página y franjas), `logo.png` (logo oficial, cabecera y barra
  lateral) y `portada_x360.jpg` (carátula abajo en la barra lateral), como el launcher final de Dead Space Extraction.
  Al distribuir el port hay que copiar la carpeta `launcher\` junto a GOWJ.exe.
- **Modo usuario comprobado**: `GOWJ.exe` sin argumentos + `gowj.ini` (ISO) → descifra el XEX en memoria y juega la campaña.
- **Saltar intro** adaptado a Gears (`Movies\Open_Logos.bik`, Bink normal): el título llega en ~5 s en vez de 24.
- **32:9 / 21:9 con Hor+ real**: `sub_826427F8` = FPerspectiveMatrix de la vista principal (MultFOVX, MultFOVY = 16/9): se divide
  MultFOVX entre k. La escena se dibuja en superficies k veces más anchas (maquinaria de NG2) y se presenta a la proporción
  elegida. La detección de "cuadro con escena" ahora incluye la superficie HDR de Gears (pitch 1280, formato 3). Se desactivó la
  compresión en el VS de NG2 (`GOWJ_PANOVS=1` la vuelve a poner) y el parche de cámara de NG2 (dirección de NG2) no se usa.
  `GOWJ_NOPANOFOV=1` = sin Hor+. (`sub_826428F8` es la de las capturas de escena.)
- **Límite de fps**: `UGameEngine::GetMaxTickRate` (`sub_8255DDE8`, vtable +0x134) devuelve `MaxSmoothedFrameRate` (30) y
  `FEngineLoop::Tick` (`sub_829D7500`) duerme el resto. `fps=` en gowj.ini (`GOWJ_MAXFPS`, 0 = sin límite) lo sustituye y sube el
  VBlank emulado a 2×fps (el juego presenta con intervalo 2). Menús y cargas llegan a 160-240 fps; **en partida el render sigue
  en ~27-30 fps** (el hilo de comandos de la GPU está al ~85 %: driver/D3D12 ~40 %, `D3D12DrawImpl`, `BindTexture`).
  Pendiente para 60/120: bajar el coste por draw (~3.900 draws por cuadro).
- **Online P2P (enlace de sistema)** — `runtime/net_gowj.cpp`:
  - Sin online: sockets locales falsos (como en la sesión 7).
  - Con `online=crear|unirse` (`GOWJ_NET`): sockets UDP reales; puerto del juego P → P + (base − 1000) (base `online_puerto`,
    3074); XNADDR.ina = IP del PC (`online_ip` / `GOWJ_NET_IP`, si no la primera IPv4 no local); difusiones (y 0.0.0.0) → IP del
    anfitrión (`online_direccion`) y pares aprendidos; `XNetXnAddrToInAddr` = identidad; `XNetRandom` real; enlace Ethernet
    activo. Nombre `online_nombre` (`GOWJ_GAMERTAG`) y XUID distinto por jugador (`UserXuid()` en xam.cpp).
  - XGI 0xB0010 (`XSessionCreate`): como anfitrión se rellena `XSESSION_INFO` (XNKID, XNADDR propia, clave).
  - **Funciona**: menú principal → BACK → Party Options → **System Link** → Y (crear grupo); el otro PC ve "Anfitrion 1/10" en el
    System Link Browser, se une y el grupo muestra a los dos; el invitado ve "Anfitrion IS SELECTING: SURVIVAL…".
  - **Falta**: al empezar la partida (Survival → Private → START) el anfitrión se queda en "PLAYERS CONNECTING" y el invitado en la
    pantalla de carga. El invitado viaja (`Browse 127.0.0.1 … GearPreGameLobbyGame`) y manda el mismo "hola" de 19 bytes que en la
    conexión del grupo, pero el anfitrión contesta con 11 y 10 bytes con números de secuencia 0x97/0x98 (los de la conexión del
    grupo): **parece que el anfitrión toma el nuevo intento como la conexión vieja** (misma IP:puerto de origen 127.0.0.2:1000).
    Siguiente paso: cerrar/olvidar la conexión anterior (o usar otro puerto de origen por driver), y comprobar el "Welcome".
  - Prueba en un solo PC: `python tools/run_pair.py logs/A.log logs/B.log 150 scripts/net_host3.txt scripts/net_join2.txt`
    (A = 127.0.0.1 en `run`, B = 127.0.0.2 en `run2`). `GOWJ_NETHEX=1` vuelca los bytes de los primeros paquetes.

## Otros cambios

- `GOWJ_SLEEPLOG=1`: esperas de 1-60 ms con pila de invitado. `GOWJ_VIDEOW/H`: modo de vídeo emulado (el juego lo ignora).
- `[engine]`: al cargar el primer mapa se registra la vtable de GEngine y su GetMaxTickRate.

## Pendiente

1. Online: empezar partida (ver arriba); probar campaña cooperativa por enlace de sistema y entre dos PCs reales/VPN.
2. Rendimiento en partida para 60/120 fps.
3. Texto corrupto intermitente (caché de glifos k_8) visto otra vez en una de las dos instancias.
