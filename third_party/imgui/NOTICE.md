# Dear ImGui

- **Versión**: `1.93.0` (tag `v1.93.0`, publicada el 2026-10-14).
- **Origen**: `https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz`,
  SHA-256 `f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee` (calculado al
  descargarlo el 2026-11-01).
- **Licencia**: MIT (texto en `LICENSE.txt`, copia del archivo del tarball).
- **No se versiona el código**: `imgui.cmake` lo descarga al configurar con `FetchContent`,
  comprueba el SHA-256 y compila `mpo_imgui` (núcleo, `misc/cpp/imgui_stdlib.cpp` y los backends
  `imgui_impl_sdl3` e `imgui_impl_sdlrenderer3`) sin los warnings del proyecto.
- **Uso**: solo el launcher (`profiles/mpo/launcher`, F22b). El jugador no lo compila
  (`setup.py` configura con `MPO_BUILD_LAUNCHER=OFF`); la release trae el launcher ya compilado.
- Sin red: `MPO_IMGUI_SOURCE_DIR`; sin launcher: `MPO_BUILD_LAUNCHER=OFF`.
