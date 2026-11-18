# Gears of War: Judgment para PC

Recompilación estática para PC de **Gears of War: Judgment** (Xbox 360, 4D530A26) para Windows 10/11 con DirectX 12.

> **No se incluyen archivos del juego.** Necesitas tu propio ISO de Xbox 360 de Gears of War: Judgment.
> Gears of War es una marca de Microsoft. Este proyecto no está afiliado a Microsoft, Xbox Game Studios, The Coalition,
> Epic Games ni People Can Fly.

## Descargar y jugar

1. Descarga el `.zip` desde la página de
   [Releases](https://github.com/Piscacho/Gears-of-war-judgment-Pc/releases/latest).
2. Descomprímelo en una carpeta, por ejemplo `C:\Juegos\Gears of War Judgment`.
3. Abre **`GOWJ.exe`**. Si Windows muestra "Windows protegió tu PC", pulsa
   **Más información → Ejecutar de todas formas**.
4. En el launcher, página **Avanzado**, elige el ISO y pulsa **JUGAR**.

- **Campaña** completa (con guardado), **Supervivencia** y **cooperativo por enlace de sistema** (LAN/VPN, P2P).
- **Controles**: mando de Xbox (XInput) o de PlayStation (DualSense/DualShock 4), o teclado y ratón
  (distribución de shooter de PC; la lista completa está en `LEEME.txt`).
- **Imagen**: 30 / 60 / 120 / 144 fps o sin límite, panorámico **21:9 y 32:9** (Hor+, con el HUD en proporción),
  **resolución interna ×2/×3** (1440p, 4K), filtrado anisotrópico, nitidez, brillo y desenfoque de movimiento opcional.
- **Idiomas**: español latino (con doblaje) por defecto; inglés, francés, alemán, italiano, polaco, ruso, coreano y chino.
- **Partidas**: carpeta `userdata\` junto al ejecutable (consérvala si actualizas a mano).

### Teclas

F11 o Alt+Enter: pantalla completa · F3: contador de FPS · F12: captura (`shot_9xx.bmp`) ·
F9: registra en `gowj.log` los dos cuadros siguientes con todo el detalle (para informar de un fallo gráfico:
apunta con la mira a lo que se ve mal, pulsa F9 y luego F12).

### Problemas conocidos

- La primera vez que aparece un efecto puede faltar un instante mientras se compila su shader (luego queda en `shadercache\`).
- Algunas calcomanías (manchas en paredes, cristales rotos en el suelo) pueden verse negras.
- Al entrar en una zona nueva hay un tirón breve en el primer cuadro.
- El juego en línea es solo por enlace de sistema (no hay Xbox Live).

Para informar de un fallo, adjunta `gowj.log` (el registro de la última partida) y, si es gráfico, la captura de F12.

## Compilar desde el código

El repositorio **no contiene nada del juego**: ni el ISO, ni el XEX, ni el código C++ que XenonRecomp genera a partir
de él (`ppc/`). Cada persona lo genera desde su propio ISO.

Requisitos:
- Visual Studio 2022 Build Tools (MSVC 14.4x, con CMake y Ninja).
- LLVM 23 para Windows (clang-cl + lld-link) y FFmpeg 8 compartido (`avcodec-62`, `avutil-60`, `swresample-6`).
  Las rutas están al principio de `tools/build_game.bat`, `tools/build_msvc.bat` y `project/CMakeLists.txt`.
- Python 3.8+ y `openssl` en el PATH (para `tools/xex2.py`).

Pasos (desde la raíz del repositorio):

```bat
git clone https://github.com/Piscacho/Gears-of-war-judgment-Pc.git
cd Gears-of-war-judgment-Pc

rem 1. Sacar el ejecutable del ISO y descifrarlo (genera xex\basefile.bin)
python -I tools\xdvdfs.py "<ruta al ISO>" extract default.xex xex\default.xex
python -I tools\xex2.py xex\default.xex xex

rem 2. XenonRecomp con los cambios para este juego
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git
cd XenonRecomp
git checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
git apply ..\xenonrecomp_gowj.diff
cd ..
tools\build_msvc.bat

rem 3. Generar el código C++ del juego (ppc\)
python -I tools\make_functions.py
mkdir ppc
python tools\run_recomp.py

rem 4. Compilar y crear la release
tools\build_game.bat
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Ver 1.0.0
```

El historial del trabajo (qué se hizo en cada sesión, causas de cada fallo y variables de diagnóstico `GOWJ_*`) está en
los archivos `NOTAS_SESION*.md`.

## Licencia

El código de este proyecto (runtime, launcher y herramientas) está bajo licencia MIT (`LICENSE`). Los componentes de
terceros (FFmpeg, XenonRecomp, partes basadas en Xenia) están en `THIRD-PARTY-NOTICES.txt`.
