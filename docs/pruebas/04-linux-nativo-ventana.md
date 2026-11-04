<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 4. Linux nativo: la ventana con la GPU de verdad

Lo mismo que se prueba en Windows ([01-windows.md](01-windows.md)), pero en Linux, más lo que solo
tiene sentido aquí (el idioma por `LANG`, las capas de validación de Vulkan y softgpu contra Vulkan
en la misma máquina). Requiere [03-linux-nativo-preparar.md](03-linux-nativo-preparar.md).

`setup.py` es `python3 -I profiles/mpo/scripts/setup.py`, desde la carpeta del port. Las opciones
de `mpo_boot` van después de `--`.

Al lado de cada paso, ✅ o ❌ y una línea si algo sale raro. Primero con Vulkan (por defecto) y
después repetir C1–C3 y L3 con `MPO_RENDERER=soft`.

## L1. El launcher en los dos idiomas (F25d)

`setup.py` no compila el launcher (lo trae la release de Windows y el Flatpak). Desde el código,
en un build aparte que no toca la instalación:

```bash
cmake -S . -B build-launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-launcher --target mpo_launcher
find build-launcher -name mpo_launcher -type f      # la ruta del ejecutable
```

Encuentra solo el `setup.py` de esta carpeta y usa la misma instalación que `setup.py`.

- [ ] L1.1. `LANG=es_ES.UTF-8 mpo_launcher` sale en español y `LANG=en_US.UTF-8 mpo_launcher` en
  inglés. Sin `LANG` especial, sale en el idioma del sistema.
- [ ] L1.2. **Ajustes › Idioma** cambia al momento, también el título; cerrar y abrir lo mantiene.
- [ ] L1.3. Ningún "?" ni caja en español (instalar, el navegador, la ventana de error).
- [ ] L1.4. Con el mando: las filas **Idioma** y **Proporción** se recorren con la cruceta y se
  cambian con A sin que el foco salte.

## L2. Pantalla completa (mejoras)

- [ ] L2.1. F11 y Alt+Enter (los dos Alt) alternan la pantalla completa sin bordes.
- [ ] L2.2. Alt+Enter no confirma nada en el título ni en los menús; Enter solo es Start.
- [ ] L2.3. Con el teclado en pantalla (NEW GAME), Alt+Enter no termina el nombre.
- [ ] L2.4. Con el mouse capturado, F11 cambia la pantalla y la cámara sigue con el mouse; F1 lo
  suelta.
- [ ] L2.5. En X11 y en Wayland, si tu escritorio tiene los dos (en la pantalla de inicio de
  sesión se elige).

## L3. Widescreen (F25b)

```bash
setup.py play -- --aspect 64:27 --scale 3
```

- [ ] L3.1. La ventana se abre a 1934×816.
- [ ] L3.2. En una misión, el 3D más ancho y bien proporcionado; el HUD estirado (es lo esperado).
- [ ] L3.3. El video del título y el cómic de `flashdemo` con barras negras a los lados, y después
  la imagen estirada otra vez.
- [ ] L3.4. Si el menú del título tiene el video detrás, todo el menú sale con barras: anotarlo.
- [ ] L3.5. Lo mismo con `--render-scale 4`, y con `MPO_RENDERER=soft`.
- [ ] L3.6. Capturas locales de un frame de misión y uno de video:
  `setup.py play -- --aspect 64:27 --capture-window 1 --capture ~/mpo-capturas`.
- [ ] L3.7. Desde el launcher: **Ajustes › Proporción › 21:9 (2560x1080)** escribe
  `aspect = "64:27"` en `~/.config/mpo-pc-port/settings.toml`, y **Jugar** lo usa.

## L4. Íconos con mandos reales (F25a)

- [ ] L4.1. En la pausa, el mapa: con el teclado, el △ es una tecla CTL; con un mando de Xbox, una
  Y en un disco; con uno de PlayStation, el △ del juego. Cambia en cuanto usas el otro.
- [ ] L4.2. Con `--render-scale 4`, el ícono se ve pixelado: es lo esperado (se dibuja a 1×).
- [ ] L4.3. Con `--prompt-icons off`, siempre el △ del juego.

## L5. Texturas en la VRAM (F25c)

Como W6 de Windows, con la carpeta de Linux:

```bash
setup.py play -- --dump-textures ~/mpo-pruebas/dump
# pintar una pared de la celda y copiarla, con el textures.ini, a:
mkdir -p ~/.local/share/mpo-pc-port/textures
```

- [ ] L5.1. La pared pintada se ve, con Vulkan y con softgpu.
- [ ] L5.2. Va igual de fluido que sin pack (mirar los FPS o si hay tirones al entrar a la celda).
- [ ] L5.3. Los videos no cambian.
- [ ] L5.4. Vaciar `~/.local/share/mpo-pc-port/textures`: todo como antes.

## L6. Vulkan sin errores

- [ ] L6.1. Una partida corta con las capas de validación:
  ```bash
  MPO_VK_VALIDATION=1 setup.py play -- --aspect 64:27
  ```
  Pasar por el título, un video y la celda, con la pausa abierta. En la salida, **0 errores de
  validación**.

## Si algo falla

- Las mismas opciones para bisecar que en Windows: `--prompt-icons off`, sin `--aspect`,
  `--textures none`, y `MPO_RENDERER=soft` para saber si es de Vulkan.
- Si una prueba en la ventana falla, repetir la grabación correspondiente en WSL
  ([02-wsl-repeticiones.md](02-wsl-repeticiones.md)) dice si el problema está en la lógica o solo
  en cómo se presenta.
