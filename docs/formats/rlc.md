# Formato `.rlc` (relocaciones de etapa) y su contenedor `_zar`

Este documento describe de dónde sale la tabla `.rlc` de cada etapa, cómo es por dentro y qué hace.
Solo hay prosa, números y nombres, nada de bytes del juego. Todo se midió sobre `ULUS-10202` 1.02
(F04, 2026-10-25), con los 106 `stage/<s>/_zar` y `stage/<s>/<s>.prx` validados contra
`supported.toml` y el `mgp_main.elf` descifrado en F02.

Etiquetas: **[V]** verificado en el disco o el binario, **[S]** de una fuente externa, **[?]** sin
verificar.

## Resumen

- **El `.rlc` no es un archivo suelto** [V]. Cada etapa lo trae **dentro de su `_zar`**: el `_zar` es
  un DAR comprimido con zlib, y su última entrada siempre es `<s>.rlc`. Así es en las 106 etapas.
- **El formato es el de las relocaciones PRX clásicas** (`PT_PRXRELOC`, `0x700000A0`) [V]: un array
  de entradas de 8 bytes `(u32 r_offset, u32 r_info)`, sin cabecera.
- **La diferencia está en las bases** [V]. `ofs_base` es un segmento **de la etapa** (la palabra que
  se parchea) y `addr_base` es un segmento **de `mgp_main`** (la dirección que se suma). O sea, el
  `.rlc` enlaza las referencias de la etapa hacia `mgp_main`: llamadas, punteros a funciones y
  direcciones de variables globales.
- Las etapas **no importan nada de `mgp_main` por NID** [V]. Sus únicos imports son librerías de Sony
  (`ThreadManForUser`, `sceUtility`, `sceNet*`, `sceMpeg`…). Todo el enlace con `mgp_main` pasa por
  el `.rlc`.
- Quién lo aplica y en qué momento todavía falta confirmarlo en Ghidra (F04, parte 2) [?]. Ver
  "Lo que falta confirmar".

## De dónde sale

- No hay ningún `.rlc` suelto en el disco (F01) [V].
- Las strings `%sstage/%s/%s.rlc` y `rlc: preloaded %s.rlc for %s - %d bytes, %d entries` que cita
  `RESEARCH.md` §4 **no son del juego** [V]:
  - No aparecen en `mgp_main`, `kjfs.prx`, `sound2.prx`, `zlibdec.prx` ni en ningún PRX de etapa.
  - Están en `savempo_plugin.prx` (offsets `0xF56C` y `0xF5A0`), dentro de `assets/savempo.zip` de
    [snakeswiss/MPO-Guide](https://github.com/snakeswiss/MPO-Guide). Es un plugin homebrew que, en
    MPO+, sirve las etapas desde la Memory Stick (`stageredirect: …`) y por eso precarga el `.rlc`
    como archivo suelto.
- En `mgp_main` la cadena `rlc` aparece solo en la tabla de extensiones del sistema de recursos [V]
  (offsets de archivo `0x1E54B0` y `0x1E5624`, dos copias). Esa tabla tiene `bin`, `gcx`, `mdp`,
  `txp`, `prx`, `dar`, `qar`, `data.cnf`… Concuerda con el tipo `rlc` (`0x32`) del hash de recursos
  de [MGS-MPO-Noesis](https://github.com/waspothegreat/MGS-MPO-Noesis) [S].
- La entrada `<s>.rlc` de cada `_zar` es la que usa el juego [V]: `data.cnf` la nombra y su
  contenido encaja exactamente con el PRX de la misma etapa (ver "Evidencia").

## `stage/<s>/_zar` [V]

| Offset | Tipo | Contenido |
|---|---|---|
| `0x0` | `u32` | Tamaño del DAR descomprimido |
| `0x4` | — | Un único stream zlib (cabecera `78 9C`), sin bytes después |

- Los 106 coinciden exactamente con su tamaño declarado. El más grande descomprime a 6 889 321 bytes
  (`gsmenu`) y el más chico a 27 441 (`gsclimenu`).
- No está cifrado.

## DAR [V]

Un DAR es un `u32` con el número de entradas y, después, para cada entrada:

1. El nombre, en ASCII y terminado en NUL.
2. Relleno con ceros hasta alinear a 4.
3. Un `u32` con el tamaño (`size`).
4. Relleno hasta alinear a 16 (desde el inicio del DAR).
5. `size` bytes de datos.
6. **Un byte NUL** después de los datos. La siguiente entrada empieza justo ahí, sin alinear.

- El último NUL termina el archivo: no hay bytes de más.
- Esto difiere un poco de la descripción de `RESEARCH.md` §4 ("datos alineados a 16"), porque falta
  el NUL final. Sin ese byte, el parser se desfasa a partir de la segunda entrada.
- Se verificó en los 106 DAR de primer nivel y en los 105 DAR anidados (`cache.dar` y
  `resident.dar`, con 6 819 entradas en total).

## Contenido del DAR de una etapa [V]

- Siempre empieza con `data.cnf` y termina con `<s>.rlc`. En medio, según la etapa, aparecen
  `cache.qar`/`cache.dar` (o `resident.qar`/`resident.dar` en las 3 `r_*`), `scenerio.gcx`, y
  `*.geom`, `*.zon`, `*.nav` o `minifont.fcx`.
- **`data.cnf`** es texto, una línea por elemento:
  - Las líneas que empiezan con `.` son directivas: `.nocache`, `.cache` y `.resident`, y
    `.slot <nombre> <n> <bytes>`/`.endslot` (solo en las `r_*`).
  - El resto son nombres de archivo. **Coinciden, en orden, con las entradas del DAR que siguen a
    `data.cnf`** en las 106 etapas. Los `.qar` llevan el prefijo `@`.
  - La última línea es siempre `<s>.rlc`.
  - El significado de las directivas no se verificó [?]. Lo más probable es que digan en qué
    región de memoria (caché o residente) se carga cada recurso.

## Entradas del `.rlc` [V]

Es un array de entradas de 8 bytes, little-endian, sin cabecera ni terminador. El tamaño es múltiplo
de 8 en las 106 etapas.

| Offset | Tipo | Campo |
|---|---|---|
| `0x0` | `u32` | `r_offset`: offset de la palabra a parchear, relativo al inicio del segmento `ofs_base` de la etapa |
| `0x4` | `u32` | `r_info` = `type \| ofs_base << 8 \| addr_base << 16` (el byte alto siempre es 0) |

| `type` | Nombre | `ofs_base` (etapa) | `addr_base` (`mgp_main`) | Entradas (106 etapas) | Uso |
|---:|---|---:|---:|---:|---|
| 4 | `R_MIPS_26` | 0 (código) | 0 (código) | 338 314 | `jal`/`j` a una función de `mgp_main` |
| 5 | `R_MIPS_HI16` | 0 | 1 (datos) | 96 534 | `lui` de una dirección de datos de `mgp_main` |
| 6 | `R_MIPS_LO16` | 0 | 1 | 126 254 | `addiu`/`lw`/`sw`… de esa dirección |
| 5 | `R_MIPS_HI16` | 0 | 0 | 147 | `lui` de una dirección de código de `mgp_main` |
| 6 | `R_MIPS_LO16` | 0 | 0 | 153 | parte baja de esa dirección |
| 2 | `R_MIPS_32` | 1 (datos) | 0 | 8 417 | puntero a una función de `mgp_main` guardado en datos de la etapa |

- En total son 569 819 entradas. La tabla más grande tiene 8 128 (`st09`) y la más chica 34
  (`r_main02`).
- Todos los `r_offset` están alineados a 4 y caen dentro de su segmento.
- Las entradas `R_MIPS_26` van ordenadas por offset. Los pares `HI16`/`LO16` van intercalados entre
  ellas y los `R_MIPS_32` van al final.
- **Cada `R_MIPS_HI16` va seguido de su `R_MIPS_LO16`** (94 676 casos) **o de otro `R_MIPS_HI16`**
  (2 005 casos, que comparten el mismo `LO16`). Nunca hay otro tipo entre los dos.

## Semántica

Es la del cargador de PRX de PPSSPP, `ElfReader::LoadRelocations` (`Core/ELF/ElfReader.cpp` @
`v1.20.4`). La única diferencia es de dónde sale cada base:

- `patch = base_etapa + vaddr(segmento ofs_base de la etapa) + r_offset`.
- `relocate_to = 0x08804000 + vaddr(segmento addr_base de mgp_main)`. La base de `mgp_main` se
  verificó en F02 [V]. Su segmento 0 tiene vaddr `0x0` y el segmento 1, `0x1EED40`.
- Primero se leen todas las palabras originales y después se escriben los resultados:
  - `R_MIPS_32`: `w += relocate_to`.
  - `R_MIPS_26`: `w = (w & 0xFC000000) | (((w & 0x03FFFFFF) + (relocate_to >> 2)) & 0x03FFFFFF)`.
  - `R_MIPS_HI16`: se busca el siguiente `LO16`, saltando otros `HI16`. Con
    `addr = (w_hi.imm << 16) + (s16)w_lo.imm + relocate_to`, la parte alta queda en
    `(addr - (s16)(addr & 0xFFFF)) >> 16`, que compensa el acarreo cuando la parte baja es negativa.
  - `R_MIPS_LO16`: `imm = (imm + relocate_to) & 0xFFFF`.
- Los addends son **relativos al inicio del segmento** `addr_base`, no al inicio de la imagen de
  `mgp_main`. Todos los pares `HI16`/`LO16` con `addr_base` 1 dan un valor menor que `0x108A80`, el
  `memsz` del segmento 1.
- **Cuidado: el orden importa poco, pero no del todo.** Como el `LO16` siempre va después de su
  `HI16`, aplicar las entradas en orden y en el mismo lugar da el mismo resultado que leer primero
  todas las palabras originales. Las relocaciones propias del PRX y las del `.rlc` nunca tocan la
  misma palabra (ver "Evidencia"), así que tampoco importa cuál de los dos se aplica primero.
- El código de las etapas y el de `mgp_main` **no usan `$gp`**: hay 0 instrucciones con base `$gp`
  en el `.text` de las 91 etapas distintas y en el de `mgp_main` [V], y `.sbss` mide 0 bytes. Está
  compilado con `-G0`. Por eso una etapa puede llamar directo a `mgp_main` (y al revés) sin cambiar
  `$gp`, aunque cada módulo declare el suyo en `sceModuleInfo`.

## Evidencia [V]

Se midió sobre las 106 etapas con los scripts de `profiles/mpo/tools/` (ver abajo).

1. **El `.rlc` complementa al PRX, no lo repite.** Ninguna de las 569 819 entradas parchea una
   palabra que también relocalice el `PT_PRXRELOC` de la etapa (2 470 676 entradas).
2. **Los saltos van a funciones de `mgp_main`.**
   - Los 338 314 `R_MIPS_26` son `jal` o `j`, y todos sus destinos caen dentro del segmento de código
     de `mgp_main` (`0x1EED14` bytes).
   - El 89.6 % (303 240) coincide con destinos de `jal`/`j` del propio `mgp_main` (según su
     `PT_PRXRELOC`).
   - El 61.4 % de los destinos empieza con `addiu sp, sp, -n`, el prólogo típico de una función. El
     resto pueden ser funciones hoja, que no reservan pila.
   - Lo mismo pasa con los `R_MIPS_32`: el 88.2 % apunta a un prólogo.
3. **Cobertura total de los `jal`.** De las 1 151 358 palabras con opcode `jal` en el `.text` de las
   etapas, todas menos 33 están cubiertas por el `PT_PRXRELOC` (llamadas internas o a stubs) o por el
   `.rlc` (llamadas a `mgp_main`). Las 33 que quedan son, con toda probabilidad, datos dentro de
   `.text`.
4. **Los datos caen dentro de `mgp_main`.** Los 96 681 `R_MIPS_HI16` parchean un `lui`. Con su
   `LO16`, ellos y los 8 417 `R_MIPS_32` dan valores dentro del segmento `addr_base` de `mgp_main`.
5. **Mismo PRX, mismo `.rlc`.** Hay 11 grupos de etapas con el PRX idéntico (por ejemplo,
   `st08 = st0801 = st0802`) y en todos ellos el `.rlc` también es idéntico.
6. **Interfaz entre la etapa y `mgp_main`.** Es igual en las 106 etapas:
   - La función de `.text+0x0` (0x6C bytes) tiene las mismas 4 primeras entradas del `.rlc`, con las
     mismas palabras:
     - llama a `mgp_main+0x1CBE80` y a `mgp_main+0x1CB880`;
     - escribe en el global `mgp_main` segmento 1 `+0x69EA0`.
   - `module_start` (`e_entry 0x6C`) **no** toca `mgp_main`. Lee un puntero de su `argp` y escribe
     ahí tres direcciones de la propia etapa: dos a `.text+0x0` y una al inicio de su segmento de
     datos.
   - Hipótesis [?]: `mgp_main` pasa una estructura en `sceKernelStartModule(argp)`, la etapa la
     rellena y `mgp_main` entra a la etapa por `.text+0x0` después de aplicar el `.rlc`.
   - En los exports, las etapas solo tienen `syslib` (`module_start`, `module_stop` y `module_info`).

## Carga observada

- La lista de módulos de PPSSPP (F02) muestra `mgp_stage` en `0x08B8F900` con tamaño `0x3C600` [V].
- `0x3C600` es exactamente la imagen de **`title`** (`0x3C5C0` redondeado a `0x100`), y ninguna otra
  etapa tiene ese tamaño. Esa captura era, entonces, de la pantalla de título [V].
- Falta ver si todas las etapas se cargan en la misma base [?].
- `mgp_main` importa de `ModuleMgrForUser`:
  - `sceKernelLoadModuleByID`, no `sceKernelLoadModule`, así que carga la etapa desde un descriptor
    ya abierto;
  - `sceKernelStartModule`, `sceKernelStopModule` y `sceKernelUnloadModule`;
  - `sceKernelGetModuleId`, `sceKernelGetModuleIdByAddress` y
    `sceKernelStopUnloadSelfModuleWithStatus`.
- De `UtilsForUser` importa `sceKernelDcacheWritebackAll`, `…WritebackRange` y
  `…WritebackInvalidateRange`, pero **ningún `sceKernelIcacheInvalidate*`** [V].

## Herramientas

Todas están en `profiles/mpo/tools/` y se corren con `python -I`. Validan cada tamaño y offset antes
de usarlo.

| Script | Para qué |
|---|---|
| `zar.py` | Descomprimir un `_zar`, listar el DAR y extraer una entrada (`list`, `extract`) |
| `rlc.py` | Validar un `.rlc` contra su etapa y `mgp_main` (`check`, solo cuenta), y generar la imagen relocalizada de una etapa en una base dada (`relocate`; la salida es del juego y va a `analysis/`) |
| `psp_prx.py` | Leer un PRX: segmentos, secciones, `sceModuleInfo`, imports, exports y `PT_PRXRELOC` |
| `stage_inventory.py` | El inventario de `docs/analysis/stages.md` |

- Los tests (`*_test.py`) usan solo datos sintéticos y corren en `ctest`: `mpo_psp_prx_test`,
  `mpo_zar_test` y `mpo_rlc_test`.
- `mpo --install` extrae los `stage/<s>/_zar` desde F04 (el patrón `*_zar` de `extract` en
  `supported.toml`). Un `game/` instalado antes de F04 hay que reinstalarlo para tenerlos.

```bash
python -I profiles/mpo/tools/rlc.py check --stage game/disc/PSP_GAME/USRDIR/stage/st01/st01.prx \
    --zar game/disc/PSP_GAME/USRDIR/stage/st01/_zar --main game/elf/mgp_main.elf
python -I profiles/mpo/tools/rlc.py relocate --stage …/title/title.prx --zar …/title/_zar \
    --main game/elf/mgp_main.elf --stage-base 0x08B8F900 --output analysis/title_08B8F900.bin
```

## Lo que falta confirmar (Ghidra, F04 parte 2)

1. **La cadena de carga**: quién abre `stage/<s>/<s>.prx` (`kjfs` o `sceIoOpen` en `mgp_main`), con
   qué `flags` y `SceKernelLMOption` se llama a `sceKernelLoadModuleByID` (partición y posición, que
   deciden la base) y qué recibe `sceKernelStartModule` en `argp`.
2. **Dónde se aplica el `.rlc`**:
   - la función que recorre las entradas de 8 bytes;
   - cómo obtiene las bases de los segmentos de la etapa y de `mgp_main`;
   - si sigue la misma semántica de `HI16`/`LO16`;
   - si lo hace antes o después de `module_start` y antes de entrar por `.text+0x0`.
3. **Cómo llega el `.rlc` a memoria**: el cargador de recursos de `data.cnf`, la tabla de
   extensiones, si se libera después de aplicarlo y qué significan `.cache`, `.resident` y `.slot`.
4. **La descarga**: `sceKernelStopModule` y `UnloadModule`, y qué punteros a la etapa limpia
   `mgp_main`.
5. **Si la base es fija**: con PPSSPP, comparar la base de `mgp_stage` en el título, en una misión
   (`st01`) y en un menú. También ver si una `r_*` (residente) convive cargada con otra etapa.
6. **Comparar byte a byte**: un volcado de memoria de PPSSPP de `0x08B8F900`–`0x08BCBEC0` en el
   título, contra `rlc.py relocate` de `title`, sin contar `.bss` ni los datos que cambian en
   ejecución.
