# PSMF (`.pmf`): vídeos del juego

Formato de los vídeos de `USRDIR/mpg/` y de `PSP_GAME/ICON1.PMF`. Lo lee `mpo_video`
(`profiles/mpo/host/video/`, F17a). Etiquetas: **[V]** verificado en los 31 archivos del ISO US
(`ULUS-10202`), **[S]** de PPSSPP `v1.20.4` (`Core/HLE/scePsmf.cpp`, `Core/HW/MpegDemux.cpp`,
`Core/HW/MediaEngine.cpp`), **[?]** sin verificar.

## Estructura

Un archivo es una cabecera de 0x800 bytes seguida de un MPEG-2 *program stream* en packs de 2048
bytes. Los enteros de la cabecera son big-endian, salvo la firma y la versión, que son ASCII.

| Offset | Tamaño | Campo | Valor en MPO |
|---|---:|---|---|
| `0x00` | 4 | firma `PSMF` | igual en los 31 [V] |
| `0x04` | 4 | versión ASCII | `0014` en los 31 [V] |
| `0x08` | 4 | offset del stream | `0x800` en los 31 [V] |
| `0x0C` | 4 | tamaño del stream | exactamente `tamaño del archivo − 0x800` en los 31 [V] |
| `0x50` | 4 | `streamDataTotalSize` [S] | `0x4E` en los 30 vídeos, `0x3E` en `ICON1` [V] |
| `0x54` | 6 | primer timestamp (90 kHz) | `90000` (1 s) en `MPO_OP` [V] |
| `0x5A` | 6 | último timestamp | inicio + frames × 3003 (fin del último frame) [V] |
| `0x80` | 2 | número de streams | 2 en los vídeos, 1 en `ICON1` [V] |
| `0x82` | 16 × n | entradas de stream | ver abajo |

Entrada de stream (16 bytes) [S], con los valores de MPO [V]:

- **Vídeo** (`0xE0`–`0xEF`): bytes 4–7, offset del mapa de *entry points* (EP); 8–11, número de
  entradas; 12 y 13, ancho y alto en unidades de 16 píxeles.
  - En MPO es `E0 00 20 FB`, sin mapa EP (0 entradas) y `1E 11` = 480×272. `ICON1` es 144×80.
  - **Ningún vídeo trae mapa EP** [V]: `scePsmf`/`sceMpeg` no pueden saltar a un *entry point*.
    Si el juego busca, PPSSPP avanza decodificando (`MediaEngine::seekTo`).
- **Audio** (`0xBD`, *private stream 1*): el byte 1 es el id privado (`0x00` = ATRAC3plus, canal 0;
  con el nibble alto distinto de 0, PPSSPP lo trata como PCM); los bytes 14 y 15 son los canales y la
  "frecuencia".
  - En MPO es `BD 00 20 04 …`, con 2 canales y frecuencia `2` [V].

## Program stream

- Cada pack de 2048 bytes empieza con una cabecera de pack MPEG-2 (`00 00 01 BA`, 14 bytes). En el
  primero le siguen la cabecera de sistema (`BB`) y un *private stream 2* (`BF`) de 0xFE bytes,
  probablemente datos para el Media Engine de la PSP [?] [V].
- El vídeo va en PES `0xE0` (cabecera PES MPEG-2, con PTS). Es **H.264** a 29.97 fps: los PTS avanzan
  3003 ticks por frame [V].
- El audio va en PES `0xBD`. Tras la cabecera PES hay 4 bytes propios de la PSP: el canal (`0x00`) y
  3 bytes más [S].
- La carga del audio es una secuencia de frames ATRAC3plus. Cada uno lleva una cabecera de 8 bytes:
  `0F D0 c1 c2` y 4 bytes más.
  - Tamaño del frame: `((c1 & 3) << 8 | c2 × 8) + 0x10` [S].
  - En MPO la cabecera es siempre `0F D0 28 5C 00 00 00 00` [V]: frames de 752 bytes con 744 de
    datos, estéreo (`c1` = `0x28`; `0x24` significaría mono [S]), 2048 muestras a 44.1 kHz, unos
    128 kbps.

## Contenido [V]

- 30 vídeos y 2479.641 s en total (41 min 20 s). Suman 74 315 frames, que coinciden **exactamente**
  con la duración de cada cabecera. El más largo es `s18_01` (322.055 s) y el más corto, `s10_03a`
  (9.676 s).
- El audio de cada uno dura unos 0.1 s más que el vídeo (el último frame ATRAC3plus está completo).
- `MPO_OP` y `MPO_loop` duran 10.010 s; `MPO_OP_LONG`, 130.130 s, y `MPO_OP_LOOP`, 120.120 s.
  - Los cuatro empiezan o terminan en la pantalla de título roja con el logo de MPO.
  - `MPO_OP_LONG` incluye el aviso "©1987 2006 Konami Digital Entertainment Co., Ltd.".
  - Qué vídeo usa la pantalla de título se sabrá en F17b.
- Las cinemáticas `sNN_MM*` están dibujadas en el estilo de cómic de Ashley Wood. `staffroll` son los
  créditos sobre un fondo animado oscuro.
- `ICON1.PMF` es la animación del XMB: 144×80, sin audio, 10.010 s.
