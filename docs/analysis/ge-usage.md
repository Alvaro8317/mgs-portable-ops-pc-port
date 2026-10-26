# Uso del GE en MPO — inventario con frame dumps de PPSSPP (F05)

Qué funciones del GE usa **MGS: Portable Ops** (`ULUS-10202`), medido sobre *frame dumps* (`.ppdmp`)
de PPSSPP. Esta página es la entrada de F11, F12, F14 y F15. Solo lleva números y nombres: los dumps
traen texturas y vértices del juego, así que **nunca entran al repo** (`CLAUDE.md` §1.1).

Estado (2026-10-25): **medido sobre 11 dumps de MPO**:
- el logo de KP, la intro, el título (2), el menú, el codec (2), las cinemáticas (2) y la primera
  misión (2);
- los resultados están en la sección 4 y la lista priorizada en la 5;
- lo que falta capturar está al final de la sección 6.

## 1. De dónde salen los datos

- **PPSSPP v1.20.4**, que es la versión instalada en el PC del usuario **[V]**: el ejecutable
  `PPSSPPWindows64.exe` contiene la cadena `v1.20.4`.
- Un *frame dump* es la grabación de **un frame completo**, de un flip de pantalla al siguiente. Como
  el gameplay de MPO va a 20 fps (divisor de vsync 3), un dump de gameplay es un frame lógico del juego
  [S: `CLAUDE.md` §4]. Contiene el estado inicial del GE, todos los comandos que se ejecutaron y los
  datos que esos comandos leen (vértices, índices, texturas, CLUT, transferencias).
- El formato se tomó del código de PPSSPP **v1.20.4** (commit `fa50bb1976065c4f8b1b47af227d367fe9771555`):
  `GPU/Debugger/Record.cpp` (escritura), `RecordFormat.h` (estructuras) y `Playback.cpp` (lectura)
  [S]. Lo que sigue está verificado contra dumps reales (ver 1.3).

### 1.1 Formato `.ppdmp`

Todo es little-endian.

| Campo | Tamaño | Notas |
|---|---|---|
| `magic` | 8 | `PPSSPPGE` |
| `version` | 4 | PPSSPP lee de la 2 a la 6. v1.20.4 escribe la **6**. |
| `gameID` + relleno | 9 + 3 | Solo desde la versión 4 (antes la cabecera mide 12 bytes). Es el `DISC_ID` (`ULUS10202`). |
| `command_count` | 4 | Número de registros. |
| `pushbuf_size` | 4 | Tamaño del buffer de datos descomprimido. |
| tabla de registros | `u32` + datos | `u32` con el tamaño comprimido y después los datos. Descomprimida mide `command_count × 9`. |
| pushbuf | `u32` + datos | Igual, y descomprimido mide `pushbuf_size`. |

- **Compresión**: las versiones 2 a 4 usan **snappy** (bloque crudo) y la 5 y la 6 usan **zstd**
  (PPSSPP comprime con nivel 6). Versión 3: añade `FRAMEBUF0-7`. Versión 6: corrige la marca de VRAM
  sin cambios (`flags & 2`).
- Cada **registro** ocupa 9 bytes empaquetados: `u8 type; u32 sz; u32 ptr`, donde `ptr` y `sz`
  apuntan al pushbuf. El grabador reutiliza datos ya emitidos, así que varios registros pueden apuntar
  a los mismos bytes.

| Tipo | Nombre | Contenido |
|---|---|---|
| 0 | `INIT` | 2048 bytes de `GPUgstate::Save`: en las palabras 5 a 7 `vaddr`, `iaddr` y `offsetaddr`, y desde la palabra 17 el valor de los registros del GE en los rangos de `contextCmdRanges` (209 registros), seguidos de las matrices. |
| 1 | `REGISTERS` | Los comandos del GE (`u32`: comando en el byte alto), en el orden en que se ejecutaron. |
| 2, 3 | `VERTICES`, `INDICES` | Los datos de la siguiente primitiva. |
| 4, 10 | `CLUT`, `CLUTADDR` | Datos de la paleta y, si viene de VRAM, `{u32 addr, u32 flags}`. |
| 5 | `TRANSFERSRC` | Origen de una transferencia de bloque **a VRAM**. |
| 6 | `MEMSET` | `{u32 dest, i32 value, u32 sz}` sobre VRAM, hecho por la CPU. |
| 7, 8 | `MEMCPYDEST`, `MEMCPYDATA` | Una copia de la CPU a VRAM: el destino y los datos. |
| 9 | `DISPLAY` | `{u32 addr, i32 stride, u32 fmt}` del framebuffer mostrado, al principio y al final del frame. |
| 11 | `EDRAMTRANS` | El valor de `sceGeEdramSetAddrTranslation` cuando cambia. |
| 0x10-0x17 | `TEXTURE0-7` | Los datos de la textura de cada nivel, cuando está en RAM. |
| 0x18-0x1F | `FRAMEBUF0-7` | La textura de cada nivel cuando está en VRAM: `{u32 addr, i32 bufw, u32 flags, u32 pad}` y los datos. En `flags`, el bit 0 indica que la dirección es un render target ya usado en el dump, el bit 1 que la VRAM no cambió y el bit 2 que se dibujó encima. |

### 1.2 Qué **no** queda en un dump

Esto limita lo que se puede medir así, y lo que falta tiene que salir de trazas del runtime:

- **La estructura de las display lists.** El grabador no guarda `VADDR`, `IADDR`, `JUMP`, `CALL`,
  `RET`, `END`, `SIGNAL`, `FINISH`, `BASE`, `OFFSETADDR`, `ORIGIN`, `BOUNDINGBOX` ni `BJUMP`. Los
  signals, los stalls, los finish y los saltos condicionales por bounding box se ven con el histograma
  HLE de F08 y F11.
- **Las transferencias de bloque cuyo destino es RAM** (descargas de VRAM hechas por el GE), porque
  `Recorder::EmitTransfer` sale antes de registrarlas.
- **Las lecturas de VRAM hechas por la CPU.** Solo se ven las escrituras (`MEMSET` y `MEMCPY*`). Si el
  juego lee píxeles con la CPU (capturas, efectos), eso se mide en F15 con una traza.
- Un dump es **un solo frame**. Lo que se dibujó en el frame anterior (por ejemplo, el framebuffer que
  se muestra mientras tanto) solo aparece como "no dibujado en este dump" o como "framebuffer
  mostrado".

### 1.3 La herramienta

`profiles/mpo/tools/ge_usage.py`, con `ppdmp.py` (contenedor, snappy) y `zstd_lite.py` (zstd en
Python puro, RFC 8878). No necesita nada fuera de la biblioteca estándar y se ejecuta con
`python -I`. Si están `zstandard` o `compression.zstd` (Python 3.14), los usa para ir más rápido.

- Mantiene los 256 registros del GE como el hardware (último valor escrito por comando), empezando por
  el `INIT`. En cada `PRIM`, `BEZIER` y `SPLINE` cuenta el estado que importa para un renderer.
- Detecta **render-to-texture**: una textura o una CLUT en VRAM que cae dentro de un render target
  dibujado antes en el mismo dump, del target actual (*self-texturing*), del depth buffer o del
  framebuffer mostrado. También las transferencias de VRAM a VRAM y las escrituras de la CPU sobre
  render targets.
- Valida cada tamaño y offset: la cabecera, los tamaños comprimidos y descomprimidos (con tope de
  salida), que cada registro apunte dentro del pushbuf, el tamaño de cada estructura y que los valores
  del `INIT` correspondan a su comando. Un dump mal formado se rechaza con un mensaje y no aborta el
  resto.
- **Verificación con dumps reales [V]**: se probó con tres dumps públicos de la batería de pruebas de
  PPSSPP (`framedump.ppsspp.org/repro`, de otros juegos: uno v3 y uno v4 con snappy y uno v6 con
  zstd). Se descargaron al scratchpad y se borraron después. Los tres se leen sin errores. En los 5 848
  draws sin índices, el tamaño del registro `VERTICES` coincide en el 100 % de los casos con
  `count × tamaño de vértice`, calculado a partir del `VERTEXTYPE` que sigue la herramienta. Eso
  confirma que el `INIT`, el seguimiento de registros y las reglas de tamaño de vértice son correctos.
  La salida con zstd en Python puro y con `zstandard` es idéntica.
- El decodificador zstd se comparó además con libzstd 1.5.7 en 600 entradas aleatorias (niveles -5 a
  19, con y sin checksum y hasta 400 KB) sin diferencias. El test de CI usa 15 vectores fijos de esa
  misma librería.

## 2. Cómo se ejecuta

```bash
# Un resumen de todos los dumps de una carpeta
python3 -I profiles/mpo/tools/ge_usage.py dumps/
# La tabla en Markdown, total y por dump (para pegarla en esta página)
python3 -I profiles/mpo/tools/ge_usage.py --format markdown --per-file dumps/
# JSON para compararlo con scripts
python3 -I profiles/mpo/tools/ge_usage.py --format json --per-file dumps/ > ge.json
```

- Se puede leer directamente de la carpeta de PPSSPP
  (`/mnt/c/Users/<usuario>/Documents/PPSSPP/PSP/SYSTEM/DUMP/`) o de `dumps/` en la raíz del checkout,
  que está en `.gitignore` igual que `*.ppdmp`.
- Con Python puro se procesan unos 4 MB de datos descomprimidos por segundo. Para dumps grandes se puede usar un
  venv **fuera del repo** con `zstandard` (`uv venv ~/.venvs/mpo-tools && uv pip install -p
  ~/.venvs/mpo-tools zstandard`) y ejecutar con su `python -I`. `--pure-python` fuerza el
  decodificador propio.
- El código de salida es 0 si todo va bien, 1 si algún dump no se pudo leer (el resto se cuenta igual)
  y 2 si no se encontró ningún dump.

## 3. Qué mide el informe

Cada sección es un histograma: cuenta **draws**, salvo donde dice otra cosa.

| Sección | Contenido |
|---|---|
| `features` | Resumen por función: líneas, puntos, sprites, beziers y splines, skinning, morphing, modo through, formatos de textura, swizzle, mipmaps, funciones de textura, doblado de color, blending (ecuaciones MIN/MAX/ABSDIFF, colores fijos, factores dobles, lectura de alpha de destino), logic op, stencil (test y escritura), alpha y color test, depth test y clamp, fog, iluminación, dither, antialias, máscaras de escritura, framebuffers de 16 bits, texturas desde VRAM, render-to-texture, self-texturing, CLUT desde VRAM, transferencias y escrituras de la CPU sobre render targets. |
| `dump_records`, `ge_commands` | Registros del dump por tipo y comandos del GE por nombre. |
| `prims`, `prim_vertices` | Draws y vértices por primitiva (`through` o `transform`). |
| `clears` | Draws en modo clear y los canales que escriben. |
| `vtypes` | `VERTEXTYPE` decodificado, con el tamaño del vértice. |
| `patches` | Beziers y splines: tamaño de la malla de control, `PATCHDIVISION`, primitiva y tipo de spline. |
| `tex_*`, `unique_textures` | Formato (y formato de paleta), modo de índice de la CLUT, tamaño, swizzle, mips y LOD, filtro, wrap, función de textura, generación de UV, origen (RAM o VRAM) y texturas distintas. |
| `blend`, `logic_op`, `stencil`, `alpha_test`, `color_test`, `depth`, `depth_range`, `lighting`, `raster`, `color_mask` | Estado del pipeline de píxeles en cada draw. |
| `render_targets`, `depth_buffers`, `scissor` | Dirección, formato y stride de los targets, y rectángulos de scissor. |
| `vram_reads` | Cada lectura de textura o CLUT desde VRAM, con lo que había en esa dirección. |
| `clut_loads`, `transfers`, `cpu_vram_writes`, `display`, `edram_translation` | Cargas de CLUT, transferencias de bloque, escrituras de la CPU, framebuffer mostrado y valores de `EdramSetAddrTranslation`. |

## 4. Resultados

Once dumps que el usuario capturó el 2026-10-25 con PPSSPP v1.20.4. Todos son versión 6 (zstd) con
`gameID = ULUS10202`. Se leyeron en su sitio (`C:\Users\eduar\Documents\PPSSPP\PSP\SYSTEM\DUMP\`) con
`ge_usage.py --per-file`, que terminó con código 0. Todo lo de esta sección es **[V]** salvo que se
indique lo contrario.

| Dump | Escena | Draws | Vértices | Texturas distintas |
|---|---|---:|---:|---:|
| `ULUS10202_0001_kojima_productions` | Logo de Kojima Productions | 6 | 42 | 1 |
| `ULUS10202_0001_intro` | Video de introducción | **0** | 0 | 0 |
| `ULUS10202_0001-titulo` / `_0001_title_2` | Pantalla "Press Start" del título (la segunda unos segundos después) | **0** / **0** | 0 | 0 |
| `ULUS10202_0002-menu-juego` | Menú del juego | 87 | 346 | 4 |
| `ULUS10202_codec_1` / `_2` | Codec | 164 / 178 | 596 / 652 | 25 / 26 |
| `ULUS10202_cutscene_1` / `_2` | Cinemática in-engine | 816 / 640 | 12 872 / 17 171 | 111 / 128 |
| `ULUS10202_mission1_1` / `_2` | Primera misión (con el radar del HUD) | 1 505 / 541 | 35 105 / 16 076 | 137 / 66 |
| **Total** | | **3 937** | **82 860** | 498 |

En "Vértices", los splines cuentan sus puntos de control, no los vértices teselados.

### 4.1 El video de introducción y la pantalla de título no usan el GE

- **[V] El logo de Kojima Productions sí lo dibuja el GE**: 6 draws en modo through (5 `RECTANGLES`
  y 1 `TRIANGLE_FAN`), una textura CLUT8 de 512×512, blending (también con color fijo) y 2 clears,
  sobre un target 5551 en `0x044000` mientras se muestra `0x04000000`. Es la misma ruta que el menú.
- **[V] El video de introducción y la pantalla "Press Start" no se dibujan con el GE**: sus tres dumps
  tienen **0 draws**.
  - Solo traen un `INIT`, una CLUT, un bloque de 8 comandos (`FRAMEBUFPTR`, `FRAMEBUFWIDTH` y
    `FRAMEBUFPIXFORMAT` dos veces, `ZTESTENABLE` y `ZWRITEDISABLE`) y dos `DISPLAY`.
  - El framebuffer que se muestra es **8888**, con stride 512, y rota entre **tres buffers**:
    `0x04000000`, `0x04088000` y `0x04110000`, de `0x88000` cada uno. Por pares, la intro usa
    `0x088000`→`0x110000`, el primer título `0x110000`→`0x000000` y el segundo `0x000000`→`0x088000`.
  - El resto del juego usa **5551** con doble buffer.
  - La imagen del título, texto incluido, llega a la pantalla **sin pasar por el GE**.
- **[?]** Hipótesis, muy probable pero sin confirmar con un log HLE: son videos PMF que
  `sceMpegAvcDecode` decodifica directamente en los tres framebuffers 8888.
  - La etapa `title` importa `sceMpeg` (F03).
  - En el disco están `USRDIR/mpg/MPO_OP.pmf`, `MPO_OP_LONG.pmf`, `MPO_OP_LOOP.pmf` y
    `MPO_loop.pmf` (`docs/formats/disc-layout.md`). Los dos últimos son candidatos al bucle del
    título, con el "Press Start" incluido en el video.
- **Consecuencia para el plan** (aprobada por el usuario el 2026-10-25):
  - F11 incluye un stub mínimo de `sceMpeg`/`scePsmf`, que entrega frames en negro y el estado del
    stream, para pasar la intro y el "Press Start".
  - El decodificador real llega en F17.
  - Está en `docs/PLAN.md`.

### 4.2 Histograma total (las 3 937 draws)

**Primitivas y vértices**

| Draws | Vértices | Primitiva | Vertex type |
|---:|---:|---|---|
| 1 942 | 31 072 | `SPLINE` transform, siempre 4×4 puntos de control, `PATCHDIVISION` 6×6, tipo 3×3, `PATCHCULLENABLE` y `PATCHFACING` (normales invertidas) | `0x00011E`: tc 16, color 8888, pos 16 (16 B) |
| 1 435 | 49 384 | `TRIANGLE_STRIP` transform | `0x00015E` (964), `0x004342` (175), `0x00019E` (170), `0x00C342` (77), `0x00115E` (28), `0x01C342` (15), `0x005342` (6) |
| 442 | 1 672 | `TRIANGLE_FAN` through (HUD y 2D) | `0x80011E`: tc 16, color 8888, pos 16 |
| 92 | 408 | `RECTANGLES` through (16 son clears) | `0x80011C`: color 8888, pos 16, sin tc |
| 20 | 60 | `TRIANGLE_STRIP` through | `0x80011E` |
| 4 | 244 | `LINE_STRIP` through, solo en las misiones (el radar) | `0x800180`: pos float, sin color ni tc |
| 2 | 20 | `RECTANGLES` transform, uno por dump de misión | `0x00019E` |

- **No aparecen**: puntos, `LINES`, `TRIANGLES`, beziers ni morphing.
- **Índices**: 34 draws con índices de 16 bits.
- **Skinning**: 273 draws, solo en `TRIANGLE_STRIP`, con pesos de 8 bits ×2, ×4 u ×8. **Los splines
  nunca usan skinning.**

**Texturas** (3 841 draws texturizadas)

- Formato:
  - **CLUT4** en 2 901 draws y **CLUT8** en 940, siempre con **paleta 8888** e índice simple (sin
    shift, máscara ni offset).
  - No hay texturas de color directo, CLUT16/32 ni DXT.
- **Siempre swizzled**, siempre **un solo nivel** (sin mipmaps) y la UV sale siempre de las
  coordenadas del vértice (sin matriz de textura ni environment map).
- Filtro: LINEAR/LINEAR en 3 778 draws y NEAREST/NEAREST en 63.
- Wrap: repeat en 1 953 draws y clamp en 1 888.
- Función: **`MODULATE` rgba con doblado de color (×2)** en 3 821 draws y `MODULATE` rgba sin doblar
  en 20. No aparecen `REPLACE`, `DECAL`, `BLEND` ni `ADD`.
- Tamaños: de 8 a 512 texels por lado. Los más usados son 64×64 (842), 64×128 (712) y 128×128
  (432).
- CLUT: 1 778 cargas, **todas desde RAM**. Son de 64 bytes (16 colores, CLUT4) en 1 231 cargas y de
  1 024 bytes (256 colores, CLUT8) en 547.
- Origen: **3 162 draws texturizan desde VRAM** y 679 desde RAM (ver 4.3).

**Pipeline de píxeles**

| Función | Draws | Detalle |
|---|---:|---|
| Alpha test | 3 917 | Siempre `GREATER ref=0x00` (descarta alpha 0). |
| Blending | 968 | `ADD(SRCALPHA, INVSRCALPHA)` 810 · `ADD(SRCALPHA, FIX 0xFFFFFF)` 152 (aditivo) · `REVERSE_SUBTRACT(SRCALPHA, FIX 0xFFFFFF)` 6 |
| Depth test | 3 379 | Siempre **`GEQUAL`** (Z invertida). Escribe en 3 173 y no escribe en 206. |
| Depth range | todos | `MINZ = 0x2710` y `MAXZ = 0xC350`: **no es el rango por defecto**. |
| Depth clamp | 2 053 | **Los 1 942 splines lo usan**, además de 65 strips 3D y parte del HUD. |
| Fog | 2 906 | El 90 % de los splines y casi todo el 3D. |
| Dither | 3 207 | Solo en 3D. Importa porque el framebuffer es de 16 bits. |
| Iluminación | 285 | 3 luces direccionales con difusa y especular, en los modelos con skinning. |
| Culling | 931 | CW. |
| Shading | todas | Gouraud. |
| Clears | 16 | Dos por frame en cada escena con draws: uno completo (color, alpha/stencil y Z) y uno **solo de Z** (antes del HUD). |

- **No aparecen**: logic ops, stencil test, color test, máscaras de escritura, antialias, ecuaciones
  MIN/MAX/ABSDIFF, factores dobles y lectura del alpha de destino.

**Framebuffers**

- Color: **5551**, stride 512, con **doble buffer** en `0x000000` y `0x044000`. Se dibuja en uno
  mientras se muestra el otro.
- Depth: `0x088000`, stride 512.
- Scissor: siempre `0,0-479,271`.
- Esos tres buffers ocupan exactamente VRAM `0x000000-0x0CBFFF`.
- `EdramSetAddrTranslation`: ningún dump tiene registros `EDRAMTRANS`, así que el valor se quedó en
  el `0x400` por defecto durante los 11 frames.
- No hay transferencias de bloque ni escrituras de la CPU en VRAM (`MEMSET`/`MEMCPY`) en ninguno de
  los 11 frames. Esto no descarta la hipótesis del video: en PPSSPP v1.20.4 `sceMpeg` escribe con
  `GPUCommon::PerformWriteFormattedFromMemory`, que no avisa al grabador [S: `Core/HLE/sceMpeg.cpp`,
  `GPU/GPUCommon.cpp`].

**Presencia por escena**

| Función | Logo KP | Intro y título | Menú | Codec | Cinemática | Misión |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Display 8888 sin draws del GE (tres buffers) | | ✓ | | | | |
| 2D (through: fans, rects), swizzle, MODULATE ×2, alpha test, blending, 5551 y clears | ✓ | | ✓ | ✓ | ✓ | ✓ |
| CLUT4 | | | ✓ | ✓ | ✓ | ✓ |
| CLUT8 | ✓ | | | ✓ | ✓ | ✓ |
| Splines, depth test `GEQUAL`, depth clamp, fog, dither y texturas en VRAM | | | | | ✓ | ✓ |
| Skinning e iluminación | | | | | ✓ (`_2`) | ✓ |
| Índices de 16 bits | | | | | ✓ | ✓ (`_1`) |
| Líneas (`LINE_STRIP` through) y `RECTANGLES` transform | | | | | | ✓ |
| `REVERSE_SUBTRACT` | | | | | ✓ | ✓ |

### 4.3 Render-to-texture y lecturas de VRAM

- **[V] En las escenas capturadas no hay render-to-texture.**
  - Las 3 162 lecturas de textura desde VRAM usan 211 direcciones distintas, todas entre `0x0CC000`
    y `0x165900`, es decir, justo después de los dos framebuffers y el depth buffer.
  - **Todas son CLUT4 o CLUT8**, un formato en el que el GE no puede renderizar.
  - Ninguna tiene las marcas del grabador de "es un render target" o "se dibujó encima".
  - Es una **caché de texturas residente en VRAM**: los `.txp` se suben a VRAM y se samplean desde
    ahí.
- **[V]** No hay self-texturing, ni texturas desde el depth buffer o el framebuffer mostrado, ni CLUT
  desde VRAM, ni transferencias de VRAM a VRAM.
- **[?]** Cómo llegan las texturas a VRAM (memcpy de la CPU, `sceDmac` o transferencia del GE). No pasa
  en ninguno de los 11 frames, así que se hace antes, seguramente al cargar la etapa. Se ve con una
  traza en F09 o F15, o con un dump del primer frame de una etapa.

## 5. Lista priorizada para F11, F12, F14 y F15

Ordenada por lo que hace falta para cada hito y, dentro de cada fase, por número de draws.

**F11, para el logo, el título y el menú (M2 parcial)**

1. **El GE por software alcanza para el logo de KP y el menú, pero no para la intro ni el "Press
   Start"** (4.1).
   - Esas dos pantallas llegan como framebuffers 8888 que no escribe el GE (tres buffers que rotan).
   - Por eso F11 incluye un **stub de `sceMpeg`/`scePsmf`** que entrega frames en negro, informa bien
     el estado y el fin del stream y deja que el juego acepte Start.
   - El display tiene que presentar 8888 además de 5551.
2. 2D en modo through: `TRIANGLE_FAN` (`0x80011E`), `RECTANGLES` sin textura (`0x80011C`) y
   `TRIANGLE_STRIP`.
3. Texturas **CLUT4** (el menú) y **CLUT8** (el logo y el codec), con paleta 8888, **swizzle** y cargas de CLUT
   de 64 y 1 024 bytes desde RAM.
4. `MODULATE` rgba con **doblado de color**, alpha test `GREATER 0`, blending `ADD(SRCALPHA,
   INVSRCALPHA)` y aditivo `ADD(SRCALPHA, FIX 0xFFFFFF)`.
5. Salida **5551** con doble buffer (`0x000000` y `0x044000`), clear completo y clear solo de Z.
6. Filtro lineal, wrap repeat y clamp, y scissor de pantalla completa.

**F12, para las cinemáticas, la misión y el codec (el resto del histograma)**

1. **Splines** 4×4, división 6×6, tipo 3×3, con patch culling y normales invertidas, sin skinning ni
   iluminación. Son el **49 % de las draws** (el suelo y el escenario).
2. Profundidad: test `GEQUAL`, rango `0x2710-0xC350` y **depth clamp** (todos los splines).
3. Fog y dither sobre 5551.
4. Skinning con pesos de 8 bits (×2, ×4 y ×8) y 3 luces direccionales con difusa y especular.
5. `REVERSE_SUBTRACT(SRCALPHA, FIX 0xFFFFFF)`.
6. **Líneas** `LINE_STRIP` through con posición float (el radar). La referencia es el `DrawLine`
   de softgpu; `[CenteredLines]` no lista MPO en PPSSPP v1.20.4 y solo lo leen los renderers por hardware (F12 [V]).
7. Casos raros: índices de 16 bits, `RECTANGLES` en modo transform, `MODULATE` sin doblado y filtro
   NEAREST.
8. Sacar de F12 lo que no aparece: mipmaps, DXT, CLUT16/32, texturas de color directo, logic ops,
   stencil, color test, máscaras, beziers, puntos, morphing, matriz de textura y environment map. No
   se implementan antes de que una captura los muestre. Si aparece algo nuevo, el intérprete de
   comandos lo registra de forma visible.

**F14, Vulkan base**

1. Hay poco estado distinto:
   - 11 vtypes;
   - 2 formatos de textura (CLUT4 y CLUT8 con paleta 8888, que se resuelven en el shader o al
     subir);
   - 3 modos de blending más "sin blending";
   - 3 modos de profundidad;
   - fog, alpha test y doblado de color en el shader.
   Una caché de pipelines pequeña alcanza.
2. Todo el blending visto **cabe en el blending fijo de Vulkan**: `FIX 0xFFFFFF` es `ONE`, y
   `REVERSE_SUBTRACT` existe. No hace falta blending por shader para estas escenas.
3. Depth clamp con `depthClamp` y el rango de viewport `0x2710-0xC350`, y comparación
   `GREATER_OR_EQUAL`.
4. Destino 5551 con dither. Hay que decidir si se renderiza en un formato de 16 bits o en RGBA8 con
   cuantización y dither al presentar (afecta a F15 y a la comparación con el renderer por software).
5. Splines: teselado en CPU (como el software) o en GPU. Son el 49 % de las draws.

**F15, framebuffers y efectos**

1. **No hay render-to-texture en las escenas capturadas**, así que la prioridad baja frente a lo que
   suponía `RESEARCH.md`. Antes de implementarlo hace falta una captura que lo muestre.
2. Caché de texturas residentes en VRAM (`0x0CC000` en adelante): hay que invalidarla cuando la CPU,
   `sceDmac` o el GE escriban en VRAM.
3. Los tres framebuffers 8888 de la intro y del título, escritos fuera del GE: el renderer Vulkan
   tiene que poder presentar un framebuffer de VRAM que no dibujó él, con otro formato y en otra
   dirección que los del juego.
4. Las líneas del radar contra el `DrawLine` de `mpo_ge`. `[CenteredLines]` no aplica: no lista MPO
   en v1.20.4 (F12 [V]).

### 5.1 Las hipótesis previas contra los dumps

| Hipótesis (fuente) | Resultado |
|---|---|
| Líneas para el radar y el mapa, `[CenteredLines]` ([S] `compat.ini`, [#20142](https://github.com/hrydgard/ppsspp/issues/20142)) | **[V] Hay líneas**: 4 `LINE_STRIP` through en las misiones. **[?]** El efecto de `[CenteredLines]` necesita una comparación visual, y falta el mapa a pantalla completa. |
| Splines y beziers **con skinning** para el suelo ([S] [#1884](https://github.com/hrydgard/ppsspp/issues/1884), [#3343](https://github.com/hrydgard/ppsspp/issues/3343)) | **[V] Hay splines** (1 942, el 49 %). **[V] No se vieron ni splines con skinning ni beziers**: los splines usan `0x00011E`, sin pesos, y el skinning solo aparece en triangle strips. El "vtype 12000780" del issue no aparece. |
| Clamp de profundidad y guard-band ([S] [#12348](https://github.com/hrydgard/ppsspp/issues/12348)) | **[V] Clamp**: 2 053 draws, todos los splines, con rango `0x2710-0xC350`. **[?]** El guard-band no se puede medir con un dump; se ve comparando imágenes. |
| Render-to-texture y textura desde el target actual ([S] [#2438](https://github.com/hrydgard/ppsspp/issues/2438), [#1202](https://github.com/hrydgard/ppsspp/issues/1202)) | **[V] No se vio** en ninguna escena capturada: lo que se lee de VRAM es una caché de texturas CLUT. **[?]** Puede aparecer en otras escenas (pausa, efectos, transiciones). |
| `sceGeEdramSetAddrTranslation` ([S] `RESEARCH.md` §4) | **[V]** El valor se quedó en `0x400` (el de por defecto) en los 11 frames. **[?]** Si el juego llama a la función (con ese valor u otro) en otro momento lo muestra el histograma HLE de F08. |
| CLUT de 4 y 8 bits con paleta RGBA8888 y swizzle (`.txp`) ([S] Noesis) | **[V]** El 100 % de las draws texturizadas. |
| `REPLACE` o `MODULATE` con doblado de color, y blending (flags de `.txp`) ([S] Noesis) | **[V]** `MODULATE` ×2 en 3 821 draws y `MODULATE` en 20. **[V] No se vio `REPLACE`.** Blending: 3 modos (4.2). |

## 6. Guía de captura (para el usuario)

Con PPSSPP **v1.20.4** para Windows, en inglés (la configuración del usuario tiene `Language = en_US`):

1. Abrir PPSSPP **en ventana**, porque en pantalla completa la barra de menús no aparece
   (Alt+Enter alterna).
2. Cargar `ULUS-10202` como siempre. **No hace falta tocar la configuración.** El dump guarda los
   comandos del GE, no la imagen del renderer, así que da igual usar Vulkan, OpenGL o software. Basta
   con dejar `Skip buffer effects` desactivado y sin frameskip, que es lo que ya tiene el usuario.
3. Llegar a la escena con el juego **corriendo**, no en la pantalla de pausa de PPSSPP. La grabación
   empieza en el siguiente flip y termina en el que sigue.
4. Menú **Debug › Save frame dump**.
   - Después de un frame, PPSSPP abre el Explorador con el archivo seleccionado:
     `C:\Users\eduar\Documents\PPSSPP\PSP\SYSTEM\DUMP\ULUS10202_0001.ppdmp`. El número sube solo
     (`_0002`, `_0003`…) y nunca sobrescribe.
   - Alternativas: **Actions › Record Next Frame** en la ventana **Debug › GE Debugger…**, o
     **Create frame dump** en el DevMenu (si hay una tecla asignada a `DevMenu`).
5. **Renombrar** cada archivo al terminar, con el nombre de la escena, por ejemplo
   `ULUS10202_title_1.ppdmp`. La herramienta acepta cualquier nombre que termine en `.ppdmp`.
6. Escenas. Una de cada como mínimo, mejor dos de momentos distintos:
   - `title`: el logo o "press start", y el menú del título.
   - `menu`: el menú del juego (gestión de la unidad, selección de misión).
   - `mission1`: la primera misión, al aire libre, con el suelo cerca de la cámara y el HUD con el
     radar a la vista. Mejor si sale algún soldado (skinning).
   - `codec`: una conversación por codec.
   - `radar`: la pantalla de mapa o el radar en grande (líneas).
   - `cutscene`: una cinemática in-engine (no un video PMF).
   - Opcionales: el menú de pausa, una zona oscura o con niebla, efectos (explosión, chaff o humo) y
     la pantalla de reclutamiento por Wi-Fi (AP Scan).
7. **Los dumps no se suben al repo**: traen texturas y vértices del juego. Pueden quedarse en `DUMP\`
   o copiarse a `dumps/` en la raíz del checkout, que está en `.gitignore`.
8. Después se ejecuta `python3 -I profiles/mpo/tools/ge_usage.py --format markdown --per-file <carpeta>`
   y se completan las secciones 4 y 5 de esta página.

### Capturas que faltan

Ninguna bloquea F11. Sirven para confirmar las [?] de la sección 5.1:

- **El mapa o el radar a pantalla completa**, para ver las líneas a otra escala y comparar
  `DrawLine`. Las misiones solo traen el radar del HUD (4 `LINE_STRIP`).
- **El log HLE de PPSSPP en la intro y en el "Press Start"**, que confirma que el video es PMF y qué
  archivo se reproduce. Para eso hay que:
  - activar "Enable debug logging" y subir los canales `ME` y `HLE` en Settings › Tools › Developer
    tools › Logging channels;
  - abrir Debug › Log console;
  - anotar las llamadas `sceMpeg*` y `scePsmf*` y el `sceIoOpen` del `.pmf`.
- **Escenas candidatas a render-to-texture**, que en estas capturas no apareció: el menú de pausa
  sobre el juego, las transiciones entre áreas, efectos de pantalla completa (stun, chaff, granada
  aturdidora) y la cámara o la mira si existen.
- **El primer frame al entrar a una etapa**, para ver cómo se suben las texturas a la caché de VRAM
  (`MEMCPY` o transferencias en el dump).
