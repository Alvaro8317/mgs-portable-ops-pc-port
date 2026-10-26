# Dependencias de terceros

El proyecto es **GPL-3.0-or-later** (ver `LICENSE`). Toda dependencia nueva se registra aquí con su
versión fijada y su licencia, y lleva su `LICENSE` o `NOTICE` al lado en `third_party/<nombre>/`. No
se aceptan dependencias GPL-2.0-only ni con licencias incompatibles con GPLv3.

Estado: **PSPRecomp**, **toml++**, **libkirk + PrxDecrypter** (descargados al configurar), **SDL3** (descargado al configurar), **at3_standalone** y el **código de audio portado de PPSSPP** están incorporados; el resto está **planificado**. La columna "Versión" se llena al incorporar cada uno.

| Componente | Uso | Licencia | Versión / commit | Notas |
|---|---|---|---|---|
| PSPRecomp | Framework de recompilación y runtime base | MIT | `f6e7d415c7f447b934cc3865a31eb725f353d659` (2026-08-14) | Submódulo en `framework/psprecomp` (incorporado en F00; su `LICENSE` está dentro). Nuestros cambios genéricos se quedan en MIT para enviarlos upstream. |
| toml++ (tomlplusplus) | Leer `profiles/mpo/config/supported.toml` | MIT | `v3.4.0` (single header, SHA-256 `6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e`) | En `third_party/tomlplusplus/` con su `LICENSE` (incorporado en F01). Se compila con `TOML_EXCEPTIONS=0`. |
| SDL3 | Ventana, input y audio | zlib | `3.4.18` (tarball `SDL3-3.4.18.tar.gz`, SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`) | `third_party/sdl3/sdl3.cmake` lo descarga con `FetchContent` y lo compila estático (incorporado en F16a, de momento solo el subsistema de audio: `MPO_SDL3_AUDIO_ONLY`). `LICENSE.txt` y `NOTICE.md` al lado. Sin red: `MPO_SDL3_SOURCE_DIR`; sin SDL: `MPO_ENABLE_SDL3=OFF`. |
| Vulkan-Headers | API gráfica | Apache-2.0 / MIT | — | |
| volk | Loader de Vulkan | MIT | — | |
| VulkanMemoryAllocator | Memoria de GPU | MIT | — | |
| glslang o shaderc | Compilar shaders a SPIR-V | BSD-3 / Apache-2.0 (varía según el archivo) | — | Solo en tiempo de build. |
| PPSSPP (código portado) | GE (software, splines y beziers), SAS, VFPU, HLE y NIDs | GPL-2.0-or-later | tag `v1.20.4` | Cada archivo portado indica el archivo y el tag de origen. F16a: `profiles/mpo/host/audio/` (SasAudio, SasReverb, sceSas, AtracCtx2, AtracTrack, sceAtrac, Atrac3Standalone, sceAudio, `__sceAudio` y StereoResampler). El difftest opcional `mpo_audio_ppsspp_difftest` compila los originales desde `MPO_PPSSPP_SOURCE_DIR`, sin copiarlos al repo. |
| at3_standalone (de PPSSPP/FFmpeg) | Decodificar ATRAC3 y ATRAC3plus | LGPL-2.1-or-later (3 archivos de pegamento de PPSSPP, GPL-2.0-or-later) | PPSSPP `v1.20.4` (`fa50bb1976065c4f8b1b47af227d367fe9771555`), `ext/at3_standalone/` | Copia sin modificar en `third_party/at3_standalone/src/` (SHA-256 por archivo en su `NOTICE.md`), más `shim/` para las 3 cabeceras de PPSSPP que incluye. `LICENSE` con el texto de la LGPL-2.1 (incorporado en F16a). |
| FFmpeg (build mínimo) | Video del título (`sceMpeg`) | LGPL-2.1-or-later | — | Solo los decoders necesarios. |
| libkirk + `PrxDecrypter.cpp` (de PPSSPP) | Descifrar el `EBOOT.BIN` (`~PSP`) en el instalador | GPL-3.0+ (`kirk_engine`), GPL-2.0/3.0 (`bn`, `ec`), dominio público (`AES`, `SHA1`), GPL-2.0-or-later (`PrxDecrypter.cpp`) | PPSSPP `v1.20.4` (`fa50bb1976065c4f8b1b47af227d367fe9771555`), 10 archivos con SHA-256 fijado en `third_party/ppsspp_crypto/ppsspp_crypto.cmake` | **No se versionan**, porque traen claves KIRK y tablas de claves (§1.1). CMake los descarga al configurar y solo los enlaza `mpo_host` (incorporado en F02). La procedencia y las licencias están en `third_party/ppsspp_crypto/NOTICE.md`. Se puede compilar sin red con `MPO_PPSSPP_SOURCE_DIR` o con `MPO_ENABLE_PRX_DECRYPT=OFF`. |
| Dear ImGui | UI de depuración y diálogos propios | MIT | — | |
