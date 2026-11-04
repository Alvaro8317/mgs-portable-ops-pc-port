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
| 5 ✅ | F23a, F23b, F23c | Adelantadas a pedido del usuario el 2026-11-03, antes de cerrar M6 (F21b y F22c siguen abiertas). **No cambian el ABI del runtime** (`psprecomp::Runtime`, `AllegrexContext`, `kOverlayAbiVersion`) ni los headers del framework: las etapas que el jugador ya compiló tienen que seguir cargando. F23a: F22 · F23b: F15b · F23c: F18b (F23c se sumó el mismo día). Integrada el 2026-11-03, junto con el spike de los controles en pantalla (`docs/analysis/button-prompts.md`). |
| 6 ✅ | F24a, F24b, F24c y un spike de widescreen | Pedidas por el usuario el 2026-11-03, con las mismas reglas que la ola 5: **no cambian el ABI del runtime** ni los headers del framework. F24a: F23b · F24b: el spike de los controles (`docs/analysis/button-prompts.md`) y F23c · F24c: F15b y el mismo spike. Integrada el 2026-11-03 (`docs/analysis/widescreen.md` es el spike). |
| — | F10b ✅ → F11b ✅ → F13 ✅ → F14b ✅ → F16b ✅ → F17b ✅ → F18b ✅ → F15b ✅ → F19 ✅ → F18c ✅ → F20b ✅ → F21 🟨 → F22 ✅ → F22b ✅ → F22c 🟨 | en serie. F15b va después de F18b desde el 2026-10-27 (ver su ficha). F18c (autónoma) se hizo en la sesión principal el 2026-10-31. **F21b** (el recorrido del usuario) corre en paralelo con F22, F22b y F22c desde el 2026-11-01 (ver su ficha). |

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
| F10b | Presentación e input en el juego | M2 | ✅ hecha |
| F11a | GE por software: reproductor de `.ppdmp` y 2D | M2 | ✅ hecha (ola 1) |
| F11b | `sceGe*` en el juego y stub de `sceMpeg` | M2 | ✅ hecha |
| F12 | GE por software: completo | M2 | ✅ hecha (ola 2) |
| F13 | Overlays de etapa en runtime | M2 | ✅ hecha |
| F14a | Renderer Vulkan: base sobre los dumps | M3 | ✅ hecha (ola 3) |
| F14b | Renderer Vulkan en el juego | M3 | ✅ hecha (Windows: pendiente del usuario) |
| F15a | Renderer Vulkan: blending por shader y efectos (autónomo) | M3 | ✅ hecha (ola 4) |
| F15b | Renderer Vulkan: framebuffers en el juego | M3 | ✅ hecha (M3 cerrado; Windows con GPU: pendiente del usuario) |
| F16a | Audio autónomo: SAS, ATRAC3 y salida SDL3 | M4 | ✅ hecha (ola 1) |
| F16b | Audio en el juego: `sceAudio`, `sceSas` y `sceAtrac` | M4 | ✅ hecha (Windows y la música de misión: pendientes) |
| F17a | Decodificador de PMF y fuente PGF (autónomos) | M4 | ✅ hecha (ola 2) |
| F17b | Video del título y fuentes en el juego | M4 | ✅ hecha (Windows y la sincronía contra PPSSPP: pendientes) |
| F18a | Savedata, OSK y diálogos (autónomo) | M4 | ✅ hecha (ola 4) |
| F18b | Savedata, OSK y diálogos en el juego | M4 | ✅ hecha (la contraseña pasa a F19; el teclado del PC y Windows: pendientes del usuario) |
| F18c | Conversor de saves cifrados en el instalador (autónomo) | M4 | ✅ hecha (CI en Windows pasa; cargar los exports en PPSSPP: pendiente del usuario) |
| F19 | Primera misión y repetición de input | M5 | ✅ hecha (M5 cerrado en Linux; Windows: pendiente del usuario; la contraseña pasa a F20b) |
| F20a | Generador de APs virtuales (autónomo) | M6 | ✅ hecha (ola 2) |
| F20b | AP Scan, red y GPS en el juego | M6 | ✅ hecha (se recluta por AP Scan; Windows y el menú de red de la antena: pendientes) |
| F21 | Partida completa y estabilidad | M6 | 🟨 hecha en parte (las 92 etapas compiladas y tramos con saves; el recorrido pasa a F21b) |
| F21b | Recorrido completo (Linux y Windows) | M6 | ⬜ pendiente (en paralelo con F22) |
| F22 | Distribución: compilar en la máquina del jugador | M6 | ✅ hecha (modelo decidido, `setup.py` de punta a punta en Linux; el resto se partió en F22b y F22c) |
| F22b | Toolchain portátil, launcher y paquetes | M6 | ✅ hecha (Windows con llvm-mingw en la CI, launcher, etapas en dos niveles y release revisada; la prueba en Windows y en la Steam Deck: pendiente del usuario) |
| F22c | README, aviso legal e instalaciones limpias | M6 | 🟨 hecha en Linux (README en inglés y español, aviso legal, smoke test e instalaciones limpias en Linux; Windows y el Flatpak: pendientes del usuario) |
| F23a | Cámara con el stick derecho (autónomo) | M7 | ✅ hecha (ola 5; integrada el 2026-11-03; probarla con mando en el juego: pendiente del usuario) |
| F23b | Resolución interna configurable, hasta 4K (autónomo) | M7 | ✅ hecha (ola 5; integrada el 2026-11-03; `--render-scale 1..8`; la prueba con GPU real en Windows y el selector del launcher: pendientes) |
| F23c | Fuente libre de respaldo para los diálogos (autónomo) | M7 | ✅ hecha (ola 5; integrada el 2026-11-03; `sceFont` sigue necesitando las PGF; Windows sin fuentes: pendiente del usuario) |
| F24a | Ajustes del juego en el launcher (autónomo) | M7 | ✅ hecha (ola 6; integrada el 2026-11-03; probar `--fullscreen` en el juego y el launcher con mando: pendiente del usuario) |
| F24b | Controles en pantalla según el dispositivo, parte I (autónomo) | M7 | ✅ hecha (ola 6; integrada el 2026-11-03; `--prompts`, los diálogos del sistema; probar con mandos reales: pendiente del usuario) |
| F24c | Reemplazo de texturas por hash (autónomo) | M7 | ✅ hecha (ola 6; integrada el 2026-11-03; solo texturas en RAM: las de VRAM, el 91 % de las búsquedas, quedan para las HD) |
| F25a | Íconos del HUD según el dispositivo (autónomo) | M7 | ✅ hecha (ola 7; `--prompt-icons`, íconos dibujados por el port; el inventario completo y la prueba en el juego: pendientes del usuario) |
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
- **Notas (F10b)**: hecha, ver `docs/sessions/F10b.md`.
  - `sceCtrl` es `hle` (`Ctrl`, 12 NIDs), muestreado al **final** del vblank como en PPSSPP. Con
    Start simulado (`mpo_boot --press start@12000+200`), el juego lee Start en 6 frames y abre la
    intro 1,6 s antes.
  - `mpo_boot --window 1` muestra cada flip en una ventana SDL a velocidad real; `--capture <dir>`
    guarda PNG.
  - Decidido: `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "1"`, como PPSSPP. El `input.toml` del
    usuario es `--input-map`, o si no `<config dir>/input.toml` (`%APPDATA%\mpo-pc-port`,
    `$XDG_CONFIG_HOME/mpo-pc-port` o `~/.config/mpo-pc-port`), o si no el mapeo por defecto.
  - **Pasan a F11b dos criterios**: "algo dibujado por el juego aparece en la ventana" y "con
    Start, el juego abre `slot.dat`". El juego solo dibuja con el GE y `sceMpeg`, y sin el HLE del
    GE cree que no tiene VRAM y no arranca la intro.

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
- **Notas (F10b)**: ver "Para la próxima sesión" en `docs/sessions/F10b.md`.
  - **Criterios heredados de F09 y F10b**: con Start pulsado (`mpo_boot --press start@<ms>`), la
    traza muestra al juego abriendo y leyendo `slot.dat` (LBN `0x39650`) sin errores; y algo
    dibujado por el juego aparece en la ventana (`--window 1`) o en las capturas
    (`--capture <dir>`).
  - Hoy `sceGeEdramGetAddr` y `sceGeEdramGetSize` devuelven 0, así que el juego hace
    `SetFrameBuf(0x00000000, 512, 5551)`. Registra un callback del GE (`sceGeSetCallback` a los
    227 834 µs) y hace un `sceGeListSync`.
  - Sin el GE, el juego da vueltas cada 13,95 s: `preseek` abre `MPO_OP_LONG.pmf`, pero nadie
    llama a `sceMpeg`. Primero hay que ver si con el GE arranca el reproductor, y después
    escribir el stub.
- **Notas (F11b)**: hecha, ver `docs/sessions/F11b.md`. **M2 parcial.**
  - `sceGe` es `hle` (`GeManager`, 18 NIDs): la cola de PPSSPP sobre `mpo_ge`, síncrona dentro de
    la llamada, con el tiempo del GE en ciclos y la interrupción 25 encolada como en PPSSPP.
  - Se ven los logos de Konami y de Kojima Productions, la intro y el TITLE MENU. El logo de KP y el
    menú dan **0 píxeles distintos** contra la reproducción de los dumps de PPSSPP.
  - **Cambio de plan**: el "stub" de `sceMpeg` es el HLE de PPSSPP portado sobre el
    `MediaEngine` de F17a (23 NIDs), que escribe negro y silencio. El video avanza y termina como en
    la PSP. A F17b le queda la salida (imagen y PCM), `scePsmf` y `sceFont`.
  - **Cambio de plan**: los stubs de import de las etapas se ligan al HLE al cargarlas (era de
    F13). Sin eso, cada import de una etapa devolvía basura sin llegar al HLE.
  - **El criterio de `slot.dat` pasa a F18b**: no se abre ni en la intro ni en el título. NEW GAME
    llega a "Input your name" y espera al OSK.

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
- **Notas (F11b)**: ver "Para la próxima sesión" en `docs/sessions/F11b.md`.
  - **Los stubs de import de las etapas ya se ligan** al HLE en `ModuleManager::load`
    (`Module::bound_stubs`) y se desligan al descargarlas; un stub con código de un corpus no se
    toca. Queda recompilar las etapas y el ciclo de carga y descarga.
  - **[V]** No todas las etapas van en `0x08B8F900`: `init` y `r_main01` se cargan en `0x08AFB800`
    (justo después de `mgp_main`), y `title` en `0x08B8F900`.
  - Para llegar a la primera etapa jugable hace falta pasar el OSK del nombre (F18b): NEW GAME se
    queda en "Input your name".
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
- **Notas (F13)**: hecha, ver `docs/sessions/F13.md`. **M2 cerrado.**
  - **Cambio de criterio** (acordado con el usuario): entrar a `st01` necesita el OSK (F18b). Se
    cumple con las etapas que el juego carga hoy: `init`, `r_main01` y `title` corren
    recompiladas, con `--interpreter 0`, hasta "Input your name", con 0 instrucciones
    interpretadas y el ciclo de carga y descarga verificado. Las 69 capturas son iguales, flip por
    flip, a las de la misma ejecución con el intérprete.
  - Las etapas son librerías compartidas (`build_overlays.py`, `-DMPO_GENERATED_OVERLAYS_DIR`,
    `mpo_boot --overlays`), identificadas por el SHA-256 del PRX y la base (`config/stages.toml`)
    y verificadas contra el `.text` enlazado en `sceKernelDcacheWritebackAll`.
  - `st01` se recompila (52 unidades, 54 MB de C++) y su librería compila y carga; se verifica
    contra el juego cuando se pueda entrar (F18b/F19). Compilar las 91 etapas tal cual sería unas
    25 veces `mgp_main`: hay que deduplicar (F04, (b) o (b')) antes de F21.
    **F21**: la (b') solo comparte el 3 % de las unidades; se compilaron las 92 tal cual (6 h 45 min a
    `-j5`) y la (b) se decide en F22.

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
- **Notas (F13)**: "la primera etapa" depende de F18b (el OSK del nombre). Con el título alcanza
  para cerrar F14b; la primera etapa 3D en Vulkan se comprueba cuando se pueda entrar. Con
  `build_overlays.py --stages init,r_main01,title` las etapas corren recompiladas
  (`--interpreter 0`).
- **Notas (F14b)**: hecha en Linux, ver `docs/sessions/F14b.md`.
  - `mpo_boot --renderer vulkan`: `GeRenderer` es el backend del GE del juego.
    - Después de cada `ge::Ge::Run` (`GeManager::set_after_run`), `EndFrame(true)` escribe en la
      VRAM lo dibujado.
    - En el primer uso de un target en cada frame, las filas de la VRAM que cambiaron (la CPU, el
      video) se vuelven a subir. Se detectan con una sombra por target.
  - Con `--window 1`, la ventana es una swapchain de Vulkan. Un flip presenta el target del GE si
    coincide con la VRAM, o sube el framebuffer (`PresentRgba`) si no.
    `MPO_VK_PRESENT_UPLOAD=1` fuerza la subida, y `--capture-window 1` captura la swapchain.
  - **Hasta "Input your name" (70 s), sobre lavapipe y con validación más sincronización**:
    - 0 errores;
    - la traza HLE es **idéntica byte a byte** a la de `soft` (295 065 líneas);
    - las 69 capturas son idénticas;
    - la ventana muestra en RGB lo mismo que la VRAM, por los dos caminos de presentación;
    - va en tiempo real con la ventana.
  - **Pendiente del usuario**: la misma prueba en Windows con la GPU (procedimiento en la nota).

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
- **Requisitos**: F14b y F15a. **Desde el 2026-10-27 va después de F18b** (decidido con el
  usuario al cerrar F14b).
  - Su criterio pide las escenas de F05 corriendo el juego. El codec, las cinemáticas y la misión
    1 solo se alcanzan pasando el OSK del nombre (F18b).
  - Casi todo lo que le queda (render-to-texture, transferencias de bloque y el coste del readback)
    se ve en esas escenas 3D, no en el título.
  - El orden en serie queda F16b → F17b → F18b → F15b → F19.
  - **Notas (F18b)**: ya se llega a una escena 3D, la celda de `st04vo`, con
    `analysis/f18b/walk_new_game.txt` más Start a los 90 s para saltar el cómic. Vulkan avisa ahí de
    "framebuffer reused with another format or stride" y "texture from a render target" (F15).
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
- **Hecha (2026-10-31)**: ver `docs/sessions/F15b.md`. **M3 cerrado.**
  - En el juego, Vulkan sale **idéntico a softgpu**:
    - 599 de 599 frames hasta la celda de `st04vo` y 1078 de 1078 hasta su gameplay con HUD y
      radar (360 s);
    - la traza HLE idéntica;
    - 0 errores de validación hasta los 100 s.
  - En los 11 dumps de F05 también es 0 píxeles, en los caminos 1.3 y 1.2.
  - Lo que lo hizo:
    - un readback anticipado cuando una textura, una CLUT, una transferencia o otro target toca
      filas dibujadas sin leer (`DrawBackend::SyncMemory`, `MPO_VK_NO_HAZARD_FLUSH`);
    - la **interpolación exacta de softgpu** en el fragment shader para triángulos y rectángulos
      texturizados (`GE_EXACT`, `MPO_VK_NO_EXACT`), incluidos los triángulos sin área, que softgpu
      sí dibuja (el 71-74 % de los triángulos del juego).
  - **Medido en el juego**: 0 render-to-texture, 0 transferencias de bloque y 0 stencil. Los splines
    en la CPU alcanzan (Vulkan va más rápido que softgpu en lavapipe). El stencil se queda sin
    hacer y el readback por ejecución se mantiene (ver la nota).
  - `profiles/mpo/tools/capture_compare.py` compara capturas del juego con el umbral de F15a.
  - **Pendiente del usuario**: la prueba en Windows con la GPU (procedimiento en la nota).
  - **Para F19**: `analysis/f15b/walk_dialog.txt` llega al gameplay de la celda. Hay un uso de
    vértices inmediatos (VAP) que `mpo_ge` no implementa.
- **Notas (F14b)**: ver "Pendiente" y "Para la próxima sesión" en `docs/sessions/F14b.md`.
  - **Ya está**: subir desde la VRAM los framebuffers que no dibuja el GE (`PresentRgba`) y la
    resincronización de los targets cuando la CPU escribe en la VRAM (por filas, contra una
    sombra).
  - **Falta el seguimiento fino**:
    - hoy hay un readback y una espera a la GPU después de **cada** ejecución de lista: hay que
      medirlo en una misión y saltárselo cuando nadie lea;
    - el render-to-texture y la textura desde el target dentro de un mismo frame;
    - las transferencias de bloque desde un target, que hoy leen la VRAM sin pasar por el
      renderer.
  - En el título aparecen dos aproximaciones, una vez cada una en 70 s y sin efecto en las
    capturas: "framebuffer reused with another format or stride" (el video 8888 y el juego 5551
    en `0x04000000`) y "texture from a render target" (**[?]** de qué draw es).
  - Para comparar en el juego: `analysis/f14b/run.sh` (`soft` y `vulkan`), las capturas por hash y
    `cmp` de las trazas.

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
- **Notas (F11b)**: con la política `stop`, el primer import sin HLE ya es
  `sceSasCore::__sceSasInit`, a los 232 848 µs (`docs/sessions/F11b.md`).
- **Notas (F16b)**: hecha, ver `docs/sessions/F16b.md`.
  - `hle::Audio` registra `sceAudio` (23 NIDs), `sceSasCore` (32) y `sceAtrac3plus` (38) sobre
    `mpo_audio`. El mezclador es un evento cada 64 muestras; `mpo_boot --audio 1` lo reproduce con
    SDL3 y `--audio-wav` lo vuelca. `mpo_audio_report` compara contra las grabaciones de PPSSPP.
  - **[V]** Los efectos de Start y Círculo del menú salen idénticos bit a bit a los de la grabación
    `extra2` de PPSSPP.
  - **Pendiente**:
    - la prueba en tiempo real en Windows (el audio de WSLg se atasca a los ~17 s);
    - la música de misión y el codec (`sceAtrac`), que se comparan en F19 contra
      `analysis/f16b/mission_*.wav`.

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
  - `sceMpeg` y `scePsmf` sobre el decodificador de F17a, en lugar del stub de F11b. **F11b ya portó
    `sceMpeg`** (23 NIDs) sobre `MediaEngine`: falta escribir la imagen y el PCM, y `scePsmf`.
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
- **Notas (F11b)**: ver `docs/sessions/F11b.md`.
  - `Mpeg::avc_decode` (`profiles/mpo/host/hle/hle_mpeg.cpp`) llama a `stepVideo(mode, true)` y
    escribe negro; `atrac_decode` consume el audio y escribe silencio. Falta `writeVideoImage` sobre
    la memoria de la PSP (con swizzle en VRAM) y el PCM.
  - **[V]** La intro es `MPO_OP_LONG.pmf` y el fondo del "Press Start" es `MPO_OP_LOOP.pmf`; los dos
    los reproduce la etapa `title` en los framebuffers 8888 `0x04000000`, `0x04088000` y
    `0x04110000`.
  - El texto de "Checking Memory Stick…" (después de NEW GAME) sale de `sceFont`.
- **Notas (F16b)**: **[V]** la música del título es el audio de `MPO_OP_LONG.pmf`: la grabación
  `analysis/f16b/menu_*.wav` de PPSSPP coincide con su PCM (desde el segundo 57,9) con correlación
  1,000. Con el PCM real en `sceMpegAtracDecode`, se comprueba con
  `mpo_audio_report <nuestro.wav> --ref analysis/f16b/menu_*.wav --ref analysis/f16b/extra1_*.wav`.
  - Después, comparar otra vez el recorrido Start → menú → Círculo contra `extra2`: en PPSSPP hay un
    fondo de banda ancha a −45 dB que aquí no se pide, porque el flujo del menú depende de
    `sceFont` y `sceUtilityMsgDialog` (ver `docs/sessions/F16b.md`).
- **Notas (F05)**: el decodificador real sustituye al stub de `sceMpeg`/`scePsmf` de F11.
  - La intro y el "Press Start" se muestran en tres framebuffers 8888 en `0x04000000`, `0x04088000`
    y `0x04110000`, sin draws del GE.
  - El bucle del título es probablemente `MPO_OP_LOOP.pmf` o `MPO_loop.pmf` [?].
- **Hecha (F17b)**: ver `docs/sessions/F17b.md`.
  - `sceMpegAvcDecode` escribe la imagen y `sceMpegAtracDecode` el PCM. En el juego, las capturas
    son idénticas a `mpo_pmf_decode` y el audio, idéntico muestra a muestra, con `MPO_OP_LONG` (la
    intro) y `MPO_OP_LOOP` (el fondo del "Press Start" al volver con Cruz) [V]. La música del título
    coincide con PPSSPP (forma de onda 1,000).
  - El vídeo sale unos 57 ms después de su audio, constante y sin deriva. Viene del juego (decodifica
    dos frames antes de enseñarlos). Falta compararlo con PPSSPP: hace falta una grabación con audio
    y vídeo en el mismo reloj, porque el AVI de PPSSPP graba un frame por flip.
  - `scePsmf` no se implementó: no lo importa nadie [V].
  - `sceLibFont` (`host/hle/hle_font.cpp`): los 8 NIDs de `mgp_main`. El juego pide `ltn0` y `jpn0`,
    y "Checking Memory Stick…" coincide en posición con PPSSPP. `mpo_boot --fonts`, por defecto
    `flash0/font` junto al ISO.

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
- **Notas (F11b)**: ver `docs/sessions/F11b.md`.
  - **Criterio heredado de F09, F10b y F11b**: con NEW GAME y un nombre en el OSK, la traza muestra
    al juego abriendo y leyendo `slot.dat` (LBN `0x39650`) sin errores. `iso_lbn.py --trace` nombra
    los `sce_lbn` de una traza.
  - **[V]** En el TITLE MENU, Circle confirma y Cross vuelve. NEW GAME comprueba la Memory Stick,
    muestra "Input your name. You may not use lowercase letters." y espera con
    `sceUtilityOskGetStatus` y `sceUtilityMsgDialogGetStatus` (`--press start@30000+200 --press
    circle@40000+200`).
- **Criterios de hecho**: se guarda y se carga entre ejecuciones, se ingresa una contraseña de
  reclutamiento y se borra un save desde el menú del juego (`LISTALLDELETE`), comprobando con
  hashes que el resto de `SAVEDATA` no cambia. Los saves de prueba se hashean antes y después.
  **M4 cerrado.**
- **Notas (F17b)**: `sceFont` ya dibuja "Checking Memory Stick…", pero hoy solo se ve 8 flips
  porque `sceUtilitySavedata*` es un stub. Con F18b tiene que quedarse lo que dure la comprobación.
  Volver a comparar el recorrido Start → menú → Círculo contra `extra2` (el fondo a −45 dB de F16b).
- **Notas (F13)**: al pasar el OSK se entra a la primera etapa jugable. Hay que recompilarla
  (`build_overlays.py --stages <s>`) y comprobar en el reporte de `mpo_boot` que su overlay sale
  "verified" (si sale con un aviso de base, añadirla a `config/stages.toml`).

- **Notas (cierre de F18b)**: hecha, ver `docs/sessions/F18b.md`.
  - NIDs sobre `mpo_savedata` (`host/hle/utility.hpp`), dibujo con PPGe (`host/hle/ppge.hpp`) y
    texto PGF del firmware, `ms0:/` por defecto en la carpeta por sistema operativo,
    `mpo_boot --osk-text` y el teclado del PC en el OSK.
  - **[V]** NEW GAME → nombre (OSK) → `AUTOSAVE` → dificultad → `r_main02` → `flashdemo` →
    **`st04vo`, la celda, la primera etapa jugable**, todo recompilado. `slot.dat` se lee por
    `umd1:` por sectores, no por `sce_lbn`.
  - **[V]** Cargar (`LISTLOAD`) en otro proceso, y borrar (`LISTALLDELETE`) solo el save elegido,
    con hashes.
  - **Cambios de plan**: la contraseña de reclutamiento se prueba en F19 (está en `briefing`,
    después del prólogo), y el conversor de saves cifrados es la fase F18c.

### F18c — Conversor de saves cifrados en el instalador (autónomo)

- **Objetivo**: poder usar en el port un save de una PSP o de PPSSPP, y llevar uno del port a ellas.
- **Requisitos**: F18a (la interfaz `SavedataCipher` y el contrato de MPO) y F02 (libkirk en el
  instalador). No necesita el juego en marcha: se puede hacer en una ola, en su worktree.
- **Decidido por el usuario el 2026-10-30**: un conversor en el instalador, y el runtime nunca lleva
  claves.
- **Tareas**:
  - `mpo --import-save <dir>` y `mpo --export-save <dir>`: un save de MPO en modo 5 (de una PSP o de
    PPSSPP) a uno sin cifrar en `ms0/PSP/SAVEDATA`, y al revés.
  - Implementar `SavedataCipher` portando `EncryptData`, `DecryptData`, `UpdateHash` y `BuildHash`
    de `Core/Dialog/SavedataParam.cpp`, más `Core/HLE/sceChnnlsv.cpp`, que se **descarga al
    configurar con su SHA-256** (como `third_party/ppsspp_crypto`) porque trae claves.
  - Tests sin datos del juego (un save sintético cifrado y descifrado de ida y vuelta).
- **Criterios de hecho**: un save de PPSSPP del usuario (`PSP/SAVEDATA/ULUS10202*`) se importa, el
  port lo carga con LOAD GAME, y exportarlo de vuelta da un save que PPSSPP carga; todo en local.
- **Notas (cierre de F18c)**: hecha, ver `docs/sessions/F18c.md`.
  - `mpo --import-save <dir>` y `mpo --export-save <dir>` (`--ms0`, `--save`, `--game-dir`,
    `--overwrite`), sobre `sceChnnlsv.cpp` de PPSSPP sin modificar (descargado con su SHA-256) y
    `crypto::KirkSavedataCipher`. La clave del juego sale del `title.prx` instalado del usuario
    (`config/savedata.toml`, con los SHA-256 del archivo y de la clave).
  - **[V]** El save de PPSSPP del usuario es modo 5, se importa con el hash comprobado, el port lo
    carga con LOAD GAME (sigue a `briefing`, 19 de noviembre de 1970), y `UpdateHash` reproduce bit a
    bit los `SAVEDATA_PARAMS` que escribió PPSSPP.
  - **Pendiente del usuario**: cargar en PPSSPP los dos exports de `analysis/f18c/for-ppsspp/`
    (ranuras `002` y `003`). En Windows, la CI pasa el test con el cifrador real.

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
- **Notas (F18b)**: ver `docs/sessions/F18b.md`.
  - La primera etapa jugable es **`st04vo`** (la celda), ya recompilada, igual que `r_main02` y
    `flashdemo`. El recorrido para llegar es `analysis/f18b/walk_new_game.txt` más Start para
    saltar el cómic.
  - **Criterio que viene de F18b**: ingresar una contraseña de reclutamiento en `briefing` (OSK con
    límite de 10).
- **Notas (F15b)**: ver `docs/sessions/F15b.md`.
  - `analysis/f15b/walk_dialog.txt` (Círculo cada 3 s desde los 120 s) pasa la cinemática de la
    celda y deja el gameplay en `st04vo` a los ~200 s. Es un buen punto de partida para la
    repetición de input.
  - Para comparar los dos renderers en el juego: `analysis/f15b/walk.sh` con
    `capture_compare.py`. Hoy dan 0 píxeles de diferencia.
  - `mpo_boot` informa `ge: unsupported: immediate vertices (VAP)` una vez por recorrido: hay que
    ver qué dibuja.
- **Notas (F13)**: el criterio de F13 "entrar a la primera etapa sin código interpretado" se
  comprueba aquí con el reporte de `mpo_boot` (tabla "stage overlays"). Las etapas de la misión y
  su transición tienen que estar en `build_overlays.py --stages`.
- **Notas (F16b)**: el criterio de audio de F16b que falta (la música y el codec sin cortes) se
  comprueba aquí: `mpo_boot --audio-wav` en la misión y
  `mpo_audio_report <nuestro.wav> --ref analysis/f16b/mission_*.wav`. Es lo primero que usa
  `sceAtrac` en el juego (los `.sep` de `USRDIR/oss/`).

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
- **Notas (F19)**: ver `docs/sessions/F19.md`.
  - **Criterio que viene de F19 (y antes de F18b): ingresar una contraseña en `briefing`** (OSK con
    límite de 10). Los textos "PASSWORD:", "ENTER PASSWORD", "KEYWORD:" y "VERIFY PASSWORD" están en
    `briefing.prx` junto a los del punto de acceso ("ACCESS POINT:", "SEARCHING…", "CONNECTING…"),
    y en la barra de íconos de `briefing` la antena sale en gris: es el menú de red [?].
  - `analysis/f19/play3.input` llega al mapa de `briefing` (1135 s), y `rp3c.input` entra a `st02`.
    Se continúan con `analysis/f19/play.sh <nombre> <grabación>`.
  - `sceWlanGetEtherAddr` se llama cientos de veces en `briefing` y en las misiones (hoy devuelve
    0 sin implementar).

- **Notas (cierre de F20b)**: hecha, ver `docs/sessions/F20b.md`. Se recluta por AP Scan en el
  juego desde el save del usuario, con 64 escaneos seguidos y dos salidas limpias; la contraseña
  (OSK, límite 10) se valida; GPS SCAN dice que no hay receptor y vuelve; MATCH (ad hoc) crea y busca
  partidas sin nadie y sale limpio. La antena de `briefing` (infraestructura) sigue en gris en las
  partidas que hay.

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
- **Notas (F19)**: ver `docs/sessions/F19.md`.
  - Las partidas se juegan por tramos con `--record-input` y `--replay-input` (`analysis/f19/play.sh`)
    y se verifican sin ventana: cada grabación lleva el hash del frame cada 60 flips.
  - **Hacen falta savestates o un renderer más rápido** para retomar: con softgpu la repetición va a
    ~1,6x, y retomar después de 1135 s ya tarda ~12 min.
  - Etapas recompiladas hasta ahora: `init`, `r_main01`, `r_main02`, `title`, `flashdemo`, `st01`,
    `st04vo`, `st04m1`, `st04m3`, `st04`, `st01t1` (mismo PRX que `st01t2`), `briefing` y `st02`.
    `play.sh` corre en el intérprete las que falten, y el informe las nombra (`mgp_stage@<sha>`).
  - Los menús de MPO se mueven con la cruceta, no con el stick [V].
- **Notas (F18c)**: el diálogo de la lista de saves no dibuja `ICON0.PNG` (sale un recuadro con el
  número de ranura; PPSSPP muestra el ícono). Un save de PPSSPP del usuario importado con
  `mpo --import-save` deja empezar desde mitad del juego (`briefing`, 19 de noviembre de 1970).
- **Notas (F20b)**: ver `docs/sessions/F20b.md`.
  - `analysis/f20b/ap-user2.input` es la grabación más avanzada: el save importado, el AP Scan, la
    contraseña, GPS SCAN y MATCH. Se sigue con `analysis/f20b/play.sh <nombre> <grabación>`.
  - Falta probar el menú de red de la antena (Netconf e infraestructura) cuando se active en la
    historia: tiene que decir que no hay red y dejar volver.
  - `n01` y `ntmenu2` (MATCH) corren en el intérprete; hay que recompilarlas.
  - `mpo_boot --watch-call <dirección>` anota en la traza los argumentos y el resultado de una
    función del juego: sirve para investigar un bug sin depurador.
  - Una repetición necesita los mismos `--ap-seed`, `--ap-place` y `--mac` que su grabación (los tres
    tienen valores por defecto fijos).

- **Notas (cierre de F21)**: hecha en parte, ver `docs/sessions/F21.md`. **Se partió** (acordado con
  el usuario): lo técnico queda hecho y el recorrido pasa a F21b, en paralelo con F22.
  - Las 92 etapas distintas están recompiladas y compiladas (`build-f09-release/overlays`, 2,8 GB):
    ninguna depende ya del intérprete. Todas las etapas de la historia tienen sus imports en el HLE.
  - Los tramos se encadenan con los saves del juego (`analysis/f21/play.sh <nombre> <ms0>`), no con
    savestates.
  - Jugadas en el port: `st03t1` y `st07m1`, con 0 instrucciones interpretadas; las repeticiones
    sin ventana con softgpu dan los mismos hashes que las partidas con Vulkan.
  - **En WSL, Vulkan corre en `llvmpipe`** (por software, sin la GPU): en WSL se juega con
    `MPO_RENDERER=soft`.

### F21b — Recorrido completo (Linux y Windows)

- **Objetivo**: terminar el juego en el port, en Linux y en Windows, y que no queden crashes
  conocidos sin documentar.
- **Requisitos**: F21. **Corre en paralelo con F22** (decidido el 2026-11-01): el recorrido depende
  del tiempo de juego del usuario y F22 no necesita que termine para empezar. M6 se cierra cuando
  terminen las dos.
- **Tareas**:
  - El usuario juega por tramos (`MPO_RENDERER=soft analysis/f21/play.sh <tramo> <ms0 anterior>`)
    hasta los créditos, guardando antes de cerrar cada tramo.
  - Cada bug se anota en la lista de `docs/sessions/F21.md` (o de la nota de F21b), se reproduce con
    la grabación del tramo y se arregla.
  - Atajos nativos (`register_native_fast_path`) solo si un tramo no llega a tiempo real con softgpu.
  - Probar la estabilidad en sesiones largas (la memoria de cada tramo sale en `<tramo>.time.txt`).
  - Windows: compilar y jugar al menos un tramo con la GPU de verdad (Vulkan) y el mando (XInput).
- **Criterios de hecho**: el juego se completa en Linux y en Windows; las repeticiones de los tramos
  salen idénticas sin ventana; la lista de bugs no tiene crashes sin documentar.
- **Notas (F21)**: ver `docs/sessions/F21.md`.
  - El último save es `analysis/f21/ms0-tramo3` (después de `st07m1`).
  - Pendiente con el usuario: si la X blanca grande sobre Snake en `st07m1` (bug 8) sale también en
    PPSSPP o en la PSP.
  - **Trampa**: cambiar un header del framework recompila las 92 etapas (unas 24 h de CPU); para
    depurar el runtime, otro directorio de build.

### F22 — Distribución: compilar en la máquina del jugador

- **Objetivo**: que alguien sin conocimientos técnicos pueda instalarlo y jugarlo.
- **Requisitos**: F21 (F21b, el recorrido, corre en paralelo).
- **Notas (F21)**: ver `docs/sessions/F21.md`.
  - **Coste de compilar las etapas** (para decidir el modelo de distribución): las 78 que faltaban
    tardaron 6 h 45 min a `-j5` con Clang; una unidad de misión tarda 20,8 s con `-O2`, 15,9 s con
    `-O1` y 1,5 s con `-O0`. La deduplicación (b') solo comparte el 3 % de las unidades; la (b)
    (funciones parametrizadas en el emisor) es la única que ahorraría de verdad.
  - El usuario encontró los controles de PC "toscos" (refuerza la nota de F19 sobre el mapeo).
  - El mando del usuario (XInput "Xbox 360", USB o Bluetooth) no se probó: WSL no tiene `evdev`.
    Probarlo en Windows y en Steam Deck.
  - La calidad de imagen (480×272 escalado al vecino más cercano) le pareció baja: es de F23, pero
    exponer `linear_filter` de `SdlPresenter` en `mpo_boot` es barato.
- **Notas (F18c)**: el README tiene que explicar `mpo --import-save` y `mpo --export-save` (pasar
  saves de una PSP o de PPSSPP al port y al revés). El launcher puede ofrecerlo como botón. No hay
  opción para cambiar de ranura (ver `docs/sessions/F18c.md`).
- **Notas (F19)**: el mapeo de teclado por defecto es el de PPSSPP (IJKL para el stick), y al
  usuario le resultó incómodo. Propone WASD para el stick, Espacio para Cruz (correr y rodar) y Ctrl
  para agacharse; su `input.toml` está en `docs/sessions/F19.md`. Hay que decidir el mapeo de PC por
  defecto (los menús siguen en la cruceta, así que las flechas tienen que quedar en la cruceta).
- **Tareas**:
  - Flujo de instalación: el usuario elige el ISO, se valida, se descifra, se genera y se compila, o
    se usa un binario precompilado del runtime más la generación en local. **Hay que decidir el
    modelo.**
  - **Decisión abierta (del usuario): distribuir el código del juego ya compilado o compilarlo en la
    máquina del jugador.** Los otros ports de PSP están en `docs/RESEARCH.md` §3, "Cómo distribuyen
    los ports":
    - **compilar en local** es lo que pide hoy `CLAUDE.md` §1 (el código traducido no se
      distribuye) y lo que hacen gow-psp-recomp, fuc-recomp y tkdr-recomp. El modelo más cómodo es el
      launcher de GoW: descarga un compilador portátil (llvm-mingw, CMake y Ninja), no necesita
      Visual Studio y compila una sola vez. Para que la espera sea aceptable, habrá que bajar el
      coste de compilar el corpus: optimización menor del código generado (hoy
      `PSPRECOMP_GENERATED_OPT_LEVEL` no se aplica a MSVC), clang en vez de `cl.exe`, o un emisor por
      funciones en vez de por palabras (F07b);
    - **distribuir binarios con el código compilado** es lo que hace Yakumo: el jugador no espera
      nada, pero el proyecto publica una traducción del código del juego, y eso cambia su riesgo
      legal y la regla de `CLAUDE.md` §1.
    - Arrancar ya compilado es casi inmediato en los dos casos: registrar el dispatch tarda unos
      60 ms (F08b).
  - Linux: AppImage o Flatpak. Windows: ZIP.
  - CI que publique solo el código y el runtime, nunca nada del juego.
  - `README.md` para el usuario, smoke test manual y aviso legal.
- **Criterios de hecho** (redefinidos al partirla, el 2026-11-01): el modelo de distribución está
  decidido, y en Linux `setup.py install` lleva de un ISO a un `mpo_boot` que juega, sin Ghidra y
  sin la carpeta `analysis/`. La instalación limpia que cierra M6 pasa a F22c.
- **Notas (cierre de F22)**: ver `docs/sessions/F22.md` y `docs/analysis/distribution.md`.
  - **Decidido por el usuario**: un launcher que compila en la máquina del jugador (`CLAUDE.md` §9,
    decisión 10) y el mapeo de PC (decisión 11, hecho).
  - `profiles/mpo/scripts/setup.py` (`install`, `stages`, `play`, `status`) hace el camino entero
    sin Ghidra: los hints van versionados en `profiles/mpo/config/recomp/`.
  - Las etapas se compilan en segundo plano y el juego las toma al cargarlas (`OverlayManager`
    vuelve a mirar la carpeta).
  - `mpo_boot --filter linear` (el filtrado bilineal de la ventana).
  - **Se partió**: el toolchain portátil, el launcher, los paquetes y la CI de release pasan a F22b;
    el README, el aviso legal y las instalaciones limpias, a F22c.

### F22b — Toolchain portátil, launcher y paquetes

- **Objetivo**: que el jugador no tenga que instalar compiladores ni saber qué es CMake.
- **Requisitos**: F22. Para probar en Windows y en Steam Deck hace falta el usuario.
- **Tareas** (el diseño y las opciones están en `docs/analysis/distribution.md` §3 y §4):
  - **Windows**: compilar el proyecto con llvm-mingw (nunca se probó) y el juego entero en Windows
    (tampoco). Medir el límite de 65 535 exports de `mpo_boot`. Que el launcher baje llvm-mingw,
    CMake, Ninja y Python embebible con su SHA-256 fijado. Resolver FFmpeg en Windows (prebuilt
    LGPL desde la CI, o el `sh` de un MSYS2 portátil).
  - **Linux y Steam Deck**: un Flatpak con `org.freedesktop.Sdk` como runtime, para compilar dentro
    del sandbox. Verificar qué trae el SDK (los [?] del documento).
  - **El launcher**: elegir el ISO y las fuentes, el progreso de `setup.py install`, el botón
    Jugar, la compilación de las etapas en segundo plano y las acciones de saves (F18c). Decidir
    entre Python con Tkinter (PyInstaller) y C++ con SDL3 y Dear ImGui.
  - **La CI de release**: que publique solo el código, el launcher y lo que no es del juego. Un
    test que falle si un artefacto trae algo de `game/`, `analysis/` o `generated/`.
  - Que `setup.py play` decida `--unimplemented` para la release (ver el documento, §5).
  - **Compilar las etapas en dos niveles** (acordado con el usuario el 2026-11-01), para que todo
    el juego esté compilado en ~30 min y no en horas:
    - primero las 92 a `-O0` (1,5 s por unidad de misión, contra 20,8 s a `-O2`, F21);
    - después, a `-O2` en segundo plano, reemplazando a las de `-O0`.

    Hay que medir antes cuánto corre una etapa a `-O0` frente al intérprete y a `-O2`, con la
    repetición de `tramo3`. El runtime tiene que preferir la librería optimizada cuando las dos
    describen el mismo PRX: hoy `OverlayManager` toma la primera que coincide. Lo mismo vale para
    la clave de `setup.py stages`, que tiene que incluir el nivel.
- **Criterios de hecho**: en una máquina Windows y en una Linux sin herramientas de desarrollo, el
  launcher lleva del ISO al juego sin pasos manuales.
- **Notas (cierre de F22b)**: ver `docs/sessions/F22b.md` y `docs/analysis/distribution.md`.
  - **Decidido**: el launcher en C++ con SDL3 y Dear ImGui (el usuario); FFmpeg para Windows
    precompilado por la CI con el mismo llvm-mingw; `--unimplemented stop`, con el error a la
    vista en el launcher.
  - `setup.py toolchain` baja llvm-mingw, CMake y Ninja fijados en `config/toolchain.toml`. Con
    ellos, el proyecto compila y pasa los tests en la CI (`windows-llvm-mingw`). libc++ necesitó
    `-include algorithm` (el header del framework se arregla en el fork más adelante, para no
    recompilar el corpus).
  - **Las etapas en dos niveles**: `stages` las compila primero a `-O0` (**las 92 en ~40 min**,
    contra 6 h 45 min a `-O2`) y después optimizadas.
    `OverlayManager` instala la más optimizada y vuelve a mirar la carpeta en cada carga. Con
    `-O0`, la repetición de `tramo1` coincide entera (511/511); **con el intérprete se separa en
    `st03t1`**.
  - `packaging/make_release.py` arma el ZIP de Windows y el tarball (sin el perfil de ejemplo del
    framework, que trae código generado de otro juego) y los revisa por ruta y por contenido.
    `release.yml` hace eso, el Flatpak y un borrador de release.
  - **Pendiente del usuario**: el ZIP en su Windows, del ISO al juego, y el Flatpak en Linux o en
    la Steam Deck.

### F22c — README, aviso legal e instalaciones limpias

- **Objetivo**: que alguien sin conocimientos técnicos pueda instalarlo y jugarlo siguiendo solo
  el README. **Cierra M6**, junto con F21b.
- **Requisitos**: F22b, y la prueba del usuario del ZIP de Windows (ver
  `docs/sessions/F22b.md`, "Para la próxima sesión").
- **Notas (F22b)**: el ZIP de Windows ya trae un `README.txt` corto
  (`packaging/windows/README.txt`). El README de la raíz puede apuntar a él y al launcher. El
  launcher dice "Not affiliated with Konami"; el aviso legal completo va en el README.
- **Tareas**:
  - `README.md` en la raíz, para el jugador: qué hace falta (el ISO de `ULUS-10202`, las fuentes
    del firmware, espacio en disco y tiempo de la primera compilación), el launcher, los
    controles (el mapeo de F22), el mando, cómo pasar los saves (`mpo --import-save` y
    `--export-save`, F18c) y qué hacer si algo falla.
  - El aviso legal: no se distribuye nada del juego, el usuario pone su copia.
  - El smoke test manual de `CLAUDE.md` §7, con los saves hasheados antes y después.
  - Una instalación limpia en Windows y en Linux (y Steam Deck si se puede), siguiendo solo el
    README.
- **Criterios de hecho**: las instalaciones limpias llegan a jugar siguiendo solo el README.
  **M6 cerrado** (con F21b).
- **Notas (cierre de F22c)**: ver `docs/sessions/F22c.md`.
  - `README.md` (inglés) y `README.es.md` (español), decidido por el usuario, con el aviso legal.
    `docs/smoke-test.md` y `packaging/hash_saves.py` son el smoke test de `CLAUDE.md` §7.
  - **Dos instalaciones limpias en Linux** siguiendo solo el README: en esta máquina desde el
    tarball (20 min 05 s, y la repetición de `tramo3` coincide entera) y en un contenedor
    `ubuntu:24.04` vacío con solo los paquetes del README.
  - Arreglado en el camino: el navegador del launcher no llegaba a los saves de PPSSPP en Linux
    (carpetas ocultas, ruta escrita y un botón a `SAVEDATA`); el Flatpak no podía exportar saves
    (Descargas); sin los headers de audio, SDL3 sale sin sonido (la lista de paquetes); y FFmpeg
    tiene un espejo, porque `ffmpeg.org` se cayó durante la prueba.
  - **Pendiente del usuario**: el smoke test en Windows con el ZIP y en el Flatpak. Con eso y F21b
    se cierra M6.

---

## F23+ — Mejoras (se planifican cuando M6 esté cerrado)

### F25a — Íconos del HUD según el dispositivo, autónomo (ola 7)

- **Objetivo**: el paso 2 del spike (`docs/analysis/button-prompts.md` §7.2): que los íconos de
  botón de los menús y del HUD del juego (texturas propias, §2 del spike) muestren los botones del
  último dispositivo, como ya lo hacen los diálogos del sistema (F24b).
- **Requisitos**: F24b (`DeviceTracker`, `PromptSet` y el registro `p`) y F24c
  (`TextureReplacer`, `ReplacementSet` y el cambio en caliente).
- **Cómo se combina con el pack del jugador** (la pregunta que quedó abierta en F24c):
  - por capas, de arriba abajo: (1) los íconos propios del jugador para ese estilo, un pack de
    F24c en `<datos>/prompts/<estilo>/`; (2) los íconos que dibuja el port para las claves del
    inventario; (3) el pack del jugador (`--textures`), y por último la textura del juego;
  - los íconos del dispositivo ganan al pack del jugador **solo en las claves de los íconos** y
    **solo con un juego de botones que no es el de la PSP**. Con el de la PSP (sin ventana, con
    `--prompts psp` o en una repetición sin `p`), todo queda exactamente como en F24c. Un pack
    del jugador está hecho para los botones de la PSP, así que sus íconos serían los equivocados
    con otro mando; el resto del pack no cambia;
  - `--prompt-icons off` deja solo el pack del jugador y el juego.
- **Tareas**:
  - Un **inventario de los íconos** en el perfil (`config/prompt_icons.toml`): la clave de F24c
    sin la dirección, el botón de la PSP y el marco del ícono dentro de la textura. Solo claves y
    números, ningún asset (CLAUDE.md §1). El jugador añade las suyas sin recompilar en
    `<datos>/prompts/inventory.toml`. Hoy se conoce una sola clave [V], el △ de la pausa
    (`0000000001e57e0adb2960d5`, F24c); el resto se saca en el juego.
  - Una herramienta que arme el inventario de un volcado (`--dump-textures`): compara cada PNG
    con las páginas de las TXP (`txp.py`) y escribe las entradas de las páginas que son botones.
  - **Íconos dibujados por el port** (código GPL-3.0-or-later, sin PNG en el repo ni nada sacado
    de las texturas del juego, y sin logos de marca): letras en un círculo para los botones de la
    cara de un mando, una tecla con su nombre corto para el teclado, un mouse con el botón
    marcado y una píldora con texto para L, R, Start y Select. Las figuras (○ × △ □) se quedan con
    la textura del juego. Salen del `PromptSet` que ya se muestra, así que siguen el mapeo de
    `input.toml`.
  - Elegidos con el mismo `PromptSet` que los diálogos (`DeviceTracker::generation()` y
    `PromptSet::style`, F24b), en la misma muestra del mando; el cambio se aplica en la siguiente
    lista del GE, como `SetActiveSet`.
  - Un registro `i <muestra> off|on <digest>` en las grabaciones, para que la repetición ponga
    los íconos donde los tenía la partida; una grabación sin `i` (las de antes) se repite sin
    íconos, como se grabó.
  - `mpo_boot --prompt-icons auto|off|<carpeta>`, y README.md y README.es.md a la vez.
  - Sin tocar el ABI del runtime ni el framework, y con cambios mínimos en
    `host/texreplace/` (F25c también lo toca).
- **Criterios de hecho**: tests sin el juego (el inventario, los íconos dibujados, las capas y su
  prioridad, que con el juego de la PSP y sin íconos la VRAM queda igual byte a byte, el cambio
  en la lista siguiente, el registro `i` al grabar y al repetir, y que las grabaciones viejas no
  cambian). En el juego (pendiente del usuario): el inventario de los íconos y la pausa con un
  mando de Xbox y con el teclado, en softgpu y Vulkan. `ctest` completo en Clang y GCC.

### F24a — Ajustes del juego en el launcher, autónomo (ola 6)

- **Objetivo**: elegir en el launcher, con mouse o con mando, cómo corre el juego, sin variables
  de entorno.
- **Requisitos**: F23b (`--render-scale`), F22b (el launcher).
- **Tareas**:
  - Una sección "Settings" en el launcher instalado:
    - la resolución interna de 1× a 8×, mostrando los píxeles (8× = 3840×2176);
    - el tamaño de la ventana (`--scale`) y la pantalla completa (si `mpo_boot` no la tiene,
      añadirla con SDL3);
    - el renderer (Vulkan o software, para diagnóstico).
  - Se guardan en un archivo de ajustes de la carpeta de configuración (`%APPDATA%\mpo-pc-port`
    o `~/.config/mpo-pc-port`), que `setup.py play` lee y convierte en opciones de `mpo_boot`. Lo
    que el jugador pase a mano después de `--` gana.
  - Con un valor inválido o un archivo dañado, se usan los valores por defecto con un aviso; nunca
    se impide jugar.
  - README.md y README.es.md a la vez.
- **Criterios de hecho**: tests de `setup.py` (leer, validar y convertir en opciones) y del
  núcleo del launcher (sin ventana). Una captura del launcher con la sección nueva. `ctest`
  completo en Clang y GCC.

### F24b — Controles en pantalla según el dispositivo, parte I, autónomo (ola 6)

- **Objetivo**: el paso 1 del spike (`docs/analysis/button-prompts.md`): saber cuál fue el último
  dispositivo, y que nuestros diálogos muestren sus botones.
- **Requisitos**: el spike y F23c (PPGe con la fuente de respaldo).
- **Tareas**:
  - `last_device` en el frontend: teclado, o el tipo de mando (Xbox, PlayStation, Nintendo,
    genérico) con `SDL_GetGamepadType`. Cambia con el último input real, no con ruido del stick.
  - Un `PromptSet` sacado del `input.toml` activo (las teclas y botones que de verdad están
    asignados). PPGe y `hle_utility.cpp` lo usan en vez de los ○ × △ □ fijos.
  - Una opción `--prompts auto|psp|xbox|playstation|keyboard` (`auto` por defecto).
  - Las grabaciones guardan el cambio de juego de botones en un registro `p <muestra> <juego>`. Las
    viejas se repiten con el juego de la PSP, y la repetición sigue siendo determinista.
- **Criterios de hecho**: tests sin el juego (el último dispositivo, el `PromptSet` según el
  mapeo, el registro `p` al grabar y al repetir, y que las grabaciones viejas no cambian). Una
  captura local de un diálogo con cada juego de botones. `ctest` completo en Clang y GCC.

### F24c — Reemplazo de texturas por hash, autónomo (ola 6)

- **Objetivo**: la capa que sustituye texturas del juego por PNG propios, en softgpu y en Vulkan.
  Es la base de las texturas HD y de los íconos por dispositivo (paso 2 del spike).
- **Requisitos**: F15b y el spike (§7.2 de `docs/analysis/button-prompts.md`).
- **Tareas**:
  - Una clave por contenido, sin la dirección: los texels más la CLUT, con xxh64, como el
    `ignoreAddress` de PPSSPP v1.20.4 (`GPU/Common/TextureReplacer`). Un `textures.ini` compatible
    en lo posible con el de PPSSPP. Si hace falta xxHash, va en `third_party/` con su licencia.
  - Una carpeta de reemplazos (`--textures <dir>`, por defecto en la carpeta de datos del
    jugador) y un modo para volcar las texturas que se usan (`--dump-textures <dir>`), para hacer
    los packs. Los volcados no van al repo.
  - softgpu usa el reemplazo a la resolución nativa (lo reduce si el PNG es mayor) y Vulkan a la
    resolución del PNG. A 1×, con un PNG nativo, los dos renderers quedan iguales.
  - Sin carpeta de reemplazos, todo exactamente como hoy. Con reemplazos, las capturas y los hashes
    de las repeticiones pueden cambiar; se documenta.
  - Dejar listo el cambio en caliente entre dos juegos de reemplazos (lo usará F24b para los
    íconos), aunque la regla que elige cuál quede para después.
- **Criterios de hecho**: tests sin el juego (la clave, el parseo de `textures.ini`, que sin
  carpeta no cambia nada, el reemplazo en los dos renderers y el cambio en caliente). En el juego,
  un reemplazo de prueba de un ícono (el △ del menú de pausa, del spike) visible en softgpu y en
  Vulkan, con capturas locales. `ctest` completo en Clang y GCC.

### F23a — Cámara con el stick derecho, autónomo (ola 5)

- **Objetivo**: girar la cámara del juego con el stick derecho del mando, como ya se hace con el
  mouse (`[mouse_camera]`, F22).
- **Requisitos**: F22. Adelantada el 2026-11-03 a pedido del usuario (ver la ola 5).
- **Tareas**:
  - La cámara del juego se mueve con la cruceta [V por el usuario, F22]. Un `[pad_camera]` en
    `input.toml` hace que el eje X del stick derecho apriete ← o → pasado un umbral, con
    histéresis. El eje Y apretaría ↑ o ↓, pero solo con `vertical = true` (apagado por defecto,
    como en `[mouse_camera]`). Que el giro siga la inclinación si se puede (pulsos cuya
    frecuencia crece con el ángulo); si no, todo o nada.
  - Solo el frontend (`profiles/mpo/host/frontend`: `InputTranslator`, `input_config`,
    `sdl_input`). El stick derecho no tiene binding por defecto. Si el `input.toml` del jugador
    le pone uno en `pad_axes`, ese gana y `[pad_camera]` no actúa sobre ese eje.
  - Soltar todo al perder el foco o al desconectar el mando, como `ReleasePad`.
  - Las grabaciones no cambian, porque guardan muestras de `sceCtrl`.
  - README.md y README.es.md (Controls), los dos a la vez (§7 de CLAUDE.md).
- **Criterios de hecho**: tests sin el juego del umbral, la histéresis, la parte proporcional,
  el parseo de `[pad_camera]`, la prioridad de `pad_axes` y el soltar al perder el foco. `ctest`
  completo en Clang y GCC. Las repeticiones de `analysis/f21` salen idénticas, porque sin mando
  no cambia nada.

### F23c — Fuente libre de respaldo para los diálogos, autónomo (ola 5)

- **Objetivo**: que los diálogos del sistema (guardar, cargar, borrar, mensajes y el teclado en
  pantalla) muestren su texto aunque el jugador no tenga las fuentes del firmware.
- **Requisitos**: F18b (PPGe y las fuentes PGF). Adelantada el 2026-11-03 a pedido del usuario:
  en su Windows, sin fuentes, el teclado y el diálogo de guardado salieron vacíos (F22c).
- **Tareas**:
  - Elegir una fuente con licencia compatible con GPLv3 (SIL OFL o similar, con cobertura
    latina; el OSK solo pide dígitos y mayúsculas, F18b) y ponerla en `third_party/` con su
    licencia al lado y en `THIRD_PARTY.md`.
  - Que llegue al jugador sin pasos aparte, idealmente dentro del binario (por ejemplo, un atlas
    o una tabla de glifos generada y versionada, como el SPIR-V de F14a, con su generador en el
    repo).
  - PPGe (`profiles/mpo/host/hle/ppge.*`) la usa cuando no hay PGF del firmware. Con las PGF
    todo queda exactamente como hoy.
  - `sceFont` (el texto propio del juego, como "Checking Memory Stick") solo si cabe sin
    riesgo. Las métricas cambian el layout del juego; si no se hace, se anota.
  - `mpo_boot` dice qué fuente usa. Actualizar los mensajes que hoy dicen "show no text"
    (`setup.py`, el launcher, README.md y README.es.md a la vez).
- **Criterios de hecho**: tests sin el juego (los glifos de la fuente de respaldo, que PPGe la
  elige solo sin PGF y que con PGF no cambia nada). Una captura local del OSK y de un diálogo de
  guardado sin fuentes, que no va al repo. `ctest` completo en Clang y GCC.

### F23b — Resolución interna configurable, hasta 4K, autónomo (ola 5)

- **Objetivo**: que el renderer Vulkan dibuje a N veces la resolución de la PSP (480×272), de 1×
  a 8× (8× = 3840×2176, "4K"), y que el resto del runtime no lo note.
- **Requisitos**: F15b. Adelantada el 2026-11-03 a pedido del usuario (ver la ola 5).
- **Tareas**:
  - Una opción `--render-scale N` en `mpo_boot` y una variable `MPO_RENDER_SCALE` (A/B, como los
    `MPO_VK_*`), y que el launcher la pueda pasar después (se anota, no hace falta ahora). Con
    1× todo tiene que quedar exactamente como hoy.
  - Los targets de color y profundidad de `GeRenderer` a escala N: viewport, scissor, región,
    líneas, puntos, sprites y clears. La interpolación exacta de softgpu (F15b, aristas enteras)
    tiene que seguir siendo exacta a 1×; a N× vale el muestreo a la resolución nueva (como
    PPSSPP).
  - Todo lo que vuelve a la VRAM (`EndFrame(readback)`, `SyncMemory`, el hazard flush, las
    copias de un target a otro y la textura de un render target) se baja a 1×. El CPU del juego
    y las capturas (`--capture`) ven siempre la resolución de la PSP.
  - El presentador (`SdlVulkanWindow`) presenta el target a N× directo, sin bajar a 1×.
  - Los shaders se cambian en `shaders/ge.*` y se regeneran con `compile_shaders.py` (el test
    `mpo_vulkan_shaders_fresh` lo exige).
- **Criterios de hecho**: los tests de Vulkan (`mpo_vulkan_*`) pasan a 1× sin cambios y tienen
  casos nuevos a 2× y 4× (los readbacks a 1× coinciden con los de 1×, dentro del umbral de F15a).
  Hay capturas del juego a 1× y a 4× de los mismos frames (una repetición de `analysis/f21`) y
  `capture_compare.py` a 1× contra softgpu da lo mismo que antes. Las capas de validación quedan
  sin errores. `ctest` completo en Clang y GCC.

- **El emisor por funciones, con deduplicación** (la opción (b) de F21; acordado con el usuario el
  2026-11-01 como fase propia después de la 0.1). Las 92 etapas son casi el mismo programa: de
  54,2 MB de `.text`, las funciones distintas suman 2,0 MB (`docs/analysis/stages.md`). Un emisor
  que genere cada función una sola vez, con las direcciones relocalizadas como parámetro,
  compilaría una fracción de lo de hoy (gow-psp-recomp, que emite por funciones, compila en unos
  4 min). Es una reescritura grande de PSPRecomp (MIT, para upstream) y cambia el difftest del
  emisor.
- Resolución interna configurable (**hecha en F23b**: `--render-scale`, ver
  `docs/sessions/F23b.md`; falta el selector en el launcher), MSAA y filtrado anisotrópico.
- Widescreen y ultrawide con el HUD ajustado (CWCheat `0x201E9728` como pista).
  - **El spike (2026-11-03) lo resolvió**, ver `docs/analysis/widescreen.md`:
    - la pantalla de la PSP ya es 16:9 (480×272), así que el trabajo es el ultrawide;
    - la constante está en `0x089E9728` y es la escala en x de la proyección. Cambiarla no cambia
      la lógica: la repetición y el save salen idénticos [V];
    - el HUD no se mueve y sale estirado;
    - fases:
      - **W1**, corta: `--aspect` parchea la constante al arrancar, estira la imagen y pone
        barras negras en el video;
      - **W2**: el HUD centrado en Vulkan, con los marcadores que siguen al 3D;
      - **W3**: anclar cada elemento al borde con los `.la2`, solo si se pide;
    - se descartan un framebuffer más ancho y anclar por la posición del dibujo.
- 60+ fps con **interpolación** (la lógica sigue a 20 o 30 fps).
- Texturas HD en formato de PPSSPP. **La capa está hecha en F24c** (`--textures`, `--dump-textures`,
  claves de PPSSPP). Falta reemplazar las texturas que están en VRAM sin tocar los framebuffers ni
  los frames de video: son el 91,5 % de las búsquedas de una partida (F24c [V]).
- **Íconos del HUD según el dispositivo** (paso 2 del spike de los controles): un `ReplacementSet`
  por dispositivo con PNG propios de los íconos (las claves salen de `--dump-textures` y
  `txp.py`), elegido con `DeviceTracker::generation()` y `PromptSet::style` (F24b) y guardado en
  el registro `p` para que la repetición siga siendo determinista. **Hecha en F25a** (ver su
  ficha): por capas, delante del pack del jugador, y con un registro `i` en las grabaciones.
- **`sceFont` con la fuente de respaldo** (F23c): sin las PGF del firmware, solo falta el aviso
  "Checking Memory Stick" del arranque. Las métricas de `sceFont` deciden el layout del juego,
  así que habría que imitarlas con cuidado.
- **Controles en pantalla según el dispositivo** (propuesto por el usuario el 2026-11-03): que
  el juego muestre START o Enter, A o Cruz, según lo último que se usó (mando, y de qué tipo, o
  teclado). El frontend ya sabe qué dispositivo mandó el último input. Va por partes:
  - los diálogos del sistema (PPGe, nuestros): fácil, porque el texto de los botones lo pone el
    runtime;
  - los íconos del HUD y de los menús del juego: primero hay que averiguar si salen de texturas
    (`.txp`) o de glifos de la fuente. Si son texturas, el camino es el reemplazo de texturas de
    arriba, con un juego de íconos por dispositivo que se cambia en caliente;
  - los textos que nombran un botón con palabras (tutoriales, códec): habría que reemplazar
    cadenas en los datos del juego. Es lo más caro y lo último.
  - **El spike (2026-11-03) lo resolvió**, ver `docs/analysis/button-prompts.md`. Los íconos
    del HUD y de los menús son texturas sueltas (32×16, T4), iguales byte a byte en todas las
    etapas. Los botones dentro del diálogo son glifos propios de cada script (`0x8C nn`).
    START, SELECT, L y R van siempre como palabras [V]. Orden recomendado:
    1. `last_device` en el frontend, un `PromptSet` para PPGe y un registro `p` en las
       grabaciones;
    2. el reemplazo de texturas por hash de contenido, en softgpu y Vulkan (sirve también para
       las texturas HD);
    3. los glifos de los GCX y de `codec.dat`;
    4. las palabras se dejan fuera, o solo con reemplazos del mismo largo.
- Versión EU (`ULES-00645`) y otras regiones.
- Ad hoc (compatible con el servidor de PPSSPP) y online (SaveMGO).
- Teclado y mouse, y cámara con el stick derecho (**hecha en F23a**: `[pad_camera]`, ver
  `docs/sessions/F23a.md`).
  - **La cámara del juego se mueve con la cruceta** (el usuario, en Windows, el 2026-11-01: con las
    flechas del teclado y con la cruceta del mando) [V por el usuario]. Así que el nivel 1 de la
    cámara con mouse (`docs/sessions/F21.md`, "Ideas para F23+") es solo del frontend: el
    movimiento horizontal del mouse aprieta ← o → mientras pasa de un umbral. Tiene que activarse
    solo con el mouse capturado (modo relativo de SDL), porque los menús también se mueven con la
    cruceta, y soltarse al perder el foco. La captura no puede depender de Esc, que es Start. Las
    grabaciones no cambian, porque guardan las muestras de `sceCtrl`.
  - **Hecho en F22** (después del cierre, a pedido del usuario): `[mouse_camera]`, clic para
    capturar y F1 para soltar. Queda para F23+ la cámara libre de verdad, sobre el RE de la cámara.
- MPO+ (`ULUS-10290`).
- macOS (MoltenVK).
- **El intérprete no es exacto en `st03t1`** (F22b): la repetición de `tramo1` con las etapas en el
  intérprete se separa de la grabación en el flip 7740 (con las etapas compiladas, a `-O0` o a
  `-O2`, coincide). Hay que bisecar contra el código recompilado (probablemente la VFPU del
  intérprete).
- **La repetición depende de la hora local de los archivos del `ms0` grabado** (F22c): copiado
  sin conservar las fechas, o en otra zona horaria, `tramo3` se separa en el flip 1620 (el diálogo
  de saves muestra la fecha del save). Que `--record-input` guarde las fechas y la zona, o que
  `--replay-input` las fije.
- **Leaks intermitentes de lavapipe en la CI** (2026-11-03): el job `Linux gcc asan-ubsan` falló dos
  veces seguidas en los `mpo_vulkan_*` con fugas pequeñas (224 a 560 bytes) desde módulos ya
  descargados (`<unknown module>`, el driver o las capas) y pasó al repetirlo, con la misma Mesa
  (25.2.8-0ubuntu0.24.04.4). En local no se reproduce. Si sigue, una supresión de LSan o
  `LSAN_OPTIONS` para esos tests.
- **`#include <algorithm>` en `allegrex_context.hpp`** (el fork, F22b): cuando convenga
  recompilar todo el corpus; después se quitan el `-include algorithm` del `CMakeLists.txt` raíz y
  el `CXXFLAGS` del job `windows-llvm-mingw`.
