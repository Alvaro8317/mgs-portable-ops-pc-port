# Perfil `mpo` — Metal Gear Solid: Portable Ops

Perfil de PSPRecomp para **MGS: Portable Ops** (base `ULUS-10202`, disco 1.02). Aquí va todo lo que
depende de una dirección, un hash o un layout del juego (`CLAUDE.md` §1.3). El framework
(`framework/psprecomp`) tiene que seguir compilando sin este perfil y sin el juego.

Estado: **esqueleto (F00)**. Por ahora solo hay un smoke test que enlaza contra `psprecomp_core`.

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

Con `-DPSPRECOMP_PROFILE=""` se compila solo el framework.
