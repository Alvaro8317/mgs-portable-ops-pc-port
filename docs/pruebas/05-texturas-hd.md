<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 5. Un pack de texturas HD, paso a paso (Windows)

Prueba F25c, F26a y F26b de punta a punta: volcar las texturas de la primera misión, escalar las
más usadas con Real-ESRGAN, comprobar el pack y jugar con él. Se hace en Windows, con la GPU de
verdad.

Las texturas volcadas y el pack escalado salen del arte del juego: **se quedan en tu PC**, no se
comparten ni van al repo (CLAUDE.md §1).

Al lado de cada paso, ✅ o ❌ y una línea si algo sale raro.

## 0. Antes de empezar

1. El ZIP nuevo de la rama, del commit `830ed5a` o posterior (**Actions › Release**, artefacto
   `release`). Descomprímelo y abre su `mpo_launcher.exe`.
2. Si sale **Actualizar**, púlsalo y espera a que termine.
   - [ ] 0.1. Después, **Jugar** funciona.
3. Abre un `cmd` en la carpeta del ZIP (la que tiene `mpo_launcher.exe`, `python\` y `source\`).
   Todos los comandos de abajo se escriben ahí. Uso `C:\mpo-hd` como carpeta de trabajo:
   ```bat
   mkdir C:\mpo-hd
   ```

## 1. Volcar las texturas

El volcado se hace **sin pack** (`--textures none`), para que salgan las texturas originales:

```bat
python\python.exe -I source\profiles\mpo\scripts\setup.py play -- --textures none --dump-textures C:\mpo-hd\dump
```

Juega desde NEW GAME (o carga tu save) por la primera misión: la celda, los pasillos, el exterior,
el códec y el menú de pausa. Todo lo que veas se vuelca. Después cierra el juego.

- [ ] 1.1. En `C:\mpo-hd\dump` hay PNG, un `textures.ini` y un `dump.csv`.
- [ ] 1.2. Abre `dump.csv` (con el Bloc de notas o Excel): la columna `first_stage` tiene nombres
  de etapa (`st04vo`, `st02`…). Si está casi vacía, avísame.
- [ ] 1.3. Entre los PNG no hay ninguno con la pantalla entera ni con un frame de video (mira los
  más grandes).
- [ ] 1.4. Si vuelves a jugar con el mismo comando, los números de `uses` y `frames` crecen y
  `first_stage` no cambia (los volcados se suman).

## 2. Ver por dónde empezar

```bat
python\python.exe -I source\profiles\mpo\tools\texture_pack.py report C:\mpo-hd\dump --limit 40 > C:\mpo-hd\reporte.txt
python\python.exe -I source\profiles\mpo\tools\texture_pack.py report C:\mpo-hd\dump --sort stage > C:\mpo-hd\reporte-etapas.txt
```

- [ ] 2.1. En `reporte.txt`, arriba están las texturas que más se dibujan. Abre algunos de esos PNG:
  tendrían que ser paredes, suelos y el HUD.
- [ ] 2.2. `reporte-etapas.txt` agrupa por etapa, en el orden de la historia.

## 3. Instalar Real-ESRGAN (una vez)

1. Baja `realesrgan-ncnn-vulkan-…-windows.zip` de
   <https://github.com/xinntao/Real-ESRGAN/releases> (en "Assets").
2. Descomprímelo en `C:\mpo-hd\realesrgan`. Tiene que quedar
   `C:\mpo-hd\realesrgan\realesrgan-ncnn-vulkan.exe` y una carpeta `models`.

El port no trae el escalador ni sus modelos: lo instalas tú.

## 4. Escalar las 100 texturas más usadas

```bat
python\python.exe -I source\profiles\mpo\tools\texture_pack.py upscale C:\mpo-hd\dump C:\mpo-hd\pack --scale 4 --top 100 --realesrgan C:\mpo-hd\realesrgan\realesrgan-ncnn-vulkan.exe
```

Usa la GPU y puede tardar unos minutos.

- [ ] 4.1. Termina sin errores, y en `C:\mpo-hd\pack` hay hasta 100 PNG (4 veces más grandes que
  los originales) y un `textures.ini`.
- [ ] 4.2. Abre algunos: se ven más nítidos, y los que tenían transparencia (rejas, plantas, el
  HUD) la conservan.
- [ ] 4.3. Si lo corres otra vez, salta los PNG que ya hizo (para seguir otro día). `--overwrite`
  los rehace.

## 5. Comprobar el pack

```bat
python\python.exe -I source\profiles\mpo\tools\texture_pack.py check C:\mpo-hd\pack C:\mpo-hd\dump
```

- [ ] 5.1. Dice **0 errores**.

## 6. Poner el pack

Cópialo a la carpeta de texturas del port:

```bat
mkdir "%APPDATA%\mpo-pc-port\textures"
xcopy /E /Y C:\mpo-hd\pack "%APPDATA%\mpo-pc-port\textures\"
```

- [ ] 6.1. En el launcher, **Ajustes › Pack de texturas** está en **Usarlo si hay uno** y debajo
  dice "Hay un pack en …\mpo-pc-port\textures".

## 7. Jugar con el pack

Pulsa **Jugar** (con Vulkan) y ve a la primera misión.

- [ ] 7.1. **Se ve**: las paredes y suelos escalados se ven más nítidos, sobre todo con
  **Resolución** en 4x u 8x.
- [ ] 7.2. **Sin parpadeo de lejos**: mira una pared o un suelo escalado a lo lejos y muévete. No
  tiene que brillar ni parpadear. Para comparar, arranca una vez sin los mipmaps:
  ```bat
  python\python.exe -I source\profiles\mpo\scripts\setup.py play -- --textures-mipmaps 0
  ```
  Ahí sí debería parpadear un poco (es el control).
- [ ] 7.3. **Sin tirones**: al entrar a una etapa nueva no hay saltos. Puede que un instante se vea
  la textura original antes de la HD: es lo esperado (se carga en segundo plano).
- [ ] 7.4. **Recargar con F5**: con el juego abierto, abre un PNG del pack en
  `%APPDATA%\mpo-pc-port\textures`, píntale algo encima, guárdalo y aprieta **F5** en el juego.
  Tiene que cambiar en el momento. F5 no hace nada en el juego mismo.
- [ ] 7.5. **Apagarlo**: cierra el juego, pon **Ajustes › Pack de texturas › Las texturas del
  juego** y juega: se ve como el original. Vuelve a **Usarlo si hay uno**.
- [ ] 7.6. Los videos (título, cómics) y el menú de pausa se ven como siempre.

## 8. Opcional: más texturas

- Las 500 más usadas: lo mismo que el paso 4 con `--top 500`.
- Una etapa entera: `--stage st04vo`.
- Las que se usan mucho: `--min-uses 1000`.
- Otro modelo (más rápido, para 2x o 3x): `--model realesr-animevideov3 --scale 2`.

Después, otra vez `check` (paso 5) y copiar (paso 6), y en el juego **F5** para verlas sin cerrarlo.

## Qué contarme

- Los ✅ y ❌ de arriba.
- Si se nota la diferencia, qué texturas valen más la pena y cuáles salen mal (Real-ESRGAN a veces
  inventa detalle en el texto o en texturas muy pequeñas).
- Si 7.3 tiene tirones: en qué parte, y las líneas `textures:` del final de la salida del juego
  (con `setup.py play` en el `cmd`, salen ahí al cerrar).
