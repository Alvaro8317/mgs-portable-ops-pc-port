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
| `stages` | las 92 librerías de etapa, de a una, en el orden de `config/build_order.toml` | 6 h 45 min a `-j5` (F21); `title` 115 s, `briefing` 163 s, `st07m1` 410 s (F22) |

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

- **Windows** (como gow-psp-recomp y fuc-recomp): llvm-mingw portátil, CMake y Ninja portátiles y
  Python embebible, bajados por el launcher con su SHA-256 fijado, sin Visual Studio ni permisos de
  administrador.
  - **FFmpeg**: hoy, con MSVC o en Windows, `MPO_FFMPEG=auto` cae a `system` u `off` porque el
    `configure` de FFmpeg necesita un shell POSIX. Sin FFmpeg no hay video (el título y los
    cómics). Opciones: que la CI publique un FFmpeg mínimo ya compilado con el mismo llvm-mingw
    (LGPL: con su código fuente al lado), o compilarlo con el `sh` de un MSYS2 portátil. Hay que
    decidirlo en F22b.
  - **Riesgo sin medir**: el juego nunca se compiló en Windows (F14b, F19 y F21 lo dejan como
    pendiente del usuario). `mpo_boot` exporta todos sus símbolos para las librerías de etapa
    (`WINDOWS_EXPORT_ALL_SYMBOLS`), y un ejecutable PE admite como mucho 65 535 exports; con el
    corpus de `mgp_main` dentro, puede que no alcance [?].
  - Tampoco se probó compilar el proyecto con llvm-mingw (la CI usa MSVC y clang-cl) [?].
- **Linux y Steam Deck**: la idea es un **Flatpak cuyo runtime sea `org.freedesktop.Sdk`** (y no
  `Platform`), que trae GCC, CMake, Ninja, Python y las cabeceras de X11 y Wayland [?]. Así se
  compila dentro del sandbox, igual en cualquier distribución y en SteamOS (que tiene la raíz de
  solo lectura y no trae compiladores). Clang se puede sumar con la extensión `llvm` del SDK [?].
  - Sin Flatpak (AppImage o el código fuente) hacen falta los paquetes del sistema: CMake ≥ 3.22,
    Ninja, Clang o GCC con C++20, **Python ≥ 3.11** (los scripts usan `tomllib`), `libx11-dev` y
    `libxext-dev` (o los de Wayland), `make` y `sh` para FFmpeg.
- **macOS**: fuera de alcance hasta F23+.

## 4. El launcher (F22b)

Una ventana con tres cosas: elegir el ISO (y, si no las encuentra, la carpeta de las fuentes),
mostrar el progreso de `setup.py install` y después un botón **Jugar**, mientras `setup.py stages
--low-priority` sigue en segundo plano con su progreso. También las acciones de saves de F18c
(`mpo --import-save` y `--export-save`).

- La forma más directa es Python con Tkinter, empaquetado con PyInstaller (como gow-psp-recomp),
  porque `setup.py` ya es Python y el toolchain ya trae un Python. La alternativa es un launcher en
  C++ sobre SDL3 con Dear ImGui (el SDL3 ya está), que no necesita PyInstaller pero hay que
  compilarlo en la CI para cada plataforma.

## 5. Pendiente

- `--unimplemented`: `setup.py play` deja el valor por defecto (`stop`). Los únicos imports sin
  implementar están en los menús de red (F21), que no hacen falta para terminar el juego; si el
  jugador entra, el juego se detiene con el nombre del NID. Para la release puede convenir
  `continue` con aviso.
- Las rutas por defecto de `mpo_boot` (las tablas de la VFPU y las librerías) apuntan al directorio
  de build. Eso vale mientras el build viva en `<data>/build`; un paquete que mueva los binarios
  tiene que pasarlas por parámetro.
- La tabla de NIDs (`--nid-table`) solo pone nombres en el informe y no hace falta para jugar.
