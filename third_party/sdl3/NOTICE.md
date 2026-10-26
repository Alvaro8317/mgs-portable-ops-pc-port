# SDL3

- **Versión**: `3.4.18` (tag `release-3.4.18`, publicada el 2026-10-02).
- **Origen**: `https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz`,
  SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`.
- **Licencia**: zlib (texto en `LICENSE.txt`, copiado del tarball).
- **No se versiona el código**: `sdl3.cmake` lo descarga al configurar con `FetchContent`, comprueba
  el SHA-256 y lo compila como librería estática (`mpo::sdl3`). Sin red se puede usar
  `-DMPO_SDL3_SOURCE_DIR=<árbol de SDL3 3.4.18 descomprimido>`, y `-DMPO_ENABLE_SDL3=OFF` lo quita
  del build.
- **Solo audio por ahora** (`MPO_SDL3_AUDIO_ONLY=ON`, F16a): el resto de subsistemas está apagado
  para que el configure no exija las cabeceras de X11/Wayland. En Linux, SDL solo compila los
  backends de audio cuyas cabeceras encuentra (`libasound2-dev`, `libpulse-dev`,
  `libpipewire-0.3-dev`); sin ellas quedan `dummy` y `disk`, que bastan para los tests.
