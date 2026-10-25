# Uso del GE en MPO — inventario con frame dumps de PPSSPP (F05)

Qué funciones del GE usa **MGS: Portable Ops** (`ULUS-10202`), medido sobre *frame dumps* (`.ppdmp`)
de PPSSPP. Esta página es la entrada de F11, F12, F14 y F15. Solo lleva números y nombres: los dumps
traen texturas y vértices del juego, así que **nunca entran al repo** (`CLAUDE.md` §1.1).

Estado (2026-10-25): **la herramienta está lista y probada, pero todavía no hay dumps de MPO.** La
sección 4 (resultados) está vacía y se llena cuando el usuario capture los dumps (sección 6).

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

**Pendiente: todavía no hay dumps de MPO.** El 2026-10-25 en
`C:\Users\eduar\Documents\PPSSPP\PSP\SYSTEM\DUMP\` solo estaba `ULUS10202_EBOOT.BIN`, el volcado de
F02.

| Escena | Dumps | Estado |
|---|---|---|
| Título (logo y menú del título) | — | pendiente |
| Menú del juego | — | pendiente |
| Primera misión (gameplay con HUD y radar) | — | pendiente |
| Codec | — | pendiente |
| Radar y mapa | — | pendiente |
| Cinemática in-engine | — | pendiente |

### 4.1 Histograma total

*(Se pega aquí la salida de `--format markdown`.)*

### 4.2 Render-to-texture y lecturas de VRAM

*(Se pega aquí la sección `vram_reads`, junto con `transfers` y `cpu_vram_writes`.)*

## 5. Lista priorizada para F11, F12, F14 y F15

**Se completa con los resultados de la sección 4.** Criterios:

1. Lo que aparece en el **título o el menú** va a **F11**, porque es lo mínimo para M2.
2. Lo que aparece en la **primera misión, el codec, el radar o las cinemáticas** va a **F12**. El
   criterio de F12 es cubrir el 100 % de la sección `features`.
3. En F14 se ordena por número de draws: lo más usado se resuelve primero en el pipeline fijo de
   Vulkan, y lo que el hardware no puede hacer (logic ops sin `logicOp`, factores dobles, MIN/MAX con
   factores, ABSDIFF) pasa al shader.
4. Lo que salga en `vram_reads`, `transfers` y `cpu_vram_writes` va a **F15**.

Hipótesis previas, de fuentes externas. **Ninguna está medida todavía**; los dumps las confirman o
las descartan:

| Función | Fase | Fuente |
|---|---|---|
| Líneas (radar y mapa), con el comportamiento de `[CenteredLines]` | F12, F15 | [S] `compat.ini` y [#20142](https://github.com/hrydgard/ppsspp/issues/20142) |
| Splines y beziers con skinning (suelo cerca de la cámara, "vtype 12000780, bones: 1") | F12, F15 | [S] [#1884](https://github.com/hrydgard/ppsspp/issues/1884), [#3343](https://github.com/hrydgard/ppsspp/issues/3343) |
| Clamp de profundidad y guard-band | F12, F15 | [S] [#12348](https://github.com/hrydgard/ppsspp/issues/12348) |
| Render-to-texture y textura desde el target actual | F12, F15 | [S] [#2438](https://github.com/hrydgard/ppsspp/issues/2438), [#1202](https://github.com/hrydgard/ppsspp/issues/1202) |
| `sceGeEdramSetAddrTranslation` | F11 | [S] `RESEARCH.md` §4 |
| CLUT de 4 y 8 bits con paleta RGBA8888 y swizzle (`.txp`) | F11, F12 | [S] `waspothegreat/MGS-MPO-Noesis` |
| Funciones de textura REPLACE y MODULATE con doblado de color, y blending (flags de `.txp`) | F12 | [S] `waspothegreat/MGS-MPO-Noesis` |

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
