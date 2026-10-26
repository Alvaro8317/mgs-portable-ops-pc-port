# `psp_analyze` sobre MPO y brechas del framework (F06)

Resultado de la fase F06: qué ve PSPRecomp de nuestros módulos, en qué se diferencia de Ghidra
(F03 y F04), cómo entran los datos de Ghidra al framework y qué le falta al framework para MPO.
Aquí van **solo números, nombres y direcciones**; nada de código del juego. Las etiquetas siguen
`CLAUDE.md` §11: **[V]** verificado en el binario, **[S]** de una fuente externa y **[?]** sin
verificar o inferido.

## 1. Cómo se obtuvo

- **Framework**: PSPRecomp en el commit fijado `f6e7d41` (upstream no tiene nada más nuevo el
  2026-10-25 [V]). Compilado con Clang 18, sin parches, en un directorio de build propio.
- **Entradas** (locales, en el checkout principal):

  | Módulo | Archivo | Base | SHA-256 (prefijo) |
  |---|---|---|---|
  | `mgp_main` | `game/elf/mgp_main.elf` | `0x08804000` | `ca700ea5…` |
  | `st01` | `analysis/f04/linked/st01` (`rlc.py link`) | `0x08B8F900` | `c452297a…` |
  | `title` | `analysis/f04/linked/title` (`rlc.py link`) | `0x08B8F900` | `70c41350…` |
  | `sound2`, `kjfs`, `zlibdec` | `game/disc/PSP_GAME/USRDIR/modules/*.prx` | las de PPSSPP (`mgp_main.md` §7) | los de `supported.toml` |

- **Herramientas nuevas** (en el repo, sin datos del juego):
  - `profiles/mpo/tools/analysis_export.cpp` → `mpo_analysis_export`: corre el mismo
    `analyze_program()` que usan `psp_analyze` y `psp_recomp --auto` y exporta en JSON las semillas,
    las funciones (con sus instrucciones y delay slots), las entradas de bloque y un censo del
    decodificador. `psp_analyze` solo escribe contadores y una CSV sin rangos.
  - `profiles/mpo/tools/recomp_compare.py`: compara ese JSON con el `recomp.json` de Ghidra. Imprime
    solo números.
  - `profiles/mpo/tools/recomp_hints.py`: convierte el `recomp.json` de Ghidra en hints para
    PSPRecomp (§5).
  - `run_analysis.sh` exporta ahora también el `recomp.json` después de cada paso de siembra
    (`1-ghidra/`, `2-relocations/`, `3-prologues/`), para saber qué paso encontró cada función.
    Se volvió a correr Ghidra para `mgp_main` y `st01` en un proyecto aparte
    (`analysis/f06/ghidra`): el `recomp.json` final sale **idéntico byte a byte** al de F03/F04
    (`cmp`) [V].
- **Comandos** (desde el worktree, con los datos en el checkout principal):

  ```bash
  build/framework/psprecomp/psp_analyze game/elf/mgp_main.elf analysis/f06/mgp_main/report.json 0x08804000
  build/profiles/mpo/tools/mpo_analysis_export game/elf/mgp_main.elf 0x08804000 analysis/f06/mgp_main/export.json
  python3 -I profiles/mpo/tools/recomp_compare.py --export analysis/f06/mgp_main/export.json \
      --ghidra analysis/f06/ghidra/out/mgp_main
  # Etapa: el PRX enlazado de F04 en 0x08B8F900.
  build/profiles/mpo/tools/mpo_analysis_export analysis/f04/linked/st01 0x08B8F900 analysis/f06/stage_st01/export.json
  ```

- Todas las salidas (`report.json`, CSV, `export*.json`, `compare*.json`, hints y el C++ generado)
  quedan en `analysis/f06/`, que git ignora.

## 2. Cómo analiza y genera PSPRecomp [V, en el código del commit fijado]

- **Rangos ejecutables**: cada `PT_LOAD` con el bit X. En un PRX, el primer segmento (`flags 7`)
  lleva `.text` **y también** `.rodata`, `.lib.*`, `.sceModuleInfo`, `.eh_frame`…, así que esos
  datos se decodifican como código.
- **Semillas**: la entrada del ELF, todos los destinos de `jal` (barriendo cada palabra de los rangos
  ejecutables), los punteros que se arman con `lui`/`addiu`/`ori` (por bloque y en una ventana de 24
  instrucciones), las palabras de los segmentos no ejecutables que apuntan a código y las
  relocaciones `R_MIPS_32` que apuntan a código.
- **Función**: un recorrido del CFG desde la semilla. Sigue ramas, `j` a direcciones que no son
  semilla y la continuación de `jal`/`jalr`; corta en `jr` (no entiende tablas de saltos) y en otra
  semilla.
- **Generación (`psp_recomp --auto`)**: **no es una función de C++ por función del juego**. Emite cada
  instrucción descubierta **una sola vez**, en unidades de tamaño fijo (`0x20000` bytes por defecto),
  y registra como entradas del dispatcher todas las entradas de bloque y las semillas. Los límites de
  función no cambian el código: solo cuentan la cobertura y el conjunto de entradas.
  - Consecuencia: **sembrar de más no rompe nada** (como mucho añade una entrada al dispatcher), y
    un `jr` funciona sin conocer su tabla si todos sus destinos son entradas registradas: si el
    destino no está en la unidad, sale al dispatcher.
  - Lo que **sí** rompe es no cubrir código: sin intérprete (§7), una dirección sin función
    registrada detiene el runtime.
  - `CLAUDE.md` §5 ("una función de C++ por cada función del juego", de gow-psp-recomp) no describe
    este modelo; hay que ajustarlo (ver la nota de sesión).

## 3. `psp_analyze` contra Ghidra, sin datos externos [V]

"Código" = palabras dentro de los rangos de alguna función de Ghidra (con los tres pasos de
siembra de F03). La cobertura de PSPRecomp incluye los delay slots.

| Módulo | Funciones Ghidra | Funciones `psp_analyze` | En ambos | Código Ghidra (palabras) | No cubierto por `psp_analyze` | Palabras de más en `psp_analyze` |
|---|---|---|---|---|---|---|
| `mgp_main` | 8 484 | 9 912 | 6 635 | 481 197 | **51 160 (10.63 %)** | 17 952 (16 737 en `.rodata`) |
| `st01` | 4 379 | 4 596 | 3 754 | 210 359 | **13 946 (6.63 %)** | 1 505 (1 432 en `.rodata`) |
| `title` | 1 851 | 1 700 | 1 273 | 56 650 | **11 543 (20.38 %)** | 893 (787 en `.rodata`) |
| `sound2` | 531 | 589 | 417 | 24 027 | 2 352 (9.79 %) | 528 |
| `kjfs` | 230 | 306 | 198 | 13 491 | 549 (4.07 %) | 424 |
| `zlibdec` | 24 | 88 | 23 | 2 655 | 8 (0.30 %) | 263 |

- **Las relocaciones están bien**: 76 371 en `mgp_main`, 0 inválidas y 0 sin soporte, las mismas que
  aplica Ghidra. Los imports también: 240 de 32 librerías, como `MpoModuleTables.java` y
  `prx_imports.py` (F03). En `st01`, 31 388 relocaciones y 8 imports.
- **`gp`**: `psp_analyze` da `0x08A07E90`, y es correcto: el campo vale `0x15150` y lo reloca un
  `R_MIPS_32` contra el segmento 1. `mgp_main.md` §2 decía `0x08819150` y se corrigió. Ninguna
  instrucción de `mgp_main` direcciona relativo a `$gp` (barrido de `.text`).

### 3.1 Funciones que solo encuentra Ghidra (`mgp_main`)

| Paso de Ghidra que la encontró | Funciones | Sin cubrir por `psp_analyze` | Palabras sin cubrir |
|---|---|---|---|
| Auto-análisis de Ghidra | 5 835 | 680 | 20 564 |
| Relocaciones | 964 | 2 | 63 |
| Prólogos | 605 | 386 | 17 301 |
| Huecos | 1 080 | 730 | 13 232 |
| **Total** | **8 484** | **1 798** | **51 160** |

- De las 1 849 entradas que solo tiene Ghidra, 1 798 no las cubre ninguna función de
  `psp_analyze`, 27 quedan como bloque dentro de otra función y 24 son stubs de import que nadie
  llama con `jal`.
- **Las 680 del auto-análisis**: 679 empiezan justo después de un `jr ra` + delay slot (el
  analizador de inicios de función de Ghidra). 547 no tienen ninguna referencia en la imagen
  relocada (ni puntero ni par `lui`/inmediato); 133 tienen un par `lui`/inmediato en algún sitio,
  que `psp_analyze` no sigue (más de 24 instrucciones o entre bloques) [?].
- **Las etapas explican una parte**: las 91 PRX distintas llaman (por `R_MIPS_26` del `.rlc`) a
  **1 493** entradas de `mgp_main`. Ghidra las tiene **todas** como función; `psp_analyze` cubre
  1 029 y **464 no las cubre**. 186 de las 680 del auto-análisis son de estas. Sin datos externos,
  la primera llamada de una etapa a una de esas 464 pararía el runtime.
- En `st01` (625 solo de Ghidra, 585 sin cubrir): auto-análisis 286, relocaciones 3, prólogos 99,
  huecos 197.

### 3.2 Funciones que solo encuentra `psp_analyze`

| | `mgp_main` | `st01` | `title` |
|---|---|---|---|
| Fuera de `.text` (datos decodificados como código) | 2 291 (2 216 en `.rodata`) | 258 | 179 |
| Destinos de tablas de saltos (cada `case` es una "función") | 940 | 571 | 245 |
| Dentro de una función de Ghidra | 35 | 12 | 3 |
| En `.text`, fuera de toda función de Ghidra | 11 | 1 | 0 |

- Origen de las semillas falsas: `relocated_data_code_pointer` (punteros de `.data` a cadenas de
  `.rodata`, que para PSPRecomp es "ejecutable"), `relocated_r_mips32_code_pointer` (tablas de
  saltos) y `materialized_code_pointer`.
- Las de `.rodata` son inofensivas para la corrección, pero generan C++ inútil y unas 8 786
  palabras de datos que el decodificador no reconoce (`mgp_main`).

### 3.3 Tablas de saltos y `jalr`

| | `mgp_main` | `st01` | `title` |
|---|---|---|---|
| Tablas de Ghidra (sin resolver) | 177 (1: `0x088A77C8`) | 117 (1: `0x08BF2264`) | 42 (0) |
| Destinos | 1 220 | 914 | 327 |
| Destinos que `psp_analyze` siembra | **1 220** | **914** | **327** |
| Tablas con todo el `jr` despachable | 171 | 115 | 39 |
| Tablas cuyo `jr` no se emite (código sin cubrir) | 5 | 1 | 3 |
| `jalr` en código de Ghidra (vistos por `psp_analyze` / medidos por Ghidra) | 246 / 258 | 96 / 108 | 59 / 68 |

- **Todos los destinos de tablas son semillas** porque las tablas están relocadas: el `jr` sale al
  dispatcher y encuentra la entrada. Por eso los dos `switch` partidos que Ghidra no resuelve no
  bloquean nada; igualmente quedan como hints del perfil (§5).
- En el resto de módulos de Konami, igual: todas las tablas despachables y 0 sin resolver.

## 4. Con los datos de Ghidra (hints) [V]

Con el parche `framework/patches/0001-analysis-hints.patch` y los hints de `recomp_hints.py`
(rangos de código = bloques ejecutables de Ghidra, una semilla por función de Ghidra y sus tablas
de saltos, más `config/hints/<programa>.txt`):

| Módulo | Funciones `psp_analyze` | Ghidra cubierto | Palabras de más | Funciones de más (destinos de tablas / otras) |
|---|---|---|---|---|
| `mgp_main` | 9 469 | **481 197 de 481 197** | 835 (en `.text`) | 940 / 45 |
| `st01` | 4 963 | **210 359 de 210 359** | 27 (en `.text`) | 571 / 13 |
| `sound2` | 616 | 24 027 de 24 027 | 0 | 84 / 1 |
| `kjfs` | 262 | 13 491 de 13 491 | 0 | 31 / 1 |
| `zlibdec` | 58 | 2 655 de 2 655 | 0 | 34 / 0 |

- Las 8 484 funciones de Ghidra quedan como función en PSPRecomp; no sobra nada en `.rodata`.
- Las palabras de más en `.text` son código que Ghidra decodificó pero no metió en ninguna función
  (`mgp_main.md` §3: el 99.97 % decodificado frente al 99.80 % en funciones).
- `jalr`: 258 en `mgp_main` y 108 en `st01`, los mismos que mide Ghidra.
- **Generación**: con los parches 0001 y 0002, `psp_recomp --auto --hints` genera `mgp_main` en
  15 s y 164 MB de RAM: 397 918 instrucciones, 100 783 entradas, 15 unidades y 57 MB de C++.
  `st01`: 168 196 instrucciones, 48 442 entradas, 7 unidades y 24 MB. **No se compiló** (F07b).

## 5. Diseño: cómo entran los datos de Ghidra a PSPRecomp

**Decisión**: un formato de texto neutro ("hints") que lee el framework, y el perfil lo produce desde
Ghidra. El framework no sabe nada de Ghidra ni del juego.

```text
code <start> <end>                 # rango ejecutable [start, end); si hay alguno, sustituye a los PT_LOAD
seed <address> [source]            # entrada de función extra
jumptable <jr address> <target>... # destinos de un salto indirecto
```

- **Framework (MIT, `0001-analysis-hints.patch`)**:
  - `psprecomp::AnalysisHints` y `load_analysis_hints(path)` (valida cada línea y falla con
    `psprecomp::Error`).
  - Una sobrecarga `analyze_program(elf, memory, base, hints)`. Las semillas automáticas se
    conservan y las de los hints se añaden; los destinos de un `jumptable` pasan a ser bloques de la
    función que tiene el `jr` (se emiten y se registran como entradas).
  - `psp_analyze` y `psp_recomp --auto` aceptan `--hints <archivo>`. Sin él no cambia nada.
  - `PSPRECOMP_ANALYSIS_HINTS` permite a las herramientas detectar la función.
  - Test sintético en `tests/test_main.cpp` (`jr` sin resolver, dos `case` y una función sin
    referencias). Pasa con Clang y GCC.
- **Perfil (GPL)**:
  - `run_analysis.sh` (Ghidra) → `out/<programa>/<programa>.recomp.json`.
  - `recomp_hints.py` → `code` por bloque ejecutable, `seed … ghidra` por función y `jumptable`
    por tabla resuelta; omite las funciones del bloque de relleno de una etapa
    (`MpoExternalBlock.java`) y las tablas sin destinos.
  - `config/hints/<programa>.txt`, escritos a mano y validados: hoy los dos `switch` partidos
    (`mgp_main` `0x088A77C8`, 11 destinos; `st01` `0x08BF2264`, 5 destinos), leídos de la tabla
    relocada [V].
  - En F07b, `profiles/mpo/scripts/generate.*` encadena: instalado → (Ghidra) → hints →
    `psp_recomp --auto --hints` → `generated/`.
- **Por qué así y no otra cosa**:
  - La CSV `name,address,size` del modo manual de `psp_recomp` no sirve: trata cada instrucción
    como entrada, no admite funciones con varios rangos ni tablas, y no usa el modo por unidades.
  - Copiar las heurísticas de Ghidra al analizador de PSPRecomp sería reescribir Ghidra; los hints
    también sirven para cualquier otro desensamblador.
  - Restringir los rangos a `.text` también se puede hacer en el framework (Yakumo usa las secciones
    `SHF_EXECINSTR`, §7); con hints lo decide el perfil.
- **Robustez**: las semillas heurísticas de Ghidra (prólogos y huecos) no ponen en riesgo el código
  generado (§2). Lo que hay que vigilar es lo contrario: código sin cubrir. `recomp_compare.py` lo
  mide en cada regeneración.
- **Sin Ghidra** (por ejemplo, en CI con el juego): las 1 493 entradas que llaman las etapas salen
  exactas del `.rlc` (`stage_inventory.py`) y se pueden emitir como `seed … stage_call`.

## 6. Instrucciones que el emisor no baja [V]

Censo del decodificador de PSPRecomp sobre el código de Ghidra. `psp_recomp` convierte estas
instrucciones en `rt.unsupported(…)` y deja de emitir el bloque.

| Instrucción | `mgp_main` | `st01` | `title` | `sound2` | `kjfs` | `zlibdec` |
|---|---|---|---|---|---|---|
| `break` | 252 | 86 | 20 | 26 | 20 | 0 |
| `vi2s.q` / `vi2s.p` | 4 / 4 | 0 | 0 | 0 | 0 | 0 |
| `vi2uc.q` | 3 | 0 | 0 | 0 | 0 | 0 |
| `vcrs.t` | 2 | 0 | 0 | 0 | 0 | 0 |
| `vsgn.t` / `vsgn.s` / `vsgn.p` | 1 / 0 / 0 | 0 | 0 | 0 / 3 / 1 | 0 | 0 |
| Control de flujo en un delay slot | 0 | 0 | 0 | 0 | 0 | 0 |

- `break` es la trampa de la división por cero de GCC; solo se ejecuta si el divisor es 0. Que pare
  el runtime con un mensaje es el comportamiento correcto.
- No aparecen `syscall` (los imports van por stubs), `madd`/`msub`, `mfic`/`mtic`, COP0, `ll`/`sc` ni
  `lvl`/`lvr`/`svl`/`svr`.
- `cfc1`/`ctc1` solo se emiten para `fcr31`; el único `ctc1` de `mgp_main` lo usa (0 mensajes
  "unsupported CTC1" en el C++ generado).
- **Semántica de la VFPU que no es bit a bit** (el emisor la baja, pero con `libm` del host, no con
  los algoritmos de PPSSPP `Core/MIPS/MIPSVFPUUtils.cpp` [S]):
  - `vsin`, `vcos`, `vrsq`, `vsqrt`, `vasin`, `vexp2`, `vrexp2`, `vlog2`, `vrcp`, `vnrcp` y `vnsin`
    (`std::sin`, `1/std::sqrt`…). Uso en `mgp_main` (F03): `vrsq` 280, `vsqrt` 236, `vcos` 98,
    `vsin` 92, `vasin` 2, `vrcp` 4 y transcendentes `.t`/`.q` 4.
  - `vdot` (734) y los productos de `vmmul`/`vtfm`/`vhdp` suman en `float` normal.
  - **Corrección (F07a [V])**: esa parte ya coincide con PPSSPP, que también suma en `float` de
    izquierda a derecha (`USE_VFPU_DOT` está en `false`). `vfpu_dot()` solo entra en `vmmul` con el
    compat flag `MoreAccurateVMMUL`, que está apagado para MPO. `vrsq` y `vsqrt` también usan la raíz
    del host en PPSSPP. Lo único que hay que cambiar en el emisor son las funciones con tablas: `vsin`,
    `vcos`, `vasin`, `vexp2`, `vrexp2`, `vlog2`, `vrcp`, `vnrcp` y `vnsin` (ver `docs/sessions/F07a.md`).

## 7. El framework fijado contra el de Yakumo [V, en el código]

Yakumo (`TeamGDB/Yakumo`, MIT, commit `4a5debda`, 2026-10-21) lleva su propia copia del framework,
con cambios genéricos que upstream no tiene. Comparando con el formato normalizado:

- **Intérprete de respaldo** (`src/interpreter.cpp`, `include/psprecomp/interpreter.hpp`), con
  `PSPRECOMP_NO_INTERPRETER` y un informe de direcciones interpretadas. **Upstream no tiene
  intérprete**, aunque `CLAUDE.md` §3 lo da por hecho.
- **Ganchos de overlays**: `set_runtime_missing_function_hook`, `set_runtime_unsupported_hook` y
  `Runtime::unregister_functions(start, end)`.
- **Librería por overlay**: `psp_recomp --prefix <símbolo>` (nombres de unidad distintos por módulo),
  `--dispatch-target`, el macro `PSPRECOMP_HOST_DATA` (dllimport en Windows) y sin encadenar unidades
  en las librerías.
- **Decodificador y emisor**: `break`, `madd(u)`/`msub(u)`, `vsgn` y `vsocp`; rangos ejecutables
  limitados a las secciones `SHF_EXECINSTR` (arregla lo de `.rodata`), RAM de 64 MiB si hace falta
  y la corrección del bucle de §8.1.
- **No** tiene el arreglo de §8.2 ni hints.

## 8. Errores encontrados en `psp_recomp --auto` [V]

1. **Bucle infinito**: un `jal` a un stub de import que está en otra unidad emite la salida al
   dispatcher y hace `continue` sin avanzar `pc`, así que emite las mismas líneas para siempre.
   Con `mgp_main` no termina nunca: sin límite, el sistema lo mató por memoria (código 137), y con
   `ulimit -v` de 6 GB siguió más de 10 minutos sin escribir ninguna unidad. Yakumo lo corrigió
   con `break`.
2. **Llamadas que se saltan el import**: los stubs (`jr ra; nop` hasta que el cargador los parchea)
   se analizan como código y se vuelven etiquetas. Un `jal` o `j` desde la **misma unidad** salta
   con `goto` al stub y vuelve sin llamar al HLE.
   - En `mgp_main` son **92 llamadas**, todas en la unidad 14 (`0x089C4000–0x089E3FFF`, donde están
     los stubs): `Kernel_Library` 24, `sceGe_user` 19, `StdioForUser` 15, `ThreadManForUser` 9,
     `IoFileMgrForUser` 6, `SysMemUserForUser` 5, `sceDisplay` 4, `ModuleMgrForUser` 4,
     `InterruptManager` 3 y `UtilsForUser` 3. Entre ellas, el envoltorio de display lists del GE
     (`mgp_main.md` §6.7).
   - También está en Yakumo.
- Los dos se corrigen en `framework/patches/0002-codegen-import-stubs.patch` (MIT): `break` en vez de
  `continue`, y las palabras de los stubs no entran en las unidades. Con el parche, 0 saltos a
  stubs en el C++ generado y las 92 llamadas pasan por el dispatcher.

## 9. Lista de brechas

"MIT" = parche genérico al framework, que se puede proponer upstream. "GPL" = código del perfil.

| # | Brecha | Evidencia | Clase | Dónde / cuándo |
|---|---|---|---|---|
| 1 | Datos de análisis externos (rangos, semillas, tablas de saltos) | §3, §4 | **MIT** + GPL (`recomp_hints.py`, `config/hints/`) | Hecho como parche 0001 y herramientas; se aplica en F07b |
| 2 | Rangos ejecutables con `.rodata` (PRX) | 2 291 funciones falsas, 16 737 palabras (`mgp_main`) | **MIT** (traer el filtro `SHF_EXECINSTR` de Yakumo) | F07b; mientras, `code` en los hints |
| 3 | Bucle infinito con `jal` a un stub de otra unidad | §8.1 | **MIT** | Parche 0002 |
| 4 | `jal`/`j` a un stub de la misma unidad no llama al import | §8.2, 92 llamadas | **MIT** | Parche 0002 |
| 5 | Sin intérprete de respaldo | §7 | **MIT** (traer `interpreter.cpp` de Yakumo) | F07b |
| 6 | Ganchos de overlays (`missing_function`, `unsupported`, `unregister_functions`) | `F04.md`, §7 | **MIT** (de Yakumo) | F07b/F13 |
| 7 | Una librería por overlay (prefijo de símbolos, dllimport) | §7 | **MIT** (de Yakumo) + GPL (cargador por hash, base `0x08B8F900`) | F13 |
| 8 | Una sola disposición de unidades por `Runtime`: un segundo módulo o una etapa con otra base desactiva para siempre el camino rápido entre unidades | `register_generated_unit()` | **MIT** (una tabla por módulo o `--unit-base` común) | F07b/F13; solo rendimiento |
| 9 | Módulos residentes múltiples: imports de Konami entre módulos recompilados (71 en `mgp_main`, 3 en `kjfs`) | `mgp_main.md` §9 | **GPL** con la API actual (`register_hle` que salta a la dirección del export); MIT opcional (enlazador de módulos) | F08b |
| 10 | Tres funciones que el juego convierte en `jr ra` al arrancar | `mgp_main.md` §6.6 | **GPL** (`register_function` del perfil después del generado) | F07b/F08b |
| 11 | 464 entradas que solo llaman las etapas | §3.1 | **GPL** (ya cubiertas por las semillas de Ghidra) | Hecho |
| 12 | `switch` partidos sin resolver (uno por módulo) | §3.3 | **GPL** (`config/hints/`) | Hecho; no bloquean |
| 13 | VFPU sin bajar: `vi2s`, `vi2uc`, `vcrs.t` (`mgp_main`, 13) y `vsgn` (`mgp_main` 1, `sound2` 4) | §6 | **MIT** (`vsgn` de Yakumo; el resto, escrito desde la documentación y validado contra `mpo_vfpu`) | F07b |
| 14 | VFPU transcendente y `vdot` con `libm`, no bit a bit | §6 | **MIT** (un punto de enganche: el emisor llama a funciones reemplazables, `libm` por defecto) + **GPL** (`mpo_vfpu`, F07a, portado de PPSSPP) | F07a/F07b |
| 15 | `break` sin decodificar | §6 | **MIT** (de Yakumo; solo mejora el mensaje) | Baja prioridad |
| 16 | Relocaciones y `sceModuleInfo` solo desde la tabla de secciones (un PRX sin secciones da 0 relocaciones) | test sintético | **MIT** (leer `PT_PRXRELOC`) | Baja; los módulos reales tienen secciones [V] |
| 17 | Tamaño del C++: 57 MB `mgp_main`, 24 MB `st01` (91 PRX de etapa) | §4 | GPL (deduplicación de `F04.md`) y MIT si hace falta soporte del emisor | Medir en F07b/F13 |
| 18 | `gp` | §3 | Ninguna (es correcto) | F08b fija `$gp` desde `sceModuleInfo` |

**No es una brecha**: los delay slots con control de flujo (0), las relocaciones (0 inválidas), los
imports (240, iguales a F03) y las tablas de saltos (todos los destinos son semillas).

## 10. Recomendación sobre la base del framework

- **Partir de la capa de framework de Yakumo** (MIT, mismo copyright que PSPRecomp) en vez del
  upstream fijado: trae resueltas las brechas 2, 3, 5, 6, 7, 13 (`vsgn`) y 15, y es la que ya
  funciona con overlays y en Linux.
- Encima, nuestros parches 0001 y 0002, y después los de las brechas 8, 13 y 14.
- Forma propuesta (como decía `F00.md`): un **fork de PSPRecomp en GitHub** con una rama `mpo`
  (upstream + los commits genéricos de Yakumo + los nuestros), apuntada por el submódulo. Cada
  parche genérico se puede enviar como PR upstream. Mientras no exista el fork, los parches viven
  en `framework/patches/` y se aplican con `git am`.
- Lo decide el integrador o el usuario; crear el fork necesita su cuenta de GitHub.

## 11. Estado de las brechas tras F07b

La base del framework es desde F07b la rama `mpo` del fork (`CLAUDE.md` §9, decisión 7): upstream
`f6e7d41`, la capa genérica de Yakumo (`TeamGDB/Yakumo` `4a5debda`, MIT), los parches 0001 y 0002
rebasados y los commits genéricos de F07b. La lista de commits está en `docs/sessions/F07b.md`.

| # | Brecha | Estado |
|---|---|---|
| 1 | Hints externos | Hecho (0001, en `mpo`). Nuevo `patch <dirección> <palabra>` para código que el juego reescribe al arrancar. |
| 2 | `.rodata` como código | Hecho: filtro `SHF_EXECINSTR` de Yakumo; con hints, los rangos `code` de Ghidra. |
| 3 | Bucle con `jal` a un stub de otra unidad | Hecho (Yakumo ya lo traía). |
| 4 | `goto` a un stub de la misma unidad | Hecho (0002 rebasado: las palabras de los stubs no se emiten). |
| 5 | Intérprete de respaldo | Hecho (Yakumo) y probado con el difftest (`mpo_difftest_interpreter`). |
| 6 | Ganchos de overlays | Traídos de Yakumo; se usan en F13. |
| 7 | Una librería por overlay | `--prefix` de Yakumo; el cargador del perfil es de F13. |
| 8 | Una disposición de unidades por `Runtime` | Resuelto por diseño: un corpus con `--prefix` registra por dirección y no toca la tabla densa del ejecutable principal (test `test_prefixed_corpus_keeps_main_layout`). |
| 10 | Tres funciones parcheadas a `jr ra` | Hecho como `patch` en `config/hints/mgp_main.txt`: un `register_function` en la entrada no alcanza, porque las llamadas desde la misma unidad son `goto` directos. `mpo_aot_probe` comprueba que vuelven al instante. |
| 13 | `vi2s`, `vi2uc`, `vcrs.t`, `vsgn` | Hecho: toda la VFPU va al ejecutor (`mpo_vfpu`). |
| 14 | VFPU no bit a bit | Hecho: `psp_recomp --vfpu-executor` y `set_vfpu_executor()` (MIT); el perfil instala `mpo_vfpu`. |
| 15 | `break` | Hecho (Yakumo). |
| 16 | Relocaciones sin tabla de secciones | Pendiente (baja prioridad). |
| 17 | Tamaño del C++ | `mgp_main`: 118 unidades de `0x4000` bytes, 116 MB de C++. Medido en `docs/sessions/F07b.md`. |

**Cambio de modelo de emisión** (capa de Yakumo): `psp_recomp --auto` ya no emite solo lo que
alcanza el CFG, sino **cada palabra de los rangos ejecutables, y cada una es una entrada del
dispatcher**. Con los rangos `code` de Ghidra, `mgp_main` registra 481 676 entradas (todas las
palabras de código menos los 240 stubs, que tienen su wrapper de import). La unidad por defecto pasa
de `0x20000` a `0x4000` bytes.

**Semántica de la CPU corregida en el framework** (encontrada por el difftest del emisor contra
PPSSPP; ver `docs/sessions/F07b.md`): conversiones float→int, `divu` por cero, `INT_MIN / -1`,
`jalr` con `rd = 0`, y un salto a una dirección de una unidad que no es entrada.
