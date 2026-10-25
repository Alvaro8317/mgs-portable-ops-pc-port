# Perfil `mpo` — Metal Gear Solid: Portable Ops

Perfil de PSPRecomp para **MGS: Portable Ops** (base `ULUS-10202`, disco 1.02). Aquí va todo lo que
depende de una dirección, un hash o un layout del juego (`CLAUDE.md` §1.3). El framework
(`framework/psprecomp`) tiene que seguir compilando sin este perfil y sin el juego.

Estado: **F02**. Están el instalador (`mpo --install`), que también descifra el `EBOOT.BIN`, y los
tests de disco y de descifrado con datos sintéticos.

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
