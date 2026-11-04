# F25 (mejoras) — Atajo de pantalla completa, supresión de LSan y `<algorithm>`

- **Fecha**: 2026-11-04
- **Estado al cerrar**: ✅ dos de tres, ola 7, en su worktree. Tres entradas sueltas de la lista
  "F23+ — Mejoras" de `docs/PLAN.md`, sin ficha propia:
  1. el atajo de pantalla completa durante el juego: **hecho**;
  2. la supresión de LSan para las fugas intermitentes de lavapipe en la CI: **hecha**;
  3. el `#include <algorithm>` en `allegrex_context.hpp`: **no se hizo**, porque obliga a
     recompilar las 92 etapas del jugador (ver abajo).
- No cambia el ABI del runtime (`psprecomp::Runtime`, `AllegrexContext`, `kOverlayAbiVersion`), ni
  ningún archivo del framework, ni el puntero del submódulo. No toca el launcher.

## Hecho

### 1. F11 y Alt+Enter alternan la pantalla completa

- `host/frontend/input.h` / `input.cpp` (sin SDL):
  - `WindowShortcutFor(key, alt)`: F11, o Enter (también el del teclado numérico) con Alt, es
    `kToggleFullscreen`;
  - `WindowShortcutFilter::OnKey(key, down, repeat, alt, &shortcut)` dice si el evento es de un
    atajo: se queda con la pulsación, sus repeticiones y su suelta (aunque Alt se suelte antes que
    Enter). Una tecla que ya estaba apretada antes de Alt (Enter como Start) sigue yendo al juego,
    con su repetición y su suelta. `Reset()` al perder el foco, porque esas sueltas no llegan;
  - las constantes `kF1`, `kF11`, `kKpEnter`, `kLeftAlt` y `kRightAlt` (con `static_assert` contra
    SDL en `sdl_input.cpp`).
- `host/frontend/sdl_input.*`: `SdlInput::HandleWindowShortcut(event)` pasa el filtro y, con la
  pulsación, llama a `SDL_SetWindowFullscreen` sobre la ventana de la tecla (sin modo de pantalla:
  sin bordes a la resolución del escritorio, igual que `--fullscreen 1` de F24a). `HandleEvent` la
  llama primero, así que `mpo_frontend_demo` también tiene el atajo. El atajo **no** cuenta como
  último dispositivo (no cambia los botones en pantalla ni escribe un registro `p`) y el observador
  anota "fullscreen on/off", no la tecla.
- `host/hle/boot_main.cpp`: en los dos bucles de eventos (ventana SDL y ventana Vulkan),
  `input->HandleWindowShortcut(event)` va **antes** del OSK. Si no, con el teclado en pantalla
  abierto, Alt+Enter lo terminaría (Enter es "terminar" en el OSK). La swapchain sigue al cambio
  con el `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` que ya estaba (F24a); el presentador SDL ajusta la
  imagen en cada frame. Al salir de la pantalla completa la ventana vuelve al tamaño de `--scale`
  (el comentario de `--fullscreen` decía que `--scale` se ignoraba; ahora dice eso).
- `profiles/mpo/config/input.toml`: un comentario dice que F11 y Alt+Enter no llegan nunca al
  juego. Ninguna asignación cambia.
- `README.md` y `README.es.md`, a la vez: en **Controls** / **Controles** (teclado y mouse) y en el
  ajuste **Fullscreen**.

### 2. Supresión de LSan para lavapipe

- Las fugas del run `41726739835` (job `Linux gcc asan-ubsan`, `mpo_vulkan_renderer_test`,
  `mpo_vulkan_vram_sync_test` y `mpo_vulkan_hazard_test`): **todas indirectas**, de 56 bytes,
  con `calloc` y luego solo frames `<unknown module>` (o nombres viejos: `libubsan.so` o el propio
  test, con desplazamientos que no son de su código) hasta `asan_thread_start`. Son hilos que
  arranca lavapipe, cuyo código el loader ya descargó en `vkDestroyInstance`. Ni un frame nuestro.
- **En este contenedor se reproduce** (la misma Mesa 25.2.8-0ubuntu0.24.04.4 y el loader
  1.3.275): con ASan, `mpo_vulkan_texreplace_test` y `mpo_vulkan_hazard_test` fallaron igual que
  en la CI. Y no son fugas de verdad: los bloques solo los apunta la memoria del propio driver,
  que el loader descarga en `vkDestroyInstance`. Con
  `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` (el loader no descarga el ICD ni las capas) el
  mismo test sale limpio **sin ninguna supresión**.
- **La supresión sola no alcanza**: en una pasada, LSan atribuyó todos los frames de dos pilas a
  `libasan.so` (con desplazamientos mayores que la librería: otro módulo mapeado después en esas
  direcciones), y `leak:<unknown module>` no las tapó. Suprimir `libasan.so`, `libubsan.so` o el
  test escondería cualquier cosa.
- Así que, solo con `MPO_SANITIZERS` con `address`, `profiles/mpo/tests/CMakeLists.txt` añade al
  `ENVIRONMENT` de los seis `mpo_vulkan_*` que corren algo:
  - `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1`, que es el arreglo;
  - `LSAN_OPTIONS=suppressions='<…>/lsan_vulkan.supp'`, de respaldo: `<unknown module>`,
    `libvulkan_lvp.so`, `libLLVM.so` (lavapipe lo enlaza), `libvulkan.so` y
    `libVkLayer_khronos_validation.so`.

  El resto de los tests no ven ninguna de las dos, y el workflow no cambia.

### 3. `#include <algorithm>` en `allegrex_context.hpp`: no

- Obliga a recompilar todo:
  - el C++ generado incluye `psprecomp/runtime.hpp` (lo emite
    `framework/psprecomp/tools/codegen_main.cpp`), que incluye `allegrex_context.hpp`;
  - cada etapa es un target `mpo_overlay_<s>` del mismo árbol de CMake y Ninja que compila
    `setup.py` (`_build_stage` → `cmake --build … --target mpo_overlay_<s>`);
  - Ninja sigue los headers con depfiles: `ninja -t deps` sobre el corpus de prueba
    (`mpo_test_overlay.dir/overlay_test_corpus.cpp.o`) lista `allegrex_context.hpp` y
    `runtime.hpp`.

  Cambiar el header (o mover el submódulo a un commit que lo cambie) vuelve a compilar las 92
  etapas del jugador (6 h 45 min a `-j5`, F21) y `mgp_main`. Además, la regla de la ola es no tocar
  headers del framework.
- Sigue el arreglo de F22b: `-include algorithm` solo con libc++, desde el `CMakeLists.txt` raíz.
  Queda anotado en `docs/PLAN.md` para hacerlo junto con el próximo cambio que ya obligue a
  recompilar.

## Evidencia de los criterios de hecho

- **Tests sin ventana del atajo**:
  - `mpo_frontend_test` › `TestWindowShortcuts`: qué teclas son atajo (F11, Alt+F11, Alt+Enter,
    Alt+Enter del teclado numérico; Enter solo, F1 y Alt con otras teclas no); que el mapeo por
    defecto no asigna F1, F11, Alt ni el Enter numérico; F11 con repetición y suelta; Alt+Enter no
    aprieta Start, aunque Alt se suelte primero; Enter apretado antes de Alt sigue siendo Start;
    Alt+Space es Cruz; un F11 asignado en un mapeo propio no llega al juego; y el `Reset`.
  - `mpo_frontend_sdl_test` › `TestWindowShortcut` (driver `dummy`, ventana oculta): eventos SDL
    reales con `windowID` y `mod`: F11 pone la ventana en pantalla completa sin modo exclusivo, la
    repetición no vuelve a cambiarla, Alt+Enter la devuelve a ventana sin apretar Start, Enter
    solo es Start y no la cambia, el último dispositivo no cambia y las notas del observador.
- **`boot_main.cpp`**: `mpo_boot` no se enlaza sin el código generado, así que se comprobó con
  `clang++ -fsyntax-only -Werror` (las mismas advertencias del proyecto, `MPO_BOOT_SDL`,
  `MPO_BOOT_VULKAN` y `MPO_BOOT_AUDIO`, y todos los include del build): pasa.
- **Las fugas de lavapipe**, con los tests de verdad (GCC 13.3 con ASan, la misma Mesa):
  - sin nada, `mpo_vulkan_texreplace_test` aborta (fuga indirecta, `<unknown module>`);
  - con solo `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1`, `mpo_vulkan_texreplace_test` y
    `mpo_vulkan_hazard_test` salen con 0, sin informe de LSan.
- **La supresión**, con un caso sintético en el scratchpad (GCC 13.3, el mismo libasan que la
  CI): una librería que hace dos `calloc` en ciclo en un hilo suyo y se descarga con `dlclose`.
  - Sin supresiones, LSan da el mismo informe que la CI (fuga indirecta, `<unknown module>`,
    `asan_thread_start`).
  - Con `lsan_vulkan.supp` (y con la ruta entre comillas, como la pone ctest), sale limpio:
    "Suppressions used: 2 112 `<unknown module>`".
  - Con una fuga más desde `main.c`, esa sí se informa (`Direct leak of 99 byte(s)`, en
    `our_code_leaks main.c:6`) y el proceso falla.
- **`ctest` completo**, compilado desde cero en `/tmp` (scratchpad):
  - **Clang 18 Release**: 64/64 (los dos de Unicorn se saltan, como siempre). En este contenedor
    hay Vulkan, así que los `mpo_vulkan_*` corrieron de verdad.
  - **GCC 13.3 Debug con ASan y UBSan** (las variables del job de la CI: `detect_leaks=1`,
    `MPO_VK_REQUIRE_VALIDATION=1`): **64/64** (los de Unicorn se saltan), con los `mpo_vulkan_*`
    corriendo sobre lavapipe, **sin ningún informe de LSan y sin que la supresión llegara a
    actuar** (no sale "Suppressions used"). La primera pasada, antes de añadir
    `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING`, falló justo por estas fugas (y por el
    `psprecomp_runtime_contract_tests` de abajo). `mpo_vulkan_renderer_test` tardó 355 s a `-j2`
    con ASan en este contenedor (la CI lo corre en menos de sus 300 s).
  - Clang con ASan no se pudo: este contenedor no trae el runtime de sanitizers de Clang
    (`libclang_rt.asan_cxx`). La CI lo cubre.
  - Las dependencias se bajaron con `git clone` de sus tags y se pasaron con
    `MPO_*_SOURCE_DIR`: el proxy no deja bajar los tarballs de GitHub ni de ffmpeg.org.

## Decisiones tomadas

- **El filtro va en el frontend, antes que el OSK y el mapeo**, no en `InputTranslator`: el atajo
  es de la ventana, y así tampoco lo ven el OSK, el último dispositivo ni el observador. Las
  grabaciones guardan muestras de `sceCtrl` y el texto del OSK, que no cambian.
- **El atajo gana a `input.toml`**: un F11 asignado no llega al juego. Es lo habitual en los
  juegos de PC, y así la tecla no hace dos cosas a la vez.
- **Una tecla ya apretada no se vuelve atajo**: si el jugador mantiene Enter (Start) y aprieta
  Alt, sigue siendo Start hasta que la suelte.
- **Sin modo exclusivo**, igual que F24a.
- **No descargar el driver, antes que suprimir**: es la causa, y no esconde nada (con el driver
  cargado, LSan sigue viendo todo lo que de verdad nadie apunta). La supresión queda de respaldo.
- **Las dos van en ctest, no en el workflow**: solo las ven los `mpo_vulkan_*`, y también en
  local con ASan. Una supresión por nombre de función nuestra se descartó (escondería fugas
  nuestras), y `leak:<unknown module>` solo actúa sobre pilas que pasan por una librería
  descargada. Un objeto de Vulkan que olvidemos destruir lo informa la capa de validación, no LSan.

## Pendiente y trampas encontradas

- **Probar el atajo en el juego**, en Linux y Windows, con Vulkan y con softgpu (ver abajo).
- **Ver en la CI** que el job `Linux gcc asan-ubsan` ya no falla por lavapipe (era intermitente:
  hay que mirar varios runs). En el log del job sale "Suppressions used" si la supresión llega a
  actuar.
- **Trampa**: LSan nombra a veces los frames de la librería descargada con el nombre de otro
  módulo cargado después en las mismas direcciones (`libubsan.so`, `libasan.so`, el propio test).
  Por eso la supresión sola no basta, y no hay que suprimir esos nombres.
- **Trampa**: `psprecomp_runtime_contract_tests` (del framework, que esta rama no toca) falló una
  vez con ASan a `-j3` ("Repeated automatic generation must preserve unchanged units") y pasó
  solo; parece sensible a la carga de la máquina.
- **Trampa**: en Windows, Alt solo activa la barra de menú en algunas ventanas. SDL3 no tiene
  menú, así que no debería pasar, pero hay que mirarlo al probar.
- El `#include <algorithm>` sigue pendiente (ver arriba).

## Para la próxima sesión

- Integración de la ola 7: esta rama toca `host/frontend/input.{h,cpp}`,
  `host/frontend/sdl_input.{h,cpp}`, los dos bucles de eventos de `host/hle/boot_main.cpp` (una
  línea cada uno) y su cabecera (el comentario de `--fullscreen`), `tests/frontend_test.cpp`,
  `tests/frontend_sdl_test.cpp`, `tests/CMakeLists.txt` (el bloque de Vulkan),
  `config/input.toml` (un comentario), los dos README (**Settings** y **Controls**) y
  `docs/PLAN.md` (tres entradas de F23+). F25b (widescreen) probablemente toque `boot_main.cpp` en
  las opciones; F25d (el launcher) no debería chocar.
- **Lo que tiene que probar el usuario**:
  1. Sin el juego: `mpo_frontend_demo` (en `build/profiles/mpo/host/frontend/`): F11 y
     Alt+Enter alternan la pantalla completa; la traza (`--trace`) dice "fullscreen on/off" y no
     muestra la tecla.
  2. En el juego (`setup.py play`, Vulkan): F11 pasa a pantalla completa sin bordes y nítida, y
     F11 otra vez vuelve a la ventana del tamaño de **Window**; igual con Alt+Enter, con los dos
     Alt. En la pantalla de título o en un menú, Alt+Enter **no** debe abrir ni confirmar nada;
     Enter solo sigue siendo Start.
  3. Con el teclado en pantalla abierto (un nombre en NEW GAME), Alt+Enter cambia la pantalla y
     **no** termina el nombre; Enter solo lo termina.
  4. Con el mouse capturado (clic en la ventana), F11 cambia la pantalla y la cámara sigue con el
     mouse; F1 lo suelta.
  5. Lo mismo con `MPO_RENDERER=soft` (la ventana SDL).
  6. Una repetición de `analysis/f21` (`--replay-input`) sigue saliendo idéntica.
  7. En Windows, lo mismo de 2 y 3.
