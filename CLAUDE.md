# CLAUDE.md — MGS: Portable Ops PC Port

Port nativo no oficial de **Metal Gear Solid: Portable Ops** (PSP, Kojima Productions, 2006) a
**Windows y Linux**, construido con **recompilación estática** del código Allegrex (MIPS) del juego y
un **runtime HLE** que sustituye el sistema operativo y el hardware de la PSP.

- Estado: **fase 0 (investigación)**. Todavía no hay código.
- Objetivo de la primera etapa: que el juego sea **jugable de principio a fin** en Windows y Linux.
  Las mejoras (resolución alta, widescreen, FPS altos, online) vienen después.
- Los hallazgos completos, con todas las fuentes, están en [`docs/RESEARCH.md`](docs/RESEARCH.md).
  Antes de tomar una decisión técnica, léelo.

---

## 1. Reglas que no se negocian

1. **Nunca se sube contenido con copyright al repo**: ISOs, `EBOOT.BIN` cifrado o descifrado,
   PRX de etapa, `.rlc`, assets extraídos, capturas hechas a partir de assets, claves KIRK ni tablas de
   claves. El usuario pone su propia copia y el build la valida.
   - Los datos locales van en directorios ignorados por git: `game/`, `analysis/`, `generated/`,
     `progress/`.
   - El código generado depende del ejecutable del usuario, así que **no se versiona** (igual que en
     Yakumo).
2. **El ISO se valida por identidad exacta**: el disc ID de `PARAM.SFO` y el SHA-256 del `EBOOT.BIN`
   cifrado, más el de cada `stage/*/*.prx` que se recompila. Una versión no soportada se rechaza con
   un mensaje claro y nunca se intenta "a medias".
3. **Las direcciones del juego no entran en el código genérico.** El framework (recompilador y
   runtime) tiene que poder compilarse sin el juego. Todo lo que dependa de una dirección, un hash o
   un layout del juego va en `profiles/mpo/`.
4. **El proyecto es GPL-3.0-or-later** (ver `LICENSE` y §9).
   - Todo archivo nuevo empieza con `SPDX-License-Identifier: GPL-3.0-or-later`.
   - **Excepción**: los cambios genéricos a PSPRecomp (el framework) se quedan **MIT**, para poder
     enviarlos upstream. Esos archivos conservan su aviso MIT y no se les mezcla código GPL.
   - El código portado de PPSSPP (GPL-2.0-or-later, compatible) lleva en la cabecera el archivo y el
     tag de origen, por ejemplo `// Ported from PPSSPP GPU/Common/SplineCommon.cpp @ v1.20.4`.
   - Cada dependencia de `third_party/` lleva su LICENSE o NOTICE al lado, y se registra en
     `THIRD_PARTY.md`.
   - No se usan dependencias GPL-2.0-only ni con licencias incompatibles con GPLv3.
5. **Hay que reproducir el comportamiento del hardware, no inventarlo.** Ante la duda, la referencia
   es el intérprete y el HLE de PPSSPP (y JPCSP para la búsqueda de APs, que en PPSSPP falla).
6. **No se declara nada "funcionando" sin evidencia**: una captura comparada, una traza, un test o un
   recorrido manual descrito. Que el proceso arranque y abra una ventana no cuenta como pasar.

---

## 2. Juego objetivo y versión base

| Rol | Serial | Versión | Por qué |
|---|---|---|---|
| **Base principal (M0–M6)** | `ULUS-10202` (USA, inglés) | disco 1.02 | Es la que más herramientas cubren: el plugin de Noesis, las direcciones CWCheat (FPS y widescreen), el autosplitter y los reportes de PPSSPP. |
| Objetivo de contenido (después de M6) | `ULES-00645` (EU, 5 idiomas) | disco 1.01 | Es el build más reciente: trae más reclutas, mapas y misiones, pero no tiene GPS. |
| Fuera de alcance por ahora | MPO+ `ULUS-10290` 1.01 | — | Es una expansión con el mismo motor. Se reconsidera cuando el juego base esté terminado. |

- **No hay parches oficiales.** El servidor de parches de Sony no devuelve nada para ningún serial,
  así que los números "1.0x" son revisiones de disco.
- Los demás seriales (JP, Asia, Corea, Premium/Deluxe Pack) y sus hashes de redump están en
  `docs/RESEARCH.md` §2.

---

## 3. Enfoque técnico

**Recompilación estática ahead-of-time** de Allegrex a C++ + **runtime HLE nativo** +
**intérprete de respaldo** + **reemplazo progresivo de funciones** por código escrito a mano (camino
híbrido hacia una decompilación parcial).

### Descartado

- **Decompilación con matching**: tomaría años antes de tener algo jugable.
- **Envolver un emulador**: no es un port nativo.
- **La técnica de los ports de PS4** (Bloodborne, Uncharted, CTR Nitro-Fueled) **no aplica**. Esos
  proyectos ejecutan directamente el código x86-64 del juego y solo reemplazan las librerías del
  sistema. La PSP es MIPS, así que su código tiene que traducirse.

### Por qué la recompilación estática

- Es lo que ya demostró que funciona en PSP: Yakumo (MHP3rd, sobre PSPRecomp, Windows, Linux y
  macOS), gow-psp-recomp (God of War), tkdr-recomp (Tekken), fuc-recomp (Fate) y Ace Combat X.
- El estudio de factibilidad de gow-psp-recomp confirma que en la parte de CPU la cobertura es casi
  total: 99.88 % de `.text` desensamblado y solo 2 saltos indirectos sin resolver por juego.
- **Lo difícil de verdad es el runtime**, y sobre todo el **GE** (el motor gráfico).

### Framework base: PSPRecomp (decidido el 2026-10-24)

- Usamos **PSPRecomp** (`jessicanataliagta/PSPRecomp`, MIT, C++20). Por qué:
  - Está pensado para cualquier juego: el framework va en la raíz y cada juego en `profiles/<id>/`.
  - Trae intérprete de respaldo.
  - Tiene ganchos para overlays: `set_runtime_missing_function_hook`, `unregister_functions` y una
    librería compartida por overlay. Esto es **imprescindible** aquí porque MPO carga PRX por etapa
    (ver §4).
  - Yakumo ya lo llevó a Linux con Vulkan.
- El juego vive en `profiles/mpo/`. Los cambios al framework mismo se mantienen genéricos y bajo MIT,
  para poder proponerlos upstream.
- Si en M0 aparece una limitación seria (por ejemplo, con los PRX de etapa y sus `.rlc`), primero se
  extiende el framework con ganchos genéricos. Solo se reconsidera la base si eso no alcanza.
- Las ideas de gow-psp-recomp (export de Ghidra y difftest con Unicorn) se adoptan como herramientas
  del perfil.

### Plataformas y toolchain

| Componente | Elección | Por qué |
|---|---|---|
| Lenguaje del runtime | C++20 | Es lo que usa PSPRecomp. El C generado también vale. |
| Build | CMake ≥ 3.22 + Ninja + ccache | Es el estándar de la escena. Los builds generados son pesados. |
| Compiladores | Clang en Linux; MSVC 2022 o clang-cl en Windows | Hay que probar los dos, porque el código generado choca con los límites de cada compilador. |
| Ventana, input y audio | SDL3 | Es multiplataforma y funciona con gamepads de Xbox y PlayStation. |
| GPU | **Vulkan directo** (1.2 como mínimo, aprovechando 1.3 si está), con `volk` (loader) y VMA (memoria) | Decidido el 2026-10-24. Es un solo backend para Windows y Linux, con control total del estado que exige el GE (operaciones lógicas, clamp de profundidad, blending). Las referencias son PPSSPP (Vulkan) y Yakumo. |
| Renderer de referencia | GE por software (portado del softgpu de PPSSPP) | Sirve para llegar a la pantalla de título (M2) y como "verdad" para comparar frames contra el renderer Vulkan. |
| Video | FFmpeg mínimo (LGPL), solo con los decoders que haga falta | Para el video del título (`sceMpeg`). |
| ATRAC3 | `at3_standalone` de PPSSPP (LGPL-2.1+) | Lo usan gow y tkdr. |
| Distribución en Linux | AppImage y/o Flatpak, con Steam Deck como bonus | Es lo habitual en la escena. |

**Trampa conocida en Linux**: el código recompilado anida mucho la pila. Yakumo necesita una pila de
64 MiB en el hilo principal (`ulimit -s 65536`). Hay que reservar la pila de forma explícita al crear
los hilos y fibras, en vez de depender de `ulimit`.

---

## 4. Qué sabemos del juego (resumen; el detalle está en `docs/RESEARCH.md`)

### Código

- **El módulo principal es `mgp_main`** (seguramente `PSP_GAME/SYSDIR/EBOOT.BIN`, falta verificarlo).
- Además hay **overlays de código por etapa**: `disc0:/PSP_GAME/USRDIR/stage/<s>/<s>.prx` (módulo
  `mgp_stage`), con una tabla de relocación propia en `.rlc`. Se cargan y descargan con
  `sceKernelLoadModule` en tiempo de ejecución.
  - Hay que recompilar cada PRX de etapa por separado, como una librería compartida identificada por
    hash.
  - Hay que entender el formato `.rlc`.
  - El intérprete cubre lo que no se haya recompilado.
- Hay otros módulos de Konami: `KCEJ_FS` (el sistema de archivos) y `KCEJ_SOUND2` (el driver de
  sonido). Falta verificar si son PRX aparte o si van enlazados dentro de `mgp_main`.
- El juego trae su propia zlib (`inflate*`) y la carga de datos pasa por el manifiesto `data.cnf`.
- Los scripts son **GCL/GCX de la variante MGS3**, basada en Tcl. La herramienta `Jayveer/Gcx` los
  decompila. Es probable que haya un intérprete GCL dentro de `mgp_main`, que es un buen punto de
  anclaje para el RE.

### Datos

- Los formatos están documentados por `waspothegreat/MGS-MPO-Noesis`, la mejor referencia que hay.
  Todos son little-endian.
- **`slot.dat`**: páginas con un TOC de `(hash, offset)`. No necesita archivo `.key`.
- **`stage/<s>/_zar`**: un `u32` con el tamaño descomprimido seguido de un stream zlib que contiene
  un DAR. Dentro van `cache.dar/qar` y `resident.dar/qar`. Está comprimido pero no cifrado.
- **Hash de recursos**: `(tipo << 24) | strcode24(nombre)`, el "strcode" clásico de Kojima.
- **`.mdp`**: modelos en strips con s16 escalado y paleta de huesos.
- **`.txp`**: texturas de 4 u 8 bits con CLUT RGBA8888, siempre swizzled. Sus flags se traducen
  directamente a estado del GE.
- **`.mtar`**: animación a 60 Hz.

### Lo que esto exige del runtime

**Gráficos (GE):**
- Splines y beziers del GE con skinning. El juego los usa para subdividir el suelo cerca de la
  cámara y esquivar el clipper de la PSP.
- **Primitivas de línea** (radar y mapa): PPSSPP necesita `[CenteredLines]` para este juego.
- Render-to-texture y texturizar desde el render target actual.
- `sceGeEdramSetAddrTranslation`.
- Clamp de profundidad y guard-band. Un bug de PPSSPP en esto hacía desaparecer el suelo.

**Audio y video:**
- `sceAtrac` (ATRAC3) y `sceSas` (mezclador por voces, ADSR).
- `sceMpeg` (solo el video del título; las cinemáticas son in-engine).

**Sistema y diálogos:**
- `sceFont`, con la fuente propia `USRDIR/MGP.PGF`.
- `sceUtility`: savedata (guardar, cargar y borrar) y OSK (nombres y contraseñas de reclutamiento).

**Red (no hace falta para jugar):**
- `sceNetAdhoc*`, `sceNetApctl*`, `sceHttp*`, `sceNpAuth`.

**Timing (crítico):**
- **El gameplay corre a 20 FPS**: divisor de vsync 3 y un timestep fijo.
- La lógica está **atada al framerate**. Los cheats de 60 FPS rompen los timers de chaff, stun,
  granadas y misiles.
- En la etapa jugable **se respeta el 20/30 original**. Los FPS altos se harán **con interpolación**
  (la lógica se queda a su frecuencia, como en Yakumo y Dusklight) o desacoplando bien el timestep,
  nunca subiendo el divisor sin más.

### Funciones que hay que reemplazar en un port de PC

- **AP Scan (reclutar escaneando APs de Wi-Fi)**:
  - Cómo funciona: llama `sceNetApctlScanUser`, espera el evento `SCAN_COMPLETE` y lee los resultados
    con `sceNetApctlGetBSSDescIDListUser` y `sceNetApctlGetBSSDescEntryUser` (BSSID, SSID, canal,
    señal y seguridad).
  - En PPSSPP se queda colgado escaneando (issue #20703); en JPCSP funciona.
  - Plan: un **generador de APs virtuales con semilla** (BSSID y SSID estables y señal configurable)
    que respete el cooldown. Opcionalmente, usar el escaneo real del sistema operativo (WlanAPI o
    nl80211).
  - Esto **es necesario para el 100 %**: algunos soldados (GRU, KGB, Ocelot) solo se consiguen así.
- **GPS (PSP-290)**: existe solo en US y JP; EU no lo tiene. Se deja como stub ("no hay receptor") y
  el contenido que dependa de él se evalúa más adelante.
- **Ad hoc e infraestructura**: no hacen falta para jugar.
  - Más adelante se puede añadir compatibilidad con el servidor ad hoc de PPSSPP.
  - Para el online se puede usar el revival de SaveMGO (`mpo.savemgo.com`, STUN `stun.mgo2pc.com`,
    UDP 5730), que usa HTTP plano.
- **Importar el save de MPO desde MPO+**: no aplica mientras MPO+ esté fuera de alcance.

---

## 5. Arquitectura del runtime (contrato)

```
ISO del usuario ──► instalador/validador ──► EBOOT + stage PRX descifrados (en local, ignorados)
                                               │
              Ghidra + ghidra-allegrex ◄───────┤ (análisis, nombres, tablas de saltos)
                                               ▼
                 analizador ─► recompilador ─► generated/*.cpp  (main + 1 lib por overlay)
                                               ▼
  runtime (psprecomp_core + profiles/mpo/host):
    CPU ctx (32 GPR, HI/LO, 32 FPR, FCR31, 128 VFPU + prefijos) · dispatch addr→fn · intérprete
    memoria · kernel HLE (hilos/fibras, semáforos, event flags, VTimers, callbacks, módulos)
    IO (disc0:/ desde el ISO, ms0:/ → carpeta de saves) · GE → Vulkan · SAS/ATRAC/MPEG → SDL3 audio
    sceUtility (savedata/OSK/msg) con UI propia · sceCtrl ← SDL3 gamepad/teclado
```

### Mapa de memoria

La PSP-1000 es el objetivo: el juego es de 2006 y no usa la RAM extra.

| Región | Rango | Notas |
|---|---|---|
| Scratchpad | `0x00010000–0x00013FFF` | 16 KiB |
| VRAM (EDRAM) | `0x04000000–0x041FFFFF` | 2 MiB. Los framebuffers y texturas viven aquí. Ojo con los espejos y con `EdramSetAddrTranslation`. |
| RAM de kernel | `0x08000000–0x087FFFFF` | 8 MiB. El HLE puede usarla para sus estructuras. |
| RAM de usuario | `0x08800000–0x09FFFFFF` | 24 MiB. El EBOOT no relocatable se carga en `0x08804000`. |

- Los espejos (`0x4xxxxxxx` sin caché y `0x8xxxxxxx` de kernel) se enmascaran con
  `addr & 0x1FFFFFFF` sobre una arena reservada de una vez.
- Tanto el host como la PSP son little-endian, así que no hay byteswap.
- Si se adopta *fastmem*, se reserva un rango virtual grande y se protegen las páginas para atrapar
  accesos inválidos.

### Reglas de codegen

Son de gow-psp-recomp y valen aunque usemos PSPRecomp.

- **Una función de C++ por cada función del juego.** La firma es `void f_XXXXXXXX(Ctx*)`.
- **Delay slots**: la condición y el destino de `jr` se capturan **antes** del delay slot. En un
  branch-likely, el delay slot se ejecuta solo si el salto se toma.
- **Llamadas directas**:
  - Un `jal` a una función recompilada es una llamada directa.
  - Un `jr $ra` es un `return`.
  - Un salto fuera de la función es una tail call.
- **Saltos indirectos**: las tablas de saltos recuperadas se vuelven un `switch`. El resto de `jr` y
  `jalr` pasan por el dispatch `addr→fn`, con el intérprete como respaldo.
- **Semántica fina**: la división por cero, la conversión float→int, FCR31 (flush-to-zero y
  redondeo), `lwl/lwr`, comparaciones con NaN y las extensiones Allegrex (`ext`, `ins`, `seb`,
  `seh`, `wsbh`, `wsbw`, `bitrev`, `min`, `max`, `clz`, `clo`, `madd(u)`, `msub(u)`, `movz`, `movn`)
  se copian **del intérprete de PPSSPP**.
- **VFPU**:
  - Las instrucciones sencillas se emiten inline y las complejas como llamadas a helpers.
  - Hay que modelar los prefijos (`vpfxs/t/d`).
  - `vsin`, `vcos`, `vrsq` y otras usan **las tablas o aproximaciones de PPSSPP**, para que el
    resultado coincida bit a bit.
  - Antes de diseñar, se mide cuánto usa VFPU `mgp_main`.
- **Sin código automodificable**: se confirma en M0 buscando imports de
  `sceKernelIcacheInvalidate*`. Los overlays se tratan con el mecanismo de overlays, no como código
  que se modifica a sí mismo.

### Hilos

- La PSP tiene **un solo núcleo con scheduler de prioridad expropiativa**. Cada hilo de la PSP será
  una **fibra**, y se conmuta en las llamadas HLE bloqueantes y en los puntos de visibilidad
  (como hacen PPSSPP y Yakumo).
- Riesgo: condiciones de carrera que dependan del orden exacto del scheduler. Ante un bug raro,
  primero se compara con el orden de PPSSPP.

### Imports y NIDs

- La tabla de NIDs se genera a partir de `Core/HLE/*.cpp` de PPSSPP, con el tag fijado.
- Cada import se registra con `register_hle`.
- Un NID que no esté implementado **registra el nombre y la dirección y falla de forma visible**,
  nunca en silencio.
- Hay que mantener un histograma de las llamadas HLE (como `PSPRECOMP_HLE_HISTOGRAM` en Yakumo).

---

## 6. Hitos

| Hito | Criterio de "hecho" |
|---|---|
| **M0 — Análisis** | Tener el ISO US validado y el `EBOOT` y los PRX de etapa descifrados en local. El proyecto de Ghidra (con ghidra-allegrex) carga `mgp_main` y un `stage/*.prx` relocado. Hay un inventario de imports (NIDs por librería), conteo de VFPU, branch-likely y `jr` sin resolver, y el formato `.rlc` está entendido. PSPRecomp (`psp_analyze`) procesa `mgp_main`. Hay un histograma de los comandos del GE (blending, operaciones lógicas, stencil, splines). |
| **M1 — Arranque** | El recompilado de `mgp_main` llega a `module_start` y crea sus hilos. Los imports no implementados quedan listados. |
| **M2 — Pantalla de título** | El kernel HLE, la IO desde el ISO, el intérprete de display lists y el **GE por software** muestran el logo y el menú. El input funciona. |
| **M3 — Renderer Vulkan** | El renderer **Vulkan** maneja splines, beziers, líneas, render-to-texture y CLUT/swizzle. Sus frames coinciden con el renderer por software y con PPSSPP. |
| **M4 — Audio, video y saves** | Funcionan SAS, ATRAC3, el video del título, guardar y cargar (con diálogo propio) y el OSK. |
| **M5 — Primera misión** | Se carga un PRX de etapa recompilado, y se juega y completa la primera misión con transición de etapa. |
| **M6 — Jugable** | Se juega el juego completo de principio a fin en Windows y Linux. AP Scan funciona con APs virtuales. Hay builds reproducibles (CI). |
| M7+ — Mejoras | Resolución interna, widescreen con HUD ajustado, interpolación a 60+, texturas HD (formato de PPSSPP), versión EU, ad hoc y online (SaveMGO), MPO+. |

Para tener una idea del esfuerzo: gow-psp-recomp estima **entre 6 y 12 meses** de trabajo constante
hasta el primer juego completable. El runtime (kernel de 3 a 6 semanas, GE de 1.5 a 4 meses,
audio/video/saves de 1 a 2 meses) es lo que más pesa.

---

## 7. Cómo se verifica (testing)

- **Difftest de CPU**: se ejecutan funciones recompiladas y el intérprete (o Unicorn en modo
  MIPS32) con entradas aleatorias y se compara el estado. Esto es obligatorio para cada cambio en el
  emisor.
- **Comparación contra PPSSPP**:
  - Se compara el estado de los registros en breakpoints, usando el debugger y la API de WebSocket
    de PPSSPP.
  - Se comparan los frames contra los dumps del GE debugger de PPSSPP.
  - Se compara la secuencia de llamadas HLE.
- **Repetición de input**: se graba el input (lectura por lectura) y se reproduce de forma
  determinista. Las capturas cada N frames se comparan contra referencias locales (no van al repo).
- **Toggles A/B por variable de entorno** para cada optimización del renderer, para poder bisecar
  cuando un frame sale mal (como los `MHP3RD_NO_*` de Yakumo).
- **Tests sin el juego** (`ctest` sin datos): decoder, emisor, formatos y HLE puro. Son los únicos que
  corren en CI pública.
- **CI**: matriz de Windows (MSVC) y Linux (Clang + GCC), con ASan y UBSan en Linux.
- **Smoke test manual**: antes de cada release se sigue una lista corta de pasos. **Nunca contra la
  carpeta de saves real del usuario**, y se hashean los saves antes y después.

---

## 8. Herramientas de RE

| Herramienta | Para qué |
|---|---|
| **Ghidra** + plugin **ghidra-allegrex** (Allegrex y VFPU; hay que verificar el repo y la versión) | Desensamblado, funciones, tablas de saltos, nombres, scripts para exportar los datos del recompilador. |
| **PPSSPP** (fijado a v1.20.4 o posterior) | Volcar el `EBOOT` descifrado (Settings › Tools › Developer tools). Debugger, GE debugger y frame dumps. Es la referencia del HLE. |
| **JPCSP** | Referencia del comportamiento de `sceNetApctl` en el AP Scan. |
| **Unicorn** | Difftest de CPU. |
| `waspothegreat/MGS-MPO-Noesis` | Formatos de `slot.dat`, `_zar`, DAR, QAR, MDP, TXP y MTAR. Trae direcciones de funciones en Ghidra. |
| `Joy-Division/tools-mgs` | `zar-extract`, `dar/qar-extract_psp`, `txp-convert`, `gcx-decompile`. |
| `Jayveer/Gcx` (modo `-MGS3`) | Decompilar los scripts GCX a GCL. |
| `Zoft123/Codec` | Navegar assets (DAR, QAR, `_zar`, TXP). |
| `NickRPGreen/...-Autosplitter` | Direcciones de RAM (US, PAL y JP) del IGT, el ID de área y el estado del juego. Sirven como anclas para el RE. |
| CWCheat DB (`Saramagrean/CWCheat-Database-Plus-`) | Direcciones del divisor de FPS, el timestep y el aspect ratio en la versión US. |
| `pspdev/vfpu-docs`, uOFW, psdevwiki | Referencia de VFPU, kernel y hardware. |

---

## 9. Decisiones

### Tomadas (2026-10-24)

1. **Framework: PSPRecomp** (ver §3).
2. **Licencia: GPL-3.0-or-later**:
   - **Garantiza que el port y sus forks sigan abiertos.**
   - Nos deja portar código de PPSSPP (GE, SAS, VFPU, NIDs y savedata) y usar `libkirk`.
   - Se lleva bien con dependencias Apache-2.0, MIT, zlib, BSD y LGPL.
   - Los cambios genéricos a PSPRecomp siguen en MIT (§1.4).
3. **API gráfica: Vulkan directo**:
   - Es un solo backend para Windows y Linux, y deja abierto llegar después a macOS (MoltenVK) y a
     Steam Deck.
   - El GE por software queda como renderer de referencia y depuración.
   - Se descartaron:
     - SDL3 GPU: es una abstracción que no expone operaciones lógicas y trae menos estado dinámico.
     - OpenGL: los drivers en Windows son peores y es una API de legado.
     - D3D12: no es nativo en Linux.

### Abiertas

1. **Recompilar o hacer HLE de `KCEJ_SOUND2`**: depende de lo que muestre M0.
2. **Fibras**: con qué implementación se hacen los cambios de contexto en Windows y Linux (la API
   Fibers de Win32 y `ucontext`, o una librería como `minicoro`). Se decide en M1.

---

## 10. Estructura del repo prevista

```
CLAUDE.md
LICENSE                   # GPL-3.0 (texto oficial)
THIRD_PARTY.md            # dependencias, licencias y versiones fijadas
.gitignore                # excluye ISOs, EBOOT/PRX, game/, analysis/, generated/, progress/
docs/RESEARCH.md          # investigación y fuentes
docs/formats/             # formatos del juego (.rlc, _zar, slot.dat, txp…) — solo prosa/specs
framework/ (o PSPRecomp vendorizado/submódulo)
profiles/mpo/
  config/                 # hashes soportados, entradas para el análisis, mapa de funciones
  host/                   # HLE específico, overrides de funciones, AP-scan virtual, renderer hooks
  scripts/                # install/validate, generate, build_overlays
  tools/                  # scripts de Ghidra, extractores
  tests/
  game/ analysis/ generated/ progress/   # IGNORADOS por git
third_party/              # cada uno con su LICENSE/NOTICE al lado
```

---

## 11. Convenciones

- **C++**: C++20, `clang-format` (estilo por definir en M0) y nada de excepciones en las rutas
  calientes del runtime. El código generado no se formatea.
- **Cabeceras de licencia**: la primera línea de todo archivo fuente es
  `// SPDX-License-Identifier: GPL-3.0-or-later` (o `# ...` en Python, CMake y scripts). Los archivos
  del framework PSPRecomp usan `MIT`.
- **Vulkan**:
  - Las capas de validación van activadas en Debug y en CI.
  - Ningún error de validación se ignora.
  - Hay que poder capturar con RenderDoc en cualquier momento.
- **Python** (herramientas, generadores, scripts de Ghidra en Python): sigue la
  [guía de estilo de Google](https://google.github.io/styleguide/pyguide.html). Se importan módulos
  completos y se accede con notación de punto (`import struct` → `struct.unpack`), no nombres
  sueltos. Se ejecuta con `python -I` cuando procesa datos del juego.
- **Commits**: en inglés, con Conventional Commits.
- **Idioma**: la documentación del proyecto va en español. Los identificadores y los comentarios de
  código van en inglés.
- **Datos no confiables**: las herramientas que procesan archivos del juego o descargas validan
  tamaños y offsets (los formatos vienen de RE y pueden estar mal).
- **Evidencia**: cada hecho que se agregue sobre el juego en `docs/` lleva una etiqueta: **[V]**
  verificado en el binario o el ISO, **[S]** de una fuente externa (con su enlace) o **[?]** sin
  verificar.
