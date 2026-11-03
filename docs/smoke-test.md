<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Smoke test manual (antes de cada release)

Es la lista corta de `CLAUDE.md` §7 (F22c). Se sigue con el paquete de la release (el ZIP de
Windows, el Flatpak o el tarball del código) y **solo con lo que dice el `README.md`**. Lo que haya
que hacer fuera del README es un fallo del README.

Hay dos reglas:

- **Nunca contra la carpeta de saves real.** El juego y el launcher corren con las carpetas de datos
  apuntando a una carpeta de prueba.
- **Los saves se hashean antes y después** con `packaging/hash_saves.py`: los reales (para probar
  que no se tocaron) y los de origen de un import (PPSSPP o una PSP, que solo se leen).

El resultado de cada pasada se anota en la nota de sesión de la fase o en el borrador de la
release: la fecha, el paquete y su SHA-256, el sistema y la GPU, y cada paso con ✅ o ❌.

## 0. Preparar

Se elige una carpeta de prueba vacía, por ejemplo `~/mpo-smoke` o `C:\mpo-smoke`.

**Linux** (el tarball o el árbol). Todo lo del port se guarda bajo `XDG_DATA_HOME` y
`XDG_CONFIG_HOME`:

```bash
export SMOKE=~/mpo-smoke
export XDG_DATA_HOME=$SMOKE/data XDG_CONFIG_HOME=$SMOKE/config
python3 -I packaging/hash_saves.py snapshot ~/.local/share/mpo-pc-port/ms0 $SMOKE/real-saves.json
```

**Flatpak**. Las carpetas del sandbox se pueden cambiar con `--env` [?, sin probar]. El Flatpak
solo puede escribir en su carpeta (`~/.var/app/…`) y en Descargas, así que la carpeta de prueba va
en Descargas:

```bash
export SMOKE=$(xdg-user-dir DOWNLOAD)/mpo-smoke
python3 -I packaging/hash_saves.py snapshot \
  ~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/ms0 $SMOKE/real-saves.json
flatpak run --env=XDG_DATA_HOME=$SMOKE/data --env=XDG_CONFIG_HOME=$SMOKE/config \
  io.github.alvaro8317.MpoPcPort
```

`hash_saves.py` está en el tarball del código (o en el repo). En la Steam Deck, el Python del
sistema sirve.

**Windows** (el ZIP). En un `cmd` abierto en la carpeta del ZIP:

```bat
set SMOKE=C:\mpo-smoke
python\python.exe -I source\packaging\hash_saves.py snapshot "%APPDATA%\mpo-pc-port\ms0" %SMOKE%\real-saves.json
set APPDATA=%SMOKE%\appdata
set LOCALAPPDATA=%SMOKE%\localappdata
mpo_launcher.exe
```

El launcher (y el juego, que hereda las variables) deja la instalación en
`%SMOKE%\localappdata\mpo-pc-port\install` y los saves en `%SMOKE%\appdata\mpo-pc-port\ms0`.

Si se va a probar el import, primero se hashea el save de origen:

```bash
python3 -I packaging/hash_saves.py snapshot <la carpeta SAVEDATA de PPSSPP> $SMOKE/ppsspp-saves.json
```

## 1. La lista

| # | Paso | Qué tiene que pasar |
|---|---|---|
| 1 | Abrir el launcher | La ventana sale con el aviso "Not affiliated with Konami". Se maneja con el mouse, con el teclado y con un mando. |
| 2 | Elegir el ISO e **Install** | Muestra cada paso con su progreso. Se anota cuánto tardó (en Windows, también la descarga del toolchain). |
| 3 | Un ISO que no es (otro archivo o una copia renombrada) | Se rechaza con un mensaje claro, sin dejar nada a medias. |
| 4 | **Play** | Sale el logo y el **video del título con sonido** (FFmpeg y ATRAC). |
| 5 | "Checking Memory Stick" | Se ve el texto (las fuentes del firmware). |
| 6 | **NEW GAME** | Llega a la celda (`st04vo`). El juego va a 20 FPS en el gameplay y a 30 en los menús. |
| 7 | Controles | Se mueve con WASD y con el stick, la cámara con las flechas y con la cruceta; un clic captura el mouse para la cámara y F1 lo suelta. Círculo confirma y Cruz vuelve en los menús. |
| 8 | Guardar en el juego | El save aparece en la carpeta de prueba (`…/ms0/PSP/SAVEDATA/ULUS10202…`), no en la real. |
| 9 | Cerrar y **LOAD GAME** | Carga el save del paso 8. |
| 10 | Compilación de las etapas | El contador del launcher sube mientras se juega. **Stop** para después de la etapa en curso, y al volver a abrir el launcher sigue donde quedó. |
| 11 | **Import a save** (de PPSSPP o de una PSP) | El botón "PPSSPP saves" lleva a la carpeta (si PPSSPP está instalado). El save importado carga con LOAD GAME. |
| 12 | **Export my saves** | Deja los saves cifrados en la carpeta elegida (con el Flatpak, en Descargas). Si se puede, cargarlos en PPSSPP. |
| 13 | AP Scan (si se llega a `briefing` › RECRUIT) | Encuentra APs virtuales y se recluta un soldado sin cuelgues. |
| 14 | Cerrar el juego y el launcher | No quedan procesos colgados. |

Opcional, si hay tiempo: jugar una misión entera (`st02`) con las etapas todavía en el intérprete
y después compiladas.

## 2. Comprobar los saves

Linux:

```bash
python3 -I packaging/hash_saves.py compare $SMOKE/real-saves.json ~/.local/share/mpo-pc-port/ms0
python3 -I packaging/hash_saves.py compare $SMOKE/ppsspp-saves.json <la carpeta SAVEDATA de PPSSPP>
```

Windows (en un `cmd` nuevo, sin las variables del paso 0):

```bat
python\python.exe -I source\packaging\hash_saves.py compare C:\mpo-smoke\real-saves.json "%APPDATA%\mpo-pc-port\ms0"
```

Las dos tienen que terminar en `unchanged`. Si algo cambió, la release no sale: se anota qué archivo
cambió y se busca por qué.

## 3. Limpiar

Se borra la carpeta de prueba. Tiene el juego descifrado y compilado, así que **no se comparte ni
se sube a ningún lado**.
