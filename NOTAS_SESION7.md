# Sesión 7 — Campaña jugable (y Survival sin caídas)

Fecha: 2026-11-14. Sin commits.

## Resultado

- **La campaña ya es jugable.** Con guardados nuevos: título → menús → Select Difficulty → se cargan `GearGame_P` y después los
  niveles del capítulo (`SP_00_Museum_P`, `_Background_1_M`, `_Old_Town_Start_M/_S`, `_Con_1_M/_S`, `_Start_A`, `_E`, `_Lighting`)
  → película de introducción (Bink) → cinemáticas en tiempo real con subtítulos → "BAIRD'S TESTIMONY" → **Old Town** con
  "OBJECTIVES: Meet up with the convoy", punto de control guardado, el jugador anda, gira la cámara, apunta (LT) y dispara (RT).
- **Survival** (Private → Estate) llega a la oleada 1/10 y se juega sin la caída de animación.

## Arreglos

1. **Red: sockets locales falsos** (`runtime/xam.cpp`). La campaña es un servidor `?listen`: `UWorld::Listen` (sub_8270D3C8) crea el
   `NetDriver` y `InitListen` abre un socket UDP/VDP (`NetDll_socket(caller, 2, 2, 0xFE)`), que devolvía `INVALID_SOCKET`.
   `LoadMap` acababa con el error "Error initializing network layer." y `UGearEngine` (sub_82A69928, la que trata `?chapter=`)
   **salía antes de pedir el mapa del capítulo** (sub_8255C638 + sub_8255C978). Ahora socket/bind/getsockname/select/sendto…
   funcionan en local sin red real (sendto descarta, recvfrom da WSAEWOULDBLOCK). `GOWJ_NONET=1` = antes.
2. **Sesgo de exponente del color** (`gpu_d3d12.cpp`, RB_COLOR_INFO bits 20-25): la salida del PS se multiplica por 2^sesgo al
   escribir en EDRAM, como en Xenia. El posproceso de partida (bloom, DOF, exposición) dibuja en superficies 16_16_16_16 con +5 y
   las resuelve con −5; sin esto la partida salía casi negra (las cinemáticas no). `GOWJ_NOCOLORBIAS=1` = antes.
3. **`memset` del CRT mal recompilado** (`sub_82D9F940`, sin .pdata): XenonRecomp cortaba la función en el primer `bdzlr-`
   (BO=26), así que las colas de 2-3 bytes no se escribían. `USkeletalMeshComponent::InitSkelControls` (sub_826B0F70) llenaba
   `SkelControlIndex` con 0xFF y quedaba basura (`255 16 202` con un AnimTree de 1 lista) → `sub_826A8DF8` indexaba fuera de
   `SkelControlLists` y hacía una llamada virtual a NULL (la caída intermitente de las sesiones 3 y 6, también en campaña).
   Arreglo: entrada manual `0x82D9F940: 0xA0` en `tools/make_functions.py` + recompilación. Comprobado que **ninguna otra función**
   generada termina en un salto condicional.

## Diagnóstico nuevo (`runtime/hooks_gowj.cpp`)

- `GOWJ_TRAVELLOG=1`: además registra `[loadmap]` (sub_82A69928: llamadas que hace y sus resultados — ParseOption, wcsicmp,
  checkpoint…) y `[listen]` (UWorld::Listen con traza de imports durante la llamada y el error devuelto).
- `GOWJ_ANIMCHECK=1`: recorre la lista de SkelControls al entrar en sub_826A8DF8 y registra nodos inválidos; `[animinit]` muestra
  el `SkelControlIndex` construido. `GOWJ_ANIMFIX=1` corta la lista en el nodo malo. `GOWJ_ANIMWATCH=1` vigila escrituras en el
  primer índice de 3 huesos.
- `tools/boostmontage.py salida.png factor columnas a.bmp …`: montaje aclarado para escenas oscuras.
- Guiones: `scripts/campaign7.txt` (cinemática), `campaign7b.txt` (hasta el control), `campaign7c.txt` (andar, girar, apuntar,
  disparar). Ancla del capítulo: `sp_00_museum_lighting` (~63 s); el control llega ~150 s después.

## Pendiente

- Armadura del jugador (Baird) con tono rojizo en partida; algunos personajes de Survival con zonas raras.
- Una vez se vieron subtítulos corruptos (caché de glifos k_8), no se repitió.
- Rendimiento (~20 fps), `fetch op 19`, avisos XAM sin implementar (app 0xFA 0x7001B).
- Probar más tramo de campaña (combate, cambio de sección) y a mano con el mando (`run\jugar.bat`).
