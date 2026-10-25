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
- **Lo aplica `mgp_main`** (`0x08805C0C`), justo después de `sceKernelStartModule` de la etapa, y
  después invalida la caché de instrucciones [V, Ghidra]. Su semántica da el mismo resultado que
  `rlc.py` en las 106 tablas, y un savestate de PPSSPP con `st02` cargada coincide byte a byte con
  `rlc.py relocate` en todo `.text` [V]. Ver "Cómo lo aplica el juego".

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
   - Confirmado en Ghidra: `mgp_main` pasa esa estructura en `sceKernelStartModule(argp)` y usa sus
     dos primeros punteros como bases de los segmentos de la etapa al aplicar el `.rlc`. Después
     entra por el tercero (ver "Cómo lo aplica el juego").
   - En los exports, las etapas solo tienen `syslib` (`module_start`, `module_stop` y `module_info`).

## Cómo lo aplica el juego [V]

Se vio en Ghidra, sobre `mgp_main` en `0x08804000` (el proyecto de F03) y sobre `title` y `st01`,
importadas ya enlazadas en `0x08B8F900`. Las direcciones son de `mgp_main`, salvo que se diga otra
cosa.

### Cadena de carga

1. **`data.cnf`**. El intérprete (`0x0883D40C`, dentro de la máquina de estados `0x0883D6E4`) lee
   cada línea:
   - las directivas `.n…` (nocache), `.c…` (cache) y `.r…` (resident) fijan el modo del recurso, y
     `.s…` (slot) va a `0x0883D234`;
   - cada nombre (sin el `@` o el `*` de delante) se abre con `0x08804B54`, que arma `"%s%s"` con el
     prefijo de la etapa y llama a `kjfs_93E9297F` (la apertura de `kjfs`).
2. **`kjfs` sirve los archivos de la etapa desde `_zar`**.
   - Su tabla de directorios marca los que tienen `_zar` (o `_arc`). Al abrir un archivo de uno de
     ellos, cambia el nombre por el del archivo comprimido (`kjfs` `0x08B75C08`).
   - Lo descomprime por streaming con `zlibdec`, en un FPL `KFS_Arc` de 1 MiB + `0xB298` bytes
     (`kjfs` `0x08B78050`).
   - El acceso fino no se siguió [?].
3. **Despacho por tipo**.
   - Los `.dar` y `.qar` se recorren entrada por entrada (estados 3 y 4 de `0x0883D6E4`), con el
     mismo layout de DAR de arriba y el QAR alineado a `0x80`.
   - Cada recurso va al handler de su tipo (el byte alto del hash de recursos).
   - El tipo `0x32` (`rlc`) lo registra `0x0880495C` con `0x0883FBFC(0x32, 0x08805E04, 0)`.
   - El handler `0x08805E04` solo guarda el puntero y el tamaño de la tabla en
     `0x08A00154`/`0x08A00158`.
4. **El PRX**. `0x08804B54` abre `<s>.prx` con `kjfs` y `0x08804CC4`:
   - si hay una etapa cargada (`0x08A00150`), hace `sceKernelStopModule(id, 0, NULL, …)` y
     `sceKernelUnloadModule(id)`. **Solo hay una etapa cargada a la vez**;
   - llama a `sceKernelLoadModuleByID(fd, 0, NULL)`, **sin flags y sin `SceKernelLMOption`**: la
     partición y la posición son las de por defecto y las elige el kernel. Si falla, reintenta cada
     segundo.
5. **Enlace y arranque** (`0x08805E1C`):
   1. Llama a `sceKernelStartModule(id, 4, &p)`, con `p = 0x08A0015C`. El `module_start` de la etapa
      escribe ahí tres punteros:
      - `[0]` = `.text+0x0`, que es la base del segmento 0;
      - `[1]` = la base del segmento 1 (datos);
      - `[2]` = la entrada, que también es `.text+0x0`.
   2. Llama a `0x08805C0C(bases_main, p, tabla, tamaño >> 3)`:
      - `bases_main = {0x08804000, 0x089F2D40}` son los segmentos 0 y 1 de `mgp_main`;
      - `p` son las bases de la etapa.
   3. Hace `sceKernelDcacheWritebackAll()` e invalida toda la icache: `cache 0x04` (por índice)
      cada 64 bytes durante 16 KiB, desde la base de la etapa.
   4. Llama a la entrada (`[2]`). En las 106 etapas esa función:
      - guarda en el global `0x08A5CBE0` la base de los datos de la etapa;
      - llama al `printf` de depuración (`0x089CFE80`, que se anula al arrancar);
      - limpia el `.bss` con `memset` (`0x089CF880`).
   5. Libera el buffer del `.rlc` con `0x0883E80C`, que baja el contador de referencias del recurso.
6. **La entrada se vuelve a llamar** desde `0x088063C8` (reinicio de la escena), a través de
   `0x088079C8`, que además cuenta las entradas de la tabla de la etapa.
7. **La descarga no es un paso aparte**: es el `StopModule`/`UnloadModule` de la carga siguiente.
   - `0x08807968` vuelve a apuntar `0x08A5CBE0` a una tabla vacía de `mgp_main` (`0x08AFB7C0`).
   - `sceKernelStopUnloadSelfModuleWithStatus` no tiene que ver con las etapas: lo usan `mgp_main`
     (`0x089DA360`, para su propia salida) y `kjfs` (`0x08B78518`).

### La tabla de la etapa

- Los datos de cada etapa empiezan con una tabla de pares `(u32 clave, u32 puntero)`, terminada en
  un puntero 0 y **ordenada por clave** en las 106 etapas.
- `0x088079F4(clave)` la busca (con `0x08842018`) y devuelve el puntero. Así llega `mgp_main` al
  código de la etapa: por punteros a función, o sea, por `jalr`.
- Tiene entre 3 y 232 entradas (mediana 192; `title` 79 y `st01` 219), 16 531 en total:
  - 8 114 apuntan a la etapa, con `R_MIPS_32` propios;
  - **8 417 apuntan a `mgp_main`, y son exactamente los 8 417 `R_MIPS_32` del `.rlc`**. Es decir,
    todos los `R_MIPS_32` del `.rlc` están en esta tabla.

### `0x08805C0C` contra `rlc.py`

| Aspecto | `0x08805C0C` | `rlc.py` (`apply`) | ¿Mismo resultado en las 106 tablas? |
|---|---|---|---|
| Campos | `type = info & 0xFF`, `ofs_base = (info >> 8) & 0xFF`, `addr_base = (info >> 16) & 0xFF` | Igual | Sí |
| Bases | Etapa: `p[ofs_base]` (de `module_start`). `mgp_main`: `{0x08804000, 0x089F2D40}` | `base_etapa + vaddr(seg)` y `0x08804000 + vaddr(seg)` | Sí: `p[0]` y `p[1]` son la base y la base más el `vaddr` del segmento 1 |
| `R_MIPS_32` | `w += base` | Igual | Sí |
| `R_MIPS_26` | Extiende el signo de `(w << 2)` en 28 bits, suma la base y guarda `>> 2` | `field + (base >> 2)` en 26 bits | Sí, porque las bases son múltiplos de 4 |
| `R_MIPS_HI16` | Busca hacia adelante la primera entrada de tipo 6 (saltando **cualquier** otro tipo), suma el `(s16)` de su palabra **actual** y deja `(addr - (s16)addr) >> 16` | Busca el siguiente `LO16` saltando solo `HI16`, con la palabra original | Sí: el `LO16` siempre va después de su `HI16` (todavía sin parchear) y nunca hay otro tipo entre los dos |
| `HI16` sin `LO16` | Usa su propia palabra como parte baja | Error | No aparece |
| `R_MIPS_LO16` | `imm = (imm + base) & 0xFFFF` | Igual | Sí |
| Tipo `0xFA` | Cadena de `lui` enlazados por el desplazamiento de su inmediato, con el addend en la entrada siguiente | Se rechaza (error) | No aparece en ninguna tabla |
| Otros tipos | Se ignoran en silencio | Error | No aparecen |
| Orden | En el sitio, entrada por entrada | Lee primero todas las palabras originales | Sí: no hay dos entradas sobre la misma palabra |

### Comparación byte a byte [V]

- En la carpeta `PSP/PPSSPP_STATE/` del usuario había un savestate de PPSSPP `v1.20.4`, hecho en el
  menú de pausa de una misión ("Soviet Patrol Base").
  - Se descomprimió en local con `zstd_lite.py`: 40 869 620 bytes, con la RAM a partir del offset
    `0x48`.
- La etapa cargada es **`st02`, en `0x08B8F900`**:
  - `0x08A00164` (la entrada) vale `0x08B8F900`;
  - `0x08A0015C..64` vale `{0x08B8F900, 0x08C675C0, 0x08B8F900}`;
  - `0x08A5CBE0` vale `0x08C675C0` (la base de sus datos);
  - el tamaño guardado en `0x08A00158` es `0xDEA8`, el de `st02.rlc` (7 125 entradas).
- Comparada con `rlc.py relocate --stage-base 0x08B8F900` de `st02`:
  - **`.text`: 215 884 de 215 884 palabras iguales**;
  - **las 7 125 palabras que parchea el `.rlc`, iguales**;
  - en el resto del segmento 0 solo difieren las 8 palabras `syscall` de `.sceStub.text`, que
    escribe el cargador del kernel al resolver los imports;
  - en el segmento 1 difieren 3 de 6 518 palabras (estado en ejecución), ninguna de la tabla de la
    etapa.
- **La base se repite**: `title` (lista de módulos de PPSSPP, F02) y `st02` (este savestate) se
  cargaron las dos en `0x08B8F900`. Falta ver un menú [?].
- El juego no pide esa base, porque no pasa `SceKernelLMOption`.
  - Es lo primero libre después de `KCEJ_FS`, que termina en `0x08B86F00`, más un hueco de `0x8A00`
    bytes que no se identificó [?].
  - No es `libfont`: PPSSPP lo simula por HLE y no le reserva memoria.
- No se importa ningún `sceKernelIcacheInvalidate*`: la icache se invalida a mano con `cache 0x04`.

## Herramientas

Todas están en `profiles/mpo/tools/` y se corren con `python -I`. Validan cada tamaño y offset antes
de usarlo.

| Script | Para qué |
|---|---|
| `zar.py` | Descomprimir un `_zar`, listar el DAR y extraer una entrada (`list`, `extract`) |
| `rlc.py` | Validar un `.rlc` contra su etapa y `mgp_main` (`check`, solo cuenta); generar la imagen relocalizada de una etapa en una base dada (`relocate`), y escribir el PRX de la etapa con el `.rlc` ya aplicado (`link`), que cualquier cargador de PRX deja igual que el juego. Las dos últimas salidas son del juego y van a `analysis/` |
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
python -I profiles/mpo/tools/rlc.py link --stage …/st01/st01.prx --zar …/st01/_zar \
    --main game/elf/mgp_main.elf --output analysis/f04/linked/st01
```

- **Importar una etapa en Ghidra**: se importa el PRX enlazado (`rlc.py link`) en `0x08B8F900`, con
  `MPO_EXTERNAL_BLOCK="mgp_main 08804000 2F7800"` y
  `MPO_ANCHORS=profiles/mpo/config/anchors_modules.txt`:
  `profiles/mpo/tools/ghidra/run_analysis.sh analysis/f04/linked/st01 stage_st01 08B8F900`.
  - El bloque externo (`MpoExternalBlock.java`) es un bloque relleno de `jr ra; nop` y no
    ejecutable.
  - Sin él, cada llamada a `mgp_main` corta el desensamblado: `st01` quedaba en el 66 % de `.text`
    decodificado y `title` en el 77 %. Con él: `title` 100 % (1 851 funciones) y `st01` 99.99 %
    (4 379 funciones, 1 `jr` sin resolver y 108 `jalr` sin resolver).

## Lo que falta confirmar

- La base de `mgp_stage` en un menú (por ejemplo, `gsmenu`) y en una etapa de red. Con `title` y
  `st02` en `0x08B8F900`, lo más probable es que sea siempre la misma, pero el juego no la fija.
- Qué ocupa los `0x8A00` bytes entre `KCEJ_FS` y `mgp_stage`.
- El acceso fino de `kjfs` a `_zar` (cómo ubica una entrada sin descomprimir todo).
- El papel exacto de las claves de la tabla de la etapa (probablemente `strcode` de comandos GCL o
  de actores).
