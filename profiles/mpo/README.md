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
  - `rlc.py relocate` escribe la imagen relocalizada de una etapa. Es contenido del juego: va a
    `analysis/` y nunca al repo;
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
- Todo lo que generan (el proyecto de Ghidra, los JSON y `*.strings.tsv`) queda en `analysis/`, que
  git ignora. `*.strings.tsv` contiene texto del juego: no se cita en `docs/`.
- Las bases de los módulos de Konami son las que muestra el debugger de PPSSPP (`RESEARCH.md` §4).

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
