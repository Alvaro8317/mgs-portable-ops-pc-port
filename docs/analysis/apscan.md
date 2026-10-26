# AP Scan: qué espera MPO de `sceNetApctl` — `ULUS-10202` 1.02

Este documento es el contrato entre el juego y el modelo de `profiles/mpo/host/apscan/` (F20a), que
el HLE de F20b tiene que respetar. Solo lleva direcciones, offsets, números y comportamiento; nada de
código del juego.

- **Cómo se obtuvo** (F20a, 2026-10-26):
  - las etapas `apscan` y `briefing` se enlazaron con `rlc.py link` y se analizaron en Ghidra en
    `0x08B8F900`, con `mgp_main` como bloque externo (`docs/formats/rlc.md`);
  - `mgp_main` se leyó en una copia del proyecto de F03/F06;
  - todo quedó en local, en `analysis/f20a/` (ignorado por git).
- Las direcciones de etapa son las de la etapa **cargada en `0x08B8F900`**. Las de `mgp_main` son las
  de su carga en `0x08804000`.

## Resumen

- **El AP Scan vive en las etapas `apscan` y `briefing`**, que tienen el mismo código del escaneo en
  direcciones distintas [V]. `mgp_main` no interviene más allá de sus stubs de import y de las
  funciones de reclutas.
- La etapa registra **su propio handler** con `sceNetApctlAddHandler`. El handler **solo reacciona si
  `newState == 0` (`DISCONNECTED`)** [V]:
  - con el evento `SCAN_COMPLETE` (2), marca el escaneo como terminado;
  - con el evento `ERROR` (6), guarda el código de error y marca el fallo.
- **El juego escanea en bucle**: en cuanto ve el escaneo terminado, lee los resultados y vuelve a
  llamar a `sceNetApctlScanUser` [V]. **No mira lo que devuelve `ScanUser`** [V], y **borra sus
  banderas justo después de la llamada** [V]. Por eso:
  - el estado tiene que volver a `DISCONNECTED` **antes** de que el handler vea `SCAN_COMPLETE`, o
    el siguiente `ScanUser` falla en silencio y el juego espera para siempre;
  - ningún evento puede llegar al handler **dentro** de `ScanUser`.
- Lee los infoId **4 (señal), 0 (BSSID), 1 (SSID) y 2 (longitud del SSID)**. **No lee el canal (3)
  ni la seguridad (5)** [V].
- **Descarta un AP con señal ≤ 9** y elige el más fuerte (con la señal redondeada hacia abajo a un
  múltiplo de 5) [V].
- El recluta sale de una semilla calculada con **BSSID[2..5] y los bytes del SSID** [V]. El mismo AP
  da siempre la misma semilla.
- La búsqueda de **SSID especiales está desactivada en este build**: la función devuelve siempre −1
  [V].
- **Por qué se cuelga PPSSPP** (#20703): lo más probable es que al terminar el primer escaneo el
  estado siga en `SCANNING` cuando el juego vuelve a llamar a `ScanUser`. Es una hipótesis de leer el
  código; no se ejecutó PPSSPP [?] (§7).

## 1. Dónde está [V]

| Qué | `apscan` | `briefing` |
|---|---|---|
| Crear el actor del AP Scan (objeto de `0x2B0` bytes) | `0x08BAFE44` | — |
| Constructor (estado inicial, umbrales, red) | `0x08BAFD00` | — |
| Destructor | `0x08BAFC98` | — |
| Puntero global al objeto | `0x08BC43FC` | `0x08BE67BC` |
| Abrir la red y registrar el handler | `0x08BAED04` | `0x08BD599C` |
| Cerrar la red | `0x08BAED80` | — |
| **Handler de `sceNetApctl`** | `0x08BAECC4` | `0x08BD595C` |
| Iniciar un escaneo (`ScanUser`) | `0x08BAEDE8` | `0x08BD5A80` |
| Comprobar el interruptor WLAN | `0x08BAF3E8` | `0x08BD6080` |
| Máquina de estados (un paso por frame) | `0x08BAF554` | `0x08BD61EC` |
| Leer la lista de resultados | `0x08BAF13C` | `0x08BD5DD4` |
| Leer una entrada | `0x08BAEE20` | `0x08BD5AB8` |
| Comparador para ordenar (señal, descendente) | `0x08BAEFE0` | `0x08BD5C78` |
| Armar el registro del recluta | `0x08BAEFF0` | `0x08BD5C88` |
| Buscar un SSID especial (devuelve −1) | `0x08BB15AC` | `0x08BDE050` |
| Pasar el registro al generador de soldados | `0x08BAEF54` | `0x08BD5BEC` |
| Cargar/descargar los módulos de red | `0x08BAFF08` / `0x08BAFFA8` | — |

- Las columnas con "—" no se buscaron en `briefing`; el código del escaneo es el mismo.
- La etapa llama a la red **a través de los stubs de import de `mgp_main`** (los enlaza el `.rlc`):

  | Stub en `mgp_main` | Import |
  |---|---|
  | `0x089DAB74` / `0x089DAB6C` | `sceNetInit` / `sceNetTerm` |
  | `0x089DAC64` / `0x089DACAC` | `sceNetInetInit` / `sceNetInetTerm` |
  | `0x089DAC54` / `0x089DAC4C` | `sceNetApctlInit` / `sceNetApctlTerm` |
  | `0x089DAC44` / `0x089DAC34` | `sceNetApctlAddHandler` / `sceNetApctlDelHandler` |
  | `0x089DADC4` | `sceWlanGetSwitchState` |
  | `0x089DAD5C` / `0x089DAD7C` | `sceUtilityLoadNetModule` / `sceUtilityUnloadNetModule` |

  Los tres imports propios de la etapa son `sceNetApctlScanUser` (`0xE9B2E5E6`),
  `sceNetApctlGetBSSDescIDListUser` (`0x6BDDCB8C`) y `sceNetApctlGetBSSDescEntryUser`
  (`0x04776994`).
- `mgp_main` tiene **otro handler de Apctl**, en `0x0885C588`, para el modo infraestructura (registrado
  en `0x0885D27C`). Guarda el último evento en `0x089F55D4`, el error en `0x08A45EF4`, `oldState` en
  `0x08A45F08` y `newState` en `0x08A45F0C`. **El AP Scan no lo usa** [V]: la etapa no llama a
  `0x0885D27C` ni lee esas variables.

## 2. Secuencia de llamadas [V]

1. **Al crear el actor** (`0x08BAED04`):
   - `sceUtilityLoadNetModule(1)` y `sceUtilityLoadNetModule(3)`;
   - `sceNetInit(0x80000, 0x1E, 0, 0x1E, 0)`;
   - `sceNetInetInit()`;
   - `sceNetApctlInit(0x8000, 0x21)`;
   - `sceNetApctlAddHandler(0x08BAECC4, objeto)`: el id se guarda en `objeto+0xA0`.
   - **No se comprueba ningún valor de retorno.**
2. **Estado 1** (`0x08BAF3E8`): pone a 0 las banderas y contadores y llama a
   `sceWlanGetSwitchState()`. Si devuelve 0, muestra un diálogo de error y sale. Si no, pasa al
   estado 2.
3. **Estado 2**: `sceNetApctlScanUser()` (`0x08BAEDE8`) y pasa al estado 3.
   - Después de la llamada, `objeto+0x90 = 0` (banderas) y `objeto+0x98 += 1` (contador de escaneos).
4. **Estado 3**, cada frame:
   - si `objeto+0x90 & 1` (escaneo terminado):
     - si el jugador pidió salir (`objeto+0x8C & 8`), sale (estado 7);
     - si no, lee los resultados (`0x08BAF13C`), calcula el nuevo objetivo de la barra y **vuelve a
       llamar a `ScanUser`** (paso 3);
   - si `objeto+0x90 & 2` (error): pasa al estado 4, con el código de `objeto+0x94`, y muestra el
     diálogo de error con el código en hexadecimal.
   - **La salida pedida por el jugador también espera a `SCAN_COMPLETE`**: si nunca llega, el jugador
     no puede ni salir.
5. **Al destruir el actor** (`0x08BAED80`):
   - `sceNetApctlDelHandler(id)` si el id es ≥ 0;
   - `sceNetApctlTerm()`, `sceNetInetTerm()` y `sceNetTerm()`;
   - `sceUtilityUnloadNetModule(3)` y `sceUtilityUnloadNetModule(1)`.
   - **No llama a `sceNetApctlDisconnect` ni a `sceNetApctlGetState`.**

## 3. El handler [V]

Firma: `handler(oldState, newState, event, error, pArg)`, con `pArg` = el objeto.

| Condición | Efecto |
|---|---|
| `newState != 0` | nada |
| `newState == 0` y `event == 6` (`ERROR`) | `objeto+0x94 = error`; `objeto+0x90 |= 2` |
| `newState == 0` y `event == 2` (`SCAN_COMPLETE`) | `objeto+0x90 |= 1` |
| cualquier otro evento | nada |

- `oldState` no se mira. `SCAN_REQUEST` (1) se ignora, pero no estorba.
- Un evento `SCAN_STOP` (11, el que inventa PPSSPP) se ignora.

## 4. Cómo lee los resultados [V]

**Lista de IDs** (`0x08BAF13C`):
- Llama a `sceNetApctlGetBSSDescIDListUser(&size, buf)` con `size = 0x80` y un buffer en la pila:
  caben **16 entradas** de 8 bytes (`{u32 next, u32 id}`).
- Si la llamada falla o `size` vuelve a 0, no hay resultados.
- Si no, recorre la lista: lee el `id` de la primera entrada y sigue los punteros `next` (direcciones
  de la PSP) **hasta encontrar un 0**. El último `next` escrito tiene que ser 0.
- Guarda el número de APs válidos en `objeto+0xA4`.

**Cada entrada** (`0x08BAEE20`), en este orden, con un buffer de 0x20 bytes:

| infoId | Qué | Cómo lo usa |
|---|---|---|
| 4 | señal | Lee **solo el byte bajo**. Si es ≤ 9, descarta el AP (y no pide nada más). |
| 0 | BSSID | Lee 6 bytes. |
| 1 | SSID | Copia 32 bytes. |
| 2 | longitud del SSID | `u32`; la recorta a 32 y pone un 0 en esa posición. |

- Luego copia el SSID con un `strncpy` de 0x21 bytes (`0x089D1004`), así que **un NUL dentro del
  SSID lo corta**.
- Cualquier error (< 0) de `GetBSSDescEntryUser` descarta ese AP.

**Registro de 0x48 bytes por AP** (en la pila; el elegido se copia a `objeto+0x1F8` y, al
reclutar, a `objeto+0x240`):

| Offset | Tamaño | Qué |
|---|---|---|
| `+0x00` | 4 | id de SSID especial, o −1 |
| `+0x04` | 4 | semilla del recluta (abajo) |
| `+0x08` | 4 | tipo: 2 normal, 3 especial |
| `+0x10` | 6 | copia del BSSID |
| `+0x18` | 6 | BSSID |
| `+0x1E` | 33 | SSID terminado en NUL |
| `+0x3F` | 1 | señal |
| `+0x40` | 1 | 1 si es especial |

## 5. Cómo elige el AP y arma el recluta

- **Elección** [V]:
  1. Ordena una copia por señal, de mayor a menor (`0x089D005C`, que por su uso es un `qsort` [?]).
  2. Busca el primer AP **especial** con señal ≥ `objeto+0xB0` (vale `0x19` = 25).
  3. Si no hay ninguno, redondea la señal de todos hacia abajo a un múltiplo de 5, vuelve a ordenar y
     toma el primero. Con empates, el orden depende de `qsort` [?].
- **SSID especiales**: la búsqueda (`0x08BB15AC`) es `jr ra; li v0, -1` en `apscan` y lo mismo en
  `briefing` (`0x08BDE050`) [V]. En US 1.02 **ningún SSID es especial**, así que el paso 2 nunca
  elige nada.
- **Semilla** (`0x08BAEFF0`) [V]:
  - parte del `u32` little-endian formado por **BSSID[2..5]**;
  - le aplica XOR, byte a byte y de forma cíclica sobre sus 4 bytes, con los `strlen(SSID)` bytes del
    SSID;
  - con BSSID[0..1] no hace nada (solo los copia a `+0x10`).
- **Del registro al soldado**:
  - `0x08BAEF54` llama a `0x08836E18(…, −1, id especial, 1, semilla, tipo, 0)` y a `0x08836F38` de
    `mgp_main` [V]. Que sean el generador de soldados es una deducción [?].
  - `0x0883754C(soldado, …, 1)` tiene que devolver 1 para que el AP cuente; si no, el escaneo no da
    objetivo [V]. Devuelve −1, −2, −4 o −5 según cuatro comprobaciones distintas. Que una de ellas
    rechace un soldado repetido es una hipótesis [?]. Si lo es, un AP ya usado no vuelve a servir.
  - Al terminar de llenar la barra, `0x08839D88` confirma el recluta [?] y el juego guarda el momento
    en `0x08A5D704` [V].
- **Seguridad del AP**: el juego **no la consulta** [V]. Lo de que "cuenta si tiene contraseña" (un
  foro, `docs/RESEARCH.md`) no se ve en este build.

## 6. Barra de señal y cooldown [V salvo lo marcado]

- La barra (`objeto+0x288`) suma tres componentes:
  - un **nivel base** que tiende al objetivo del último escaneo (`objeto+0x28C` → `objeto+0x290`);
  - una **componente del jugador** (`objeto+0x294`): sube 10 por frame mientras se mantiene el botón y
    baja `caída` por frame si no;
  - una oscilación (`objeto+0x298`, con `vsin`).
- El objetivo sale de la señal del AP elegido: se acota a **[10, tope]** y se convierte en
  `señal × 480 / 100`, con un mínimo fijo. Si el escaneo no da AP, el objetivo vuelve a 0.
- Cuando `0x08BB0FD4` devuelve 1 con el botón pulsado (probablemente, con la barra llena [?]), el
  AP elegido se copia a `objeto+0x240`. **El recluta empieza en el siguiente `SCAN_COMPLETE`**
  (estados 5 a 7), así que también depende de que los escaneos sigan terminando.
- **Cooldown global del juego** (no es por AP):
  - `t = reloj/300 − último_recluta/300`, con `reloj = *(*(0x089F51D0) + 0x3C)` y
    `último_recluta = 0x08A5D704`;
  - si `t < 180` y ya hubo un recluta:
    - el tope del objetivo es `30·t²/32400 + 20` (sube de 20 a 50);
    - y la `caída` es `5 − 4·t²/32400` (de 5 a 1).
  - Si no, el tope es 50 y la `caída` es 1.
  - **La unidad de `reloj` no se confirmó** [?]. Si son frames a 60 Hz, el cooldown dura 15 minutos;
    si son a 30 Hz, 30. Tampoco se sabe si `0x08A5D704` va en la partida guardada [?].
- **No hay historial por AP** en la etapa [V]. El "cooldown de 24 h por AP" de los foros
  (`docs/RESEARCH.md`) no aparece aquí. Podría estar en `0x0883754C` [?].
- Qué botón llena la barra y cuál pide salir (las máscaras de `0x08A793F0` sobre el pad) no se miró
  [?].

## 7. Por qué se cuelga en PPSSPP (#20703)

Referencia: PPSSPP `v1.20.4` (`fa50bb19`), `Core/HLE/sceNet.cpp`, `__NetApctlCallbacks` y
`NetApctl_ScanUser` [S].

- En PPSSPP, cada evento cambia `netApctlState` **al newState con el que se encoló**:
  - `SCAN_REQUEST` encola `SCAN_COMPLETE` con newState `SCANNING`;
  - al procesar `SCAN_COMPLETE`, el estado se queda en `SCANNING`. Al handler le pasa
    `DISCONNECTED`, y encola `SCAN_STOP` (11), que es el que pone `DISCONNECTED`;
  - entre los dos eventos pasan `adhocEventPollDelay` = 100 ms.
- El juego reacciona a `SCAN_COMPLETE` en el frame siguiente y vuelve a llamar a `ScanUser`. En ese
  momento el estado sigue en `SCANNING`, así que `ScanUser` devuelve `NOT_DISCONNECTED`
  (`0x80410A04`) **y no encola nada**. El juego no mira el retorno, borra sus banderas y espera un
  `SCAN_COMPLETE` que no llega: **se queda "escaneando" para siempre** y ni siquiera puede salir
  (§2.4).
- Además, en esa función `args`, `newState` y compañía son **referencias a `apctlEvents.front()` que
  se siguen usando después de `pop_front()`**, y el `push_front()` siguiente puede ocupar la misma
  ranura. Si pasa, el handler recibe los datos del evento siguiente: `SCAN_REQUEST` llegaría como
  `(0, SCANNING, SCAN_COMPLETE)` y `SCAN_COMPLETE` como `(…, DISCONNECTED, SCAN_STOP)`. Ninguno
  cumpliría `newState == 0 && event == 2`, y el juego no vería ni el primer escaneo.
- Las dos explicaciones salen de leer el código; **no se comprobaron ejecutando PPSSPP** [?]. Las dos
  llevan al mismo síntoma que describe el issue.

## 8. Cómo lo hace JPCSP (que funciona) [S]

Referencia: `jpcsp/jpcsp`, `src/jpcsp/HLE/modules/sceNetApctl.java` @ `00ac4d6` (2026-09-24).
Se leyó solo como referencia de comportamiento (es GPL); no se copió código.

- `ScanUser` solo marca `doScan` y despierta un hilo propio de Apctl.
- El hilo pasa a `SCANNING`, con el evento `SCAN_REQUEST`, y a los **700 ms** (`SCAN_DURATION_MILLIS`)
  vuelve a `DISCONNECTED`, con el evento `SCAN_COMPLETE` (`oldState` `SCANNING`).
- `changeState` **cambia el estado antes de llamar al handler**, "por si el handler vuelve a llamar a
  sceNetApctl".
- El evento depende de la transición, "como en pruebas con una PSP real": `→JOINING` es
  `CONNECT_REQUEST`, `→GETTING_IP` es `ESTABLISHED`, `→GOT_IP` es `GET_IP`, `→SCANNING` es
  `SCAN_REQUEST`, y `→DISCONNECTED` es `SCAN_COMPLETE` si venía de `SCANNING` y
  `DISCONNECT_REQUEST` si no.
- `GetBSSDescIDListUser`: 8 bytes por entrada (`next`, `id`), y devuelve en `*size` el tamaño total
  necesario. Solo da 1 AP.
- `GetBSSDescEntryUser`:
  - 0 (IBSS) son 6 bytes;
  - 1 (SSID) son 32 bytes;
  - 2 (longitud) es un `u32`;
  - 4 (señal) es 1 byte;
  - 5 (seguridad) es un `u32`, siempre `WPA_AES`;
  - el 3 no lo trata (devuelve −1).

PPSSPP coincide en el formato de la lista y de las entradas. Además:
- trata el 3 como el canal (1 byte);
- devuelve 4 APs, con la señal al azar entre 1 y 99;
- usa `SCE_NET_APCTL_ERROR_INVALID_CODE` (`0x80410A02`) para un infoId desconocido.

## 9. Qué tiene que cumplir el HLE (F20b)

Todo esto lo cumple ya el modelo de F20a (`apctl_model.hpp`) y lo prueba `mpo_apscan_test`:

1. **Entregar los eventos desde un hilo de Apctl**, nunca dentro de `sceNetApctlScanUser`.
2. **Cambiar el estado antes de llamar al handler.** `SCAN_COMPLETE` va con
   `(SCANNING → DISCONNECTED)`.
3. Un error de escaneo va como `ERROR` con `newState = DISCONNECTED` y el código en `error`.
4. La lista de IDs en el formato `{next, id}`, con el último `next` a 0 y el tamaño total en
   `*size`.
5. Las entradas con el tamaño exacto de cada infoId (§4); la señal es un byte entre 0 y 100.
6. Como mucho 16 APs útiles (el buffer del juego es de 0x80 bytes); el resto no se lee.
7. Además, fuera del modelo:
   - `sceWlanGetSwitchState` tiene que devolver 1 (si no, el juego muestra un error y sale);
   - hay que aceptar `sceNetInit`, `sceNetInetInit`, `sceNetApctlInit`/`Term`,
     `sceUtilityLoadNetModule(1, 3)` y sus `Unload`, aunque no hagan nada.

## Lo que falta confirmar

- Cuánto tarda un escaneo en una PSP real (JPCSP usa 700 ms) y si el firmware entrega
  `SCAN_REQUEST`.
- Qué devuelve el firmware en `GetBSSDescIDListUser` mientras escanea, y el código de error de un
  `entryId` inválido (el modelo usa `INVALID_ID`, `0x80410A09`).
- La unidad del reloj del cooldown y si el último recluta va en la partida guardada (§6).
- Qué comprueba `0x0883754C` y si un AP ya usado se puede reutilizar (§5).
- El código de `briefing` se comparó solo en las funciones del escaneo (§1).
