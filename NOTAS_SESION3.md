# Sesión 3 — Imagen en partida, LUT, texto del título y primeras pruebas de juego

Fecha: 2026-11-14. Sin commits.

## Resultado

- **La escena 3D se ve en pantalla** en título (fondo animado con la estatua), menús y partida.
- En partida: HUD, **disparo con RT** (fogonazo, trazadoras, munición 240 → 225), menú de pausa con START, "Declassified" con Y.
- Texto del título y de los menús principales nítido.
- Lanzador para probar a mano: `run\jugar.bat` (registro en `run\jugar.log`). Mando: DualSense/XInput; teclado: WASD mover,
  IJKL cámara, Espacio = A, E/Esc = B, F = X, R = Y, Intro = START, Retroceso = BACK, Q = RT, Z = LT, C/V = LB/RB.

## Arreglos

1. **Escena 7e3 en bruto (alias EDRAM 2 sobre 3)**: UE3 dibuja la escena en 2_10_10_10_FLOAT (formato 3, emulado con RGBA16F) y la
   resuelve con una vista 2_10_10_10 (formato 2) en la misma base; el tonemap (PS 64E4BB659DD2D065) la lee como fmt 54 y decodifica
   el 7e3 en el shader. Ahora el resolve de una superficie 2 sobre una 3 de la misma base y ancho lee la 3 y empaqueta float7e3
   (`Float32To7e3`, como Xenia). `GOWJ_NORAW7E3=1` = antes. `[7e3]` en el log (estadística cada 4 M píxeles).
2. El atajo de la sesión 2 "fmt 54 → resolve 8888 por GPU" enlazaba copias **antiguas**: desactivado por defecto (`GOWJ_54AS8888=1` lo activa).
3. **LUT de color**: `tf4` del tonemap es un **array 2D apilado** 16×16×16 (dim 2D, profundidad en los bits 26-31 de dword2) leído con
   tfetch3D. `BindTexture3D` lo sube como textura 3D (cada capa una 2D tileada, separación pitch32×alto32×bpp). `GOWJ_NOSTACKED3D=1` = antes.
4. La LUT se genera con un resolve 8888 que se queda en la GPU: `WriteBackResolvedForDraw(mask, maskMem)` fuerza el volcado a memoria
   de los slots leídos como 3D/cubo.
5. **Texto (caché de glifos k_8)**: se rellena entre draws del mismo cuadro → sin memo del hash por cuadro para k_8 (`GOWJ_K8MEMO=1` = antes).
6. **Guiones de entrada con anclas**: `ANCHOR <texto>` = los pasos siguientes cuentan desde que se abre un archivo cuya ruta contiene
   ese texto. Guiones: `scripts/play_anchor.txt` (título → PlayerStorage → menús → nivel `gud_cogbairdg_sf` → mover/disparar).
   Las capturas de pasos con ancla N se llaman `shot_<N+1><segundos>.bmp`.
7. Para pruebas automáticas sin interferencias: `GOWJ_NOXINPUT=1 GOWJ_NOPSPAD=1` (había un DualSense conectado).

## Investigación del final de la sesión (movimiento y texto)

- Tras Select Difficulty solo se cargan `GearGame_P` + paquetes de Baird: **nunca se abre el mapa del capítulo**. La escena de la
  estatua es la misma del fondo del título. La cámara está fija; los sticks no desplazan la vista (sí aparece el punto de mira con LT
  y se dispara con RT). Pasado un rato aparecen menús negros (uno de ~7 opciones con A/B, parece la pausa) y una transición roja con
  la calavera. Sin poder leer el texto no se sabe qué pide el juego.
- Mensajes XAM sin implementar en ese tramo: app 0xFC (XLiveBase) `0x58004`, `0x58020`, `0x58046` (XMsgInProcessCall) y app 0xFA
  `0x7001B`; app 0xFB (XGI) `0xB0006/0xB0007` (contexto/propiedades del usuario). No llega a crear sesión (0xB0010).
- **Texto de partida**: lo dibuja el PS AEAA818C642460EF con una k_8 de 512×512 (en este caso `01D1F000`, fetch 84004802). Su memoria
  es ruido denso (92 % no nulo, ni k_8 ni DXT5 legibles): **nunca se escribe**. Volcados: `GOWJ_TEXDUMP=<base>` ahora guarda también
  `texraw_*` (bytes crudos + constante de fetch).
- **memexport**: hay VS que exportan a memoria (eA, eM0-4) y el traductor los descarta (`[shader] exportacion descartada ... (memexport)`).
  Sospecha principal para (a) los personajes rotos (skinning por GPU que escribe vértices con memexport) y (b) quizá la caché de glifos.
  **Siguiente paso: implementar memexport** (RWByteAddressBuffer sobre la memoria física + volcado a la memoria del invitado tras el draw,
  formato/endian según eA como en Xenia) y registrar el hash del VS en ese aviso.

## Pendiente

- **Moverse en partida**: los sticks mueven la selección en menús pero en partida no se ve desplazamiento ni giro de cámara
  (¿bloqueo de la introducción del capítulo? comprobar a mano con el mando). A los ~90 s sin moverse aparece un menú negro.
- Una caída vista una vez al moverse (sesión 3, run54): llamada virtual a NULL en sub_826A8DF8 (animación), objeto = texto UTF-16.
  No se repitió con las anclas.
- **Personajes**: Baird sale como fragmentos azules/rayas (piel/normales). Escena oscura en general.
- Texto corrupto en algunas pantallas de partida (fuente distinta de la k_8 de 512×512; posiblemente DXT5 `fmt 20`).
- Rendimiento ~20 fps (17 resolves por CPU por cuadro).
