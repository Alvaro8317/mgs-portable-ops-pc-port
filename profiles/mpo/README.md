# Perfil `mpo` — Metal Gear Solid: Portable Ops

Perfil de PSPRecomp para **MGS: Portable Ops** (base `ULUS-10202`, disco 1.02). Aquí va todo lo que
depende de una dirección, un hash o un layout del juego (`CLAUDE.md` §1.3). El framework
(`framework/psprecomp`) tiene que seguir compilando sin este perfil y sin el juego.

Estado: **F03**. Están el instalador (`mpo --install`), que también descifra el `EBOOT.BIN`, los
tests de disco y de descifrado con datos sintéticos, y las herramientas de análisis en Ghidra
(`tools/ghidra/`). Los resultados están en `docs/analysis/mgp_main.md`.

## Instalar la copia del usuario

```bash
mpo --install game/ULUS-10202.iso      # valida, descifra a game/elf/, extrae a game/disc/ y escribe game/install.toml
mpo --list game/ULUS-10202.iso         # lista nombres y tamaños
mpo --manifest game/ULUS-10202.iso \
    --ignore PSP_GAME/SYSDIR/UPDATE/ --ignore PSP_GAME/SYSDIR/BOOT.BIN   # bloque para supported.toml
```

- `--install` compara el `DISC_ID` y el `DISC_VERSION` de `PARAM.SFO`, y después el conjunto exacto de
  archivos, con sus tamaños y SHA-256, contra `config/supported.toml`. Si algo no coincide, rechaza la
  imagen (código de salida 1) y no escribe nada.
- Solo extrae lo que piden las fases de análisis: `PARAM.SFO`, `EBOOT.BIN` (cifrado), todos los
  `*.prx` y los `stage/<s>/_zar` (F04). El resto se leerá directamente del ISO.
- Descifra los módulos de la lista `decrypt` de `supported.toml` (hoy solo `EBOOT.BIN` →
  `game/elf/mgp_main.elf`) y compara el ELF con su SHA-256 fijado **antes de escribir nada**. El
  descifrado usa libkirk y `PrxDecrypter.cpp` de PPSSPP, que CMake descarga al configurar porque
  traen claves y no se versionan (ver `third_party/ppsspp_crypto/NOTICE.md`).
- `python -I profiles/mpo/tools/prx_info.py game/elf/mgp_main.elf` muestra la identidad de un módulo
  descifrado: la cabecera ELF, los program headers y `sceModuleInfo`. Solo imprime números y
  nombres.
- Herramientas de etapas (F04), en `tools/`, con tests sintéticos en `ctest`:
  - `zar.py` lista y extrae las entradas del DAR de un `stage/<s>/_zar`;
  - `rlc.py check` valida el `<s>.rlc` contra su PRX y `mgp_main`;
  - `rlc.py relocate` escribe la imagen relocalizada de una etapa, y `rlc.py link`, el PRX de la
    etapa con el `.rlc` ya aplicado (para Ghidra o el recompilador). Los dos son contenido del
    juego: van a `analysis/` y nunca al repo;
  - `psp_prx.py` es el lector de PRX que usan los demás;
  - `stage_inventory.py` genera `docs/analysis/stages.md`.
  Ver `docs/formats/rlc.md`. `--install` extrae también los `_zar` de las etapas.

## Analizar los módulos en Ghidra (F03)

Herramientas locales (no se versionan ni se distribuyen), instaladas fuera del repo, por ejemplo en
`~/tools/`:

- JDK 21 (Temurin `21.0.13+9`).
- Ghidra `12.1.3` (`ghidra_12.1.3_PUBLIC_20260817.zip`).
- [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex) `v21.4`
  (`ghidra_12.1.3_PUBLIC_20260825_ghidra-allegrex.zip`), descomprimido en
  `Ghidra/Extensions/`. Trae el loader "PSP Executable (ELF)" (`PspElfLoader`), que aplica las
  relocaciones PRX en la base que se le pida.

Los SHA-256 de las descargas están en `docs/sessions/F03.md`.

```bash
# 1. Tabla de NIDs desde PPSSPP v1.20.4 (descarga Core/HLE/*.cpp del commit fijado).
python -I profiles/mpo/tools/build_nid_table.py --fetch \
    analysis/ppsspp-v1.20.4/Core/HLE analysis/nids/ppsspp-v1.20.4.tsv

# 2. Importar, analizar, sembrar funciones y exportar (proyecto en analysis/ghidra/mpo).
export GHIDRA_INSTALL_DIR=~/tools/ghidra_12.1.3_PUBLIC JAVA_HOME=~/tools/jdk-21.0.13+9
profiles/mpo/tools/ghidra/run_analysis.sh game/elf/mgp_main.elf mgp_main 08804000
MPO_ANCHORS=profiles/mpo/config/anchors_modules.txt \
  profiles/mpo/tools/ghidra/run_analysis.sh game/disc/PSP_GAME/USRDIR/modules/sound2.prx sound2 08B24100

# Una etapa (F04): el PRX ya enlazado, con un bloque de relleno donde está mgp_main.
python -I profiles/mpo/tools/rlc.py link --stage game/disc/PSP_GAME/USRDIR/stage/st01/st01.prx \
    --zar game/disc/PSP_GAME/USRDIR/stage/st01/_zar --main game/elf/mgp_main.elf \
    --output analysis/f04/linked/st01
MPO_EXTERNAL_BLOCK="mgp_main 08804000 2F7800" MPO_ANCHORS=profiles/mpo/config/anchors_modules.txt \
  profiles/mpo/tools/ghidra/run_analysis.sh analysis/f04/linked/st01 stage_st01 08B8F900

# 3. Tablas en Markdown (solo números y nombres) para docs/analysis/.
python -I profiles/mpo/tools/ghidra_summary.py analysis/out/mgp_main

# Sin Ghidra: imports y exports de cualquier PRX descifrado.
python -I profiles/mpo/tools/prx_imports.py --nids analysis/nids/ppsspp-v1.20.4.tsv --summary \
    game/disc/PSP_GAME/USRDIR/stage/apscan/apscan.prx
```

- `run_analysis.sh` corre, en una sola sesión headless, los scripts de `tools/ghidra/`:
  - `MpoMetrics.java`: bloques, cobertura y clases de instrucciones (VFPU, FPU, branch-likely,
    `jr`/`jalr`). Se toma después de cada paso de siembra.
  - `MpoSeedFunctions.java`: siembra funciones en tres pasos (`relocations`, `prologues`, `gaps`).
  - `MpoModuleTables.java`: lee `.lib.stub` y `.lib.ent`, nombra los stubs con la tabla de NIDs y
    cuenta las llamadas.
  - `MpoExportRecompInput.java`: funciones, rangos, tablas de saltos y `jalr` resueltos, para el
    recompilador (F06).
  - `MpoFindAnchors.java`: los anclajes de `config/anchors.txt` (cadenas, direcciones e imports).
  - `MpoExternalBlock.java` (opcional, con `MPO_EXTERNAL_BLOCK`): crea un bloque relleno de
    `jr ra; nop`, no ejecutable, donde va otro módulo. Así, las llamadas de una etapa a `mgp_main`
    no cortan el desensamblado.
- Todo lo que generan (el proyecto de Ghidra, los JSON y `*.strings.tsv`) queda en `analysis/`, que
  git ignora. `*.strings.tsv` contiene texto del juego: no se cita en `docs/`.
- Las bases de los módulos de Konami son las que muestra el debugger de PPSSPP (`RESEARCH.md` §4).

## Inventario del GE con frame dumps de PPSSPP (F05)

```bash
python3 -I profiles/mpo/tools/ge_usage.py --format markdown --per-file dumps/
```

- `tools/ge_usage.py` lee los `.ppdmp` de PPSSPP (versiones 2 a 6; snappy o zstd) y genera el
  histograma de lo que usa el GE: primitivas, vtypes, texturas y CLUT, blending, logic ops, stencil,
  alpha y color test, splines y beziers, render targets, render-to-texture y accesos a VRAM. Salida en
  texto, Markdown o JSON, solo con números y nombres.
- `tools/ppdmp.py` (contenedor y snappy) y `tools/zstd_lite.py` (zstd en Python puro) lo acompañan.
  No hace falta instalar nada. Si `zstandard` está disponible (en un venv **fuera del repo**), se
  usa para ir más rápido.
- Los dumps traen datos del juego: se quedan en la carpeta de PPSSPP o en `dumps/` (ignorada por git).
  El método, la guía de captura y los resultados están en `docs/analysis/ge-usage.md`.
- Los tests (`mpo_zstd_lite_test` y `mpo_ge_usage_test` en `ctest`) generan dumps sintéticos y no
  necesitan el juego.

## `psp_analyze` contra Ghidra y datos de análisis para PSPRecomp (F06)

```bash
# 1. Lo que ve PSPRecomp (el mismo analyze_program() de psp_analyze y psp_recomp --auto).
build/profiles/mpo/tools/mpo_analysis_export game/elf/mgp_main.elf 0x08804000 \
    analysis/f06/mgp_main/export.json
# 2. Comparación con la salida de run_analysis.sh (solo números).
python3 -I profiles/mpo/tools/recomp_compare.py --export analysis/f06/mgp_main/export.json \
    --ghidra analysis/out/mgp_main
# 3. Datos de Ghidra como "hints" para PSPRecomp (en la rama mpo del fork, F07b).
python3 -I profiles/mpo/tools/recomp_hints.py analysis/out/mgp_main/mgp_main.recomp.json \
    --extra profiles/mpo/config/hints/mgp_main.txt --output analysis/f06/mgp_main/mgp_main.hints.txt
psp_recomp game/elf/mgp_main.elf --auto generated/mgp_main 0x08804000 \
    --hints analysis/f06/mgp_main/mgp_main.hints.txt
```

- `tools/analysis_export.cpp` (`mpo_analysis_export`): exporta en JSON las semillas, las funciones,
  las entradas de bloque y un censo del decodificador de PSPRecomp. Con el framework parcheado
  acepta un cuarto argumento con hints.
- `tools/recomp_compare.py`: funciones y cobertura de cada lado, por paso de siembra de Ghidra,
  tablas de saltos y lo que el emisor no sabe bajar.
- `tools/recomp_hints.py`: convierte el `recomp.json` de Ghidra al formato de
  `psprecomp::load_analysis_hints()` y le añade los hints escritos a mano de `config/hints/`.
- Los resultados, el diseño y la lista de brechas están en `docs/analysis/psp_analyze.md`. Los
  parches genéricos del framework (MIT) están desde F07b en la rama `mpo` del fork de PSPRecomp
  (`framework/psprecomp`).

## Generar y compilar `mgp_main` recompilado (F07b)

```bash
# 1. Instalado -> análisis -> generated/ (reproducible: generate.json guarda los hashes).
python3 -I profiles/mpo/scripts/generate.py \
    --psp-recomp build/framework/psprecomp/psp_recomp \
    --analysis-export build/profiles/mpo/tools/mpo_analysis_export --coverage
# Volver a generar y exigir el mismo C++ byte a byte:
python3 -I profiles/mpo/scripts/generate.py --psp-recomp build/framework/psprecomp/psp_recomp --check
# 2. Compilar el corpus (118 unidades) y la sonda, con el pool de memoria del framework.
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMPO_GENERATED_DIR=$PWD/profiles/mpo/generated/mgp_main/generated
cmake --build build-rel -j4 --target mpo_aot_probe
# 3. Tabla de dispatch, parches de arranque y module_start hasta el primer import.
build-rel/profiles/mpo/host/recomp/mpo_aot_probe --elf game/elf/mgp_main.elf \
    --hints profiles/mpo/generated/mgp_main/mgp_main.hints.txt \
    --tables build-rel/_deps/ppsspp_vfpu_tables/assets/vfpu
```

- `scripts/generate.py`: comprueba el SHA-256 del ELF instalado contra `config/supported.toml`,
  arma los hints (`tools/recomp_hints.py` con el export de Ghidra y `config/hints/mgp_main.txt`) y
  corre `psp_recomp <elf> --auto <out>/generated 0x08804000 0x4000 --hints … --vfpu-executor`.
  Con `--coverage` mide la cobertura con `tools/recomp_compare.py`. La salida va a `--out`
  (por defecto `profiles/mpo/generated/mgp_main/`, ignorado por git).
- `config/hints/mgp_main.txt` lleva también los `patch` de las tres funciones que el juego convierte
  en `jr ra` al arrancar.
- `host/recomp/` (`mpo_recomp`): `mpo_vfpu` como ejecutor de la VFPU del framework
  (`set_vfpu_executor`). Todo el código generado y el intérprete de respaldo ejecutan la VFPU con
  la semántica de PPSSPP, bit a bit.
- Con `-DMPO_GENERATED_DIR` se compila `mpo_generated_mgp_main` (sin warnings, sin sanitizers,
  `-ffp-contract=off` y `-O${PSPRECOMP_GENERATED_OPT_LEVEL}`), en el pool `psprecomp_generated`
  (`-DPSPRECOMP_GENERATED_JOBS`, por defecto 1 por cada 4 GiB de RAM), y la sonda `mpo_aot_probe`.

### Difftest del emisor (sin el juego, en `ctest`)

`mpo_aot_casegen` coloca los casos del arnés de F07a como código de un ELF sintético,
`psp_recomp --auto --vfpu-executor` los recompila al compilar (como al juego) y `mpo_difftest_aot`
los ejecuta:

```bash
build/profiles/mpo/tests/mpo_difftest_aot --a emitter --b mpo --class all \
    --count 1000 --seed 5066831 --tables <MPO_VFPU_TABLES_DIR>
```

- `--a emitter`: el código generado; `--a interp`: el intérprete de respaldo del framework;
  `--a interp-native`: el intérprete con la VFPU propia del framework (solo para medir).
- `--b mpo` (intérprete de referencia), `--b ppsspp` y `--b unicorn`, como `mpo_difftest`.
- El número de casos por clase y la semilla se fijan al configurar
  (`MPO_AOT_DIFFTEST_COUNT`, `MPO_AOT_DIFFTEST_SEED`).

## Estructura

| Directorio | Contenido | ¿Se versiona? |
|---|---|---|
| `config/` | Hashes soportados (`supported.toml`), entradas del análisis y mapa de funciones | Sí |
| `host/` | HLE específico, overrides de funciones, AP Scan virtual y hooks del renderer | Sí |
| `scripts/` | Instalar y validar, generar y compilar overlays | Sí |
| `tools/` | Scripts de Ghidra y extractores | Sí |
| `tests/` | Tests del perfil que no necesitan el juego | Sí |
| `game/` | La copia del usuario (ISO, EBOOT y PRX descifrados) | **No** |
| `analysis/` | Salidas locales de Ghidra y `psp_analyze` | **No** |
| `generated/` | C++ generado a partir del ejecutable del usuario | **No** |
| `progress/` | Notas locales y benchmarks sueltos | **No** |

Los cuatro últimos están en `.gitignore`. Nunca se suben ISOs, EBOOTs, PRX, `.rlc`, assets ni
capturas (`CLAUDE.md` §1.1).

## Compilar

Desde la raíz del repo:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # PSPRECOMP_PROFILE=mpo por defecto
cmake --build build
ctest --test-dir build --output-on-failure
```

- Con `-DPSPRECOMP_PROFILE=""` se compila solo el framework.
- Configurar descarga de PPSSPP 10 archivos fijados por SHA-256. Sin red, se puede usar
  `-DMPO_PPSSPP_SOURCE_DIR=<checkout>` o `-DMPO_ENABLE_PRX_DECRYPT=OFF`.
