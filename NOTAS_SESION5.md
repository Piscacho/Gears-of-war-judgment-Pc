# Sesión 5 — Por qué no se mueve: el viaje al capítulo no ocurre

Fecha: 2026-11-14. Sin commits.

## Conclusión

**El mapa del capítulo nunca se carga.** Tras Select Difficulty el juego hace un viaje *seamless* de UE3: carga el mapa de transición
`GearGame_P` (el escenario de la estatua, el mismo del fondo del título) y debería cargar en segundo plano el destino
`SP_00_Museum_P.xxx` (+ sub-niveles `SP_00_Museum_Old_Town_Start_S`, `_Convoy_S`, `_Con_1_S`…). Ese segundo paso **nunca empieza**:
no se abre ningún `SP_00_*` (están en `Xbox360TOC.txt`, así que no es un problema del índice). El jugador queda en el mapa de transición:
puede disparar (RT), cambiar de arma (Y), apuntar (LT) y pausar, pero no moverse ni girar la cámara; a los ~70 s aparece
**"OBJECTIVE FAILED"** y se recarga el punto de control. La pausa ya dice "THE MUSEUM OF MILITARY GLORY: OLD TOWN [NORMAL]".

**Siguiente paso:** averiguar qué espera el viaje para cargar el destino. Candidatos:
- Sesión online (OnlineSubsystemLive): `XSessionCreate` (sub_82BF7EE8, usa `XamSessionCreateHandle`/`XamSessionRefObjByHandle`,
  **stubs que devuelven 0 sin escribir handle**) **no se llega a llamar**. Funciones de sesión del XDK en 0x82BF80xx-0x82BF89xx
  (mensajes XGI 0xB0011/14/15/16/18/1B/25/26). XLiveBase (app 0xFC) 0x58004/0x58020/0x58046 sin implementar.
- `XexGetProcedureAddress` de xam.xex: ordinales 1164, 2491, 2493, 2494 devuelven "no encontrado" (2493/2494 cada cuadro desde
  el bucle principal: sub_82A6AD10 → sub_829D9DAC → sub_82DBCE98).
- Revisar la carga asíncrona de paquetes (¿se pide `SP_00_Museum_P` y falla antes de abrir?).

## Entrada: verificada de punta a punta (no es el fallo)

- Hilo de entrada `sub_8299C980`: XInputGetState(i, 1|8) → anillo de muestras de 0x54 bytes en `obj+4`
  (+0 tiempo float, +4 conectados[4], +0x14 estados XINPUT_STATE[4] de 0x10).
- Consumidor `sub_8299C3D0` → muestra combinada (con el stick: LX = 0x8000 durante LLEFT).
- `sub_829A18C8` (DeltaTime ~0,048 s) → `ViewportClient->InputAxis(…, valor/32768, DeltaTime)` (vtable +0x1C) para cada eje.
- Ganchos de diagnóstico: `GOWJ_INPUTLOG=1` (`[inputlog]`, `[inputout]`, `[inputdt]`).

## Cambios

- `mftb` desde el arranque del proceso (+1 s) en vez del arranque del PC (`GOWJ_MFTBHOST=1` = antes): las marcas de tiempo float
  de la entrada tenían valores de horas.
- `GOWJ_IMPTRACE=1` registra también el valor devuelto de cada import (`d< tid nombre -> valor`).
- Vigilancia de **lecturas** de memoria (`GOWJ_WATCHREAD=1` + `GOWJ_WATCHSTATIC`/`GOWJ_WATCHRING=1`, página sin acceso, RIP host).
- `XexGetProcedureAddress` registra la pila de invitado de los ordinales desconocidos.
- `xex/iso/Xbox360TOC.txt` extraído del disco.
