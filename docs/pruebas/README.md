<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Pruebas de la ola 7

La ola 7 (F25a, F25b, F25c, F25d y las mejoras pequeñas) se integró el 2026-11-04 **sin probar en
el juego**. Estas guías la prueban entera, repartida por dónde tiene sentido cada cosa:

| # | Guía | Dónde | Qué prueba |
|---|---|---|---|
| 1 | [01-windows.md](01-windows.md) | Windows, con el ZIP de esta rama | Actualizar sin recompilar las etapas, el launcher en los dos idiomas, F11/Alt+Enter, los íconos con el mando, el widescreen y las texturas en la ventana |
| 2 | [02-wsl-repeticiones.md](02-wsl-repeticiones.md) | WSL, como siempre | Las repeticiones con hashes: que nada cambie por defecto, el volcado de la VRAM, el parche del widescreen y los íconos frame a frame |
| 3 | [03-linux-nativo-preparar.md](03-linux-nativo-preparar.md) | Una partición con Ubuntu 24.04 | Instalar Linux con los drivers de la GPU y el port |
| 4 | [04-linux-nativo-ventana.md](04-linux-nativo-ventana.md) | Ese Linux | Lo de Windows en Linux, más las capas de validación de Vulkan |

**Orden sugerido**: 1 y 2 se pueden hacer el mismo día (la 2 corre sola mientras haces la 1). La 3
y la 4, cuando tengas la partición.

Todas tienen casillas para marcar ✅ o ❌. El detalle técnico de cada fase está en su nota
(`docs/sessions/F25a.md`, `F25b.md`, `F25c.md`, `F25d.md` y `F25-mejoras.md`).

## La CI

- [ ] Mirar varias corridas del job `Linux gcc asan-ubsan`: ya no tiene que fallar por las fugas de
  lavapipe en los `mpo_vulkan_*` (antes era intermitente). Si en el log sale "Suppressions used",
  la supresión actuó.
- [ ] Los jobs de Windows (MSVC y `windows-llvm-mingw`) pasan.

## Las reglas de siempre

- Nada de lo que salga (capturas, volcados, grabaciones, saves) va al repo.
- Nunca contra tus saves reales sin una copia: `packaging/hash_saves.py` antes y después.
- Para bisecar, cada fase se apaga sola: `--prompt-icons off` (F25a), sin `--aspect` (F25b),
  `--textures none` (F25c).
