# SDL3

- **Versión**: `3.4.18` (tag `release-3.4.18`, publicada el 2026-10-02).
- **Origen**: `https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz`,
  SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`.
- **Licencia**: zlib (texto en `LICENSE.txt`, copiado del tarball).
- **No se versiona el código**: `sdl3.cmake` lo descarga al configurar con `FetchContent`, comprueba
  el SHA-256 y lo compila como librería estática (`mpo::sdl3`). Sin red se puede usar
  `-DMPO_SDL3_SOURCE_DIR=<árbol de SDL3 3.4.18 descomprimido>`, y `-DMPO_ENABLE_SDL3=OFF` lo quita
  del build.
- **Subsistemas** (`sdl3.cmake`): audio (F16a) y video, render, joystick/gamepad con HIDAPI (el driver
  de joystick virtual de los tests depende de él), haptic, power y sensores (F10a). GPU, cámara,
  diálogos y bandeja están apagados. `-DMPO_SDL3_AUDIO_ONLY=ON` vuelve a la build de F16a, solo con
  audio.
- **Linux**: SDL solo compila los drivers cuyas cabeceras encuentra al configurar.
  - Video X11: `libx11-dev` y `libxext-dev`. Las extensiones opcionales (`libxcursor-dev`,
    `libxi-dev`, `libxrandr-dev`, `libxfixes-dev`, `libxss-dev`, `libxtst-dev`) se encienden solo si
    están sus cabeceras; SDL, por su cuenta, aborta el configure si falta una que tiene encendida.
  - Video Wayland: `libwayland-dev` y `libxkbcommon-dev`.
  - Sin X11 ni Wayland, el build sigue con un aviso y solo los drivers `dummy` y `offscreen`, que
    bastan para los tests sin ventana.
  - Audio: `libasound2-dev`, `libpulse-dev` o `libpipewire-0.3-dev`; sin ellos quedan `dummy` y
    `disk`, que bastan para los tests.
