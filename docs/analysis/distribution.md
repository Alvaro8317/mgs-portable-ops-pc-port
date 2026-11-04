# Distribución del port (F22)

Decisión del usuario (2026-11-01, `CLAUDE.md` §9, decisión 10): **un launcher que compila en la
máquina del jugador**, como gow-psp-recomp. La release solo trae el código del proyecto y las
herramientas; el código del juego (traducido o compilado), los assets y las claves nunca salen de la
máquina del jugador. `CLAUDE.md` §1 queda como estaba.

Las alternativas que se descartaron, con sus datos, están en la ficha de F22 de `docs/PLAN.md` y en
`docs/RESEARCH.md` §3 ("Cómo distribuyen los ports").

## 1. Qué se distribuye y qué no

| Va en la release | No va nunca |
|---|---|
| El código del port (framework, perfil, scripts), con su licencia | El ISO, el `EBOOT.BIN` y los PRX, cifrados o descifrados |
| Los hints del análisis (`profiles/mpo/config/recomp/`, solo direcciones) | El C++ generado y las librerías compiladas del juego |
| El launcher y, en F22b, el toolchain portátil o la forma de bajarlo | Las claves KIRK y `PrxDecrypter`/`sceChnnlsv` de PPSSPP (se bajan al configurar, en la máquina del jugador) |
| | Las fuentes del firmware (`flash0:/font`) |

- **Los hints son direcciones**: rangos de código, semillas de funciones, destinos de las tablas de
  saltos y los seis `patch` de arranque (que escriben `jr ra` y `nop`). No contienen bytes del
  juego, y `CLAUDE.md` §1.3 ya pone las direcciones en `profiles/mpo/`. Son 96 archivos (7,7 MB,
  ~1,1 MB comprimidos) marcados como generados en `.gitattributes`.
- **Con ellos, el jugador no necesita Ghidra ni la carpeta `analysis/`** [V]: `generate.py
  --hints-from profile` produce los mismos archivos, byte a byte, que la generación de F07b, F08b
  y F13 con Ghidra (`--check`: 120/120 archivos en `mgp_main`, 6/6 en `kjfs`, 55/55 en `st02`).

## 2. El camino en la máquina del jugador (`setup.py`)

`profiles/mpo/scripts/setup.py` es lo que corre el launcher. Cada paso se reanuda.

| Paso | Qué hace | Coste medido (Linux, esta máquina) |
|---|---|---|
| `tools` | configura CMake (baja SDL3, FFmpeg, las fuentes de PPSSPP fijadas por SHA-256) y compila `mpo` y `psp_recomp` | ~1 min |
| `disc` | `mpo --install`: valida el disc ID y el SHA-256 de cada archivo, descifra el `EBOOT.BIN` | 3 s |
| `generate` | el C++ de `mgp_main`, de los 3 módulos y de las 92 etapas, con los hints versionados | 52 s (8 a la vez; 370 s en serie) |
| `main` | configura con el código generado y compila `mpo_boot` (`mgp_main` y los módulos) | 19 min 53 s |
| `stages` | las 92 librerías de etapa, de a una, en el orden de `config/build_order.toml`; desde F22b en dos pasadas, primero a `-O0` y después optimizadas | `-O0`: **~40 min** las 92, ninguna más de 47 s (F22b). `-O2`: 6 h 45 min a `-j5` (F21); `title` 115 a 142 s, `briefing` 163 s, `st07m1` 410 s (F22, F22b) |

- Medido el 2026-11-01 en esta máquina (WSL2, 28 hilos, 15 GB de RAM, Clang 18, sin ccache, el
  pool del código generado en 3): **del ISO a poder jugar, ~22 min**.
- **Se puede jugar después de `main`.** Una etapa sin librería corre en el intérprete de
  respaldo: es más lento, pero se puede jugar (F19: `st01t1` se jugó entera así, con un bajón de
  FPS al final, mientras se compilaba en paralelo). En F22, `tramo3` de F21 (`init`, `r_main01`,
  `title`, `briefing` y `st07m1`) con las cinco etapas en el intérprete se repitió sin ventana con
  los mismos hashes (`135 matched`) a ~2,8x del tiempo real: 42,7 millones de instrucciones
  interpretadas en 49 s de reloj para 136 s de juego [V].
- **`stages` corre en segundo plano**: con `--low-priority` (nice 15 en POSIX,
  `BELOW_NORMAL_PRIORITY_CLASS` en Windows). Cada librería terminada se copia a `<data>/overlays`
  con un rename atómico. Desde F22, `OverlayManager` vuelve a mirar la carpeta cuando el juego carga
  una etapa sin corpus, así que la librería nueva se usa en la siguiente carga de la etapa, sin
  reiniciar el juego.
- **El orden** (`config/build_order.toml`) sigue la historia: el arranque, el título y el principio
  del juego en el orden de F19, después el mapa de misiones. El resto va en orden natural de nombre,
  que sigue más o menos a las misiones, y los menús de red al final.
- **Las dos pasadas (F22b)**: a `-O0`, las 92 etapas quedan compiladas en ~40 min, y después
  `stages` sigue con la pasada optimizada, que reemplaza cada `-O0` al terminar. Las dos dan el
  mismo resultado: la repetición completa de `tramo1` (512 s de juego, con la misión `st03t1`)
  coincide entera con las etapas a `-O0` y a `-O2` (511/511), y con el intérprete **no** (se
  separa en `st03t1`). En esa repetición sin ventana, `-O0` y `-O2` tardan lo mismo, dentro del
  ruido: el código de las etapas pesa poco frente a softgpu y `mgp_main`.
  - Las librerías `-O0` se llaman `stage_<s>-O0.<ext>` (Windows no deja reemplazar una DLL
    cargada) y ocupan 4,6 GB las 92. `stages` las borra a medida que instala las optimizadas.
  - `OverlayManager` vuelve a mirar la carpeta en cada carga de etapa y, si hay dos librerías del
    mismo PRX, instala la más optimizada (export `mpo_overlay_opt_level`).
- **Cuándo se recompila una etapa**: si cambia su C++ (el `generate.json`) o la "clave del
  runtime", que es el hash de las cabeceras del framework que incluye el código generado, más
  `overlay_module.*`, el compilador y el nivel de optimización. Es lo mismo que comprueba
  `OverlayManager` con el tamaño de `Runtime`, pero antes de compilar.

### Disco y memoria

- Por etapa: unos 54 MB de C++, 81 MB de objetos y 30 MB de librería (F21). Con las 92 etapas:
  ~5 GB de C++, ~7,5 GB de objetos y 2,8 GB de librerías.
- `stages` borra los objetos de cada etapa ya instalada (`--keep-objects` para conservarlos). **El
  C++ no se puede borrar**: el CMake del perfil busca los `.cpp` con `CONFIGURE_DEPENDS`, y si
  faltan, el siguiente build vuelve a configurar sin ellos. Ocupación final: ~9 GB.
- Memoria: ~390 MB por proceso de compilación de una unidad de etapa (Clang 18, `-O2`, F21). El
  pool `psprecomp_generated` limita las compilaciones del código generado a 1 por cada 4 GiB de RAM
  (`--generated-jobs` lo cambia).

## 3. El toolchain de cada plataforma (F22b)

El código generado y `mpo_boot` se enlazan entre sí (las librerías de etapa resuelven el runtime
contra el ejecutable), así que **todo se compila con el mismo compilador en la máquina del
jugador**. No se puede publicar un `mpo_boot` precompilado y compilar solo las etapas.

### Windows: llvm-mingw portátil (hecho en F22b)

- `setup.py toolchain` baja a `<data>/toolchain` lo que fija `profiles/mpo/config/toolchain.toml`:
  **llvm-mingw 20261020** (UCRT, 182 MB), **CMake 4.4.5** y **Ninja 1.13.2**. Comprueba el tamaño
  y el SHA-256 antes de descomprimir (los SHA-256 son los digests de las releases de GitHub,
  comprobados al bajarlos), rechaza rutas absolutas y `..` en el ZIP, y guarda las rutas en
  `state.json`. Sin Visual Studio ni permisos de administrador.
- `setup.py install` lo corre primero en Windows (`--toolchain auto`; `system` usa el `PATH`, y
  `--cc/--cxx` también lo saltan). Después todos los pasos usan esa CMake, ese Ninja y ese Clang, y
  el `PATH` de los procesos hijos lleva su `bin` (libc++ y libunwind son DLLs).
- **El Python lo trae la release** (`python/`, el embebible 3.14.10, fijado en el mismo
  `toolchain.toml`), porque el launcher lo necesita para correr `setup.py`. Los scripts solo usan
  la biblioteca estándar.
- **Compila y pasa los tests** [V, CI `windows-llvm-mingw`]: el proyecto entero con
  `-DMPO_WARNINGS_AS_ERRORS=ON`, y `ctest` (incluidos `mpo_overlay_library_test`, que carga una DLL
  de etapa enlazada contra el `.exe`, y el de los dos niveles). Hicieron falta dos cosas:
  - `mpo_boot` exporta sus símbolos con `-Wl,--export-all-symbols` (con MinGW,
    `WINDOWS_EXPORT_ALL_SYMBOLS` no hace nada) y las librerías de etapa importan los datos del
    runtime con `dllimport` (`PSPRECOMP_IMPORT_HOST_SYMBOLS`, antes solo con MSVC);
  - libc++ no trae `<algorithm>` de rebote, y `framework/.../allegrex_context.hpp` usa `std::max`
    sin incluirlo. El `CMakeLists.txt` raíz fuerza `-include algorithm` **solo con libc++**:
    arreglar el header recompilaría todo el corpus de un build existente (la trampa de F21). El
    include va al fork cuando convenga recompilar (anotado en `docs/sessions/F22b.md`).
- **Los 65 535 exports de un PE**: el usuario enlazó `mpo_boot` con MSVC en F22. Como referencia,
  el `mpo_boot` de Linux con `mgp_main` y los módulos exporta 8 784 símbolos definidos (`nm -D`),
  muy lejos del límite [V en Linux; en Windows con llvm-mingw, pendiente del build del usuario].
- **FFmpeg** (decidido en F22b): la release de Windows trae un FFmpeg 7.1.5 mínimo **ya
  compilado**, porque en Windows no hay un shell POSIX para su `configure`.
  - Lo compila la CI de release, de forma cruzada desde Linux con el **mismo llvm-mingw**
    (`packaging/ffmpeg`, `cmake/toolchains/llvm-mingw.cmake`), con los mismos flags que el modo
    `bundled` (más `--enable-w32threads`, para no depender de winpthreads).
  - `setup.py` lo encuentra junto al código (`ffmpeg/` en el ZIP) y configura con
    `MPO_FFMPEG=prebuilt` y `MPO_FFMPEG_ROOT`. Son librerías estáticas de C: no dependen de la
    versión de libc++.
  - LGPL-2.1: va con su tarball de código (`ffmpeg-7.1.5.tar.xz`) y un `BUILD.txt` con la línea
    de `configure` exacta.
  - Se descartó compilarlo con el `sh` de un MSYS2 portátil: otra descarga grande y otro toolchain
    que mantener.

### Linux y Steam Deck: Flatpak sobre `org.freedesktop.Sdk` (escrito en F22b, sin probar)

- `packaging/flatpak/io.github.alvaro8317.MpoPcPort.yml`: el runtime es **`org.freedesktop.Sdk`
  25.08**, no `Platform`, para compilar dentro del sandbox con el GCC, CMake, Ninja y Python del
  SDK [?]. Así es igual en cualquier distribución y en SteamOS (raíz de solo lectura, sin
  compiladores).
- El bundle trae el launcher y el código (`/app/share/mpo-pc-port/source`). `setup.py` deja todo
  lo del juego en `~/.var/app/io.github.alvaro8317.MpoPcPort/data`.
- Permisos: red (CMake baja SDL3, FFmpeg y los archivos de PPSSPP fijados), lectura de `home`, de
  `/run/media` y de `/media` (el ISO, también en la microSD de la Deck), Wayland/X11, audio y
  `--device=all` (los mandos).
  - **Desde F22c, escritura en Descargas** (`--filesystem=xdg-download`): con `home:ro`, "Export my
    saves" no tenía dónde escribir fuera del sandbox [?, sin probar en un Flatpak instalado].
- La CI de release lo construye con `flatpak-builder` (sin probar todavía en la CI ni en una Deck).
- Sin Flatpak (el tarball del código) hacen falta los paquetes del sistema: CMake ≥ 3.22, Ninja,
  Clang o GCC con C++20, **Python ≥ 3.11** (`tomllib`), `make` y `sh` para FFmpeg, y los headers
  que SDL3 busca al configurarse. **Sin los de audio, SDL3 compila solo con los drivers `dummy` y
  `disk`: el juego no tiene sonido** (F22c). La lista de Debian y Ubuntu del `README.md` se probó
  en un contenedor `ubuntu:24.04` vacío (ver `docs/sessions/F22c.md`).
- **macOS**: fuera de alcance hasta F23+.

## 4. El launcher (hecho en F22b)

`mpo_launcher` (`profiles/mpo/launcher`), en **C++ con SDL3 y Dear ImGui** (decidido por el
usuario: se maneja con el mando, que en el modo juego de la Steam Deck es clave, y el Python
embebible no trae Tkinter). La release lo trae ya compilado: no tiene nada del juego.

- Solo maneja `setup.py`, que hace el trabajo, deja los logs y se reanuda:
  1. sin instalación: elegir el ISO y, si hace falta, la carpeta de las fuentes, con un navegador
     propio (mouse, teclado o mando; los diálogos del sistema no sirven en el modo juego de la
     Deck), e **Install**, con el paso, el progreso y el log;
  2. con el juego listo: **Play** (`setup.py play`), cuántas etapas hay compiladas y cuántas
     optimizadas, y la compilación de las etapas en segundo plano (`stages --low-priority`), que
     arranca sola al terminar la instalación, con **Stop** (para después de la etapa en curso,
     con el archivo `<data>/stop-stages`);
  3. **Import a save** y **Export my saves** (`setup.py import-save/export-save`, F18c);
  4. **Settings** (F24a): el renderer, la resolución interna (1× a 8×, con los píxeles), el
     tamaño de la ventana y la pantalla completa. `status --json` trae la ruta de
     `settings.toml` (en la carpeta de configuración, junto a `input.toml`), sus valores y los
     avisos de lo que estaba mal; el launcher reescribe el archivo en cada cambio
     (`save_settings`, por un temporal) y `setup.py play` lo convierte en opciones de `mpo_boot`
     (`settings_options`). Lo que se pase después de `--` gana, y también `MPO_RENDERER` y
     `MPO_RENDER_SCALE`.
- **En inglés y en español** (F25d): todas las cadenas están en
  `launcher/launcher_strings.inc`, una línea por cadena con los dos idiomas
  (`MPO_TEXT(kId, "English", "Español")`), y se piden con `tr(Text::kId)`. El idioma sale de
  `language` en `settings.toml` (`"auto"`, `"en"` o `"es"`; también se elige arriba de todo antes
  de instalar); con `"auto"`, del locale del sistema: Windows (y macOS) por
  `SDL_GetPreferredLocales`, y Linux por `LC_ALL`, `LC_MESSAGES`, `LANG` y `LANGUAGE` como
  gettext (SDL solo lee `LANG` y `LANGUAGE`). Español si el primer idioma que el launcher tiene es
  `es`, inglés en cualquier otro caso. La fuente es la vectorial de Dear ImGui, con Latin-1
  (`mpo_launcher_font_test` comprueba cada carácter de la tabla). Lo que imprime `setup.py` (los
  pasos, los avisos de `settings.toml`, los errores y los logs) **queda en inglés**: es lo que
  trae un reporte de un problema. `mpo_launcher --settings <file>` usa otro `settings.toml` (para
  tests y capturas).
- Lee el estado con `setup.py status --json` y el progreso de las líneas `setup: <paso>: …`
  (`mpo_launcher_core`, probado sin ventana en `mpo_launcher_core_test`).
- **Si el juego o un paso termina con error**, muestra sus últimas líneas (con un botón para
  copiarlas). Así, un import sin implementar no pasa en silencio (ver §5).
- **El navegador** (F22c): se puede escribir la ruta, mostrar las carpetas ocultas (PPSSPP guarda
  los saves en `~/.config/ppsspp` en Linux) y, al importar o exportar saves, saltar a las
  carpetas `SAVEDATA` de PPSSPP que existan (`ppsspp_save_folders`: `Documents\PPSSPP` en
  Windows; `~/.config/ppsspp` y el Flatpak de PPSSPP en Linux).
- Busca `setup.py` en `--source`, en `source/` junto al ejecutable (el ZIP de Windows), en
  `../share/mpo-pc-port/source` (el Flatpak) y en el árbol del que se compiló; el Python, en
  `--python`, en `python/python.exe` (Windows) o `python3`.
- `setup.py` tiene un candado por carpeta (`setup.lock`, un lock del sistema operativo que se
  suelta solo si el proceso muere): nunca corren a la vez dos `install` o `stages` sobre el mismo
  build.
- Al jugador no le hace falta compilarlo: `setup.py` configura con `MPO_BUILD_LAUNCHER=OFF`, así
  que su build no baja Dear ImGui.

## 5. La release (F22b)

- **`packaging/make_release.py`** arma:
  - el **tarball del código**: los archivos versionados del repo y del submódulo, **sin
    `framework/psprecomp/profiles/`** (el perfil de ejemplo de upstream trae 109 MB de código
    generado de otro juego y binarios de FFmpeg; el build no lo usa);
  - el **ZIP de Windows**: `mpo_launcher.exe`, `python/` (el embebible, comprobado contra su
    SHA-256), `source/`, `ffmpeg/` (con su código) y un `README.txt`.
- **`make_release.py check`** falla si un archivo trae algo del juego: por la ruta (`game/`,
  `analysis/`, `generated/`, `progress/`, capturas y dumps, ISO, `EBOOT.BIN`, PRX, `.rlc`, PGF,
  grabaciones, formatos del juego) y **por el contenido**, aunque el archivo esté renombrado
  (`~PSP`, ELF de MIPS, ISO 9660, PBP y PSF), también dentro de los ZIP anidados. Lo prueba
  `mpo_release_check_test`.
- **`.github/workflows/release.yml`** (con un tag `v*`, o a mano): FFmpeg cruzado, el launcher con
  el toolchain portátil, el paquete con el `check`, el Flatpak y un **borrador** de release con los
  SHA-256. Nunca baja ni sube nada del juego.

## 6. Pendiente

- **`--unimplemented`** (decidido en F22b): `setup.py play` deja **`stop`**, la regla de
  `CLAUDE.md` §5. Los 15 imports sin implementar (ad hoc, `Ptp`, `Discover` y
  `sceKernelStopUnloadSelfModuleWithStatus`) no tuvieron ninguna llamada en los tramos de F21. Si
  un jugador llega a uno, el juego se detiene con el nombre del NID y el launcher lo muestra (§4).
  `continue` devolvería 0 sin avisar y podría colgar el juego o corromper un save.
- Las rutas por defecto de `mpo_boot` (las tablas de la VFPU y las librerías) apuntan al directorio
  de build. Eso vale mientras el build viva en `<data>/build`; un paquete que mueva los binarios
  tiene que pasarlas por parámetro.
- La tabla de NIDs (`--nid-table`) solo pone nombres en el informe y no hace falta para jugar.
- Pendiente del usuario: el camino entero en Windows con el toolchain portátil (el ZIP de la CI),
  y el Flatpak en Linux y en la Steam Deck.
