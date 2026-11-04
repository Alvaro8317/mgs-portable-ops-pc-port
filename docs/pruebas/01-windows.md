<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# 1. Windows (con tu GPU de verdad)

Aquí se prueba lo que necesita la ventana, la GPU y el mando: la pantalla completa, el
widescreen, los íconos que cambian con el dispositivo, las texturas en la ventana y el launcher en
los dos idiomas. Las repeticiones con hashes van aparte, en WSL
([02-wsl-repeticiones.md](02-wsl-repeticiones.md)).

Todo se hace con el ZIP de la release armado desde esta rama, igual que lo haría un jugador. No
hace falta Visual Studio: el launcher compila con el llvm-mingw portátil que baja `setup.py`.

Al lado de cada paso, ✅ o ❌ y una línea si algo sale raro.

## W0. Conseguir el ZIP de esta rama (unos 45 min de la CI)

1. En GitHub: **Actions › Release › Run workflow**, elegir la rama
   `claude/modest-ptolemy-y9sau6` y **Run workflow**. Sin un tag `v*` no publica nada: solo deja
   los archivos en la corrida.
2. Cuando termine, en la corrida, bajar el artefacto **`release`** y sacar de él
   `mpo-pc-port-<versión>-windows-x86_64.zip`.

## W1. Actualizar sin recompilar las etapas

Las etapas que ya compilaste en Windows siguen sirviendo: `setup.py` solo las da por viejas si
cambian los headers del framework, `overlay_module.*`, el compilador (`runtime_key`) o el C++
generado de la etapa, y la ola 7 no tocó nada de eso.

1. Cerrar el launcher y el juego.
2. Descomprimir el ZIP nuevo **en la misma carpeta que el anterior**, reemplazando los archivos.
   En otra carpeta también funciona, pero CMake vuelve a configurar desde cero.
3. Desde el ZIP de `1762fdb` en adelante (con el arreglo del 2026-11-04), el launcher muestra
   **Actualizar** encima de **Jugar** y deja **Jugar** apagado hasta que actualices: pulsarlo hace
   lo mismo que el comando de abajo. Con un ZIP anterior (como el de `01a6447`), el launcher no se
   da cuenta y **Jugar** arranca el `mpo_boot` viejo, que falla con `unknown option
   --fullscreen`; ahí se actualiza desde un `cmd` abierto en la carpeta del ZIP. La ruta del ISO es
   la que el launcher muestra abajo, en **Instalación › ISO**:
   ```bat
   python\python.exe -I source\profiles\mpo\scripts\setup.py install --iso "C:\ruta\a\ULUS-10202.iso"
   ```
   Tiene que decir `disc: already installed`, generar poco o nada (`generate: N corpora to
   generate`) y terminar con `main: mpo_boot ready`. Recompila `mgp_main` y `mpo_boot`, que tarda
   un rato (los archivos del ZIP tienen fecha nueva y Ninja los recompila).
4. Comprobar las etapas:
   ```bat
   python\python.exe -I source\profiles\mpo\scripts\setup.py stages --low-priority
   ```
   La primera línea de cada pasada tiene que decir `92 ready, 0 to compile`. Si dice que hay que
   compilar etapas, cerrar con Ctrl+C y **avisar**. No es grave: las bibliotecas que ya tienes
   siguen en su carpeta y el juego las sigue usando, pero no tendría que pasar.
   - Si `generate` del paso 3 regeneró las 92 etapas, es la misma causa (el `psp_recomp` nuevo no
     salió idéntico al viejo): anotarlo también.
   - [ ] W1 ✅/❌
5. Abrir `mpo_launcher.exe`: tiene que decir que las etapas están compiladas.

Para no tocar tus saves reales mientras pruebas, haz una copia de
`%APPDATA%\mpo-pc-port\ms0` antes de empezar (o sigue el paso 0 de `docs/smoke-test.md`).

## W2. El launcher en español y en inglés (F25d)

- [ ] W2.1. Con Windows en español, el launcher sale en español: los acentos, la ñ y los signos
  ¿ ¡ se ven bien, sin "?" ni cajas.
- [ ] W2.2. **Ajustes › Idioma › English**: cambia al momento, también el título de la ventana.
  Cerrar y abrir: sigue en inglés.
- [ ] W2.3. Volver a **El del sistema**: sale otra vez en español.
- [ ] W2.4. Mirar en español el navegador de carpetas (por ejemplo, **Importar una partida**) y,
  si sale alguna, la ventana de error.
- [ ] W2.5. Con el mando: recorrer las filas **Idioma** y **Proporción** con la cruceta, cambiar
  con A y comprobar que el foco no salta a otro botón.

## W3. Pantalla completa durante el juego (mejoras)

Pulsar **Jugar** con los ajustes por defecto y empezar desde NEW GAME.

- [ ] W3.1. **F11** pasa a pantalla completa sin bordes; F11 otra vez vuelve al tamaño de
  **Ventana**.
- [ ] W3.2. Lo mismo con **Alt+Enter**, con el Alt izquierdo y con el derecho. En Windows, que Alt
  no haga nada raro (ningún menú, ningún sonido).
- [ ] W3.3. En la pantalla de título y en un menú, Alt+Enter **no** confirma ni abre nada; Enter
  solo sigue siendo Start.
- [ ] W3.4. En el teclado en pantalla (el nombre de NEW GAME), Alt+Enter cambia la pantalla y
  **no** termina el nombre; Enter solo lo termina.
- [ ] W3.5. Con el mouse capturado (un clic en la ventana), F11 cambia la pantalla y la cámara
  sigue con el mouse; F1 lo suelta.

## W4. Los íconos que siguen al dispositivo (F25a)

Por defecto están encendidos en la ventana (`--prompt-icons auto`). Por ahora el port solo conoce
un ícono: el △ del mapa del menú de pausa ("△ : FLOOR CHANGE").

- [ ] W4.1. En una misión, abrir la pausa y el mapa usando el **teclado**: el △ es una tecla
  **CTL**.
- [ ] W4.2. Tocar el mando de **Xbox**: el △ pasa a ser una **Y** en un disco, sin cerrar el menú.
- [ ] W4.3. Con un mando de **PlayStation**, si tienes: sale el △ del juego.
- [ ] W4.4. Volver al teclado: vuelve la tecla CTL.
- [ ] W4.5. Los diálogos del sistema (guardar, el teclado en pantalla) también muestran los
  botones del último dispositivo (eso es de F24b, pero conviene verlo junto).

## W5. Widescreen (F25b)

En **Ajustes › Proporción**, elegir **21:9 (2560x1080)** y pulsar **Jugar**.

- [ ] W5.1. `%APPDATA%\mpo-pc-port\settings.toml` dice `aspect = "64:27"`.
- [ ] W5.2. La ventana se abre más ancha (con **Ventana** en 3x, 1934×816).
- [ ] W5.3. En una misión, el 3D se ve más ancho y bien proporcionado (los personajes no salen
  aplastados). El HUD sale estirado, con el radar ovalado: es lo esperado en esta fase.
- [ ] W5.4. El video del título y el cómic del principio (`flashdemo`) salen con **barras negras a
  los lados**, y al terminar el video la imagen vuelve a estirarse.
- [ ] W5.5. Si el menú del título tiene el video detrás, todo el menú sale con barras: anotarlo (es
  aceptable).
- [ ] W5.6. Lo mismo con **Resolución** en 4x.
- [ ] W5.7. Si tienes un monitor ultrawide: con **Pantalla completa**, la imagen llena la pantalla.
- [ ] W5.8. Probar **32:9**, y volver a **16:9 (nativa)**: todo como antes.

## W6. Texturas en la VRAM (F25c)

1. Volcar las texturas mientras juegas un rato en la celda del principio (`st04vo`):
   ```bat
   python\python.exe -I source\profiles\mpo\scripts\setup.py play -- --dump-textures C:\mpo-pruebas\dump
   ```
2. En `C:\mpo-pruebas\dump`, elegir el PNG de una pared o del suelo de la celda, pintarle algo
   encima (sin cambiar el tamaño) y copiarlo, con el `textures.ini` del volcado, a
   `%APPDATA%\mpo-pc-port\textures`.
3. Jugar otra vez, sin opciones.

- [ ] W6.1. En el volcado no hay PNG con la pantalla entera ni con frames de video (mirar los de
  480×272 o de 512 de ancho, si hay).
- [ ] W6.2. La pared pintada se ve en el juego, y el juego va igual de fluido que sin pack.
- [ ] W6.3. Los videos y lo que el juego dibuja él mismo no cambian.
- [ ] W6.4. Al terminar, borrar `%APPDATA%\mpo-pc-port\textures` (o vaciarla): el juego se ve
  como antes.

Las texturas volcadas son arte del juego: no se comparten ni van al repo.

## W7. La CI

- [ ] W7.1. En la corrida de W0, o en el CI de la rama, los jobs de Windows (MSVC y
  `windows-llvm-mingw`) pasan: compilan el launcher con las cadenas en español.

## Si algo falla

- El launcher muestra las últimas líneas; los logs completos están en
  `%LOCALAPPDATA%\mpo-pc-port\install\logs`.
- Para saber de qué fase es, cada una se apaga por separado, por ejemplo:
  ```bat
  python\python.exe -I source\profiles\mpo\scripts\setup.py play -- --prompt-icons off --textures none
  ```
  y la Proporción en 16:9 (nativa).
- Anotar qué pasó, en qué parte del juego y con qué ajustes.

## Resultados (2026-11-04, el usuario, Windows, ZIP de `01a6447`)

- **W1 ❌ → arreglado**: con el ZIP en otra carpeta, **Jugar** arrancaba el `mpo_boot` viejo
  (`unknown option --fullscreen`) y compilar las etapas fallaba en CMake (la caché apuntaba a la
  carpeta anterior). Después de `setup.py install` a mano, `stages` quiso recompilar 90 etapas: el
  `psp_recomp` recompilado desde otra carpeta es otro binario (lleva rutas), y la etapa se daba por
  vieja con todo el `generate.json`. Y una DLL cargada por el juego paró `stages` con
  `WinError 5`. Arreglado en `1762fdb` (el botón **Actualizar**) y en el commit siguiente: la
  etapa se compara por su C++ generado, y una DLL en uso se salta y queda para la próxima vez.
- **La primera misión, a 4K (Resolución 8x, pantalla completa)**: sin bajones de FPS. Los
  personajes son lo que más gana; el entorno (texturas de 2006) pide un pack HD.
- **W4 ✅**: el △ del mapa de pausa cambia en el momento entre △/Y y CTL.
  - El "PRESS SELECT BUTTON FOR HELP SCREEN" del selector de misiones **no** cambia: es texto
    (palabras, §7.4 del spike), no un ícono. Queda en F23+.
- **W3 ⚠️ → arreglado**: F11 y Alt+Enter funcionaban, pero con **Ventana** en 8x la ventana
  (3840×2176) era más grande que la pantalla 4K, y salir de la pantalla completa casi no cambiaba
  nada. Ahora la ventana se ajusta al 90 % del área útil de la pantalla.
- **Fluidez**: a veces se nota un poco de falta de fluidez. El juego corre a **20 fps** en el
  gameplay (la lógica está atada a eso, CLAUDE.md §4); los 60 fps con interpolación están en F23+.
- **Cámara**: el eje Y del stick derecho y del mouse quedan **encendidos por defecto** (pedido por
  el usuario); `vertical = false` en `input.toml` los apaga.
- **La pantalla de título**: el ® del logo se ve borroso a 4K (es arte de 480×272, del juego).
  Queda en F23+.
