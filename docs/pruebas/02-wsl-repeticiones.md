<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 2. Las repeticiones automáticas, en WSL

Son las pruebas que comparan hashes y capturas: **no hace falta mirar la pantalla ni una GPU de
verdad**, así que se hacen en tu WSL de siempre, con tus builds (`build-f24c-game`,
`build-f09-release`) y tus scripts (`analysis/f24c/run.sh`, `analysis/spike-widescreen/run.sh`).
Así se hicieron todas las de F19 a F24c. Lo que necesita la GPU (la ventana) está en
[04-linux-nativo-ventana.md](04-linux-nativo-ventana.md) y en [01-windows.md](01-windows.md).

Las convenciones son las de F24c: `mpo_boot` del build del juego, las etapas de
`build-f09-release/overlays`, `--interpreter 0`, `TZ='<-05>5'`, una copia `cp -a` del `.ms0` en
cada corrida y `--capture-every 60`. En los scripts, cambiar `mpo_boot` por el nuevo. Nada de lo
que salga va al repo. Vulkan corre en `llvmpipe`: va lento, pero las repeticiones no dependen del
tiempo real.

Al lado de cada paso, ✅ o ❌ y una línea si algo sale raro.

## 0. Preparar (una vez)

1. Traer la rama y comprobar que **las etapas no se recompilan**:
   ```bash
   git pull origin claude/modest-ptolemy-y9sau6
   git submodule status          # tiene que seguir en 92c9425
   ninja -C build-f09-release -n | grep -c 'Building CXX object.*mpo_overlay_'   # tiene que dar 0
   ```
   Si da más de 0, **parar**: algo tocó lo que incluyen las etapas, y no tendría que pasar (ni el
   framework ni `overlay_module.*` cambiaron en la ola 7). Que `mpo_boot` se recompile es normal.
2. Recompilar solo `mpo_boot` en el build del juego:
   ```bash
   cmake --build build-f24c-game --target mpo_boot
   ```
3. Los tests sin el juego (opcional; en la integración pasaron 68 de 68 con GCC y Clang):
   ```bash
   ctest --test-dir build-f24c-game -j4
   ```

## A. Repeticiones automáticas (sin mirar la pantalla)

Son las que dicen si algo cambió sin querer. Cada una deja sus capturas y la línea de
comprobaciones al final.

| # | Qué | Cómo | Tiene que dar |
|---|---|---|---|
| A1 | Nada cambia por defecto (todas las fases) | `play1` sin opciones, softgpu | 224 iguales, 0 distintas; el reporte dice `prompt icons: off` |
| A2 | Lo mismo en Vulkan | A1 con `--renderer vulkan` | 224 iguales; `capture_compare.py` contra A1: idénticas |
| A3 | Sin pack de texturas (F25c) | A1 con `--textures none` | 224 iguales |
| A4 | El volcado con la VRAM (F25c) | `--textures none --dump-textures dump-vram` | capturas idénticas a A1; el volcado trae los 736 PNG de F24c **más** texturas del mundo de la VRAM; el reporte cuenta `skipped as framebuffers` y `as video frames` |
| A5 | Widescreen nativo (F25b) | `spike-widescreen/run.sh w1-native game/elf/mgp_main.elf 136000000` | 135/135; ninguna línea `aspect:` |
| A6 | Widescreen 21:9 (F25b) | lo mismo con `w1-wide ... --aspect 64:27` | la línea `aspect: 64:27 ... 0x089E9728 = 0.744485 (0x3F3E9697)`; las comprobaciones difieren desde el flip 3600; **`diff -r` de las `ms0` de A5 y A6: sin diferencias** |
| A7 | El rechazo (F25b) | `--aspect 21:9` con `analysis/spike-widescreen/elf/wide.elf` | falla con "is not a version widescreen.toml knows" |
| A8 | Una repetición de F21 (mejoras) | cualquier tramo de `analysis/f21` con `--replay-input` | idéntica, como antes |

Mirar también en A4:
- que en `dump-vram` no haya PNG con la pantalla ni con frames de video (abrir los de 480×272 o de
  512 de ancho, si hay);
- las líneas `textures: framebuffer …`: deberían ser los dos buffers de la pantalla y la
  profundidad, terminando antes de `0x0CC000`. Si un rectángulo cubre la zona de las texturas,
  anotarlo.

## B. Pruebas con un pack y con los íconos (todavía repeticiones)

| # | Qué | Cómo | Tiene que dar |
|---|---|---|---|
| B1 | Un reemplazo en la VRAM (F25c) | de `dump-vram`, pintar encima de una textura del mundo (una pared de la celda de `st04vo`), copiarla con su nombre a un pack con el `textures.ini` del volcado y repetir `play1` con `--textures <pack>` en softgpu y en Vulkan | se ve en las capturas de los dos; `capture_compare.py` soft contra vulkan: 224 idénticas |
| B2 | El coste (F25c) | el tiempo y la memoria de B1 contra A1 | anotar los números (F24c midió +0,3 % solo con la RAM) |
| B3 | Los íconos de Xbox (F25a) | sacar las líneas `p 0` e `i 0` de una corrida corta con `--prompts xbox --record-input /tmp/x.input --until-us 2000000`, meterlas en una copia de `play1` después de la primera línea (`F25a.md`, paso 2) y repetirla | en `flip-013440-*.png`, el △ de "△ : FLOOR CHANGE" es una **Y en un disco**; solo difieren ese frame y los de los diálogos del sistema |
| B4 | Los íconos de teclado (F25a) | lo mismo con `--prompts keyboard` | el △ es una tecla **CTL** |
| B5 | Los íconos de PlayStation (F25a) | lo mismo con `--prompts playstation` | el △ del juego, sin cambios |
| B6 | Íconos en Vulkan (F25a) | B3 con `--renderer vulkan` y `MPO_VK_VALIDATION=1` | `capture_compare.py` contra B3: idénticas; 0 errores de validación |
| B7 | Íconos más el pack del jugador (F25a) | B3 con `--textures analysis/f24c/pack-y`; y `play1` sin cambios con el mismo pack | en B3 el ícono del port (no la Y amarilla del pack); en `play1`, la Y del pack |
| B8 | Grabar con íconos (F25a) | grabar hasta la pausa con `--prompts xbox --record-input r.input`, repetir sin `--prompts` | todas las comprobaciones iguales |

## Si algo falla

- Anotar el primer flip distinto y el comando exacto. Para bisecar, cada fase se apaga sola:
  `--prompt-icons off` (F25a), sin `--aspect` (F25b) y `--textures none` (F25c).
- Un frame raro en Vulkan: los `MPO_VK_*` de siempre, y comparar con softgpu.
- Si el `ninja -n` del paso 0 dice que las etapas se recompilan, no seguir y avisar.
