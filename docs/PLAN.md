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
- No se adelanta trabajo de fases futuras. Lo que se descubra se anota en la nota de sesión.
- Una fase puede cambiar el plan, pero el cambio se explica en la nota de sesión.
- Nunca se suben ISOs, EBOOTs, PRX, assets, capturas ni dumps (ver `CLAUDE.md` §1).

## Estado

| Fase | Nombre | Hito | Estado |
|---|---|---|---|
| F00 | Esqueleto del repo, PSPRecomp y CI | M0 | ✅ hecha |
| F01 | Lector de ISO, validador e instalador | M0 | ✅ hecha |
| F02 | Descifrado de EBOOT y PRX | M0 | ✅ hecha |
| F03 | Análisis de `mgp_main` en Ghidra | M0 | ✅ hecha |
| F04 | Overlays de etapa y formato `.rlc` | M0 | 🟨 en curso |
| F05 | Inventario del GE con dumps de PPSSPP | M0 | 🟨 en curso |
| F06 | `psp_analyze` sobre MPO y brechas del framework | M0 | ⬜ pendiente |
| F07 | Generación AOT, intérprete y difftest | M1 | ⬜ pendiente |
| F08 | Kernel HLE: memoria, módulos e hilos (fibras) | M1 | ⬜ pendiente |
| F09 | IO, tiempo y vblank | M2 | ⬜ pendiente |
| F10 | Ventana, presentación e input | M2 | ⬜ pendiente |
| F11 | GE: display lists y renderer por software (base) | M2 | ⬜ pendiente |
| F12 | GE por software: completo | M2 | ⬜ pendiente |
| F13 | Overlays de etapa en runtime | M2 | ⬜ pendiente |
| F14 | Renderer Vulkan: base | M3 | ⬜ pendiente |
| F15 | Renderer Vulkan: framebuffers y efectos | M3 | ⬜ pendiente |
| F16 | Audio: sceAudio, SAS y ATRAC3 | M4 | ⬜ pendiente |
| F17 | Video del título y fuentes | M4 | ⬜ pendiente |
| F18 | Savedata, OSK y diálogos | M4 | ⬜ pendiente |
| F19 | Primera misión y repetición de input | M5 | ⬜ pendiente |
| F20 | AP Scan virtual, red y GPS | M6 | ⬜ pendiente |
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

### F07 — Generación AOT, intérprete y difftest

- **Objetivo**: `mgp_main` recompilado y compilando en ambos sistemas operativos.
- **Requisitos**: F06.
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

### F08 — Kernel HLE: memoria, módulos e hilos (fibras)

- **Objetivo**: que el juego llegue a `module_start` y cree sus hilos.
- **Requisitos**: F07.
- **Tareas**:
  - **Decidir la implementación de las fibras** (Win32 Fibers con `ucontext`, o `minicoro`, u otra),
    reservando la pila de forma explícita.
  - Scheduler de prioridad de un solo núcleo.
  - `sceKernel*`: SysMem (particiones, bloques, FPL y VPL), módulos (`module_start`, `StartModule`),
    hilos, semáforos, event flags, mutex, callbacks, delays y alarmas.
  - Si un NID no está implementado, que falle de forma visible. Añadir el histograma de llamadas HLE.
- **Criterios de hecho**: hay una traza del arranque hasta que los hilos se crean y ejecutan, y la
  lista de imports pendientes está ordenada por frecuencia. **M1 cerrado.**

---

## M2 — Pantalla de título y primera etapa

### F09 — IO, tiempo y vblank

- **Objetivo**: que el juego lea sus datos y que el tiempo avance correctamente.
- **Requisitos**: F08.
- **Tareas**:
  - `sceIo*` (síncrono y asíncrono) sobre `disc0:/`, leyendo desde el ISO.
  - `ms0:/` mapeado a una carpeta de usuario por sistema operativo.
  - `sceUmd*`.
  - `sceRtc`, `sceKernelGetSystemTime*`, `sceDisplay` (vblank a 59.94 Hz y `WaitVblank*`) y
    `sceKernelDelayThread`.
  - Revisar si `KCEJ_FS` y `data.cnf` funcionan con todo esto.
- **Criterios de hecho**: la traza muestra al juego abriendo y leyendo `slot.dat` y los `_zar` sin
  errores, con el vblank estable.

### F10 — Ventana, presentación e input

- **Objetivo**: mostrar el framebuffer de la PSP en una ventana y recibir controles.
- **Requisitos**: F09.
- **Tareas**:
  - Ventana con SDL3 y presentación del framebuffer de `sceDisplay` desde la VRAM. Al principio basta
    con un blit por CPU. Cubrir los formatos 565, 5551, 4444 y 8888.
  - `sceCtrl` desde gamepad y teclado con SDL3 (incluido el stick analógico), con un mapeo
    configurable.
- **Criterios de hecho**: algo dibujado por el juego aparece en la ventana (aunque el GE esté
  incompleto) y el input llega a `sceCtrlReadBuffer*`.

### F11 — GE: display lists y renderer por software (base)

- **Objetivo**: ver el logo y el menú.
- **Requisitos**: F10 y F05.
- **Tareas**:
  - `sceGe*`: cola de listas, stall, signals, finish y callbacks, y `EdramSetAddrTranslation`.
  - Intérprete de comandos del GE.
  - Decodificador de vértices: todos los vtypes y el skinning.
  - Transformación y clipping.
  - Rasterizador por software portado de softgpu de PPSSPP, con cabeceras de origen.
- **Criterios de hecho**: se ven el logo y el menú del título. Las capturas comparadas contra PPSSPP
  quedan en local y se describen en la nota de sesión. **M2 parcial.**
- **Notas (F05)**: lo que hay que cubrir sale de `docs/analysis/ge-usage.md` (escenas `title` y
  `menu`). Los frame dumps **no guardan la estructura de las listas** (`CALL`, `JUMP`, `RET`,
  `SIGNAL`, `FINISH`, `BASE`, `ORIGIN`, `OFFSETADDR`, `BOUNDINGBOX`, `BJUMP`), así que los signals,
  los stalls y los saltos por bounding box se miden con el histograma HLE. `ge_usage.vertex_size()`
  sigue las reglas de tamaño de vértice de PPSSPP y coincide con los dumps reales; sirve como
  referencia para el decodificador.

### F12 — GE por software: completo

- **Objetivo**: que el renderer de referencia cubra todo lo que usa el juego, según F05.
- **Requisitos**: F11.
- **Tareas**:
  - Texturas: CLUT 4 y 8 bits, swizzle, mipmaps y DXT si aparece en F05.
  - Blending con colores fijos, color doubling, alpha y color test, operaciones lógicas, stencil y
    clamp de profundidad.
  - **Líneas** (con el comportamiento de `[CenteredLines]`).
  - **Splines y beziers con skinning.**
  - Render-to-texture.
- **Criterios de hecho**: el histograma de F05 queda cubierto al 100 % y las escenas de los dumps se
  ven igual que en PPSSPP.
- **Notas (F05)**: la lista de comprobación es la sección `features` de
  `python3 -I profiles/mpo/tools/ge_usage.py` sobre los dumps de todas las escenas. Los mismos dumps
  sirven para comparar contra PPSSPP, que los reproduce si se abren como un juego (**File › Load**,
  Ctrl+O).

### F13 — Overlays de etapa en runtime

- **Objetivo**: entrar a la primera etapa.
- **Requisitos**: F12 y F04.
- **Tareas**:
  - Implementar con HLE `sceKernelLoadModule` y `UnloadModule` para los `stage/*.prx`, con
    relocación (`.rlc`).
  - Recompilar cada etapa como una **librería compartida** identificada por hash, con un script
    `build_overlays`.
  - Instalar y desinstalar los overlays (`unregister_functions`), con el intérprete como respaldo.
- **Criterios de hecho**: se entra a la primera etapa (aunque sea sin audio) y en el reporte
  final no queda código interpretado en esa etapa. **M2 cerrado.**
- **Notas (F04)**: el diseño de los overlays está en `docs/sessions/F04.md`.
  - `mgp_main` usa `sceKernelLoadModuleByID(fd, 0, NULL)` (no `sceKernelLoadModule`) sobre un
    descriptor de `kjfs`. Después llama a `sceKernelStartModule(id, 4, &p)` y aplica el `.rlc` él
    mismo (`0x08805C0C`). No hay que hacer HLE del `.rlc`, sino respetar ese contrato.
  - Hay una sola etapa a la vez, siempre en `0x08B8F900` (`title` y `st02` verificadas; falta un
    menú). La más grande ocupa `0xFC400` bytes.
  - Para elegir la librería del overlay, se identifica por el SHA-256 de `<s>.prx` y de `<s>.rlc`
    (91 distintas).

---

## M3 — Renderer Vulkan

### F14 — Renderer Vulkan: base

- **Objetivo**: dibujar las mismas escenas que el renderer por software, pero en la GPU.
- **Requisitos**: F13.
- **Tareas**:
  - Infraestructura: volk, VMA, instancia y dispositivo (Vulkan 1.2 como mínimo, aprovechando 1.3 si
    está), swapchain, frames en vuelo y capas de validación.
  - Shaders: generar vertex y fragment shaders a partir del estado del GE, compilar a SPIR-V y
    mantener una caché de pipelines.
  - Datos: subir vértices y texturas, y convertir CLUT en el shader.
  - Elegir el renderer con una opción: `--renderer=soft|vulkan`.
- **Criterios de hecho**: el título y la primera etapa se ven en Vulkan sin errores de validación,
  en Windows y Linux.
- **Notas (F05)**: las combinaciones de estado que se ven en las secciones `blend`, `logic_op`,
  `stencil`, `depth` y `tex_*` del informe de F05 sirven para dimensionar la caché de pipelines y
  para decidir qué va por shader (logic ops, factores dobles, MIN/MAX/ABSDIFF).

### F15 — Renderer Vulkan: framebuffers y efectos

- **Objetivo**: igualar al renderer por software en todos los casos.
- **Requisitos**: F14.
- **Tareas**:
  - Seguimiento de framebuffers en VRAM, render-to-texture, texturizar desde el target actual y
    lecturas de la CPU.
  - Blending por shader cuando el hardware no alcance, operaciones lógicas, clamp de profundidad,
    líneas, y splines y beziers en GPU o pre-teselados.
  - Herramienta para comparar frames entre `soft` y `vulkan` con un umbral.
  - Toggles A/B por variable de entorno para cada optimización.
- **Criterios de hecho**: en las escenas de F05, la diferencia entre los dos renderers está por debajo
  del umbral. **M3 cerrado.**
- **Notas (F05)**: el render-to-texture, el self-texturing, las CLUT desde VRAM y las transferencias
  de VRAM a VRAM salen de las secciones `vram_reads`, `transfers` y `cpu_vram_writes`. Lo que **no**
  queda en un dump son las transferencias del GE hacia RAM y las lecturas de VRAM de la CPU: eso
  necesita una traza del runtime.

---

## M4 — Audio, video y saves

### F16 — Audio: sceAudio, SAS y ATRAC3

- **Objetivo**: música, efectos y voces.
- **Requisitos**: F13. La decisión sobre `KCEJ_SOUND2` (F03).
- **Tareas**:
  - Canales de `sceAudio` a SDL3 audio, con resampling.
  - `sceSas` portado de PPSSPP (voces, ADSR y efectos).
  - `sceAtrac` con `at3_standalone`.
- **Criterios de hecho**: la música, los efectos y el codec suenan sin cortes. `audio_report` contra
  una grabación de referencia de PPSSPP queda dentro del umbral (las grabaciones quedan en local).

### F17 — Video del título y fuentes

- **Objetivo**: que se reproduzca el video del título y que el texto use la fuente del juego.
- **Requisitos**: F16.
- **Tareas**:
  - `sceMpeg` y `scePsmf` con un FFmpeg mínimo (solo los decoders necesarios, LGPL).
  - `sceFont` con `MGP.PGF` y las fuentes del sistema que pida el juego (si las pide, el usuario las
    aporta o se buscan alternativas libres).
- **Criterios de hecho**: el video del título se reproduce con audio sincronizado y el texto del juego
  se ve correcto.
- **Notas (F01)**: el disco trae **30 PMF** en `USRDIR/mpg/` (342 MB), cinemáticas incluidas, no
  solo el título. Probar varias, no solo `MPO_OP*.pmf`.

### F18 — Savedata, OSK y diálogos

- **Objetivo**: guardar y cargar partida, y poder escribir nombres y contraseñas.
- **Requisitos**: F17.
- **Tareas**:
  - `sceUtilitySavedata` con todos los modos que use el juego (incluido **delete**, con cuidado del
    bug de PPSSPP #6600), cifrado de savedata si aplica y diálogos propios con ImGui.
  - `sceUtilityOsk` y `sceUtilityMsgDialog`.
  - Carpetas de saves por sistema operativo, y compatibilidad de saves con PPSSPP si se puede.
- **Criterios de hecho**: se guarda y se carga entre ejecuciones y se ingresa una contraseña de
  reclutamiento. Los saves de prueba se hashean antes y después. **M4 cerrado.**

---

## M5 y M6 — Jugable

### F19 — Primera misión y repetición de input

- **Objetivo**: completar la primera misión.
- **Requisitos**: F18.
- **Tareas**:
  - Grabar y reproducir el input de forma determinista (lectura por lectura).
  - Capturar frames cada N y compararlos contra referencias locales.
  - Corregir los bugs hasta completar la primera misión con su transición de etapa.
  - Verificar que el gameplay corre a 20 fps con el timestep original.
- **Criterios de hecho**: la primera misión se completa en Windows y Linux, y una repetición grabada
  la reproduce de forma idéntica. **M5 cerrado.**

### F20 — AP Scan virtual, red y GPS

- **Objetivo**: poder reclutar por AP Scan sin hardware de PSP.
- **Requisitos**: F19.
- **Tareas**:
  - Estudiar cómo implementa JPCSP `sceNetApctl` y qué evento espera MPO (PPSSPP #20703).
  - HLE de `sceNetInit` y `sceNetApctl*`, con un generador de APs virtuales con semilla configurable
    y respeto del cooldown.
  - Opcional: escaneo real de Wi-Fi del sistema operativo.
  - Hacer que ad hoc e infraestructura respondan "sin red" de forma limpia.
  - Stub de `sceUsbGps` (no hay receptor).
- **Criterios de hecho**: se reclutan soldados por AP Scan, sin cuelgues, y el menú de red no cuelga.

### F21 — Partida completa y estabilidad

- **Objetivo**: jugar de principio a fin.
- **Requisitos**: F20.
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
