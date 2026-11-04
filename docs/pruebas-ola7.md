<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Pruebas de la ola 7 (F25a, F25b, F25c, F25d y las mejoras pequeñas)

Todo lo de la ola 7 se integró el 2026-11-04 **sin probar en el juego**. Esta es la lista para
probarlo de una sola vez, en orden: primero lo automático (repeticiones con hashes, sin mirar
nada), después una sesión en la ventana y al final el launcher y las otras máquinas. El detalle
de cada paso está en la nota de su fase (`docs/sessions/F25*.md`).

Las convenciones son las de F24c y el spike de widescreen: `mpo_boot` del build del juego
(`build-f24c-game` o el que uses), las etapas de `build-f09-release/overlays`, `--interpreter 0`,
`TZ='<-05>5'`, una copia `cp -a` del `.ms0` en cada corrida y `--capture-every 60`. Los scripts
son los tuyos de siempre (`analysis/f24c/run.sh`, `analysis/spike-widescreen/run.sh`), cambiando
`mpo_boot` por el nuevo. Nada de lo que salga va al repo.

Al lado de cada paso, ✅ o ❌ y una línea si algo sale raro.

## 0. Preparar (una vez)

1. Traer la rama y comprobar que **las etapas no se recompilan**:
   ```bash
   git pull origin claude/modest-ptolemy-y9sau6
   git submodule status          # tiene que seguir en 92c9425
   ninja -C build-f09-release -n | grep -c 'Building CXX object.*mpo_overlay_'   # tiene que dar 0
   ```
   Si da más de 0, **parar**: algo tocó lo que incluyen las etapas, y no tendría que pasar (ni el
   framework ni `overlay_module.*` cambiaron en la ola 7). Que `mpo_boot` se recompile es normal;
   en Windows las etapas además se vuelven a **enlazar** contra él (segundos cada una), pero no se
   recompilan.
2. Recompilar solo `mpo_boot` en el build del juego:
   ```bash
   cmake --build build-f24c-game --target mpo_boot
   ```
3. Los tests sin el juego, en tu máquina (opcional; en la integración pasaron 68 de 68 con GCC y
   Clang):
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

## C. Una sesión en la ventana (todo junto)

Con `setup.py play` (o `mpo_boot --window 1`), en Linux con una GPU de verdad, primero con Vulkan
y después con `MPO_RENDERER=soft`. Conviene empezar desde NEW GAME, para pasar por el video, el
OSK y la primera etapa.

**Pantalla completa (mejoras):**
- [ ] C1. F11 pasa a pantalla completa sin bordes; F11 otra vez vuelve al tamaño de la ventana.
- [ ] C2. Lo mismo con Alt+Enter, con el Alt izquierdo y con el derecho.
- [ ] C3. En el título y en un menú, Alt+Enter **no** confirma ni abre nada; Enter solo sigue
  siendo Start.
- [ ] C4. Con el OSK abierto (el nombre de NEW GAME), Alt+Enter cambia la pantalla y **no**
  termina el nombre; Enter solo lo termina.
- [ ] C5. Con el mouse capturado (clic), F11 cambia la pantalla y la cámara sigue con el mouse;
  F1 lo suelta.

**Widescreen (F25b), con `--aspect 64:27 --scale 3`** (o "21:9 (2560x1080)" en el launcher):
- [ ] C6. La ventana se abre a 1934×816.
- [ ] C7. En una misión (`st07m1` si llegas, o la que tengas a mano) el 3D se ve más ancho y bien
  proporcionado. El HUD sale estirado (el radar ovalado): es lo esperado en W1.
- [ ] C8. El video del título y el cómic de `flashdemo` tienen barras negras a los lados, y al
  terminar el video la imagen se vuelve a estirar.
- [ ] C9. Si el menú del título tiene el video detrás, todo el menú sale con barras: anotarlo (es
  aceptable).
- [ ] C10. Lo mismo con `--render-scale 4` y con softgpu.
- [ ] C11. Capturas locales con `--capture-window 1 --capture <dir>` de un frame de misión y uno de
  video.
- [ ] C12. Si tienes un monitor ultrawide: `--fullscreen 1` llena la pantalla.

**Íconos (F25a), con un mando de verdad:**
- [ ] C13. En el menú de pausa (el mapa), alternar entre el teclado y el mando: el △ cambia en
  cuanto usas el otro (Y en un disco con el de Xbox, CTL con el teclado, △ con el de PlayStation).
- [ ] C14. Con `--render-scale` alto, el ícono se ve pixelado: es lo esperado (se dibuja a 1×).

**Texturas HD (F25c):**
- [ ] C15. Con el pack de B1 en la carpeta por defecto, la textura pintada se ve en la ventana y
  el juego va igual de fluido que sin pack.

## D. El launcher (F25d y los ajustes nuevos)

- [ ] D1. Linux: `LANG=es_ES.UTF-8 mpo_launcher` sale en español y `LANG=en_US.UTF-8` en inglés.
- [ ] D2. Ajustes › Idioma › English: cambia al momento, también el título de la ventana; cerrar y
  abrir: sigue en inglés. Volver a "El del sistema".
- [ ] D3. Ninguna pantalla en español muestra "?" ni cajas: instalar, el navegador de carpetas y la
  ventana de error si sale alguna.
- [ ] D4. Ajustes › Proporción › 21:9 (2560x1080): `settings.toml` dice `aspect = "64:27"` y Jugar
  pasa `--aspect 64:27`. Volver a 16:9 (nativa).
- [ ] D5. Con el mando: recorrer las filas Idioma y Proporción con la cruceta, cambiar con A, y que
  el foco no salte a otro botón.

## E. Windows y la Steam Deck

- [ ] E1. Windows en español: el launcher sale en español, con los acentos bien.
- [ ] E2. Windows: C1 a C4 (F11 y Alt+Enter), y que Alt no haga nada raro.
- [ ] E3. Windows con GPU de verdad: C6 a C8 (widescreen).
- [ ] E4. Steam Deck en modo juego: en qué idioma arranca el launcher (depende de Steam); D5 con
  los controles de la Deck.

## F. La CI

- [ ] F1. Mirar varias corridas del job `Linux gcc asan-ubsan`: ya no tiene que fallar por las fugas
  de lavapipe en los `mpo_vulkan_*` (antes era intermitente). Si en el log sale "Suppressions
  used", la supresión actuó.
- [ ] F2. Los jobs de Windows (MSVC y `windows-llvm-mingw`) compilan el launcher con las cadenas en
  español.

## Si algo falla

- Una repetición que se separa: anotar el primer flip distinto y el comando exacto. Para bisecar,
  cada fase tiene su opción para apagarla: `--prompt-icons off` (F25a), sin `--aspect` (F25b) y
  `--textures none` (F25c).
- Un frame raro en Vulkan: los `MPO_VK_*` de siempre y `MPO_RENDERER=soft` para comparar.
- Si `ninja -n` del paso 0 dice que las etapas se recompilan, no seguir y avisar.
