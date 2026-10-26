# Vulkan-Headers, volk y VulkanMemoryAllocator

Dependencias del renderer Vulkan del GE (`profiles/mpo/host/vulkan/`, F14a). **No se versiona su
código**: `vulkan.cmake` descarga cada una al configurar con `FetchContent`, comprueba el SHA-256 del
archivo y usa solo sus fuentes (sin añadir sus proyectos de CMake). Las licencias van al lado.

| Componente | Versión | Origen | SHA-256 del archivo | Licencia |
|---|---|---|---|---|
| Vulkan-Headers | tag `v1.4.350` (commit `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77`, 2026-05-01) | `https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.350.tar.gz` | `6dd105e5cc7ddab6e7b611ae2c1872740d1727557cc8bf9daf13d6de1e4b3999` | Apache-2.0 OR MIT (`LICENSE-Vulkan-Headers.md`, `LICENSE-Vulkan-Headers-Apache-2.0.txt`, `LICENSE-Vulkan-Headers-MIT.txt`) |
| volk | tag `1.4.350` (commit `3ca312a4f38baa63d8006b6905abbeeb89c8087d`, 2026-05-01) | `https://github.com/zeux/volk/archive/refs/tags/1.4.350.tar.gz` | `e47c6efe5294bb03729a976b385864bba5daf46ec60f0bdea11e1d1446345f9a` | MIT (`LICENSE-volk.md`) |
| VulkanMemoryAllocator | tag `v3.4.0` (2026-06-04) | `https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz` | `822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1` | MIT (`LICENSE-VulkanMemoryAllocator.txt`) |

- volk y Vulkan-Headers van con la misma versión del SDK (1.4.350). VMA `v3.5.0` salió el
  2026-10-26, el mismo día de F14a, así que se fijó la anterior.
- Todas son compatibles con GPL-3.0-or-later (Apache-2.0 y MIT).
- Nada enlaza contra un SDK de Vulkan: volk abre en ejecución el loader del sistema
  (`libvulkan.so.1` o `vulkan-1.dll`). Sin loader ni driver, el renderer informa "no disponible" y
  los tests se saltan.
- Se compilan como código de terceros: volk en `mpo_volk` (C) y la implementación de VMA en
  `mpo_vulkan_vma` (`profiles/mpo/host/vulkan/vma_impl.cpp`), sin los warnings del proyecto. Sus
  cabeceras entran como `SYSTEM`.
- Sin red: `-DMPO_VULKAN_HEADERS_SOURCE_DIR=...`, `-DMPO_VOLK_SOURCE_DIR=...` y
  `-DMPO_VMA_SOURCE_DIR=...` (los árboles descomprimidos de esas versiones). Sin Vulkan:
  `-DMPO_ENABLE_VULKAN=OFF`.
- **glslang no es una dependencia**: los shaders se compilan una vez con `glslangValidator`
  (glslang-tools 15.1.0 de Ubuntu) y el SPIR-V se versiona en
  `profiles/mpo/host/vulkan/shaders/ge_shaders_spv.inc`, que es código nuestro (GPL-3.0-or-later)
  traducido. El test `mpo_vulkan_shaders_fresh` comprueba que esté al día.
