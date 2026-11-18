# Sesión 4 — memexport, puntos y texto de partida

Fecha: 2026-11-14. Sin commits.

## Resultado

- **memexport implementado.** El VS escribe registros (eA, k, datos) en un búfer UAV `u0` con contador atómico; tras el draw se
  copia a la CPU (espera a la GPU) y `MemexApply` los escribe en la memoria física del invitado. Único VS con memexport visto:
  `45DE25B687582BDC` (copia elementos de 64 bytes de `vf95` a memoria en formato 16_16_16_16 uint, endian 8in16).
- **Listas de puntos** (`prim 1`) ya no se descartan (`POINTLIST`); los draws de memexport son puntos. `GOWJ_NOPOINTS=1` = antes.
- **getTextureGradients** (`fetch op 18`) = (ddx(u), ddy(u), ddx(v), ddy(v)), como Xenia: lo usa el texto SDF.
- Con las tres cosas: **texto de partida perfecto** (pistas del tutorial, menú de pausa, opciones), Lancer con su textura metálica.
- Confirmado: tras Select Difficulty **se está jugando el capítulo 1** ("THE MUSEUM OF MILITARY GLORY: OLD TOWN [NORMAL]" en la pausa).
  Funcionan: RT disparar, Y cambiar de arma (Lancer/Gnasher), LT punto de mira, START pausa, menús con stick.

## Formato de eA (deducido de los datos reales)

- `eA.x` = 0x40000000 | dirección base en **dwords**.
- `eA.y` = 0x4B000000 + índice de elemento (float 2^23 + índice); cada `eM_k` escribe en índice + k.
- `eA.z` = 0x4B0xxxxx: endian (bits 0-2), formato de vértice (bits 8-13), tipo numérico (bits 16-18: 0 unorm, 1 snorm, 2 uint,
  3 sint, 7 float).
- `eA.w` = 0x4B000000 + número de elementos (límite).
- Formatos soportados en `MemexApply`: 6, 7, 25, 26, 31, 32, 33-38. `GOWJ_MEMEXLOG=1` registra los primeros draws.
  `GOWJ_NOMEMEXPORT=1` lo desactiva.

## Problema abierto: el analógico no mueve en partida

Los sticks (guion y teclado WASD/IJKL) no desplazan al personaje ni giran la cámara, aunque `XamInputGetState` entrega ±32767
(en los menús sí se usan). Los botones sí funcionan. Pistas a revisar:
- Hilo de entrada `sub_8299C980` (y `sub_8299CAE0`): llama a XInputGetState (`sub_82DBC010`) con flags 1 y 8 para los 4 usuarios
  y guarda muestras de 0x54 bytes con marca de tiempo (QPC × 0x83523470) en un anillo; la partida las consume después.
  ¿Se descartan las muestras analógicas por la marca de tiempo? ¿Se usan las de flags 8?
- Probar con el mando real (DualSense) para descartar el camino del guion.
- `fetch op 19` sin soporte (4 avisos).
- Baird sigue con zonas azules (menos que antes).

## Herramientas nuevas

- `GOWJ_TEXDUMP=K8`: vuelca cualquier k_8 de 512×512 (caché de glifos), además de `texraw_*` con los bytes crudos.
- `GOWJ_MEMEXLOG=<hash>`/`1`: constantes y registros de memexport.
- Guion `scripts/read_ui.txt` (Y = cambiar arma + captura, START = pausa + captura).
