# Dependencias de terceros

El proyecto es **GPL-3.0-or-later** (ver `LICENSE`). Toda dependencia nueva se registra aquí con su
versión fijada y su licencia, y lleva su `LICENSE` o `NOTICE` al lado en `third_party/<nombre>/`. No
se aceptan dependencias GPL-2.0-only ni con licencias incompatibles con GPLv3.

Estado: **planificadas**, todavía no incorporadas. La columna "Versión" se llena al incorporarlas.

| Componente | Uso | Licencia | Versión / commit | Notas |
|---|---|---|---|---|
| PSPRecomp | Framework de recompilación y runtime base | MIT | — | Nuestros cambios genéricos se quedan en MIT para enviarlos upstream. |
| SDL3 | Ventana, input y audio | zlib | — | |
| Vulkan-Headers | API gráfica | Apache-2.0 / MIT | — | |
| volk | Loader de Vulkan | MIT | — | |
| VulkanMemoryAllocator | Memoria de GPU | MIT | — | |
| glslang o shaderc | Compilar shaders a SPIR-V | BSD-3 / Apache-2.0 (varía según el archivo) | — | Solo en tiempo de build. |
| PPSSPP (código portado) | GE (software, splines y beziers), SAS, VFPU, HLE y NIDs | GPL-2.0-or-later | tag `v1.20.4` | Cada archivo portado indica el archivo y el tag de origen. |
| at3_standalone (de PPSSPP/FFmpeg) | Decodificar ATRAC3 | LGPL-2.1-or-later | — | |
| FFmpeg (build mínimo) | Video del título (`sceMpeg`) | LGPL-2.1-or-later | — | Solo los decoders necesarios. |
| libkirk | Descifrar EBOOT y PRX (solo en el instalador) | GPL-3.0+ / GPL-2.0+ | — | No se distribuyen claves fuera de lo que trae la propia libkirk. Hay que revisarlo en M0. |
| Dear ImGui | UI de depuración y diálogos propios | MIT | — | |
