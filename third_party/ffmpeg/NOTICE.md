# FFmpeg (build mínimo)

- **Versión**: `7.1.5` (rama 7.1, publicada el 2026-06-20).
- **Origen**: `https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz`,
  SHA-256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`.
  - La firma `ffmpeg-7.1.5.tar.xz.asc` se verificó a mano (2026-10-26) con la clave de releases de
    FFmpeg (`FCF9 86EA 15E6 E293 A564  4F10 B432 2F04 D676 58D8`): "Good signature".
- **Licencia**: LGPL-2.1-or-later (texto en `LICENSE`, copia de `COPYING.LGPLv2.1` del tarball). El
  configure lo confirma: `License: LGPL version 2.1 or later`. Al enlazarlo de forma estática con este
  proyecto (GPL-3.0-or-later) rige la GPLv3 para el conjunto, que la LGPL-2.1+ permite.
- **No se versiona el código**: `ffmpeg.cmake` lo descarga y compila **en tiempo de build** con
  `ExternalProject` (fuera del árbol, con el compilador de C del build y ccache si está), comprueba
  el SHA-256 e instala en `<build>/third_party/ffmpeg/install`.
- **Qué se compila** (y nada más):
  - `libavcodec` con el decoder `h264` y el parser `h264`;
  - `libswscale` (la misma conversión YUV → RGB que usa PPSSPP);
  - `libavutil`.
  - Flags: `--disable-everything --disable-programs --disable-doc --disable-network
    --disable-autodetect --disable-iconv --disable-avdevice --disable-avformat --disable-avfilter
    --disable-swresample --enable-pthreads --enable-static --disable-shared --enable-pic
    --disable-debug`. **Sin** `--enable-gpl`, `--enable-version3` ni `--enable-nonfree`.
  - Sin `nasm` se añade `--disable-x86asm` (queda C y asm en línea). La salida no cambia: el decoder
    H.264 es exacto bit a bit y `mpo_video` pide a swscale `SWS_BITEXACT | SWS_ACCURATE_RND`.
- **Opciones** (`MPO_FFMPEG`):
  - `auto` (por defecto): `bundled` si hay shell POSIX y `make` y el compilador no es MSVC; si no,
    `system` si se encuentra; si no, sin FFmpeg;
  - `bundled`: el build de arriba;
  - `system`: `pkg-config` (`libavcodec>=59`, `libavutil`, `libswscale`) o, sin pkg-config,
    `find_path`/`find_library` (vcpkg en Windows: `ffmpeg[avcodec,swscale]`). Un FFmpeg del sistema
    puede venir compilado con `--enable-gpl`; sigue siendo compatible con la GPLv3 del proyecto,
    pero las distribuciones se hacen con `bundled`;
  - `off`: `mpo_video` se compila sin decoder de vídeo (el audio y el demuxer siguen) y
    `mpo_video_decode_test` se marca como omitido (código 77).
- Sin red: `-DMPO_FFMPEG_SOURCE_DIR=<árbol 7.1.5 descomprimido>` o
  `-DMPO_FFMPEG_URL=<ruta local al .tar.xz>` (el hash se comprueba igual).
- `MPO_FFMPEG_JOBS` (por defecto 3) fija los trabajos de `make`. Con `-j3` el primer build tardó
  unos 10 s en la máquina de desarrollo (sin asm; `libavcodec.a` 2.4 MB, `libswscale.a` 1.2 MB,
  `libavutil.a` 1.1 MB).
