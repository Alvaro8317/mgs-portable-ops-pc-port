<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Metal Gear Solid: Portable Ops — port no oficial para PC

[English](README.md)

Un port nativo y no oficial de **Metal Gear Solid: Portable Ops** (PSP, 2006) para **Windows y
Linux** (también la Steam Deck). El código del juego se traduce a C++ y se compila para tu PC, así
que no corre dentro de un emulador.

**Este repositorio no trae nada del juego y el port no se puede jugar por sí solo.** Necesita tu
propia copia del juego, en un archivo ISO. El port se arma en tu computador a partir de ese ISO: el
juego traducido no se descarga de ningún lado y nunca se debe compartir (ver
[Aviso legal](#aviso-legal)).

> **Estado: en desarrollo, todavía sin release.** En Linux, las primeras misiones se juegan desde
> el principio del juego y las 92 etapas compilan. El recorrido completo, en Linux y en Windows,
> sigue en curso. El juego también ha corrido en Windows desde un build de desarrollo. El ZIP de
> Windows y su launcher se compilan y se revisan de forma automática, pero todavía no se han probado
> en el PC de un jugador.

## Contenido

- [Qué necesitas](#qué-necesitas)
- [Instalar en Windows](#instalar-en-windows)
- [Instalar en Linux y Steam Deck](#instalar-en-linux-y-steam-deck)
- [Jugar](#jugar)
- [Controles](#controles)
- [Saves](#saves)
- [Si algo falla](#si-algo-falla)
- [Desinstalar](#desinstalar)
- [Aviso legal](#aviso-legal)
- [Para desarrolladores](#para-desarrolladores)

## Qué necesitas

| | |
|---|---|
| **El juego** | Un ISO de la versión de **Estados Unidos**, **ULUS-10202** (disco versión 1.02), hecho a partir de tu propio UMD. Las demás regiones y versiones se rechazan con un mensaje claro. |
| **Las fuentes de la PSP** | Las fuentes del firmware, `ltn0.pgf` y `jpn0.pgf` (de `flash0:/font` de una PSP). Si tienes PPSSPP instalado, su carpeta `assets/flash0/font` también sirve. Sin ellas el juego corre, pero falta parte del texto. |
| **Espacio en disco** | Unos **15 GB** libres mientras el juego compila, y unos 10 GB después. |
| **Memoria** | 8 GB de RAM o más. |
| **Gráficos** | Una GPU con **Vulkan 1.2** (cualquier GPU de los últimos ~8 años con los drivers al día). |
| **Tiempo** | La primera instalación compila el juego: **de 20 a 40 minutos** en un PC reciente, y más en uno viejo. Después ya puedes jugar, y el resto sigue compilando en segundo plano (ver [Jugar](#jugar)). |
| **Internet** | Solo durante la instalación. Descarga el compilador y algunas librerías con su versión fijada, nunca nada del juego. |

La forma más fácil de poner las fuentes: copia la carpeta `font` del `flash0` de tu PSP (o de
PPSSPP) en una carpeta `flash0` junto a tu ISO, para que quede así:

```
Juegos/
  ULUS-10202.iso
  flash0/
    font/
      ltn0.pgf
      jpn0.pgf
      ...
```

El instalador las encuentra ahí sin que se lo pidas. También puedes elegir la carpeta en el
launcher.

## Instalar en Windows

1. Descarga `mpo-pc-port-<versión>-windows-x86_64.zip` de la página de
   [Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases).
2. Descomprime **todo** el ZIP en una carpeta tuya, **no** dentro de `Program Files`. Por ejemplo,
   `C:\Juegos\mpo-pc-port`.
3. Abre `mpo_launcher.exe`. Si Windows SmartScreen avisa de una aplicación desconocida, elige
   *Más información › Ejecutar de todas formas*. El launcher no está firmado.
4. Elige tu ISO (y la carpeta de las fuentes, si el launcher te la pide) y pulsa **Install**. La
   primera vez descarga el compilador (llvm-mingw, CMake y Ninja, unos 250 MB) en su propia
   carpeta. No necesitas Visual Studio ni permisos de administrador.
5. Cuando diga que el juego está listo, pulsa **Play**.

El launcher se maneja con el mouse, con el teclado o con un mando.

### Si el antivirus borra un archivo

Algunos antivirus, sobre todo **Avast**, marcan parte del compilador como una amenaza y la ponen en
cuarentena (casi siempre `clang-23.exe`). **Es un falso positivo**: el compilador es la release
oficial de [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), y el instalador comprueba su
SHA-256 antes de usarlo. La instalación se para con "the compiler does not run" o "an antivirus
may have removed or blocked it".

Para arreglarlo, restaura el archivo y agrega una excepción para la carpeta del port,
`C:\Users\<tu usuario>\AppData\Local\mpo-pc-port`, para que no borre nada más de lo que
compila:

- **Avast**: en *Protección › Baúl de virus*, restaura el archivo; después, en *Menú ›
  Configuración › General › Excepciones › Agregar excepción*, agrega esa carpeta.
- **Seguridad de Windows** (Microsoft Defender): en *Protección contra virus y amenazas › Historial
  de protección*, permite el archivo; después, en *Administrar la configuración › Exclusiones ›
  Agregar una exclusión › Carpeta*, agrega esa carpeta.

Los nombres de los menús pueden cambiar entre versiones. Después vuelve a pulsar **Install**: vuelve
a descomprimir el compilador solo. Si lo haces antes de la primera instalación, te evitas el
problema.

## Instalar en Linux y Steam Deck

### Con Flatpak (lo recomendado, y la única forma en la Steam Deck)

1. Descarga `mpo-pc-port.flatpak` de la página de
   [Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases).
2. Instálalo y ábrelo:

   ```bash
   flatpak install --user mpo-pc-port.flatpak
   flatpak run io.github.alvaro8317.MpoPcPort
   ```

   También aparece en el menú de aplicaciones. En la Steam Deck, esto se hace en el modo escritorio.
   Después puedes agregarlo a Steam (*Añadir un juego que no es de Steam*) y jugarlo en el modo
   juego con los controles de la Deck.
3. Elige tu ISO, pulsa **Install** y, cuando esté listo, **Play**.

El Flatpak compila el juego con su propio compilador (el runtime `org.freedesktop.Sdk`, que Flatpak
descarga la primera vez), así que no tienes que instalar nada más. Puede leer tu ISO desde tu
carpeta personal, desde `/media`, desde `/run/media` y desde la microSD de la Steam Deck.

### Sin Flatpak (desde el código fuente)

Necesitas algunos paquetes de tu distribución: CMake 3.22 o posterior, Ninja, Clang (o GCC) con
C++20, Python 3.11 o posterior, `make`, `pkg-config`, el loader de Vulkan y los headers de
desarrollo del sonido (ALSA, PulseAudio y PipeWire), de la ventana (X11 y Wayland) y de los mandos
(udev). **Sin los headers del sonido el juego compila, pero no tiene sonido.** En Debian y Ubuntu
(24.04 o posterior):

```bash
sudo apt install cmake ninja-build clang python3 make pkg-config ca-certificates libvulkan1 \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libegl-dev libdecor-0-dev libudev-dev libdbus-1-dev
```

En otras distribuciones, instala los paquetes con los mismos nombres (normalmente terminan en
`-devel` en vez de `-dev`), o usa el Flatpak.

Descarga `mpo-pc-port-<versión>-source.tar.gz` de la página de
[Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases) y descomprímelo.
Después, desde esa carpeta:

```bash
python3 -I profiles/mpo/scripts/setup.py install --iso ~/Juegos/ULUS-10202.iso
python3 -I profiles/mpo/scripts/setup.py play
```

En otra terminal, mientras juegas (o antes), compila las etapas:

```bash
python3 -I profiles/mpo/scripts/setup.py stages --low-priority
```

`setup.py status` dice qué está hecho. Cada paso sigue donde quedó, así que si algo falla o cierras
la terminal, basta con volver a correr el mismo comando.

## Jugar

- **Puedes jugar en cuanto termina la instalación.** El juego está dividido en 92 etapas (las
  misiones, los menús, las escenas), que siguen compilando en segundo plano, en el orden de la
  historia:
  - primero todas en una versión rápida (unos 40 minutos más);
  - después en una versión optimizada (varias horas).

  Una etapa que todavía no está compilada funciona igual, pero más lenta, con bajones de FPS en las
  escenas con mucha acción. El launcher muestra cuántas van. Puedes parar la compilación con
  **Stop**, y la próxima vez sigue donde quedó.
- **El juego va a 20 FPS en el gameplay y a 30 en los menús.** Así era en la PSP, y su lógica está
  atada a esa velocidad. No es un bug.
- **El reclutamiento por AP Scan funciona**, con puntos de acceso Wi-Fi virtuales que cambian cada
  día.
- **El GPS** responde "no hay receptor", como una PSP sin él.
- **El multijugador** (ad hoc y online) todavía no está: el juego no encuentra a nadie alrededor.

## Controles

En los menús del juego se confirma con **Círculo** y se vuelve con **Cruz**, como en la PSP.

### Teclado y mouse

| PSP | Teclado / mouse |
|---|---|
| Stick analógico (moverse) | W A S D |
| Cruceta (la cámara, y los menús) | Flechas |
| Cruz | Espacio |
| Círculo | Clic derecho |
| Cuadrado | Clic izquierdo |
| Triángulo | Ctrl |
| L | Q |
| R | E |
| Start | Esc o Enter |
| Select | Tab |

**El mouse puede girar la cámara**: haz clic en la ventana del juego para capturar el mouse y
muévelo a la izquierda y a la derecha. **F1** (o cambiar a otra ventana) lo suelta.

Cuando el juego te pide escribir algo (un nombre, una contraseña), escríbelo con el teclado.
Retroceso borra, Enter termina y Esc cancela.

### Mando

Funcionan los mandos de Xbox, de PlayStation y casi todos los demás, con la misma distribución que
en PPSSPP:

| PSP | Xbox | PlayStation |
|---|---|---|
| Stick analógico | Stick izquierdo | Stick izquierdo |
| Cruceta (la cámara, y los menús) | Cruceta | Cruceta |
| Cruz | A | Cruz |
| Círculo | B | Círculo |
| Cuadrado | X | Cuadrado |
| Triángulo | Y | Triángulo |
| L | LB o LT | L1 o L2 |
| R | RB o RT | R1 o R2 |
| Start | Start / Menú | Options |
| Select | Back / Vista | Share / Create |

### Cambiar los controles

Copia [`profiles/mpo/config/input.toml`](profiles/mpo/config/input.toml) (en el ZIP, dentro de
`source\`) a:

- Windows: `%APPDATA%\mpo-pc-port\input.toml`
- Linux: `~/.config/mpo-pc-port/input.toml`
- Flatpak: `~/.var/app/io.github.alvaro8317.MpoPcPort/config/mpo-pc-port/input.toml`

Después edítalo. Los comentarios del principio explican cómo. No hace falta reinstalar nada.

## Saves

Tus saves se guardan aparte de la instalación, así que reinstalar o actualizar no los toca:

| | Carpeta de saves |
|---|---|
| Windows | `%APPDATA%\mpo-pc-port\ms0\PSP\SAVEDATA` |
| Linux | `~/.local/share/mpo-pc-port/ms0/PSP/SAVEDATA` |
| Flatpak | `~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/ms0/PSP/SAVEDATA` |

**Traer tus saves de una PSP o de PPSSPP**: en el launcher, pulsa **Import a save** y elige una
carpeta de save (`ULUS10202…`) o una carpeta `SAVEDATA` entera. PPSSPP los guarda en
`Documentos\PPSSPP\PSP\SAVEDATA` en Windows y en `~/.config/ppsspp/PSP/SAVEDATA` en Linux. Una PSP
los guarda en `PSP/SAVEDATA` de su Memory Stick. Si PPSSPP está instalado, el navegador de carpetas
tiene un botón **PPSSPP saves**, y también puedes escribir una ruta o marcar **Show hidden
folders**. Tus archivos originales solo se leen, nunca se cambian. Si el port ya tiene un save con
el mismo nombre, no se reemplaza nada y el launcher te avisa.

**Llevar tus saves a una PSP o a PPSSPP**: pulsa **Export my saves** y elige una carpeta (con el
Flatpak, tu carpeta de Descargas). Los saves exportados van cifrados como los espera una PSP.
Cópialos en la carpeta `SAVEDATA` de PPSSPP o de la PSP.

Sin el launcher, usa `setup.py import-save <carpeta>` y `setup.py export-save <carpeta>` (con
`--overwrite` se reemplaza un save que ya está).

> Haz una copia de tu carpeta de saves antes de probar una versión nueva del port.

## Si algo falla

| Lo que ves | Qué hacer |
|---|---|
| El ISO se rechaza ("Unsupported disc" o "Rejected: this image does not match") | El port solo acepta la versión de Estados Unidos ULUS-10202 (disco 1.02), volcada entera y sin cambios. Comprueba que sea esa versión y no una copia parcheada, traducida o comprimida (CSO). |
| "no PSP firmware fonts found" | Pon las fuentes en una carpeta `flash0/font` junto al ISO (ver [Qué necesitas](#qué-necesitas)) y vuelve a pulsar Install. Solo se rehace ese paso. |
| Windows: "the compiler does not run" o "an antivirus may have removed" un archivo | El antivirus puso en cuarentena parte del compilador. Es un falso positivo: ver [Si el antivirus borra un archivo](#si-el-antivirus-borra-un-archivo). |
| Windows: el juego se para enseguida y el launcher dice "a DLL it needs is missing", o "generated files are missing" | También es el antivirus, esta vez con los archivos del port. Añade la excepción de [Si el antivirus borra un archivo](#si-el-antivirus-borra-un-archivo) y pulsa Install otra vez: solo rehace lo que falta. |
| La instalación se para con un error | El launcher muestra las últimas líneas. Vuelve a pulsar Install: sigue donde quedó, y muchas veces era solo un corte de la red. Los logs completos están en la carpeta `logs` de la carpeta de instalación (ver [Desinstalar](#desinstalar)). |
| El juego se detiene y dice que una función es "unimplemented" | Llegaste a una parte del juego que el port todavía no sabe manejar. Se detiene a propósito, para no arriesgar tu save. Por favor, [abre un issue](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/issues) con las líneas que muestra el launcher y di en qué parte del juego estabas. |
| Pocos FPS en algunas escenas | Puede que esa etapa todavía no esté compilada. Mira el contador del launcher y deja que termine la compilación en segundo plano. |
| No sale la ventana, o sale un error de Vulkan | Actualiza los drivers de la GPU. En WSL o en una máquina virtual muchas veces no hay una GPU de verdad. Ahí, arranca el juego con `MPO_RENDERER=soft` (un renderer por software, más lento). |
| No hay sonido | El juego usa el dispositivo de audio por defecto del sistema. Elígelo antes de abrir el juego. Si lo compilaste desde el código, revisa que estén los paquetes del sonido. |

Cuando reportes un problema, **nunca adjuntes el ISO, la carpeta de instalación ni archivos del
juego**: basta con el texto del error y tu sistema (Windows o Linux, y la GPU).

## Desinstalar

Borra la carpeta del port (en Windows, la del ZIP), o corre
`flatpak uninstall io.github.alvaro8317.MpoPcPort`. Después borra la carpeta de instalación, que
tiene el juego descifrado, el código generado y el juego compilado (unos 10 GB):

- Windows: `%LOCALAPPDATA%\mpo-pc-port\install`
- Linux: `~/.local/share/mpo-pc-port/install`
- Flatpak: `~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/install`

Tus saves están en otra carpeta (ver [Saves](#saves)): bórrala solo si ya no los quieres.

## Aviso legal

- Es un **proyecto de fans**. No lo hacen, ni lo respaldan, ni le dan soporte Konami, Kojima
  Productions ni Sony. *Metal Gear Solid* y *Portable Ops* son marcas de Konami Digital
  Entertainment. *PSP* y *PlayStation* son marcas de Sony Interactive Entertainment.
- **No se distribuye ninguna parte del juego**, ni siquiera traducida o compilada: ni el ISO, ni el
  ejecutable, ni los archivos de las etapas, ni los gráficos, el sonido, el video o el texto, ni
  claves de cifrado, ni archivos del firmware de la PSP. El repositorio y las releases solo traen el
  código propio del port, una lista de direcciones dentro del código del juego (para traducirlo sin
  herramientas de análisis) y herramientas libres de terceros. Cada release se revisa de forma
  automática en busca de archivos del juego.
- **Tienes que tener el juego** y hacer tu ISO a partir de tu propio UMD. El instalador comprueba
  que el ISO sea exactamente el disco 1.02 de Estados Unidos y arma el port a partir de él, solo en
  tu computador.
- Lo que crea la instalación (el juego descifrado, el código generado y el juego compilado) se
  deriva del juego y es para tu uso personal. **Nunca lo compartas.**
- El port es software libre, bajo la [GNU GPL, versión 3 o posterior](LICENSE). Incluye código de
  PPSSPP (GPL-2.0-or-later) y de otros proyectos, que están con sus licencias en
  [`THIRD_PARTY.md`](THIRD_PARTY.md). Viene **sin ninguna garantía**.

## Para desarrolladores

- [`CLAUDE.md`](CLAUDE.md): el enfoque técnico (recompilación estática y un runtime HLE), las
  reglas y las decisiones tomadas.
- [`docs/PLAN.md`](docs/PLAN.md) y [`docs/sessions/`](docs/sessions/): el plan por fases y lo que
  hizo cada fase.
- [`docs/RESEARCH.md`](docs/RESEARCH.md): la investigación, con sus fuentes.
- [`profiles/mpo/README.md`](profiles/mpo/README.md): el perfil del juego y sus herramientas.
- [`docs/analysis/distribution.md`](docs/analysis/distribution.md): cómo funcionan la instalación y
  las releases.
- [`docs/smoke-test.md`](docs/smoke-test.md): la lista que se sigue antes de cada release.
