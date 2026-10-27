# Diálogos de `sceUtility`: qué espera MPO — `ULUS-10202` 1.02

Este documento es el contrato entre el juego y la librería `profiles/mpo/host/savedata/` (F18a), que
el HLE de F18b tiene que respetar. Solo lleva direcciones, offsets, números y comportamiento; nada de
código ni datos del juego (en particular, **no** copia la clave del savedata).

- **Cómo se obtuvo** (F18a, 2026-10-26):
  - los imports de las 106 etapas, con `prx_imports.py` y la tabla de NIDs de PPSSPP;
  - las etapas `title`, `briefing`, `cyberval2`, `ending`, `addscvr` y `n01` se enlazaron con
    `rlc.py link` (con los `.rlc` que extrajo F04) y se analizaron en Ghidra en `0x08B8F900`, con
    `mgp_main` como bloque externo, en un proyecto propio (`analysis/f18a/ghidra`);
  - se decompilaron los llamadores de cada `sceUtility*` y los que arman las peticiones; todo quedó
    en local, en `analysis/f18a/` (ignorado por git).
- Las direcciones son las de la etapa **cargada en `0x08B8F900`**. Salvo que se diga otra cosa, son
  de `title`.

## 1. Quién importa qué [V]

`mgp_main` no importa ningún diálogo (solo `GetSystemParam*`, `LoadNetModule`, `LoadAvModule`,
`Netconf*` y `HtmlViewer*`). Los diálogos están en 23 etapas:

| Etapas | Savedata | OSK | MsgDialog |
|---|---|---|---|
| `title`, `briefing`, `cyberval2` | sí | sí | sí |
| `ending` | sí | — | sí |
| `addscvr`, `apscan`, `gpstest`, `ntmenu2` | — | sí | sí |
| `frvsmenu`, `gsclimenu`, `gsmenu`, `n01`–`n03`, `n05`, `n09`, `n11`, `n12`, `n14`–`n18` | — | — | sí |

- De cada diálogo se importan `InitStart`, `Update`, `GetStatus` y `ShutdownStart`; de MsgDialog,
  también `Abort`.
- `gsmenu` importa además `sceUtilityGameSharing*` (fuera del alcance de F18a).
- Las cuatro etapas con savedata llevan **la misma librería** (los mismos llamadores con las mismas
  banderas; en `title` y `briefing`, incluso en las mismas direcciones). El OSK también es la misma
  librería en las cuatro etapas que se miraron (`title`, `briefing`, `cyberval2`, `addscvr`).

## 2. Savedata [V]

### 2.1 El objeto y el bucle

- Cada operación es un objeto de `0xED0` bytes. La petición `SceUtilitySavedataParam` está en
  **`obj+0x64`** y tiene el tamaño **V3 (`0x600`, 1536 bytes)**.
- El bucle (`FUN_08b978f4`; en `cyberval2` `FUN_08b8fdd0`, en `ending` `FUN_08b926f4`) llama a
  `GetStatus` en cada frame:
  - con el estado `0` en su máquina, espera a que acabe el hilo `iconload` y llama a `InitStart`;
  - con `RUNNING` (2) llama a `FUN_089c6b2c(0, 0)` (en `mgp_main`, probablemente la preparación del
    frame del GE) y a **`sceUtilitySavedataUpdate(2)`**;
  - con `FINISHED` (3) llama a `ShutdownStart`. Si `common.result == 2` (ABORT) marca la petición
    (bandera `0x100`) y, si `abortStatus` (`+0x5CC`) no es cero, la repite [V en el código; para qué,
    ?].
  - Después del cierre, una **carga** solo se acepta si `common.result == 0` y **`dataSize ==
    0xA610`**; si no, el juego usa el error `0x80110306` (`LOAD_DATA_BROKEN`). Así que el `Load`
    tiene que dejar `dataSize` con el tamaño leído, como PPSSPP.
  - `FUN_089cea9c(obj+0xB0)` (en `mgp_main`) convierte el `saveName` que deja el diálogo en el número
    de ranura (1–100), así que el `Save`/`Load` tiene que **copiar el `saveName` elegido a la
    petición**, como PPSSPP.

### 2.2 Lo que rellena el juego

Lo arman `FUN_08b97f5c` (común), `FUN_08b97fdc` (carga), `FUN_08b98218` (guardado) y `FUN_08b987ac`
(borrado), a partir de unas banderas de `obj+0x50`. Offsets relativos a la petición:

| Campo | Offset | Valor |
|---|---|---|
| `common.size` | `0x00` | `0x600` |
| `common.language` | `0x04` | `FUN_08899254()` (idioma del sistema) |
| `common.buttonSwap` | `0x08` | `1` salvo que el botón de confirmar del sistema sea círculo |
| hilos (graphics, access, font, sound) | `0x0C`–`0x18` | `0x30`, `0x18`, `0x38`, `0x28` |
| `mode` | `0x30` | ver la tabla de modos |
| `overwriteMode` | `0x38` | `1` al guardar |
| `gameName` | `0x3C` | `"ULUS10202"` |
| `saveName` | `0x4C` | `"%03d"` (`0x08BC74AC`): `"001"`… |
| `saveNameList` | `0x60` | lista de `"%03d"` en `obj+0x664`: 3 nombres (`001`–`003`), o 100 (`101`–`200`) con la bandera `0x1000` |
| `fileName` | `0x64` | `"00000000.000"` |
| `dataBuf` / `dataBufSize` | `0x74` / `0x78` | un bloque de `0x20000` bytes |
| `dataSize` | `0x7C` | **`0xA610`** al guardar |
| `sfoParam` | `0x80` | título, título del save y detalle (texto del juego, con fecha y tiempo de juego), `parentalLevel = 9` |
| `focus` | `0x5C8` | **3** (`LATEST`) |
| `key` | `0x5DC` | **16 bytes no nulos** (`0x08BC747C`), no imprimibles |
| `secureVersion` | `0x5EC` | **0** (las banderas `0x400`/`0x800`, que darían 1 o 2, no las usa nadie) |
| `multiStatus` | `0x5F0` | 0 |

- Con la bandera `0x2000` (que **ningún llamador usa**) el archivo sería `DATA.BIN` y la clave, otra
  (ASCII). Es código muerto en `ULUS-10202`.
- La bandera `0x80` (con `0x200` y `4`) carga en automático **el save de otro `gameName`**: un
  comando GCL (`FUN_08b99cf4`) recorre `ms0:/PSP/SAVEDATA` con `sceIoDopen`/`sceIoDread`
  (`FUN_08b99b54`, `FUN_08b99a18`; cadena `"%sPSP/SAVEDATA"` en `0x08BC75D4`), y para cada
  directorio cuyo nombre case con un prefijo que da el script, usa sus 9 primeros caracteres como
  `gameName` y el resto como `saveName` [V]. Para qué juego es, no se sabe [?].
- También existe la cadena `"PSP/SAVEDATA/ULUS10202%03d"` (`0x08BC75B8`): el juego comprueba
  directorios de save por su cuenta [V; el uso exacto, ?].

### 2.3 Modos que usa [V]

Los llamadores (`FUN_08b98b70`…`FUN_08b99710`, con `FUN_08b987b8` como constructor) piden:

| Bandera | Modo | Para qué [?] |
|---|---|---|
| `0x41` | `AUTOSAVE` (1), ranura dada | guardado sin diálogo |
| `0x11` | `SAVE` (3), ranura dada | guardado con confirmación |
| `3`, `0x1001`, `0x1003` | `LISTSAVE` (5) | elegir ranura (3 o 100) |
| `4` (y `0x284`) | `AUTOLOAD` (0), ranura dada | carga sin diálogo |
| `2`, `0x1002`, `0x1200` | `LISTLOAD` (4) | elegir qué cargar |
| `0x20`, `0x22` | **`LISTALLDELETE` (7)** | borrar |

- **El borrado es `LISTALLDELETE`**, que en una PSP lista **todos** los saves de la Memory Stick, de
  cualquier juego. El constructor de borrado solo pone el modo: **`gameName`, `saveName` y
  `fileName` quedan vacíos**. Es justo el caso de PPSSPP #6600 (vaciar `SAVEDATA`) y el de la
  comprobación de PPSSPP para la demo de MGS: Peace Walker. La librería solo borra el directorio de
  la entrada elegida, y nunca `SAVEDATA` (`savedata_store.hpp`).
- No usa `SIZES`, `LIST`, `FILES`, `GETSIZE`, `MAKEDATA*`, `READDATA*`, `WRITEDATA*` ni `ERASE*`
  (el bucle compara `mode == 8`, pero ningún constructor lo pone).

### 2.4 Cifrado [V en los datos; S en PPSSPP]

- `module_start` de `mgp_main` declara **SDK `0x02080010`** (`docs/analysis/mgp_main.md` §2). Para
  PPSSPP (`GetSDKMainVersion`) eso es la versión principal **4**.
- Con clave no nula y `secureVersion = 0`, `DetermineCryptMode` da **el modo 5**, así que una PSP (y
  PPSSPP con `bEncryptSave`, que viene activado) guarda **cifrado en modo 5**, con
  `SAVEDATA_PARAMS[0] = 0x41`.
- **Hace falta el cifrado** para leer los saves de una PSP o de PPSSPP, y para que los nuestros
  carguen en una PSP. Para jugar en el port, no: PPSSPP y la librería leen saves sin cifrar
  (`SAVEDATA_PARAMS` a cero). Ver la propuesta en `docs/sessions/F18a.md`.

## 3. OSK [V]

- `title` (nombre, `"INPUT YOUR NAME"`, `FUN_08bbdc00`), `briefing` (nombre; y contraseña,
  `"ENTER PASSWORD"`/`"VERIFY PASSWORD"`), `cyberval2`, `addscvr`, `apscan`, `gpstest` y `ntmenu2`.
- La petición (`FUN_08bb7ad8` y `FUN_08bbd430`) es la de PPSSPP:
  - `common.size = 0x40`, `language = 1`, `buttonSwap` como en el savedata; hilos: graphics `0x31`,
    access `0x33`, font `0x32`, sound `0x10`;
  - **un solo campo** (`fieldCount = 1`): `unk_00 = 4`, `language = 1`, **`inputtype = 9`**
    (`LATIN_DIGIT | LATIN_UPPERCASE`; la tabla de `0x08BC99D4` tiene 4 entradas y todas valen 9, en
    las cuatro etapas que se miraron), `lines = 1`;
  - `outtextlength = 0x200` sobre un búfer que el juego limpia solo en `0x200` bytes;
  - **`outtextlimit`**: 15 para el nombre, **10 para la contraseña** de `briefing`.
- El bucle (`FUN_08bb7c58`) llama a `sceUtilityOskUpdate(2)` en `RUNNING` y a `ShutdownStart` en
  `FINISHED`. Al volver a `NONE`:
  - si `common.result == 2`, **vuelve a abrir el OSK**;
  - si `field.result == 1` (`CANCELLED`), descarta el texto;
  - si no, copia el texto. PPSSPP siempre deja `CHANGED` (2).
- El texto se pasa a ASCII con `FUN_08bb7280`: **todo carácter `>= 0x80` se vuelve `'?'`**.

## 4. MsgDialog [V]

- `FUN_08bbd680` arma una petición **V2 (`0x244`, 580 bytes)**, con los hilos graphics `0x28`,
  access `0x18`, font `0x28` y sound `0x28`, y uno de estos tipos:

| Caso | `type` | `options` |
|---|---|---|
| 1 | 1 (texto) | `0x001` (`TEXTSOUND`) |
| 2 | 1 | `0x111` (`TEXTSOUND | YESNO | DEFAULT_NO`) |
| 3 | 1 | `0x011` (`TEXTSOUND | YESNO`) |
| 4 | 1 | `0` |
| 5 | 0 (código de error) o 1 (texto con el código en hexadecimal) | `0` |
| 6 | 1, con uno de tres textos fijos | `0` |

- El caso 5 elige con `FUN_08bbd4a0` si el código lo muestra el firmware (`type 0`) o el juego.
- El bucle (`FUN_08bbd944`) llama a `sceUtilityMsgDialogUpdate(2)` en `RUNNING` y cuenta los frames
  en `obj+0x50`; a partir del 61 limpia sus campos de petición [V; para qué, ?]. En `FINISHED` llama a
  `ShutdownStart`; en `NONE` lee `buttonPressed`
  (`obj+0x2A4`) y `common.result`: si vale 2 y el caso es 5 o 6, **repite el diálogo** [?: no se sabe
  quién pone 2 ahí; PPSSPP no escribe `common.result` en MsgDialog].
- `FUN_08bbd8f8` llama a `Abort` si el diálogo está en `RUNNING`.

## 5. Lo que esto implica para el HLE (F18b)

- `ms0:/` de la IO (F09) y la raíz de la librería tienen que ser **el mismo directorio**, porque
  `title` también lista `ms0:/PSP/SAVEDATA` con `sceIoDopen`/`Dread`.
- `Update` se llama con `animSpeed = 2` (20 FPS); un fundido dura 15 llamadas.
- La regla de un diálogo a la vez y los códigos `WRONG_TYPE` están en `utility_dialogs.hpp`.
