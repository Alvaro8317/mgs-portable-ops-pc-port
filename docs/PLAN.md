# Plan por fases

Cada fase se hace en **una sesión limpia**: se ejecuta `/clear` y se arranca con
"Inicia la fase FXX según docs/PLAN.md". El contexto pasa de una sesión a la siguiente **solo** a
través de los archivos del repo: `CLAUDE.md`, este plan, las notas de sesión en `docs/sessions/` y el
código.

## Protocolo de sesión

**Al empezar:**
1. Leer `CLAUDE.md` (se carga solo), este plan y la nota de cierre de la fase anterior
   (`docs/sessions/FXX.md`).
2. Comprobar que se cumplen los **requisitos** de la fase. Si falta algo que depende del usuario (por
   ejemplo, el ISO en `game/`), se le pide antes de empezar.
3. Confirmar el alcance con el usuario en 2 o 3 líneas y empezar.

**Al terminar** (o cuando la sesión se esté quedando sin espacio):
1. Verificar los **criterios de hecho** con evidencia: salida de tests, hashes, capturas locales o
   trazas.
2. Escribir `docs/sessions/FXX.md` a partir de [`docs/sessions/TEMPLATE.md`](sessions/TEMPLATE.md),
   con lo que se hizo, lo que quedó pendiente, las decisiones tomadas y las trampas encontradas.
3. Actualizar en este plan el **estado** de la fase (y de la tabla de abajo). Si la fase no cupo, se
   divide en `FXXa` y `FXXb`. Los hechos nuevos sobre el juego van a `docs/RESEARCH.md` o a
   `docs/formats/`, con su etiqueta [V], [S] o [?].
4. Hacer commit con `/commit`.

**Reglas:**
- No se adelanta trabajo de fases futuras **fuera de las olas** (ver abajo). Lo que se descubra se
  anota en la nota de sesión.
- Una fase puede cambiar el plan, pero el cambio se explica en la nota de sesión.
- Nunca se suben ISOs, EBOOTs, PRX, assets, capturas ni dumps (ver `CLAUDE.md` §1).

## Trabajo en paralelo (olas)

Desde el 2026-10-25, las fases que se pueden probar **sin el juego en marcha** se partieron en una
parte autónoma (`FXXa`) y otra de integración (`FXXb`). Las autónomas se agrupan en **olas** y las de
una misma ola se hacen a la vez, cada una en su propio worktree (un subagente o una sesión aparte).
Las fases sin letra y las `FXXb` siguen en serie, como antes.

**Reglas de una fase en paralelo:**
- **Librería autónoma**: el código va en `profiles/mpo/host/<módulo>/`, como una librería
  `mpo_<módulo>` con su `CMakeLists.txt`. Sus tests van en `profiles/mpo/tests/`. La API es estrecha:
  recibe la memoria de la PSP como puntero o `span` y callbacks. **No** registra NIDs ni llama a
  `register_hle`; eso es trabajo de la `FXXb`.
- **No depende de otra fase de la misma ola.** Si necesita algo de otra, lo anota y lo deja para la
  integración.
- **Solo toca lo suyo**: su módulo, sus tests, su nota `docs/sessions/FXXa.md` y, si añade una
  dependencia, `third_party/` y `THIRD_PARTY.md`. **No edita la tabla de estado** ni las fichas de
  otras fases. Lo que haya que cambiar en el plan va en la sección "Para la próxima sesión" de su nota.
- Cada worktree usa su propio directorio de build y compila con `-j6` como máximo (la máquina tiene
  15 GB de RAM).
- Los datos locales (`game/`, `analysis/`, los dumps) están solo en el checkout principal. Se leen
  con rutas absolutas; cada fase escribe sus salidas locales en `analysis/<fase>/`.
- El código de PPSSPP de referencia está clonado en `~/tools/ppsspp-v1.20.4` (tag `v1.20.4`, commit
  `fa50bb19`), fuera del repo.

**Integración** (la hace la sesión principal cuando termina una ola):
1. Revisar cada rama, mezclarlas una a una y resolver los conflictos (en `CMakeLists.txt`,
   `THIRD_PARTY.md`…).
2. Compilar desde cero y correr `ctest` completo en Linux con Clang y GCC.
3. Actualizar la tabla de estado y copiar a las fichas las notas que dejó cada fase.
4. Hacer commit.

| Ola | Fases | Requisitos |
|---|---|---|
| 1 ✅ | F06, F07a, F08a, F11a, F16a | F03, F04 y F05 (cumplidos). Integrada el 2026-10-25. |
| 2 ✅ | F07b, F10a, F12, F17a, F20a | F07b: F06 y F07a · F12: F11a · F17a: F16a · F10a y F20a: ninguno. Integrada el 2026-10-26. |
| 3 ✅ | F08b, F14a | F08b: F07b y F08a · F14a: F10a y F12. Integrada el 2026-10-26. |
| 4 ✅ | F15a, F18a | F15a: F14a y los 11 dumps de F05 · F18a: ninguno (F03 y F04 para ubicar en las etapas las llamadas a `sceUtility*`). Mientras tanto, la sesión principal hizo F09 en serie. Integrada el 2026-10-26. |
| — | F10b → F11b → F13 → F14b → F15b → F16b → F17b → F18b → F19 → F20b → F21 → F22 | en serie |

**Pendiente de la ola 1 (resuelto)**: en la CI, `mpo_vfpu_golden_test` fallaba **solo con MSVC**
(run `40260589237`). Se corrigió el 2026-10-26 y la CI lo confirma (run `40353663797`: MSVC Release y
Debug pasan). Las causas eran que MSVC:
- convierte en `maxss` la saturación del prefijo D, que pierde el signo de `-0.0`;
- silencia los SNaN constantes al plegarlos;
- en Debug llama a `fabsf` de la UCRT.

El detalle está en `docs/sessions/F07a.md` ("Pendiente y trampas"). Como el emisor de F07b depende de
que `mpo_vfpu` sea bit a bit en Windows, el código generado usa los helpers de `vfpu_math.hpp`.

## Estado

| Fase | Nombre | Hito | Estado |
|---|---|---|---|
| F00 | Esqueleto del repo, PSPRecomp y CI | M0 | ✅ hecha |
| F01 | Lector de ISO, validador e instalador | M0 | ✅ hecha |
| F02 | Descifrado de EBOOT y PRX | M0 | ✅ hecha |
| F03 | Análisis de `mgp_main` en Ghidra | M0 | ✅ hecha |
| F04 | Overlays de etapa y formato `.rlc` | M0 | ✅ hecha |
| F05 | Inventario del GE con dumps de PPSSPP | M0 | ✅ hecha |
| F06 | `psp_analyze` sobre MPO y brechas del framework | M0 | ✅ hecha (ola 1) |
| F07a | Semántica de la VFPU y arnés de difftest | M1 | ✅ hecha (ola 1) |
| F07b | Generación AOT, intérprete y difftest del emisor | M1 | ✅ hecha (ola 2) |
| F08a | Primitivas del kernel sobre minicoro | M1 | ✅ hecha (ola 1) |
| F08b | Kernel HLE: integración, módulos y arranque | M1 | ✅ hecha (ola 3) |
| F09 | IO, tiempo y vblank | M2 | ✅ hecha |
| F10a | Ventana, presentación e input (autónomo) | M2 | ✅ hecha (ola 2) |
| F10b | Presentación e input en el juego | M2 | ⬜ pendiente |
| F11a | GE por software: reproductor de `.ppdmp` y 2D | M2 | ✅ hecha (ola 1) |
| F11b | `sceGe*` en el juego y stub de `sceMpeg` | M2 | ⬜ pendiente |
| F12 | GE por software: completo | M2 | ✅ hecha (ola 2) |
| F13 | Overlays de etapa en runtime | M2 | ⬜ pendiente |
| F14a | Renderer Vulkan: base sobre los dumps | M3 | ✅ hecha (ola 3) |
| F14b | Renderer Vulkan en el juego | M3 | ⬜ pendiente |
| F15a | Renderer Vulkan: blending por shader y efectos (autónomo) | M3 | ✅ hecha (ola 4) |
| F15b | Renderer Vulkan: framebuffers en el juego | M3 | ⬜ pendiente |
| F16a | Audio autónomo: SAS, ATRAC3 y salida SDL3 | M4 | ✅ hecha (ola 1) |
| F16b | Audio en el juego: `sceAudio`, `sceSas` y `sceAtrac` | M4 | ⬜ pendiente |
| F17a | Decodificador de PMF y fuente PGF (autónomos) | M4 | ✅ hecha (ola 2) |
| F17b | Video del título y fuentes en el juego | M4 | ⬜ pendiente |
| F18a | Savedata, OSK y diálogos (autónomo) | M4 | ✅ hecha (ola 4) |
| F18b | Savedata, OSK y diálogos en el juego | M4 | ⬜ pendiente |
| F19 | Primera misión y repetición de input | M5 | ⬜ pendiente |
| F20a | Generador de APs virtuales (autónomo) | M6 | ✅ hecha (ola 2) |
| F20b | AP Scan, red y GPS en el juego | M6 | ⬜ pendiente |
| F21 | Partida completa y estabilidad | M6 | ⬜ pendiente |
| F22 | Empaquetado y release 0.1 | M6 | ⬜ pendiente |
| F23+ | Mejoras (ver al final) | M7+ | ⬜ pendiente |

Símbolos: ⬜ pendiente · 🟨 en curso · ✅ hecha · ⏸️ bloqueada (se explica en la nota de sesión).

---

## M0 — Infraestructura y análisis

### F00 — Esqueleto del repo, PSPRecomp y CI

- **Objetivo**: tener un repo que compile y pase los tests en Linux y Windows, todavía sin el juego.
- **Requisitos**: ninguno.
- **Tareas**:
  - Agregar PSPRecomp como **submódulo** en `framework/psprecomp`, fijado a un commit. Leer su
    `docs/ARCHITECTURE.md`, `PROFILE_GUIDE.md` y `SOURCE_PROVENANCE.md`.
  - Crear un CMake raíz que incluya el framework y `profiles/mpo/` (seleccionado con
    `-DPSPRECOMP_PROFILE=mpo` o el mecanismo equivalente del framework).
  - Crear la estructura vacía de `profiles/mpo/` según `CLAUDE.md` §10, con un `README.md` del perfil.
  - **Hacer que el framework compile en Linux** con Clang y GCC (su perfil VCS documenta solo
    Windows). Los parches genéricos se dejan en MIT y se anotan para proponerlos upstream.
  - Configurar `.clang-format` y un check de cabecera SPDX (script en `tools/`, en Python con estilo
    Google).
  - CI en GitHub Actions: Linux (Clang + GCC, con ASan y UBSan) y Windows (MSVC), ejecutando `ctest`
    sin datos del juego.
- **Criterios de hecho**: `cmake --build` y `ctest` pasan en Linux y Windows (en local o en CI), y el
  check de SPDX pasa.
- **Notas**: decidido, Windows se prueba con MSVC y clang-cl (ver `docs/sessions/F00.md`).

### F01 — Lector de ISO, validador e instalador

- **Objetivo**: un comando `mpo --install <iso>` que valide la copia del usuario y deje lo necesario
  en `game/`.
- **Requisitos**: el usuario pone su ISO de **ULUS-10202** en `game/`.
- **Tareas**:
  - Lector ISO9660 en C++, con el formato CSO como opcional.
  - Leer `PARAM.SFO` (DISC_ID, DISC_VERSION, PSP_SYSTEM_VER).
  - Listar todos los archivos y calcular hashes.
  - Crear `profiles/mpo/config/supported.toml` con el serial, la versión y el SHA-256 del `EBOOT.BIN`
    cifrado y de cada `stage/*/*.prx`. Los hashes no son contenido con copyright y sí se versionan.
  - Si la versión no está soportada, rechazarla con un mensaje claro.
  - Tests con un ISO sintético que genera el propio test.
- **Criterios de hecho**: el ISO del usuario se valida, se rechaza un ISO alterado y la lista de
  archivos del disco queda documentada en `docs/formats/disc-layout.md` (solo nombres y tamaños).

### F02 — Descifrado de EBOOT y PRX

- **Objetivo**: obtener en local el ELF de `mgp_main` y de los PRX de etapa, descifrados.
- **Requisitos**: F01.
- **Tareas**:
  - Integrar `libkirk` y portar la lógica de `PrxDecrypter` de PPSSPP (GPL, con cabecera de origen).
    Este código se usa **solo en el instalador**.
  - Descifrar `EBOOT.BIN` (~PSP) y los `stage/*/*.prx`. Primero averiguar si los PRX de etapa están
    cifrados; puede que no.
  - Comparar el resultado byte a byte contra el volcado "Dump decrypted EBOOT.BIN" de PPSSPP.
  - Revisar qué claves trae `libkirk` y confirmar que no se distribuyen fuera del instalador.
- **Criterios de hecho**: el SHA-256 del ELF descifrado coincide con el volcado de PPSSPP. El nombre
  del módulo (`mgp_main`) y su base de carga quedan verificados ([V] en `RESEARCH.md`).
- **Notas (F01)**: solo hay que descifrar `EBOOT.BIN` (`~PSP`, módulo `mgp_main`). Los 106 PRX de
  etapa y `kjfs.prx`, `sound2.prx` y `zlibdec.prx` ya son ELF sin cifrar. Las 22 librerías de Sony
  (`~SCE`/`~PSP`) probablemente se cubran con HLE, como en PPSSPP. Ver `docs/sessions/F01.md`.
- **Notas (F02)**: las claves no se versionan. libkirk y `PrxDecrypter.cpp` se descargan de PPSSPP al
  configurar, verificados por SHA-256, y solo los enlaza el instalador (ver
  `third_party/ppsspp_crypto/NOTICE.md`). `mpo --install` deja `game/elf/mgp_main.elf`. Ver
  `docs/sessions/F02.md`.

### F03 — Análisis de `mgp_main` en Ghidra

- **Objetivo**: tener el mapa del ejecutable principal y sus números.
- **Requisitos**: F02. Ghidra instalado, con ghidra-allegrex (verificar el repo y la versión).
- **Tareas**:
  - Importar `mgp_main` con relocaciones y correr el autoanálisis.
  - Scripts de Ghidra en `profiles/mpo/tools/ghidra/` (export de funciones, rangos y tablas de saltos
    en el estilo de `ExportRecompInput.java` de gow, y siembra de funciones a partir de las
    relocaciones).
  - Generar la tabla de NIDs a partir de PPSSPP `v1.20.4` y hacer el **inventario de imports por
    librería**.
  - Medir: instrucciones VFPU y prefijos, branch-likely, `jr` y `jalr` sin resolver, y uso de
    `sceKernelIcacheInvalidate*`.
  - Ubicar los puntos de anclaje: el intérprete GCL, `KCEJ_FS`, `KCEJ_SOUND2`, `data.cnf`, el bucle
    principal, el divisor de FPS (el código CWCheat `0x201F5210` es una escritura de 32 bits en el offset
    `0x01F5210` de la RAM de usuario, o sea en `0x089F5210`; hay que verificarlo) y el AP Scan.
- **Criterios de hecho**: `docs/analysis/mgp_main.md` con estadísticas, el inventario de imports, la
  lista de anclajes y el porcentaje de cobertura del desensamblado. Solo números y nombres, nada de
  código del juego. **Decisión sobre `KCEJ_SOUND2`** (recompilar o HLE) tomada o acotada.
- **Notas (F02)**:
  - La entrada es `game/elf/mgp_main.elf` (SHA-256 `ca700ea5…`), que deja `mpo --install`.
  - Es un **PRX relocatable** (`0xFFA0`), no un ELF de dirección fija. Hay que importarlo en Ghidra
    **relocado en `0x08804000`**, que es la base que muestra el debugger de PPSSPP. Tiene relocaciones
    `PT_PRXRELOC` (`0x700000A0`) y 34 secciones con nombre.
  - Con esa base, el `0x089F5210` del código CWCheat cae en el segundo segmento (datos), en la vaddr
    relativa `0x1F1210`. Es una variable, no una instrucción, y falta verificarlo.
  - `KCEJ_SOUND2` es `USRDIR/modules/sound2.prx` (ELF sin cifrar), no código de `mgp_main`. Se
    analiza por separado.
  - `profiles/mpo/tools/prx_info.py` muestra los segmentos y `sceModuleInfo`.

### F04 — Overlays de etapa y formato `.rlc`

- **Objetivo**: entender cómo se cargan y se relocan los PRX de etapa.
- **Requisitos**: F02 y F03.
- **Tareas**:
  - Seguir en Ghidra la cadena de carga: `kjfs`, `sceKernelLoadModule` con sus opciones,
    `mgp_stage`, el uso de `.rlc` y la descarga.
  - Documentar el formato `.rlc` en `docs/formats/rlc.md`, con un parser y tests sintéticos.
  - Hacer el inventario de etapas (cantidad, tamaños y hashes en `supported.toml`) y medir cuánto
    código comparten entre sí.
  - Importar 1 o 2 etapas en Ghidra, relocadas.
- **Criterios de hecho**: el formato `.rlc` está documentado con su parser y tests, el inventario de
  etapas está hecho y el diseño de los overlays para F13 queda esbozado en la nota de sesión.
- **Notas (F01)**: **no hay archivos `.rlc` en el disco**. Primero hay que averiguar de dónde salen
  (dentro de `_zar` o `slot.dat`, generados en memoria o un resto de `host0:`). El inventario de
  etapas ya está en `docs/formats/disc-layout.md` y los hashes en `supported.toml` (106 etapas).
- **Notas (F03)**: ver `docs/analysis/mgp_main.md` §6.4, §6.6 y §8.
  - El proyecto de Ghidra está en `analysis/ghidra/mpo` (`mgp_main`, `sound2`, `kjfs` y `zlibdec`).
    Para importar una etapa en la base que toque:
    `profiles/mpo/tools/ghidra/run_analysis.sh <prx> <nombre> <base>`.
  - **`mgp_main` no usa `sceKernelLoadModule`**:
    - `0x08804CC4` descarga la etapa anterior y carga la nueva con `sceKernelLoadModuleByID` sobre
      un descriptor (`0x08A00030`) que se lee por trozos con `kjfs` (`0x08804C3C` → `0x08804A90`);
    - la ruta sale de `%sstage/%s/`.
  - **El candidato a `.rlc`** está en `0x08805E1C`:
    - pasa un puntero a `sceKernelStartModule(id, 4, &info)` de la etapa;
    - aplica en `0x08805C0C` una tabla de entradas de 8 bytes (tamaño en `0x08A00158`) contra las
      bases de `mgp_main` (`0x08804000`) y de su `.data` (`0x089F2D40`);
    - invalida toda la caché de instrucciones y salta a un puntero que deja la etapa.
  - Las etapas no importan nada de `mgp_main`, así que todo enlace pasa por esa tabla.
  - `prx_imports.py` lista los imports de las 106 etapas sin Ghidra.

### F05 — Inventario del GE con dumps de PPSSPP

- **Objetivo**: saber exactamente qué funciones del GE usa el juego, antes de escribir el renderer.
- **Requisitos**: PPSSPP instalado. El usuario captura frame dumps (`.ppdmp`) en escenas clave:
  título, menú, primera misión, codec, radar y una cinemática.
- **Tareas**:
  - Una herramienta (en Python) que lea los `.ppdmp` y genere un histograma de comandos del GE:
    primitivas, vtypes, formatos de textura y CLUT, modos de blending, operaciones lógicas, stencil,
    alpha y color test, splines y beziers, y render targets.
  - Detectar render-to-texture y lecturas de VRAM.
- **Criterios de hecho**: `docs/analysis/ge-usage.md` con el histograma y una lista de funciones
  priorizadas para F11, F12, F14 y F15. Los dumps quedan **fuera** del repo.

### F06 — `psp_analyze` sobre MPO y brechas del framework

- **Objetivo**: validar PSPRecomp con nuestro ejecutable.
- **Requisitos**: F03 y F04.
- **Tareas**:
  - Correr `psp_analyze` sobre `mgp_main` y una etapa, y comparar sus funciones con las de Ghidra.
  - Diseñar cómo entran los datos de Ghidra (tablas de saltos y funciones sembradas) a PSPRecomp.
  - Hacer la lista de brechas: overlays con `.rlc`, instrucciones no soportadas y VFPU. Cada brecha
    se clasifica como "parche genérico upstream" o "código del perfil".
- **Criterios de hecho**: la diferencia de funciones entre Ghidra y `psp_analyze` está documentada y
  hay un plan de brechas. **M0 cerrado.**
- **Notas (F03)**: ver `docs/analysis/mgp_main.md` §3, §9 y §10.
  - La entrada de Ghidra es `analysis/out/mgp_main/mgp_main.recomp.json`, que contiene 8 484
    funciones con sus rangos, 177 `jr` (uno sin resolver, en `0x088A77C8`, con la tabla
    `0x089EA590`) y los `jalr` resueltos.
  - De esas funciones, 2 649 no las encuentra Ghidra solo: 947 salen de las relocaciones, 1 614 de
    las heurísticas de prólogos y huecos, y 88 del reanálisis posterior. Hay que contrastarlas con
    `psp_analyze`.
  - Brechas candidatas:
    - **módulos residentes múltiples**: `mgp_main` + `zlibdec` + `sound2` + `kjfs`, que se resuelven
      los imports entre sí por NID (71 de Konami en `mgp_main`). Es la decisión de recompilar
      `KCEJ_SOUND2`;
    - **overrides de funciones parcheadas al arrancar**: `0x089CFE80`, `0x089D002C` y `0x089CFF78`
      se convierten en `jr ra`;
    - la VFPU completa: 27 657 instrucciones, 89 mnemónicos y 349 prefijos;
    - los stubs de Konami están dentro de `.text`.
- **Notas (F04)**: ver `docs/formats/rlc.md` y `docs/sessions/F04.md`.
  - Una etapa se analiza a partir de su PRX **ya enlazado** (`rlc.py link`) en `0x08B8F900`. Las
    salidas de Ghidra de `title` y `st01` están en `analysis/out/stage_title` y `stage_st01`.
  - Las etapas llaman a 1 493 funciones de `mgp_main`; 468 no son destino de ningún `jal` de
    `mgp_main` y hay que sembrarlas (`stage_inventory.py`).
  - **Brecha de overlays**: el PSPRecomp fijado (`f6e7d41`) no tiene `unregister_functions`, ni un
    hook de función faltante, ni librerías por overlay. Su `Elf32Image::apply_relocations` sí aplica
    las relocaciones PRX en una base, que es lo que necesita un PRX enlazado.
  - **Tamaño**: los 91 PRX distintos suman 28 veces el `.text` de `mgp_main`, pero solo el 3.7 %
    son funciones distintas. Evaluar la deduplicación (ver el diseño en `F04.md`).

---

## M1 — Arranque

### F07a — Semántica de la VFPU y arnés de difftest (ola 1)

- **Objetivo**: tener la semántica de la VFPU lista y verificada, y un arnés de difftest que F07b
  pueda apuntar al emisor.
- **Requisitos**: ninguno más allá de F03 (conteo de VFPU en `docs/analysis/mgp_main.md`).
- **Tareas**:
  - Librería `mpo_vfpu` en `profiles/mpo/host/vfpu/`: el estado de la VFPU (128 registros, prefijos
    `vpfxs/t/d`, CC) y helpers para las instrucciones complejas (`vsin`, `vcos`, `vrsq`, `vexp2`,
    `vlog2`, `vdiv`, conversiones, `vrot`…), **portados de PPSSPP** (`Core/MIPS/MIPSVFPUUtils.cpp`,
    `MIPSIntVFPU.cpp`, con sus tablas) con cabecera de origen.
  - Priorizar los 89 mnemónicos que usa `mgp_main` (F03), empezando por los más frecuentes.
  - Tests con vectores dorados generados con el propio código de PPSSPP (compilado aparte en un
    programa de prueba, nunca enlazado al runtime) y comparados **bit a bit**.
  - Arnés de difftest de CPU: genera estados aleatorios por clase de instrucción (ALU, extensiones
    Allegrex, FPU, VFPU) y compara dos ejecutores. La referencia es Unicorn (MIPS32) o el intérprete
    de PPSSPP. **Verificar la licencia de Unicorn**: si es GPL-2.0-only, se usa solo como herramienta
    externa (proceso aparte, instalado con pip en un venv), nunca enlazado ni vendorizado.
- **Criterios de hecho**: los mnemónicos VFPU de `mgp_main` tienen helper y test bit a bit, o quedan
  listados como pendientes con su frecuencia. El arnés corre en `ctest` (sin datos del juego) contra
  un ejecutor de prueba.

### F07b — Generación AOT, intérprete y difftest del emisor (ola 2)

- **Objetivo**: `mgp_main` recompilado y compilando en ambos sistemas operativos.
- **Requisitos**: F06 y F07a.
- **Tareas**:
  - `profiles/mpo/scripts/generate.*`: instalado → análisis → `generated/`. Tiene que ser
    reproducible: el comando y los hashes van en la documentación.
  - Compilar en Linux y Windows, ajustando los trabajos en paralelo según la RAM y usando ccache.
  - Registrar el dispatch `addr→fn` y probar el intérprete de respaldo.
  - **Difftest** del emisor contra Unicorn (MIPS32) con entradas aleatorias, por clase de
    instrucción, incluidas las extensiones Allegrex y la FPU. La VFPU se compara contra la semántica
    de PPSSPP.
- **Criterios de hecho**: el binario se genera y compila en Windows y Linux, y el difftest pasa sin
  diferencias en N casos por clase.
- **Notas (ola 1)**: ver `docs/sessions/F06.md`, `docs/sessions/F07a.md` y
  `docs/analysis/psp_analyze.md` §9.
  - **Primero, la base del framework** (`CLAUDE.md` §9, decisión 7):
    - el fork ya existe: `https://github.com/Alvaro8317/PSPRecomp` (creado el 2026-10-25, su
      `main` es `f6e7d41`, igual que el submódulo);
    - armar en local la rama `mpo` (upstream, la capa genérica de Yakumo y
      `framework/patches/0001` y `0002`). **El push de `mpo` al fork lo hace el integrador**, con el
      visto bueno del usuario;
    - apuntar el submódulo a esa rama (URL del fork en `.gitmodules`).

    Sin el parche 0002, `psp_recomp --auto` no termina con `mgp_main`. Sin el 0001, queda sin
    cubrir el 10.6 % del código.
  - Generar con `recomp_hints.py` → `psp_recomp --auto --hints` y medir la cobertura con
    `recomp_compare.py`.
  - Brechas de esta fase:
    - 2: rangos ejecutables sin `.rodata`;
    - 5: intérprete;
    - 8: disposición de unidades;
    - 10: las tres funciones que se parchean a `jr ra`, como `register_function` del perfil;
    - 13: `vi2s`, `vi2uc`, `vcrs.t` y `vsgn`, que ya tienen vectores dorados;
    - 14: el gancho de la VFPU hacia `mpo_vfpu`.
  - VFPU:
    - las funciones que van al gancho son las que usan tablas (`vsin`, `vcos`, `vasin`, `vexp2`,
      `vrexp2`, `vlog2`, `vrcp`, `vnrcp` y `vnsin`);
    - `vrsq`, `vsqrt` y `vdot` ya coinciden con PPSSPP.
  - Difftest:
    - se reusa el arnés de F07a (`profiles/mpo/tests/difftest/`) con un `Executor` que compile los
      casos con el emisor;
    - los NaN se fijan con `vfpu::fadd`/`fmul` o se comparan con `Policy::kNanEquivalent`.
  - Las tablas de la VFPU tienen que llegar al runtime, ya sea junto al binario o embebidas.
- **Notas (ola 2)**: hecha, ver `docs/sessions/F07b.md` y `docs/analysis/psp_analyze.md` §11.
  - El submódulo apunta a `mpo` del fork, con la punta en `1014ddf`.
  - `profiles/mpo/scripts/generate.py` genera `mgp_main` de forma reproducible: 118 unidades y el
    100 % de las palabras de código de Ghidra. Compila con Clang y GCC en unos 17 minutos a `-j4`.
  - El difftest del emisor y del intérprete no da diferencias en 8 clases, con 1 000 y 5 000 casos
    por clase, contra `mpo_vfpu`, PPSSPP y Unicorn.
  - El criterio "compila en Windows" lo cubren `mpo_difftest_emitter` e `mpo_difftest_interpreter`,
    que pasan con MSVC Release y Debug (run `40368991489`), junto con los tests del framework. El
    corpus de `mgp_main` nunca se compila en la CI, porque necesita el juego.

### F08a — Primitivas del kernel sobre minicoro (ola 1)

- **Objetivo**: tener hilos, scheduler y objetos de sincronización de la PSP funcionando y probados,
  sin el juego.
- **Requisitos**: ninguno. **Decidido el 2026-10-25: las fibras usan `minicoro`** (ver `CLAUDE.md`
  §9).
- **Tareas**:
  - Vendorizar `minicoro` (`edubart/minicoro`, fijado a un commit) en `third_party/minicoro/` con su
    LICENSE y registrarlo en `THIRD_PARTY.md`. Cada fibra con **pila reservada de forma explícita**
    (tamaño configurable, generoso por defecto por la trampa de la pila de `CLAUDE.md` §3).
  - Librería `mpo_kernel` en `profiles/mpo/host/kernel/`: un hilo de la PSP es una fibra; scheduler
    de prioridad expropiativa de un solo núcleo que conmuta en las llamadas bloqueantes y en los
    puntos de visibilidad; reloj virtual inyectable para los tests.
  - Objetos: hilos (crear, arrancar, salir, borrar, esperar fin, suspender, cambiar prioridad),
    semáforos, event flags, mutex, mensajes/mbx si hace falta, callbacks, delays y alarmas, FPL y VPL
    y bloques de SysMem (particiones de usuario y kernel). La semántica (orden de despertar, códigos
    de error, timeouts) sale de PPSSPP `Core/HLE/sceKernel*.cpp`.
  - Tests deterministas con hilos sintéticos: orden de ejecución, prioridad, inversión, timeouts,
    códigos de error y estrés. ASan y UBSan en Linux.
- **Criterios de hecho**: `ctest` pasa con Clang y GCC (con sanitizers) y los resultados de los
  escenarios coinciden con lo que dice el código de PPSSPP (citado en la nota). La API queda lista
  para que F08b registre los NIDs.

### F08b — Kernel HLE: integración, módulos y arranque (ola 3)

- **Objetivo**: que el juego llegue a `module_start` y cree sus hilos.
- **Requisitos**: F07b y F08a.
- **Tareas**:
  - Conectar `mpo_kernel` con el contexto de CPU de PSPRecomp y registrar los NIDs `sceKernel*`.
  - Módulos (`module_start`, `StartModule`) y los módulos residentes de Konami.
  - Si un NID no está implementado, que falle de forma visible. Añadir el histograma de llamadas HLE.
- **Criterios de hecho**: hay una traza del arranque hasta que los hilos se crean y ejecutan, y la
  lista de imports pendientes está ordenada por frecuencia. **M1 cerrado.**
- **Notas (ola 1)**: la API está en `profiles/mpo/host/kernel/kernel.hpp`; ver
  `docs/sessions/F08a.md`.
  - Implementar `HostHooks` (`run_thread`, `call_guest` y `on_switch`).
  - `Runtime::run()` de PSPRecomp no sabe de fibras, así que hace falta un **bucle de dispatch por
    hilo**. Puede ser un parche genérico.
  - Los puntos de visibilidad llaman a `kernel.preemption_point()`.
  - Al salir de un hilo, su fibra se abandona sin desenrollar la pila. Ningún frame que cruce una
    llamada HLE puede tener objetos que sean dueños de memoria.
  - Los imports entre módulos de Konami (71 en `mgp_main` y 3 en `kjfs`) se resuelven con
    `register_hle`, saltando al export (F06).
  - El `$gp` de `mgp_main` es `0x08A07E90`.
  - Quedan para esta fase o para F09: ModuleMgr, InterruptManager, UtilsForUser, sceSuspendForUser
    y `ExitGame`.
- **Notas (ola 2)**: ver `docs/sessions/F07b.md`.
  - Se genera con `profiles/mpo/scripts/generate.py` y se compila con `-DMPO_GENERATED_DIR`.
  - El punto de partida es `mpo_aot_probe --stub-imports 1`. Con eso `module_start` ya vuelve, y el
    siguiente paso es el hilo `0x088041FC`.
  - Antes de ejecutar nada:
    - se cargan las tablas (`mpo::vfpu::load_tables`); sin ellas, `vsin` y compañía abortan;
    - se instala el ejecutor de la VFPU (`mpo::recomp::install_vfpu_executor()`).
  - **Falta generar `sound2`, `kjfs` y `zlibdec`** con `--prefix <módulo>` (para no romper la tabla
    densa de `mgp_main`) y con `--vfpu-executor`. Hoy `generate.py` solo genera `mgp_main`.
  - Hay que medir el arranque y la memoria al registrar las ~482 000 entradas.
  - Para generar, usar un `psp_recomp` de Release: con Debug y ASan tarda más de 10 minutos.
- **Notas (ola 3)**: hecha, **M1 cerrado**. Ver `docs/sessions/F08b.md`.
  - `mgp_main` arranca sobre `mpo_hle` (`profiles/mpo/host/hle/`, arnés `mpo_boot`):
    - `module_start` crea `user_main`, y este crea `Main`; los tres corren recompilados, con 0
      instrucciones interpretadas;
    - para de forma visible en el primer import sin HLE, `sceRtcGetCurrentClockLocalTime` (F09);
    - con `--unimplemented continue` carga `zlibdec`, `sound2`, `kjfs` y un `libfont` falso en las
      mismas bases que PPSSPP, y crea y corre los hilos `preseek`, `dbr`, `arc`,
      `snd_thread_seq` y `snd_thread_main`.
  - Cada hilo es una fibra con su propio bucle de dispatch (`Runtime::run_until`, parche genérico en
    `mpo`).
  - El arreglo de los slots de import (`962e407`) evita que el slot N de un corpus con `--prefix`
    ejecute el HLE del slot N de `mgp_main`.
  - `generate.py --module` genera `zlibdec`, `sound2` y `kjfs` (`profiles/mpo/config/modules.toml`).
  - Registrar las 521 972 entradas (481 916 de `mgp_main`) tarda 60–66 ms y sube el RSS en 178 MiB
    (pico de 199 MiB).
  - Quedan 164 imports pendientes, con la tabla por frecuencia en la nota. Los más llamados son
    `sceAudioOutputPannedBlocking` y `__sceSasCoreWithMix`; el de más sitios es
    `sceRtcGetCurrentTick` (39).
  - `mpo_hle_test` corre en la CI sin datos del juego. `mpo_boot` solo se compila con el corpus.
  - Al integrar, `mpo_hle_test` caía solo en MSVC Debug: los iteradores comprobados que quedan vivos
    en la pila de una fibra abandonada (un hilo que sale desde el HLE) apuntan a memoria liberada.
    Se arregló en el perfil (`2f7bfd1`) y en `mpo` (`b0f1937`). La regla está en la nota, y la CI
    tiene un job de Linux con `-D_GLIBCXX_DEBUG` (`gcc checked-iterators`) que destapa esta clase
    de fallo.

---

## M2 — Pantalla de título y primera etapa

### F09 — IO, tiempo y vblank

- **Objetivo**: que el juego lea sus datos y que el tiempo avance correctamente.
- **Requisitos**: F08b.
- **Tareas**:
  - `sceIo*` (síncrono y asíncrono) sobre `disc0:/`, leyendo desde el ISO.
  - `ms0:/` mapeado a una carpeta de usuario por sistema operativo.
  - `sceUmd*`.
  - `sceRtc`, `sceKernelGetSystemTime*`, `sceDisplay` (vblank a 59.94 Hz y `WaitVblank*`) y
    `sceKernelDelayThread`.
  - Revisar si `KCEJ_FS` y `data.cnf` funcionan con todo esto.
- **Criterios de hecho**: la traza muestra al juego abriendo y leyendo `slot.dat` y los `_zar` sin
  errores, con el vblank estable.
- **Notas (ola 1)**: el tiempo del kernel sale del `Clock` inyectado de `mpo_kernel`. El
  vblank se programa con `schedule_event` y se marca con `set_in_interrupt` (`docs/sessions/F08a.md`).
- **Notas (ola 3)**: ver `docs/sessions/F08b.md`. El arnés es `mpo_boot`.
  - Hay que reemplazar:
    - los stubs de `profiles/mpo/host/hle/hle_stubs.cpp`: `BootIo`, síncrona y de solo lectura desde
      `game/disc/`, y la UMD siempre lista. `sceKernelLoadModuleByID` lee con `BootIo::file()`, así
      que la IO nueva le tiene que dar un acceso equivalente;
    - el tiempo stub de `System`: un `VirtualClock` que salta al siguiente evento y avanza 100 µs
      cada 4096 dispatches.
  - Hay que implementar:
    - `InterruptManager`, con el vblank y los handlers de `sceKernelRegisterSubIntrHandler`;
    - `sceRtc*`, `sceDisplay*` y los `Libc*` de `UtilsForUser`;
    - la IO asíncrona y de directorios de `kjfs` (`sceIoOpenAsync`, `ReadAsync`, `WaitAsyncCB`,
      `sceIoDopen` y `Dread`).
  - El primer import sin HLE es `sceRtcGetCurrentClockLocalTime` (en `Main`). Después vienen
    `sceOpenPSIDGetOpenPSID`, `sceCtrlSetSamplingMode`, `sceUtilityGetSystemParamInt`,
    `sceImposeSetLanguageMode` y `sceDisplayGetAccumulatedHcount`, antes de cargar los módulos.
  - Un NID nuevo es una línea: `HleTable::add(lib, nid, nombre, HleKind::kImplemented, handler)`.
- **Notas (ola 4)**: F09 se hace en la sesión principal mientras corren F15a y F18a.
  - **`ms0:/`**: F09 lo mapea a un directorio que se pasa por opción (`mpo_boot --ms0 <dir>`), sin
    decidir la carpeta por defecto de cada sistema operativo. Esa carpeta la resuelve `mpo_savedata`
    (F18a), y se conecta en F18b. En las pruebas, `ms0:/` apunta a `analysis/f09/ms0/`, nunca a una
    carpeta de saves real.
  - Los imports triviales que cortan el arranque antes de que el juego lea datos
    (`sceOpenPSIDGetOpenPSID`, `sceCtrlSetSamplingMode`, `sceUtilityGetSystemParamInt`,
    `sceImposeSetLanguageMode`) se cubren aquí como `stub`, con los valores por defecto de PPSSPP,
    para poder llegar a `slot.dat`. Se completan en F10b (ctrl) y F18b (parámetros del sistema).

- **Notas (F09)**: hecha, ver `docs/sessions/F09.md`.
  - La IO (`mpo_io` + `IoManager`), el tiempo (`GameClock`, `sceRtc`), el vblank (`Display`) y las
    interrupciones (`Interrupts`) son `hle`.
  - Con el intérprete y `--unimplemented continue`, el juego lee del ISO sin errores los módulos,
    el árbol del disco, las etapas `init`, `r_main01` y `title` y el video de la intro, con 1 198
    vblanks en 20 s.
  - **`slot.dat` todavía no**: el juego espera Start en el bucle del título. Pasa a criterio de
    F10b y F11b.
  - Para llegar ahí hizo falta mapear el scratchpad en el framework (`92c9425` en `mpo`) y un
    `sceAudio` mínimo como `stub` (la salida bloqueante espera lo que duran sus muestras).

### F10a — Ventana, presentación e input, autónomo (ola 2)

- **Objetivo**: una ventana SDL3 que presente un framebuffer de la PSP y lea los controles, sin el
  juego.
- **Requisitos**: ninguno.
- **Tareas**:
  - SDL3 (fijado, con FetchContent o vendorizado) y ventana con presentación por blit de CPU de un
    framebuffer en memoria de la PSP: formatos 565, 5551, 4444 y 8888, con stride.
  - Input con SDL3 (gamepad y teclado, stick analógico) traducido al formato de `SceCtrlData`, con
    mapeo configurable.
  - Probarlo con los framebuffers que deja F11a al reproducir los dumps.
- **Criterios de hecho**: en WSLg o en Windows se ve un framebuffer de prueba en los cuatro formatos
  y una traza muestra el input traducido.
- **Notas (ola 2)**: hecha, ver `docs/sessions/F10a.md`.
  - `MPO_SDL3_AUDIO_ONLY` vale `OFF` por defecto. Un build viejo lo tiene en `ON` en la caché, así
    que hay que reconfigurar con `-DMPO_SDL3_AUDIO_ONLY=OFF`.
  - La CI de Linux instala `libx11-dev` y `libxext-dev`. Cada extensión de X11 se activa solo si
    está su cabecera.
  - **Mando real probado por el usuario** (2026-10-26): un Xbox 360 en Windows. Todos los botones,
    los gatillos (umbral 0,75) y el stick salen bien, y al desconectarlo se sueltan los botones
    (`analysis/f10a/f10a-mando*.txt`, en local).
  - **Pendiente del usuario**: ver a ojo los framebuffers de prueba (procedimiento en la nota).

### F10b — Presentación e input en el juego

- **Objetivo**: mostrar el framebuffer de la PSP en una ventana y recibir controles.
- **Requisitos**: F09 y F10a.
- **Tareas**:
  - `sceDisplay` (`SetFrameBuf`) conectado a la presentación de F10a.
  - `sceCtrl` (`ReadBuffer*`, `PeekBuffer*`, modo analógico) conectado al input de F10a.
- **Criterios de hecho**: algo dibujado por el juego aparece en la ventana (aunque el GE esté
  incompleto) y el input llega a `sceCtrlReadBuffer*`.
- **Notas (ola 2)**: la API de F10a es `mpo_frontend` (`GuestMemory`, `ConvertFramebuffer`,
  `InputTranslator`, `SceCtrlData`) más `mpo_frontend_sdl` (`SdlPresenter`, `SdlInput`,
  `LoadInputMapping`). El mapeo por defecto está en `profiles/mpo/config/input.toml`. Ver
  `docs/sessions/F10a.md`.
  - `sceDisplaySetFrameBuf` guarda un `FramebufferDesc`, que se aplica en el vblank y se presenta
    con `SdlPresenter::Present`.
  - `sceCtrl` llama a `Sample()` en cada vblank y aplica encima lo suyo: el modo digital (el stick a
    128), los latches y el anillo de 64 buffers, como `__CtrlUpdateLatch` de PPSSPP.
  - Falta decidir `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` (sin él, SDL descarta el mando cuando
    la ventana no tiene foco) y de dónde se carga el `input.toml` del usuario.
- **Notas (F09)**: ver "Para la próxima sesión" en `docs/sessions/F09.md`.
  - El flip llama a `Display::set_flip_handler`; el framebuffer que se aplica está en
    `Display::framebuffer()`.
  - `sceCtrlReadBufferPositive` es lo primero que pide `Main` en cada frame del título.
    `sceCtrlSetSamplingMode` es un `stub` en `hle_system.cpp`.
  - **Criterio heredado de F09**: con Start pulsado (un input grabado), la traza muestra al juego
    abriendo y leyendo `slot.dat` sin errores.

### F11a — GE por software: reproductor de `.ppdmp` y 2D (ola 1)

- **Objetivo**: renderizar por software los dumps 2D de F05 (logo de KP y menú) igual que PPSSPP.
- **Requisitos**: F05. Los 11 dumps en `/mnt/c/Users/eduar/Documents/PPSSPP/PSP/SYSTEM/DUMP/`.
- **Tareas**:
  - Librería `mpo_ge` en `profiles/mpo/host/ge/`: estado del GE (registros), intérprete de comandos
    sobre una memoria de PSP (RAM y VRAM con sus espejos) y salida a un framebuffer en VRAM.
  - Decodificador de vértices para todos los vtypes que salen en F05 (`ge_usage.vertex_size()` es la
    referencia del tamaño).
  - Rasterizador por software **portado de softgpu de PPSSPP** (`GPU/Software/`), con cabeceras de
    origen: modo through, sprites y triángulos, texturas CLUT4/CLUT8 con paleta 8888, swizzle,
    `MODULATE` y color doubling, alpha test, los modos de blending de F05, dither y salida 5551.
  - Reproductor de `.ppdmp` en C++ (lector del contenedor v6 con zstd, como `ppdmp.py`) que carga la
    memoria, ejecuta la lista y guarda el framebuffer como PNG en `analysis/f11a/` (fuera del repo).
  - Tests con dumps sintéticos (el `DumpBuilder` de F05 sirve de guía), sin datos del juego.
- **Criterios de hecho**: los dumps del logo y del menú se renderizan; la comparación contra
  capturas del renderer por software de PPSSPP (cuando el usuario las aporte) queda descrita en la
  nota, con la diferencia por píxel. Los dumps 3D se ejecutan sin crash aunque se vean incompletos.

### F11b — `sceGe*` en el juego y stub de `sceMpeg`

- **Objetivo**: ver el logo y el menú.
- **Requisitos**: F10b y F11a.
- **Tareas**:
  - `sceGe*`: cola de listas, stall, signals, finish y callbacks, y `EdramSetAddrTranslation`,
    conectados a `mpo_ge`.
  - Estructura de listas que no guardan los dumps (`CALL`, `JUMP`, `SIGNAL`, `FINISH`, `BASE`,
    `BOUNDINGBOX`…).
  - Stub mínimo de `sceMpeg`/`scePsmf` (decidido en F05):
    - no decodifica: entrega frames en negro en el buffer de display, o lo deja como está;
    - informa bien el estado y el fin del stream, para que el juego pase la intro y el "Press Start"
      y acepte Start;
    - cada NID del stub queda en el histograma HLE.
- **Criterios de hecho**: se ven el logo de Kojima Productions, la pantalla de título (con el fondo de
  video en negro por el stub) y el menú. Las capturas comparadas contra PPSSPP quedan en local y se
  describen en la nota de sesión. **M2 parcial.**
- **Notas (F05)**: la lista de lo que hay que cubrir está en `docs/analysis/ge-usage.md` §5 (F11).
  - El logo y el menú son 2D en modo through, con CLUT4/8, swizzle, `MODULATE` ×2, alpha test,
    2 modos de blending y salida 5551.
  - La intro y el "Press Start" no usan el GE: son tres framebuffers 8888 que rotan, seguramente
    escritos por `sceMpeg` [?]. El display tiene que presentar 8888 además de 5551.
  - Los frame dumps no guardan la estructura de las listas (`CALL`, `JUMP`, `SIGNAL`, `FINISH`,
    `BASE`, `BOUNDINGBOX`…). Esto se mide con el histograma HLE.
  - `ge_usage.vertex_size()` coincide con los dumps reales y sirve de referencia para el
    decodificador.
- **Notas (ola 1)**: el GE es `mpo_ge` (`profiles/mpo/host/ge/`); la API está en
  `docs/sessions/F11a.md`.
  - Una sola instancia de `Ge`, con una `MemoryView` sobre la `GuestMemory` del runtime. La cola de
    listas, los IDs, los callbacks y `ListSync`/`DrawSync` van en el HLE.
  - `sceGeGetMtx` necesita los valores de las matrices "visibles por la CPU" (`matrixVisible` en
    PPSSPP), que `GeState` todavía no guarda.
  - El envoltorio de display lists de `mgp_main` (`0x089C5AD8–0x089C6D50`) llama a `sceGe_user`
    desde la misma unidad que los stubs. Sin el parche 0002 de F06, esas llamadas nunca llegan al
    HLE.

- **Notas (F09)**: el primer import sin HLE con `--interpreter 0` es `sceGeEdramGetAddr` (en
  `0x089C5B08`). En el bucle del título, `Main` encola unas 32 listas por segundo
  (`sceGeListEnQueue`, `UpdateStallAddr` y `DrawSync`).

### F12 — GE por software: completo (ola 2)

- **Objetivo**: que el renderer de referencia cubra todo lo que usa el juego, según F05.
- **Requisitos**: F11a. Se trabaja sobre los dumps con el reproductor de F11a, sin el juego.
- **Tareas**:
  - Texturas: CLUT 4 y 8 bits, swizzle, mipmaps y DXT si aparece en F05.
  - Blending con colores fijos, color doubling, alpha y color test, operaciones lógicas, stencil y
    clamp de profundidad.
  - **Líneas** (el `DrawLine` de softgpu).
  - **Splines y beziers con skinning.**
  - Render-to-texture.
- **Criterios de hecho**: el histograma de F05 queda cubierto al 100 % y las escenas de los dumps se
  ven igual que en PPSSPP.
- **Notas (F05)**: la lista de comprobación es la sección `features` de `ge_usage.py` sobre los dumps
  (`docs/analysis/ge-usage.md` §5, F12). PPSSPP reproduce esos mismos dumps si se abren como un juego
  (**File › Load**), así que sirven para comparar.
  - Lo más importante:
    - los **splines** 4×4, división 6×6 y tipo 3×3, **sin skinning**, que son el 49 % de las draws;
    - depth `GEQUAL` con rango `0x2710-0xC350` y depth clamp;
    - fog y dither;
    - skinning de 8 bits e iluminación;
    - `REVERSE_SUBTRACT`;
    - líneas.
  - **No aparecen** y se dejan fuera hasta que una captura los muestre: mipmaps, DXT, logic ops,
    stencil, color test, beziers, splines con skinning y render-to-texture.
- **Notas (ola 1)**:
  - El banco de pruebas es `mpo_ppdmp_replay` sobre los 11 dumps. Lo que falta lo lista
    `Stats::unsupported`, y `ge_frame_diff.py` compara contra las capturas de PPSSPP.
  - En los dumps 3D faltan:
    - el modo transform;
    - los splines (409 y 87 en las cinemáticas);
    - el skinning;
    - las 120 líneas del radar;
    - el test de BOUNDINGBOX, que hoy siempre responde "visible".
- **Notas (ola 2)**: hecha, ver `docs/sessions/F12.md`. Los 4 dumps 3D quedan con 0 comandos
  `unsupported` y los 23 `features` de `ge_usage.py` tienen camino de código.
  - **Comparación por píxel contra PPSSPP hecha** (2026-10-26): con las capturas del usuario de
    `mission1_1` y `cutscene_1`, el renderer por software da **0 píxeles distintos** con
    tolerancia 0 en las dos.
  - `[CenteredLines]` no aplica a MPO en v1.20.4, y softgpu no lo usa [V].
  - Morphing, DXT y mipmaps quedan como `unsupported` visibles.
  - **[?] Splines de un solo parche abiertos por los dos lados** (los 1 942 de MPO): los pesos de
    PPSSPP son asimétricos y no coinciden con los de un B-spline sujeto. Se reproducen tal cual;
    falta saber qué hace el hardware.
  - Los dumps están en `/mnt/c/Users/eduar/Documents/PPSSPP/PSP/SYSTEM/DUMP/`, no en `game/`.

### F13 — Overlays de etapa en runtime

- **Objetivo**: entrar a la primera etapa.
- **Requisitos**: F11b, F12 y F04.
- **Tareas**:
  - Implementar con HLE `sceKernelLoadModule` y `UnloadModule` para los `stage/*.prx`, con
    relocación (`.rlc`).
  - Recompilar cada etapa como una **librería compartida** identificada por hash, con un script
    `build_overlays`.
  - Instalar y desinstalar los overlays (`unregister_functions`), con el intérprete como respaldo.
- **Criterios de hecho**: se entra a la primera etapa (aunque sea sin audio) y en el reporte
  final no queda código interpretado en esa etapa. **M2 cerrado.**
- **Notas (ola 2)**: `mpo_ge` ya evalúa `BOUNDINGBOX`/`BJUMP` (F12), pero solo con datos sintéticos,
  porque los dumps no los guardan. Hay que verificarlo con el runtime.
- **Notas (ola 3)**: ver `docs/sessions/F08b.md`.
  - Los corpus de etapa con `--prefix` dependen del arreglo de los slots de import (`962e407` en
    `mpo`).
  - El cargador (`ModuleManager::load` y `unload_module` en `profiles/mpo/host/hle/modules.cpp`) ya
    enlaza los exports y libera la memoria. Falta registrar y desregistrar la librería del overlay y
    la tabla por hash.
- **Notas (F04)**: el diseño de los overlays está en `docs/sessions/F04.md`.
  - `mgp_main` usa `sceKernelLoadModuleByID(fd, 0, NULL)` (no `sceKernelLoadModule`) sobre un
    descriptor de `kjfs`. Después llama a `sceKernelStartModule(id, 4, &p)` y aplica el `.rlc` él
    mismo (`0x08805C0C`). No hay que hacer HLE del `.rlc`, sino respetar ese contrato.
  - Hay una sola etapa a la vez, siempre en `0x08B8F900` (`title` y `st02` verificadas; falta un
    menú). La más grande ocupa `0xFC400` bytes.
  - Para elegir la librería del overlay, se identifica por el SHA-256 de `<s>.prx` y de `<s>.rlc`
    (91 distintas).
- **Notas (ola 1)**:
  - Los ganchos de overlays y `--prefix` vienen de la capa de Yakumo (brechas 6 y 7 de F06). Ya
    están en `mpo` (F07b). Un salto a una dirección de una unidad que no es entrada ahora para con el
    PC correcto.
  - Con un solo `Runtime`, cargar una etapa apaga la vía rápida para todo (brecha 8). Solo afecta
    al rendimiento.
  - Las 464 entradas de `mgp_main` que solo usan las etapas ya están sembradas por Ghidra.
- **Notas (F09)**: las etapas pequeñas no siempre caen en `0x08B8F900`. `init` y `r_main01` cargan
  en `0x08AFB800`, el primer hueco libre detrás de `mgp_main`; `title`, en `0x08B8F900`. La tabla
  de overlays tiene que aceptar la base que le toque a cada etapa, y el corpus de cada una se
  genera para su base real.

---

## M3 — Renderer Vulkan

### F14a — Renderer Vulkan: base sobre los dumps (ola 3)

- **Objetivo**: dibujar los dumps de F05 en la GPU igual que el renderer por software.
- **Requisitos**: F10a y F12. Comprobar antes que hay un driver Vulkan usable (en WSL2 puede no
  haberlo; si no, se hace en Windows).
- **Tareas**:
  - Infraestructura: volk, VMA, instancia y dispositivo (Vulkan 1.2 como mínimo, aprovechando 1.3 si
    está), swapchain, frames en vuelo y capas de validación.
  - Shaders: generar vertex y fragment shaders a partir del estado del GE, compilar a SPIR-V y
    mantener una caché de pipelines.
  - Datos: subir vértices y texturas, y convertir CLUT en el shader.
  - Modo del reproductor de dumps con `--renderer=soft|vulkan`.
- **Criterios de hecho**: los dumps de F05 se ven en Vulkan sin errores de validación y la
  diferencia contra el renderer por software queda medida.
- **Notas (ola 1)**: el banco es `mpo_ppdmp_replay` con `--renderer=soft|vulkan`, y el
  renderer por software es la referencia para comparar frames con `ge_frame_diff.py`.
- **Notas (ola 2)**: la referencia es `mpo_ge` (F12).
  - La teselación de splines de `spline.cpp` se puede reutilizar. Ojo con la asimetría de los
    splines de un solo parche ([?] en `docs/sessions/F12.md`).
  - El depth clamp de Vulkan no es el del GE: el GE recorta solo contra -Z y marca "fuera de rango"
    los vértices más allá de la guard-band (`transform.cpp`).
  - La ventana es de `SdlPresenter` (F10a), con `SDL_VULKAN` encendido. Para
  Vulkan, la ventana se crea con `SDL_WINDOW_VULKAN`, o se separa la creación de la ventana del
  renderer de SDL.
- **Notas (ola 3)**: hecha, ver `docs/sessions/F14a.md`.
  - `mpo_vulkan` (`profiles/mpo/host/vulkan/`) es un `DrawBackend` de `mpo_ge`
    (`Ge::SetDrawBackend`). `mpo_ge` hace la transformación hasta coordenadas de pantalla y la GPU
    solo rasteriza y sombrea. Por eso el depth clamp y la asimetría de los splines se heredan tal
    cual.
  - Los shaders son un ubershader GLSL precompilado a SPIR-V con 20 specialization constants. El
    SPIR-V se versiona (`ge_shaders_spv.inc`) y `mpo_vulkan_shaders_fresh` comprueba que esté al
    día; ni el build ni la CI necesitan glslang.
  - Los 11 dumps se ven con `--renderer=vulkan` sobre lavapipe con 0 errores de validación, también
    con la de sincronización y en el camino de 1.2 (`MPO_VK_NO_13=1`). La swapchain funciona en
    WSLg.
  - Diferencia contra `soft` (`ge_frame_diff.py`):
    - 0 % en el logo y los dumps de video;
    - 0.5 % en las misiones y 0.9 % en el menú;
    - ~21 % en los codecs y 20–35 % en las cinemáticas.

    Casi siempre es un escalón de 5 bits, y solo en dibujos con blending: el blending fijo redondea
    distinto que softgpu y falta el dither posterior.
  - Desde la integración de la ola 3, la CI de Linux instala lavapipe (`mesa-vulkan-drivers`) y
    las capas de validación, y corre `mpo_vulkan_renderer_test` con `MPO_VK_REQUIRE_VALIDATION=1`.
  - `mpo_vulkan_shaders_fresh` fallaba en Windows porque el checkout con `core.autocrlf` deja los
    shaders en CRLF. `compile_shaders.py` hashea las fuentes normalizadas a LF (`d7e779e`).
  - **Probado en una GPU real** (2026-10-26, `analysis/f14a/win-summary.md`, en local): RTX 5070 Ti
    con MSVC 14.51. Los 11 dumps dan 0 errores de validación con 1.3 y con 1.2, y `ctest -R vulkan`
    pasa sin saltarse. Contra `soft`, NVIDIA queda igual o más cerca que lavapipe (el mismo escalón
    de 5 bits del blending); solo 2 píxeles de `cutscene_2` pasan de delta 25, en una arista.

### F14b — Renderer Vulkan en el juego

- **Objetivo**: dibujar las mismas escenas que el renderer por software, pero en la GPU.
- **Requisitos**: F13 y F14a.
- **Tareas**:
  - Conectar el renderer Vulkan a `sceGe*` y a la presentación, con `--renderer=soft|vulkan`.
- **Criterios de hecho**: el título y la primera etapa se ven en Vulkan sin errores de validación,
  en Windows y Linux.
- **Notas (F05)**: el estado visto es pequeño: 11 vtypes, CLUT4/8 con paleta 8888, 3 modos de
  blending y 3 de profundidad. Todo el blending cabe en el blending fijo de Vulkan. Falta decidir el
  destino 5551 con dither (ver `docs/analysis/ge-usage.md` §5, F14).
- **Notas (ola 3)**: el renderer es `mpo::vk::GeRenderer` (`profiles/mpo/host/vulkan/`), conectado
  con `Ge::SetDrawBackend`. La ventana y la swapchain están en `mpo_vulkan_sdl`
  (`SwapchainPresenter`). Ver "Para F14b" en `docs/sessions/F14a.md`.

### F15a — Renderer Vulkan: blending por shader y efectos, autónomo (ola 4)

- **Objetivo**: que `--renderer=vulkan` salga igual que `--renderer=soft` en los 11 dumps de F05,
  sin el juego.
- **Requisitos**: F14a. Los 11 dumps en `/mnt/c/Users/eduar/Documents/PPSSPP/PSP/SYSTEM/DUMP/`.
- **Tareas**:
  - **Blending por shader** con la aritmética entera de softgpu (`((2c+1)(2f+1)) >> 10`, truncado) y
    el **dither después** del blending, leyendo el destino en el fragment shader:
    - `VK_EXT_rasterization_order_attachment_access` o `VK_EXT_fragment_shader_interlock` donde
      estén; si no, input attachment con dependencia propia o una copia del target por lote (este
      último camino tiene que existir siempre, porque es el que funciona en cualquier GPU);
    - con el blending en el shader caen solos los casos que F14a hoy aproxima: factores dobles, dos
      colores fijos distintos, `ABS_DIFF` y máscara de color por bit;
    - el blending fijo se queda como vía rápida cuando el resultado es exacto (sin blending o sin
      redondeo que se note), detrás de un toggle.
  - **Operaciones lógicas** en el shader, con el mismo camino de lectura del destino (no aparecen en
    F05; tests sintéticos contra softgpu).
  - **Clamp de profundidad**: comprobar con tests sintéticos que el clamp y la guard-band de `mpo_ge`
    llegan bien a la GPU (Z fuera de 0-65535, vértices más allá de la guard-band, `GEQUAL` con el
    rango de F05), y que el `depthClamp` de Vulkan no cambia nada cuando no está.
  - **Herramienta de comparación** `soft` contra `vulkan` con un umbral: reproduce cada dump con los
    dos renderers y falla si la diferencia pasa del umbral (píxeles distintos y delta máxima por
    canal). Puede ser un modo de `mpo_ppdmp_replay` o un script sobre `ge_frame_diff.py`. Tiene que
    servir para la prueba en Windows y para F15b.
  - **Toggles A/B por variable de entorno** para cada optimización (`MPO_VK_*`, como
    `MPO_VK_NO_13`): blending fijo frente a shader, cada camino de lectura del destino, la
    agrupación de draws y la caché de texturas. Se documentan en la nota.
- **Criterios de hecho**:
  - en los 11 dumps, la diferencia con `soft` está por debajo del umbral en lavapipe, en los
    caminos 1.3 y 1.2 y con 0 errores de validación (incluida la de sincronización). El objetivo es
    0 píxeles distintos; el umbral por defecto (pensado para GPUs reales) se fija y justifica en la
    nota;
  - logic ops y clamp de profundidad tienen tests sintéticos que pasan contra softgpu;
  - `ctest` pasa con Clang (ASan y UBSan) y GCC, y `mpo_vulkan_renderer_test` sigue corriendo en la
    CI con lavapipe;
  - el procedimiento de la prueba en una GPU real de Windows (F14a) se amplía con la herramienta
    nueva.
- **Notas (ola 4)**: es un cambio dentro de `profiles/mpo/host/vulkan/` y sus tests. Si hace falta
  tocar `ge/`, que sea mínimo y justificado, como en F14a (ninguna otra fase de la ola toca `ge/` ni
  `vulkan/`). Los shaders cambian, así que hay que regenerar `ge_shaders_spv.inc` con
  `compile_shaders.py` (`mpo_vulkan_shaders_fresh`).
- **Notas (integración de la ola 4)**: hecha, ver `docs/sessions/F15a.md`.
  - Blending, dither, logic ops y máscaras por bit van en el shader. Hay tres caminos para leer el
    destino (`MPO_VK_DST_READ=raster|barrier|copy`); el blending fijo se usa solo donde da los
    mismos bits (`MPO_VK_BLEND=auto|shader|fixed`).
  - En los 11 dumps, la diferencia con `soft` es 0 en 7 (menú y codecs incluidos, antes de 1 179 a
    29 139 píxeles). En los 4 dumps 3D queda de 8 a 31 píxeles (≤ 0,024 %, delta ≤ 17), que vienen
    de la interpolación, no del blending. 0 errores de validación en los caminos 1.3 y 1.2.
  - `mpo_vulkan_compare` compara `soft` y `vulkan` con un umbral por defecto de 130 píxeles por
    frame y delta 24.
  - En la CI (lavapipe con la capa 1.3.275) el camino 1.3 prueba `copy`; `local_read` solo se
    activa con capas ≥ 1.3.276 [?, el corte no está verificado].
  - **Pendiente del usuario**: la prueba en Windows con la GPU y un Vulkan SDK ≥ 1.3.276
    (procedimiento en la nota).
  - **CI** (run `40446734964`, los 9 jobs en verde): `mpo_vulkan_renderer_test` tarda 182 s en
    `gcc asan-ubsan` y 143 s en `clang asan-ubsan`, con un límite de 300 s por test. Si una fase
    añade escenas y se acerca, se recorta como dice `docs/sessions/F15a.md` (la pasada
    `blend shader` o los métodos extra solo en 5551).

### F15b — Renderer Vulkan: framebuffers en el juego

- **Objetivo**: igualar al renderer por software en el juego, en todos los casos.
- **Requisitos**: F14b y F15a.
- **Tareas**:
  - Seguimiento de framebuffers en VRAM, render-to-texture, texturizar desde el target actual y
    lecturas de la CPU (invalidar los targets y la caché de texturas cuando la CPU escribe en VRAM).
  - Subir desde la VRAM los framebuffers que no dibuja el GE (el video 8888 de F05 §4.1) para
    presentarlos.
  - Splines y beziers en GPU, si la teselación en la CPU de F14a no alcanza.
  - Stencil (test y operaciones) en el shader, con el destino que ya lee F15a.
  - Interpolación exacta en el fragment shader (los pesos de aristas de softgpu), si se quieren 0
    píxeles distintos.
  - Medir en Windows el coste de las barreras de `barrier` (unos 550–600 grupos por frame de
    misión) y, si pesan, probar `fragment_shader_interlock`.
- **Criterios de hecho**: en las escenas de F05, ahora corriendo el juego, la diferencia entre los
  dos renderers está por debajo del umbral de F15a (`mpo_vulkan_compare`). **M3 cerrado.**
- **Notas (F05)**: **en las escenas capturadas no hubo render-to-texture.** Lo que se lee de VRAM es
  una caché de texturas CLUT en `0x0CC000` y más arriba, y hay que invalidarla cuando se escribe en
  VRAM.
  - Hay que presentar los framebuffers 8888 de video, que no dibuja el GE.
  - Las transferencias del GE hacia RAM y las lecturas de VRAM de la CPU no quedan en un dump: hacen
    falta trazas.
- **Notas (ola 3)**: todas las diferencias que midió F14a son de blending, con un escalón de 5 bits
  en hasta el 35 % de los píxeles de las cinemáticas. La prioridad es el blending por shader con el
  dither después. Las líneas ya salen idénticas (DDA en la CPU), así que se quitan de esta fase.

---

## M4 — Audio, video y saves

### F16a — Audio autónomo: SAS, ATRAC3 y salida SDL3 (ola 1)

- **Objetivo**: tener el mezclador SAS, el decodificador ATRAC3 y la salida de audio listos y
  probados, sin el juego.
- **Requisitos**: la decisión sobre `KCEJ_SOUND2` (F03: se recompila y su capa de Sony va por HLE).
- **Tareas**:
  - Librería `mpo_audio` en `profiles/mpo/host/audio/`:
    - mezclador SAS **portado de PPSSPP** (`Core/HW/SasAudio.cpp` y `Core/HLE/sceSas.cpp`): voces
      VAG/PCM/ATRAC, ADSR, pitch, envolventes y efectos de reverb, con cabeceras de origen;
    - ATRAC3 y ATRAC3plus con `at3_standalone` de PPSSPP (LGPL-2.1+) en `third_party/`, más la lógica
      de `sceAtrac` (bucles, buffers de streaming) separada de su HLE;
    - canales de salida al estilo de `sceAudio` sobre SDL3 audio (SDL3 fijado), con resampling de
      44.1 kHz a la frecuencia del dispositivo.
  - Revisar qué funciones de `sceSasCore`, `sceAtrac3plus` y `sceAudio` importan `sound2.prx` y
    `mgp_main` (inventario de F03) y priorizarlas.
  - Tests sin datos del juego: VAG y ADSR sintéticos con salida esperada, y vectores calculados con
    el código de PPSSPP. Con datos locales: decodificar los `.at3` del disco a WAV en
    `analysis/f16a/` (fuera del repo).
- **Criterios de hecho**: `ctest` pasa; un `.at3` del juego se decodifica y se describe en la nota
  (duración, canales, comparación contra PPSSPP si el usuario aporta una grabación).

### F16b — Audio en el juego: `sceAudio`, `sceSas` y `sceAtrac`

- **Objetivo**: música, efectos y voces.
- **Requisitos**: F13 y F16a.
- **Tareas**:
  - Registrar los NIDs de `sceAudio`, `sceSasCore` y `sceAtrac3plus` sobre `mpo_audio`.
  - Conectarlo con `KCEJ_SOUND2` recompilado y con el scheduler (hilos de audio).
- **Criterios de hecho**: la música, los efectos y el codec suenan sin cortes. `audio_report` contra
  una grabación de referencia de PPSSPP queda dentro del umbral (las grabaciones quedan en local).
- **Notas (ola 1)**: la API y el cableado están en `docs/sessions/F16a.md`.
  - `AtracManager::NotifyLoadModule(0x105, bss, 0x67C)` se llama desde `sceUtilityLoadAvModule`.
    Sin eso no se puede crear ningún ID de atrac.
  - Conectar `SasCore::SetAtracBridge`.
  - Retrasar el hilo:
    - `EstimateMixUs()` después de `__sceSasCore`;
    - `LastDelayUs()` después de cada `sceAtrac*`.
  - Implementar `AudioWaitHost` en el kernel.
  - Llamar a `AudioChannels::Update()` cada 64 muestras (1 451 µs) y pasar el resultado a
    `StereoResampler` y de ahí a `SdlAudioOutput`.
  - `sound2` sin pistas deja el 9.8 % sin cubrir y usa `vsgn` (F06).
  - `__sceSasSetNoise` da silencio, igual que en PPSSPP. Hay que confirmarlo con la grabación.
  - Para oír audio en Linux hay que instalar `libpulse-dev`, `libpipewire-0.3-dev` o
    `libasound2-dev` antes de configurar. **Hecho** (2026-10-26): el usuario instaló PipeWire,
    Pulse y ALSA (y Wayland, EGL y libdecor), y SDL3 los detecta.
  - **Grabaciones de referencia del usuario** (2026-10-26), en `analysis/f16b/` (local): WAV estéreo
    de 44,1 kHz y 16 bit del menú (17,2 s) y de un tramo de misión con una llamada de radio
    (153,8 s), y dos tramos más, `extra1` (24,3 s) y `extra2` (51,4 s).
- **Notas (ola 3)**: `sound2` no tiene `module_start` [V]: sus hilos los arranca `mgp_main`. Lo
  primero que piden es `__sceSasInit`, `sceAudioChReserve` y el bucle `__sceSasCoreWithMix` +
  `sceAudioOutputPannedBlocking`, que son los imports pendientes más llamados (`docs/sessions/F08b.md`).

### F17a — Decodificador de PMF y fuente PGF, autónomos (ola 2)

- **Objetivo**: decodificar los PMF del disco y rasterizar fuentes PGF, sin el juego.
- **Requisitos**: F16a (el audio de los PMF es ATRAC3plus).
- **Tareas**:
  - Demuxer de PMF/PSMF y un FFmpeg mínimo (LGPL, solo los decoders necesarios) que entregue frames
    8888 y audio.
  - Lector de PGF y rasterizador de glifos (PPSSPP `Core/Font/PGF.cpp` como referencia).
  - Volcar frames y glifos a `analysis/f17a/` (fuera del repo).
- **Criterios de hecho**: los 30 PMF se decodifican sin errores y los glifos de una fuente PGF se
  renderizan; ambos quedan descritos en la nota.
- **Notas (ola 1)**: el audio de los PMF se puede decodificar con el ATRAC3plus de
  `mpo_audio` (`CreateAtracDecoder` o `AtracContext::DecodeLowLevel`).
- **Notas (ola 2)**: hecha, ver `docs/sessions/F17a.md`.
  - Los 30 PMF y `ICON1.PMF` se decodifican sin errores: 74 315 frames y 2479.6 s. Los hashes son
    iguales con GCC y Clang.
  - **No existe `MGP.PGF`**: el lector se probó con las fuentes del firmware que trae PPSSPP.
  - FFmpeg 7.1.5 (LGPL) se compila en el build con `MPO_FFMPEG=auto|bundled|system|off`. Con MSVC,
    `auto` lo omite y el test de decodificación sale como omitido.

### F17b — Video del título y fuentes en el juego

- **Objetivo**: que se reproduzca el video del título y que el texto use la fuente del juego.
- **Requisitos**: F16b y F17a.
- **Tareas**:
  - `sceMpeg` y `scePsmf` sobre el decodificador de F17a, en lugar del stub de F11b.
  - `sceFont` con las **fuentes del firmware** (`ltn*.pgf`, `jpn0.pgf`…), portando de PPSSPP la
    lista de fuentes y `FindOptimumFont` sobre `mpo::font::Pgf`. MPO no trae fuente propia (F17a).
- **Criterios de hecho**: el video del título se reproduce con audio sincronizado y el texto del juego
  se ve correcto.
- **Notas (F01)**: el disco trae **30 PMF** en `USRDIR/mpg/` (342 MB), cinemáticas incluidas, no
  solo el título. Probar varias, no solo `MPO_OP*.pmf`.
- **Notas (ola 2)**: `mpo_video` y `mpo_font` (F17a) tienen la API de PPSSPP. Ver
  `docs/sessions/F17a.md`.
  - `sceMpeg` y `scePsmf` se portan sobre `mpo::video::MediaEngine`, que tiene la API del
    `MediaEngine` de PPSSPP. `writeVideoImage` hace swizzle si el destino es VRAM con `0x00200000`.
  - Ningún PMF tiene mapa EP. Todos son H.264 480×272 a 29.97 fps con ATRAC3plus estéreo (752 bytes
    por frame). `ICON1.PMF` solo tiene vídeo (`docs/formats/psmf.md`).
  - **Entregado por el usuario** (2026-10-26, local e ignorado por git):
    - las fuentes del firmware: 18 archivos `.pgf` en `game/flash0/font/`;
    - la grabación del título: `analysis/f17b/title_*.avi` (FFV1, 1440×816) y su WAV (estéreo,
      44,1 kHz, 50,6 s).
  - FFmpeg en Windows: vcpkg (`ffmpeg[avcodec,swscale]`) con `MPO_FFMPEG=system`.
- **Notas (F05)**: el decodificador real sustituye al stub de `sceMpeg`/`scePsmf` de F11.
  - La intro y el "Press Start" se muestran en tres framebuffers 8888 en `0x04000000`, `0x04088000`
    y `0x04110000`, sin draws del GE.
  - El bucle del título es probablemente `MPO_OP_LOOP.pmf` o `MPO_loop.pmf` [?].

### F18a — Savedata, OSK y diálogos, autónomo (ola 4)

- **Objetivo**: tener la lógica de `sceUtilitySavedata`, `sceUtilityOsk` y `sceUtilityMsgDialog`
  lista y probada, sin el juego.
- **Requisitos**: ninguno. Para saber qué usa el juego: F03 y F04 (los imports de `sceUtility*`
  están en 23 etapas, no en `mgp_main`; `prx_imports.py` y el export de Ghidra de `title` en
  `analysis/out/stage_title`).
- **Tareas**:
  - Librería `mpo_savedata` en `profiles/mpo/host/savedata/`, con la API estrecha de las olas (la
    memoria de la PSP como `span` y callbacks; sin NIDs).
  - **Qué usa el juego**: ubicar en las etapas (Ghidra) las llamadas a
    `sceUtilitySavedataInitStart`, `sceUtilityOskInitStart` y `sceUtilityMsgDialogInitStart`, y qué
    modos y campos de los parámetros rellenan (como hizo F20a con el AP Scan). Si no se puede
    resolver en estático, se priorizan los modos por lo que hace PPSSPP y se anota como [?].
  - **Máquina de estados** de los tres diálogos, portada de PPSSPP (`Core/Dialog/PSPDialog.cpp`,
    `PSPSaveDialog.cpp`, `SavedataParam.cpp`, `PSPOskDialog.cpp`, `PSPMsgDialog.cpp`, con cabecera
    de origen): `InitStart` → `Update` → `GetStatus` → `ShutdownStart`, los retrasos de cada
    transición, los códigos de error y lo que se escribe en la estructura de parámetros.
  - **Savedata**: los modos que usa el juego, **incluido delete**. Leer el issue #6600 de PPSSPP
    (el borrado podía vaciar `SAVEDATA`) y probar que el borrado solo toca la carpeta del save
    elegido. Escribir y leer `PARAM.SFO` (se puede reusar el lector de `host/disc/param_sfo`), el
    archivo de datos y los íconos, con la misma estructura de carpetas que PPSSPP
    (`PSP/SAVEDATA/<gameName><saveName>/`) para que los saves sean compatibles.
  - **Cifrado, si aplica**: averiguar si MPO pide savedata cifrado (la clave y la versión de cifrado
    de los parámetros). Si aplica, el cifrado va detrás de una interfaz inyectable. **Ojo**: hoy
    libkirk solo lo enlaza el instalador y las claves no se versionan (F02). F18a no cambia esa regla
    por su cuenta: propone en la nota cómo llega al runtime y lo prueba con un cifrador falso.
  - **Carpetas de saves por sistema operativo**: `%APPDATA%` en Windows y `$XDG_DATA_HOME` (o
    `~/.local/share`) en Linux, con una carpeta del proyecto. Esa es la raíz por defecto de `ms0:/`
    que F18b le pasa a la IO de F09. Los tests nunca usan la carpeta real.
  - **UI del OSK y del diálogo de mensajes**: el modelo (distribución del teclado, navegación con el
    mando, edición del texto en UTF-16, los botones del diálogo y su resultado) va separado del
    dibujo y se prueba sin ventana. El dibujo se propone en la nota (Dear ImGui sobre SDL3 o
    dibujado en el framebuffer de la PSP) y, si cabe, se muestra con una demo propia.
  - Tests sin datos del juego: transiciones y tiempos de cada diálogo, guardar → cargar → borrar
    sobre una carpeta temporal (con los hashes de antes y después), el caso de #6600, parámetros
    inválidos y el OSK con una secuencia de input sintética.
- **Criterios de hecho**: `ctest` pasa con Clang (ASan y UBSan) y GCC, y la nota documenta qué modos
  y campos usa MPO, si hace falta el cifrado y cómo se dibujan los diálogos.
- **Notas (ola 4)**: si se añade Dear ImGui u otra dependencia, va en `third_party/` con su LICENSE y
  en `THIRD_PARTY.md`.
- **Notas (integración de la ola 4)**: hecha, ver `docs/sessions/F18a.md`. El contrato de MPO está en
  `docs/analysis/utility.md`.
  - El savedata lo usan las etapas `title`, `briefing`, `cyberval2` y `ending`, con la misma
    librería, en los modos AUTOLOAD, AUTOSAVE, SAVE, LISTLOAD, LISTSAVE y **LISTALLDELETE con
    `gameName` vacío**: es justo el caso de #6600, y hay un test que comprueba con hashes que solo
    se borra el directorio elegido [V].
  - Con el SDK `0x02080010`, una PSP o PPSSPP guardan **cifrado en modo 5**. El port puede guardar
    sin cifrar; el cifrado solo hace falta para intercambiar saves.
  - OSK: `inputtype` 9 (dígitos y mayúsculas), con un límite de 15 para el nombre y de 10 para la
    contraseña.
  - No hay dependencias nuevas; el dibujo de los diálogos quedó como propuesta (abajo).
  - La carpeta por defecto de `ms0:/` ya existe: `savedata::default_ms0_root`.

### F18b — Savedata, OSK y diálogos en el juego

- **Objetivo**: guardar y cargar partida, y poder escribir nombres y contraseñas.
- **Requisitos**: F17b y F18a. Los imports están en las etapas, así que necesita F13.
- **Tareas**:
  - Registrar los NIDs de `sceUtilitySavedata*`, `sceUtilityOsk*` y `sceUtilityMsgDialog*` sobre
    `mpo_savedata`, y completar `sceUtilityGetSystemParamInt` y `sceImposeSetLanguageMode`.
  - Conectar la raíz de `ms0:/` de F09 a la carpeta por defecto de `mpo_savedata`.
  - Dibujar los diálogos en la ventana del juego. La propuesta de F18a es un generador de display
    lists al estilo de PPGe, con el texto PGF de `mpo_font`, en vez de Dear ImGui (requiere F11b y
    F14b). **Decide el usuario.**
  - Cómo llega el cifrado (modo 5) al runtime, sin romper la regla de F02 (libkirk solo en el
    instalador). La propuesta de F18a es un conversor `mpo --import-save/--export-save` en el
    instalador; la alternativa, una opción de CMake apagada por defecto. **Decide el usuario.**
  - `ms0:/` de la IO (F09) y la raíz de `mpo_savedata` son el mismo directorio: `title` recorre
    `ms0:/PSP/SAVEDATA` con `sceIoDopen`/`Dread`.
- **Criterios de hecho**: se guarda y se carga entre ejecuciones, se ingresa una contraseña de
  reclutamiento y se borra un save desde el menú del juego (`LISTALLDELETE`), comprobando con
  hashes que el resto de `SAVEDATA` no cambia. Los saves de prueba se hashean antes y después.
  **M4 cerrado.**

---

## M5 y M6 — Jugable

### F19 — Primera misión y repetición de input

- **Objetivo**: completar la primera misión.
- **Requisitos**: F18b.
- **Tareas**:
  - Grabar y reproducir el input de forma determinista (lectura por lectura).
  - Capturar frames cada N y compararlos contra referencias locales.
  - Corregir los bugs hasta completar la primera misión con su transición de etapa.
  - Verificar que el gameplay corre a 20 fps con el timestep original.
- **Criterios de hecho**: la primera misión se completa en Windows y Linux, y una repetición grabada
  la reproduce de forma idéntica. **M5 cerrado.**

### F20a — Generador de APs virtuales, autónomo (ola 2)

- **Objetivo**: tener el modelo de `sceNetApctl` para el AP Scan, sin el juego.
- **Requisitos**: ninguno.
- **Tareas**:
  - Estudiar cómo implementa JPCSP `sceNetApctl` y qué evento espera MPO (PPSSPP #20703). Ubicar en
    `mgp_main` (Ghidra, F03) el código del AP Scan y qué campos lee.
  - Generador de APs virtuales con semilla configurable: BSSID y SSID estables, canal, señal y
    seguridad, con respeto del cooldown.
  - Máquina de estados del escaneo (eventos, `SCAN_COMPLETE`, lista de BSS) separada del HLE.
- **Criterios de hecho**: tests deterministas del generador y de la máquina de estados, y la nota
  documenta el contrato que espera MPO.

### F20b — AP Scan, red y GPS en el juego

- **Objetivo**: poder reclutar por AP Scan sin hardware de PSP.
- **Requisitos**: F19 y F20a.
- **Tareas**:
  - HLE de `sceNetInit` y `sceNetApctl*` sobre el modelo de F20a.
  - Opcional: escaneo real de Wi-Fi del sistema operativo.
  - Hacer que ad hoc e infraestructura respondan "sin red" de forma limpia.
  - Stub de `sceUsbGps` (no hay receptor).
- **Criterios de hecho**: se reclutan soldados por AP Scan, sin cuelgues, y el menú de red no cuelga.
  Se puede escanear varias veces seguidas y salir de la pantalla de AP Scan sin cuelgues (el síntoma
  de PPSSPP #20703).
- **Notas (ola 2)**: el modelo es `mpo_apscan` (`profiles/mpo/host/apscan/`) y el contrato de MPO está
  en `docs/analysis/apscan.md`; ver `docs/sessions/F20a.md`.
  - Los eventos de `ApctlModel` se entregan desde un hilo de Apctl del kernel, que duerme hasta
    `next_deadline_us()` y despierta en `scan_user`. **Nunca** se entregan dentro de
    `sceNetApctlScanUser`.
  - `sceWlanGetSwitchState` tiene que devolver 1. Hay que aceptar `sceUtilityLoadNetModule(1)` y
    `(3)`, `sceNetInit`, `sceNetInetInit` y sus `Term`/`Unload`.
  - Falta decidir la UX de la rotación de APs (el periodo y un control de "cambiar de lugar" con
    `epoch_offset`) y dónde se guarda la semilla.
  - Hay que comprobar con el juego las incógnitas de `apscan.md` §6: la unidad del reloj del
    cooldown y qué comprueba `0x0883754C`.
  - **Pendiente del usuario**: una partida en la que el AP Scan esté disponible (la que hay ahora
    no lo tiene).

### F21 — Partida completa y estabilidad

- **Objetivo**: jugar de principio a fin.
- **Requisitos**: F20b.
- **Tareas**:
  - Recorrer el juego capítulo por capítulo, con una lista de bugs en `docs/sessions/`.
  - Atajos nativos (`register_native_fast_path`) para las funciones más usadas si hace falta.
  - Que no quede código interpretado en las etapas recorridas.
  - Probar la estabilidad en sesiones largas.
- **Criterios de hecho**: el juego se completa en Windows y en Linux, y no hay crashes conocidos sin
  documentar.

### F22 — Empaquetado y release 0.1

- **Objetivo**: que alguien sin conocimientos técnicos pueda instalarlo y jugarlo.
- **Requisitos**: F21.
- **Tareas**:
  - Flujo de instalación: el usuario elige el ISO, se valida, se descifra, se genera y se compila, o
    se usa un binario precompilado del runtime más la generación en local. **Hay que decidir el
    modelo.**
  - Linux: AppImage o Flatpak. Windows: ZIP.
  - CI que publique solo el código y el runtime, nunca nada del juego.
  - `README.md` para el usuario, smoke test manual y aviso legal.
- **Criterios de hecho**: una instalación limpia en Windows y Linux (y Steam Deck si se puede) llega a
  jugar siguiendo solo el README. **M6 cerrado.**

---

## F23+ — Mejoras (se planifican cuando M6 esté cerrado)

- Resolución interna configurable, MSAA y filtrado anisotrópico.
- Widescreen y ultrawide con el HUD ajustado (CWCheat `0x201E9728` como pista).
- 60+ fps con **interpolación** (la lógica sigue a 20 o 30 fps).
- Texturas HD en formato de PPSSPP.
- Versión EU (`ULES-00645`) y otras regiones.
- Ad hoc (compatible con el servidor de PPSSPP) y online (SaveMGO).
- Teclado y mouse, y cámara con el stick derecho.
- MPO+ (`ULUS-10290`).
- macOS (MoltenVK).
