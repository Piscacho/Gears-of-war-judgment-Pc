# Sesión 6 (2 h sin supervisión) — Campaña vs Survival

Fecha: 2026-11-14. Sin commits.

## Resultado

- **Causa de "no me puedo mover" en la campaña, aclarada:** el jugador cae al vacío y muere ("OBJECTIVE FAILED — Player has died")
  porque **no se cargan los niveles del capítulo**. El viaje es `GearGame_P?game=geargamecontent.GearGameAID?chapter=0`
  (con guardados viejos además `?loadcheckpoint`). `GearGame_P` es el nivel base de la campaña y los niveles del capítulo
  (`SP_00_Museum_*`) se deberían cargar por streaming, pero **nunca se piden** (ni siquiera se buscan en el TOC en partida).
- El checkpoint guardado en ejecuciones anteriores apuntaba a `GearGame_P` (bucle): los guardados viejos están en
  `run\userdata_bak_s6\`. Con guardados nuevos pasa lo mismo, así que no era la causa.
- **Survival funciona hasta cargar su mapa:** Main Menu → Survival → **Private** (Standard pide Xbox Live Gold) → lobby previo →
  selección de mapa (ESTATE) → START → se cargan `GearStartTransition`, `GEARGAMEHVB_COOP_SF`, **`MP_Estate.xxx`**: mapa de Estate,
  "PROTECT THE E-HOLE COVER!" y selección de clase. Es decir: los mapas sí se cargan cuando el modo los pide directamente.
- **Nuevo bloqueo en Survival:** al poco de empezar, caída `[crash] llamada indirecta a 0x00000000 (lr=0x826A9044)` en
  `sub_826A8DF8` (animación; la misma de la sesión 3): `r16` (objeto) es basura y su vtable+0x13C es NULL.
  Se lee de `lbzx r11, r11, r14` (tabla de bytes) → índice ×12 → `lwz r16, 8(r10)` desde `*(r7+0x124)`.

## Cambios

- XLiveBase (app 0xFC) como Xenia: 0x58004/0x58006 escriben 1, **0x58020 (CreateFriendsEnumerator) y 0x58023 fallan**
  (`X_E_FAIL`), 0x58046 éxito. `GOWJ_OLDXLIVE=1` = antes.
- Ganchos de diagnóstico (`GOWJ_TRAVELLOG=1`): URL de `Browse`/`LoadMap` (sub_82559608 / sub_8255A5E0), nativas de AWorldInfo
  (SeamlessTravel, PrepareMapChange, CommitMapChange…), de UGearEngine (Save/Load/Set/ParseCheckpoint, CheckCIV,
  LoadGameTypeConfigs) y de AGearGame (GetMapNameOverride, GetMapFilename…). `GOWJ_TOCLOG=<texto>`: búsquedas en el índice de
  paquetes (tras 40 s) con pila de invitado.
- Guiones: `scripts/fresh5.txt` (campaña con guardados nuevos), `scripts/survival.txt` / `survival2.txt` (Survival privado).
  Ojo: pulsaciones de más de ~300 ms activan la repetición automática en los menús; usar 100-200 ms.
- Tabla de nativas de UE3: en `.data` hay pares {puntero al nombre "AClaseexecFuncion", puntero a la función}.

## Siguiente

1. Arreglar la caída de animación `sub_826A8DF8` (Survival es el camino más corto a algo jugable).
2. Campaña: averiguar quién debería pedir el streaming del capítulo (Kismet de GearGame_P / GearGameAID). Ideas: enganchar la
   creación de `ULevelStreaming` y `UWorld::UpdateLevelStreaming`, o `USeqEvent_ChapterPoint` / `USeqAct_WaitForStreaming`.
