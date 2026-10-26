# ppsspp_cpu — tablas de la VFPU e intérprete de referencia de PPSSPP

Este directorio **no contiene código ni datos de terceros**. Solo tiene recetas de CMake que, al
configurar, copian archivos **sin modificar** de PPSSPP y verifican el SHA-256 de cada uno:

- Origen: <https://github.com/hrydgard/ppsspp>, tag `v1.20.4`
  (commit `fa50bb1976065c4f8b1b47af227d367fe9771555`).
- Se descargan de `raw.githubusercontent.com`, o se copian de `-DMPO_PPSSPP_SOURCE_DIR=<checkout>`
  para compilar sin red. Una diferencia de hash es un error.

| Receta | Qué trae | Destino | Quién lo usa |
|---|---|---|---|
| `ppsspp_vfpu_tables.cmake` | `assets/vfpu/*.dat` (15 tablas, ~5 MB) | `<build>/_deps/ppsspp_vfpu_tables/` | `mpo_vfpu` las carga en tiempo de ejecución (`load_tables`); los tests las reciben con `MPO_VFPU_TABLES_DIR`. Opción `MPO_FETCH_VFPU_TABLES`. |
| `ppsspp_cpu_ref.cmake` | El intérprete (`Core/MIPS/MIPSInt.cpp`, `MIPSIntVFPU.cpp`, `MIPSVFPUUtils.cpp`, `MIPSVFPUFallbacks.cpp`, `MIPSTables.cpp`) y 18 cabeceras | `<build>/_deps/ppsspp_cpu/` | **Solo tests**: `mpo_ppsspp_ref`, que enlazan `mpo_vfpu_golden_gen` y `mpo_difftest`. Nunca el runtime. Opción `MPO_BUILD_PPSSPP_REF` (apagada con MSVC). |
| `extract_mips_tables.cmake` | Copia reducida de `MIPSTables.cpp`: solo las tablas de decodificación y `MIPSGetInstruction()`, sin las columnas del JIT ni del desensamblador | `<build>/_deps/ppsspp_cpu_generated/` | `mpo_ppsspp_ref` |

## Por qué no se versionan

- Las tablas son datos binarios de ~5 MB generados por PPSSPP a partir de mediciones en una PSP.
  Igual que con `ppsspp_crypto`, se fijan por hash en vez de subirlas.
- El intérprete de PPSSPP solo hace falta para generar y comprobar los vectores dorados y como
  referencia del difftest. Lo que el port usa es el código **portado** en
  `profiles/mpo/host/vfpu/` (con su cabecera de origen).

## Licencias

- Todos los archivos son de PPSSPP y llevan "version 2.0 or later versions" en la cabecera; los que
  no tienen cabecera (`ppsspp_config.h`, `Common/BitScan.h`, `Common/Math/SIMDHeaders.h`,
  `Common/Data/Convert/SmallDataConvert.h`, `Core/MIPS/MIPSVFPUFallbacks.*`, las tablas) quedan bajo
  la licencia del proyecto, "GPL 2.0 (and later)" según su README. `Common/BitSet.h` y
  `Common/Math/math_util.h` se declaran de dominio público.
- Todo es compatible con GPL-3.0-or-later. Ningún archivo es GPL-2.0-only.
- Las cabeceras sustitutas de `profiles/mpo/tests/ppsspp_ref/shim/` son propias (GPL-3.0-or-later):
  declaran solo lo que el intérprete necesita (memoria, log, VFS para las tablas) en lugar de traer
  el núcleo de PPSSPP.
