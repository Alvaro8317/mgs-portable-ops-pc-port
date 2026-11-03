# Widescreen y ultrawide con el HUD ajustado (spike, ola 6)

Spike de investigación (sin código de producción) para el punto de M7+ "Widescreen y ultrawide con
el HUD ajustado (CWCheat `0x201E9728` como pista)" de `docs/PLAN.md`. Responde qué hace el cheat,
qué pasa con la escena, el HUD, los menús, el video y el radar al cambiar la proyección, qué le
toca al renderer y cómo se haría en el port.

Etiquetas: **[V]** verificado en el binario o en el juego, **[S]** fuente externa con enlace,
**[?]** sin verificar.

## 0. Resumen

- **16:9 ya es el formato de la PSP.** 480 × 272 es 1,765:1 y 16:9 es 1,778:1: un 0,7 % de
  diferencia. El código "16:9" del cheat escribe el mismo valor que trae el juego (1,0) [V]. En
  un monitor 16:9 no hay nada que hacer: se presenta el 480 × 272 a pantalla completa (estirar un
  0,7 % no se nota). **El trabajo de verdad es el ultrawide** (21:9, 32:9).
- **El cheat cambia una sola constante**: el float de `.rodata` en `0x089E9728` (no en
  `0x089ED728`, como dicen `RESEARCH.md` y `mgp_main.md` §6.2; ver §2.1), que el juego usa como
  **escala x de las cuatro matrices de proyección** de la cámara. Con 0,71 o 0,5 la escena 3D se
  ve más ancha (Hor+) comprimida en los 480 píxeles, y con "Stretch" de PPSSPP queda bien en
  21:9 o 32:9 [V].
- **El frustum del juego se ensancha solo**: los 6 planos de culling de la CPU salen de la misma
  matriz [V]. No hay objetos que desaparezcan en los bordes [V], pero el juego dibuja un 30 % más
  (draws) [V].
- **El HUD, los menús, el codec y el radar no cambian** con la proyección [V]: quedan en
  480 × 272 y, al estirar, salen estirados (el radar se vuelve un óvalo). El video (PMF) son
  frames de 480 × 272 que escribe la CPU.
- **La lógica no cambia**: una misión entera repetida con 21:9 y con 32:9 juega las 8151
  muestras igual y deja **el mismo save byte a byte** [V]. Los hashes de los frames 3D cambian (es
  lo esperado) y también algunos 2D posteriores (animaciones del codec y del briefing) [V], por
  una causa sin aclarar [?] (§5.3).
- **Recomendación**: W1 = constante en el arranque + presentación estirada (como PPSSPP), una
  sesión corta; W2 = HUD en una zona 16:9 centrada dentro del renderer Vulkan, con el 3D y los
  fondos a todo el ancho; W3 (anclar cada elemento del HUD a su borde) solo si hace falta, porque
  es RE por elemento. Detalle en §7.

## 1. Método (reproducible)

Todo se hizo con datos locales del checkout principal (`<main>` =
`/home/alvaro8317/dev/games/mgs-portable-ops-pc-port`), sin compilar nada. Las salidas están en
`<main>/analysis/spike-widescreen/` (ignorado por git).

- **Base de datos de cheats**: `cheat.db` de
  [Saramagrean/CWCheat-Database-Plus-](https://github.com/Saramagrean/CWCheat-Database-Plus-) en
  el commit `fe33256` (2026-11-03), SHA-256 `250fbc67…`, bloque `_S ULUS-10202` [S].
- **El binario**: `profiles/mpo/tools/widescreen.py` (nuevo, solo lectura salvo `patch`, que
  escribe una copia local):

  ```sh
  T=profiles/mpo/tools/widescreen.py
  python -I $T check game/elf/mgp_main.elf      # valor, SHA-256 y dónde se carga
  python -I $T value 16/9 21/9 2560/1080 32/9   # el valor para cada aspecto
  python -I $T patch game/elf/mgp_main.elf 2560/1080 analysis/spike-widescreen/elf/wide.elf
  ```

- **Ghidra** (headless, `-readOnly -noanalysis` sobre `analysis/ghidra/mpo.gpr`, programa
  `mgp_main`): un script de una vez, `analysis/spike-widescreen/gs/WsDump.java` (no va a git),
  que descompila funciones, lista referencias a una dirección y los llamadores de una función:

  ```sh
  JAVA_HOME=~/tools/jdk-21.0.13+9 ~/tools/ghidra_12.1.3_PUBLIC/support/analyzeHeadless \
      analysis/ghidra mpo -process mgp_main -readOnly -noanalysis \
      -scriptPath analysis/spike-widescreen/gs -postScript WsDump.java \
      d:08845124 d:08859DE0 i:08859DE0 r:089E9728 c:08845124
  ```

- **En el juego**: `analysis/spike-widescreen/run.sh <nombre> <elf> 136000000 [opciones]`
  repite `analysis/f21/tramo3.input` (título, briefing, la misión `st07m1` entera con un save, y
  vuelta al briefing) con el `mpo_boot` de `build-f09-release` sin recompilar: softgpu,
  `--interpreter 0`, las 92 etapas de `build-f09-release/overlays`, `--audio 0`, `TZ='<-05>5'`,
  una copia de `tramo3.input.ms0` con `cp -a` y `--capture-every 60` (135 capturas). Es el
  procedimiento de F23b (`analysis/f23b/run.sh`). El ELF parcheado se pasa con `--elf`; el juego
  lee la constante de la memoria con `lwc1`, así que el código recompilado la ve sin regenerar
  nada.
  - Las capturas se comparan con `profiles/mpo/tools/capture_compare.py --max-pixels 0
    --max-delta 0` y los saves con `diff -r` de las `ms0` al terminar.

| Run | ELF (`0x089E9728`) | Frame checks | Draws | Comandos GE | Save al final |
|---|---|---|---|---|---|
| `base` | original (1,0) | **135/135** | 1 264 647 | 45,3 M | referencia |
| `base101` | original, `--us-per-preempt 101` (control de timing) | **135/135**; capturas idénticas a `base` | — | — | idéntico |
| `c071` | 0,71 (el código 21:9 del cheat) | 85 iguales, 50 distintas (desde el flip 3600) | 1 644 407 (+30 %) | 63,4 M (+40 %) | **idéntico** |
| `c050` | 0,5 (el código 32:9) | 83 iguales, 52 distintas (desde el flip 3600) | 1 592 379 (+26 %) | 65,0 M (+44 %) | **idéntico** |

Los tres runs reproducen las 8151 muestras de la grabación, sin errores del GE ni `runtime stop`.
Cada uno tarda unos 55 s de host.

## 2. El cheat y lo que parchea

### 2.1 Los códigos [S] y la dirección [V]

En `cheat.db`, bajo "Widescreen Hack Cheats", con la nota "Change Display Layout Editor Setting To
'Stretch' Then Enable Widescreen Hack Code" [S]:

| Código | Escritura | Valor | Aspecto que corresponde (480/272 ÷ valor) |
|---|---|---|---|
| Screen (16:9) PSP | `0x201E9728 0x3F800000` | 1,0 | 1,765:1 (el de la PSP: **no cambia nada**) |
| Screen (21:9) UltraWide | `0x201E9728 0x3F35C28F` | 0,71 | 2,49:1 (algo más ancho que 21:9 = 2,33 o 2560 × 1080 = 2,37) |
| Screen (32:9) SuperWide | `0x201E9728 0x3F000000` | 0,5 | 3,53:1 (32:9 = 3,56) |

- Un código CWCheat `0x2XXXXXXX` escribe 32 bits en `0x08800000 + XXXXXXX`, igual que el del
  divisor de FPS (`0x201F5210` → `0x089F5210`, verificado en F03). **La dirección es
  `0x089E9728`** [V]. `RESEARCH.md` ("Timing y FPS") y `mgp_main.md` §6.2 la apuntan como
  `0x089ED728` (un 9 cambiado por una D): ahí hay `1820.0`, y su única referencia (`0x08926E5C`)
  no tiene que ver. Hay que corregirlo en esos dos documentos.
- `0x089E9728` está en `.rodata` (`0x089DB730`–`0x089F2D14`, segmento escribible) y vale
  `0x3F800000` (1,0) en el `mgp_main` del disco (SHA-256 `ca700ea5…`) [V].
- **Se carga en un solo sitio** de todo `.text`: `lwc1 $f20` en `0x0884519C` [V] (`widescreen.py
  check`; Ghidra da la misma única referencia). Las etapas no se revisaron [?], pero sería raro
  que una etapa leyera una constante de `.rodata` de `mgp_main`.
- Los valores vecinos (`0x089E9720`…`3C`): 50,0; 3,0; **1,0**; −1,0; 150000,0; 2,0; 1000,0; 2,0.
  Son las constantes de la función de abajo.
- Para los aspectos de verdad (`widescreen.py value`): 2560 × 1080 → 0,744485
  (`0x3F3E9697`), 3440 × 1440 → 0,738714, 21:9 → 0,756303, 32:9 → 0,496324.

### 2.2 Qué es la constante [V]

`0x08845124` es la **puesta al día de la cámara**. `0x088188D4` la llama con la cámara principal,
`0x08A79C60` (una estructura de `0x3B0` bytes; hay un arreglo de ellas, `0x0896010C` indexa de
`0xEC` palabras en `0xEC` palabras):

1. Copia la vista (4 × 4) en `cam+0x00`.
2. Lee la constante una vez (`$f20 = 1.0`) y `−1.0` (`$f22`), y llama **cuatro veces** a
   `0x08859DE0`, el constructor de la proyección, para `cam+0x80`, `+0x360`, `+0x100` y `+0x270`
   (las variantes de near/far: la normal, una con far 150000 y dos con el near desplazado 2 y
   1000).
3. Multiplica cada proyección por la vista (`vmmul.q`): `cam+0xC0`, `+0x140` y `+0x2B0`.
4. **Saca los 6 planos del frustum** (`cam+0x2F0`…) de las filas de la matriz combinada de
   `cam+0xC0` (fila 3 ± fila 0, ± fila 1, ± fila 2, normalizados con `vrsq`): el método de
   Gribb-Hartmann.

`0x08859DE0(out=a0, f12, f13, f14, f15, f16, f17, f18, f19, far=stack)` construye
`out = B · A` con dos matrices de VFPU (`vmidt.q` y `vmmul.q`):

- `A` es diagonal con `f12` en x y en y (`f12` = `cam+0x1C8`, la escala del FOV) más las dos
  constantes de `0x089E9A30/34` en la columna de w.
- `B` lleva `f13` en `[0][0]` (**escala x**), `f14` en `[1][1]` (el aspecto
  `cam+0x1B0 / cam+0x1B4`, ancho y alto como enteros), `f15`/`f16` de desplazamiento (0), y el
  rango de profundidad: `[2][2] = far·near·(f18 − f17)/(far − near)` y
  `[3][2] = (f17·far − f18·near)/(far − near)`.
- En las cuatro llamadas, `f13 = f17 = $f20` (la constante) y `f18 = −1`. El compilador juntó los
  dos 1,0 en uno, así que **el cheat cambia a la vez la escala x y el extremo lejano del rango de
  profundidad** (de [−1, 1] a [−1, 0,71]). Lo segundo solo comprime la profundidad (el orden se
  mantiene) y alarga un poco el far efectivo. En las capturas no se ve ningún fallo de
  profundidad [V], pero un arreglo limpio solo debería tocar `f13` (§6).
- El FOV es **horizontal**: x = `f12·C`, y = `f12·aspecto`. Con `C < 1` se ve más a los lados y lo
  vertical no cambia (Hor+).

Otras piezas de la cámara:

- `0x08844FBC(cam, 10000, 50000, near, far)` (por `0x08845010`) fija el rango del viewport en Z
  (`0x2710`–`0xC350`, el `MINZ`/`MAXZ` de F05) y near/far en `cam+0x1C0/0x1C4`.
- **`0x0881877C` proyecta un punto 3D a la pantalla**: usa la matriz combinada `cam+0xC0`, dice si
  cae dentro del frustum (|x|, |y|, |z| ≤ |w|) y devuelve x = (0,5·x/w + 0,5)·`cam+0x1B0` y y =
  (0,5 − 0,5·y/w)·`cam+0x1B4`. La llaman 6 funciones (`0x0880D9A0`, `0x0880FCF4`, `0x0882C324`,
  `0x088ACBB0`, `0x08976E64`, `0x08817E9C`). Es lo que pone en pantalla las cosas 2D que siguen a
  algo del 3D (marcas sobre personajes, iconos de objetivo) [?: no se identificó cuál es cuál].

## 3. Qué cambia al tocar solo la proyección

Comparando `c071` y `c050` con `base` [V] (las capturas son de VRAM, 480 × 272):

| Parte | Con la constante cambiada | Al estirar a 21:9 / 32:9 |
|---|---|---|
| Escena 3D (`st07m1`) | Se ve más a los lados, comprimida en 480 px (el camión de la izquierda entra entero, el pasillo se ve más ancho). | **Correcta** |
| Culling de la CPU | Se ensancha con la matriz (§2.2). No hay objetos que falten en los bordes; el juego manda un 26-30 % más de draws y un 40-44 % más de comandos. | Correcto |
| Culling de patches del GE (`PATCHCULLENABLE`) | Lo hace el GE con la matriz que recibe | Correcto |
| HUD del juego: camuflaje/arma arriba a la izquierda, radar arriba a la derecha, vida y nombre abajo a la izquierda | **Idéntico píxel a píxel** a `base` en los frames donde el 3D se ve | Estirado ×1,4 o ×2: el radar es un óvalo |
| Codec, briefing, menús, diálogos de guardado (PPGe) | Mismo dibujo (las diferencias son de animación, §5.3) | Estirados |
| Video (PMF del título, cómics) | Lo escribe la CPU en el framebuffer (F11b), no pasa por el GE | Estirado |
| Marcas 2D sobre el 3D (`0x0881877C`) | Siguen al objeto en el espacio comprimido | Bien colocadas, pero estiradas |

- **El HUD no usa estas proyecciones** [V]: no cambia ni un píxel. Según F05, el 2D del juego son
  `TRIANGLE_FAN` en modo through (442 draws en los dumps, "HUD y 2D"), `RECTANGLES` through (los
  clears y rectángulos sin textura) y el radar, `LINE_STRIP` through con posición float [V en F05].
  F05 también anota "parte del HUD" con depth clamp, y hay un `RECTANGLES` transform por dump de
  misión: si alguna pieza del HUD va en modo transform con una matriz propia (ortográfica o la de
  `0x0896010C`, que usa `−ancho·FOV`), se estiraría con el 3D en el renderer [?].
- **Dónde salen las posiciones del HUD**: los layouts son `lay_<nombre>.la2` en los DAR de las
  etapas, con su `lay_<nombre>_0.txp` (ver `button-prompts.md` §2.1) [V]. Las coordenadas de
  pantalla de cada elemento vienen de la `.la2` (posición fija en 480 × 272) [?: no se decodificó
  el formato `.la2` en este spike]. El texto del codec y de los diálogos va glifo por glifo con la
  fuente del GCX (`button-prompts.md`) [V]. No hay ningún ancla izquierda/derecha/centro explícita
  a la vista: hasta leer la `.la2`, la posición es un par x, y [?].

## 4. El render target

- **La VRAM no tiene sitio para un framebuffer más ancho** [V en F05]: el juego usa dos colores
  5551 de stride 512 (`0x000000`, `0x044000`) y el Z en `0x088000`, hasta `0x0CBFFF`, y el resto de
  la VRAM lo usan las texturas (3162 de 3841 draws texturizan desde VRAM). El GE no puede dibujar
  más allá de x = 511 en ese stride. **Un juego más ancho de verdad no existe**: el ancho extra
  tiene que salir del renderer.
- **Software (softgpu)**: se queda en 480 × 272, como referencia. Con la constante cambiada dibuja
  la imagen comprimida, exactamente lo que dibuja la PSP con el cheat, y el presentador la estira
  (lo de PPSSPP). Es la "verdad" de la geometría, no del HUD.
- **Vulkan: un target virtual más ancho** encaja con lo que dejó F23b:
  - hoy los vértices y el scissor van en píxeles del GE, y la imagen mide N veces eso;
  - con un factor horizontal `k` = aspecto / (480/272), la imagen mediría `512·N·k × 512·N` y el
    `VkViewport` x sería `N·k` veces el de píxeles del GE (y `N` en y). El 3D comprimido sale a su
    aspecto y con resolución horizontal completa;
  - lo que toca cambiar: el viewport y el scissor por eje (`DrawState::scissor` ×`N·k` en x), el
    área del render pass, `CopyForDstRead` y `InflateForScale` por eje, los puntos de línea (hoy
    `gl_PointSize = N` es cuadrado: hay que pasar a quads de `N·k × N` o dibujar las líneas del
    HUD en el espacio del HUD), el ajuste de bordes de `EmitQuad` y el desplazamiento de la
    muestra (con `k` no entero los bloques no caen en píxeles enteros de la imagen);
  - **readback**: con `k` no entero, la columna de cada píxel del GE sería `⌊(x + ½)·N·k⌋`. El
    juego no lee lo que dibuja (0 render-to-texture y 0 lecturas anticipadas en F15b y F23b), así
    que la VRAM solo importa para `--capture`, los hashes y el presentador (`TargetMatchesVram`).
- **Lo que se rompe o hay que vigilar**:
  - el scissor siempre es `0,0-479,271` [V en F05]: a pantalla completa se traduce a la imagen
    entera; si un menú usa un scissor parcial, hay que mapearlo con la misma transformación que
    sus vértices;
  - la guard band y el clipping se hacen en píxeles del GE, que no cambian;
  - las lecturas del destino (blending con copia o input attachment) ya van en píxeles de la
    imagen: siguen valiendo;
  - render-to-texture: se decodifica de la VRAM a 1× (F15b, F23b), y la VRAM sigue siendo la del
    juego;
  - los frames de la CPU (video, los subidos desde la VRAM) se suben a 480 × 272: van con barras
    a los lados (pillarbox), no estirados.

## 5. Determinismo, capturas y 20/30 fps

### 5.1 La lógica no cambia [V]

- Con 0,71 y con 0,5, las 8151 muestras de `tramo3` se juegan en el mismo orden y en los mismos
  flips (en los 135 checks, el número de muestra coincide; solo cambia el hash).
- El save que el juego escribe al terminar la misión (`PSP/SAVEDATA/ULUS10202001`, distinto del
  de la grabación) es **idéntico byte a byte** al de `base`.
- No se tocó el timing: el modo de frame (20 fps en `st07m1`) y el timestep son los de siempre. La
  constante solo entra en la proyección y en el frustum.

### 5.2 Los hashes de la grabación sí cambian [V]

- El 3D cambia en cada frame, así que **una grabación hecha en 4:3/16:9 repetida en ultrawide
  sale con código 1**. Para comprobar una partida en ultrawide hay que grabarla con el mismo
  valor, o comparar capturas.
- El control `base101` (`--us-per-preempt 101`, solo cambia el tiempo virtual) da 135/135 y
  capturas idénticas: la repetición es robusta a cambios pequeños de timing.
- **Trampa encontrada**: `--watch-call` cambia la repetición (con cualquier función vigilada,
  `tramo3` difiere desde el flip 3540, 77/135) y no ve las llamadas directas del código
  recompilado (`0x08838870`, el `rand` del juego, no salió ni una vez). No sirve para comparar
  runs; solo para seguir llamadas por el dispatch.

### 5.3 Diferencias en 2D después del 3D [V; causa ?]

- Con 0,71 cambian algunos frames 2D posteriores a la misión: los medidores y el fundido del
  retrato del codec (flips 6480-6720, cajas de 8 a 1567 píxeles) y la silueta de la pantalla "A new
  report has arrived from the spy unit" del briefing (7380-7440).
- Hipótesis [?]: el juego tiene un solo generador, un MT19937 en `0x08A13798` (`0x08838870` saca
  un número, `0x0883884C` siembra; 8 funciones lo usan) [V], y con más objetos dentro del frustum
  se consumen más números (efectos, partículas), así que lo que sale después al azar es distinto.
  También podría ser una diferencia de tiempo virtual (más CPU por frame).
- Lo que lo resolvería: volcar el índice y el estado del MT en cada check de la grabación (un hash
  de `0x08A13798`, 2500 bytes) o una opción de depuración que vuelque memoria en un flip. Si el
  azar diverge, la IA también podría hacerlo en partidas largas; el save igual de una misión dice
  que no cambió el resultado de esta.

### 5.4 Coste

- En la PSP un 30 % más de draws se notaría (por eso es un cheat). En el PC no: lo que crece es el
  display list y el trabajo del GE, muy lejos de lo que aguanta una GPU. En lavapipe (WSL) sí
  cuesta: es la misma escena con más draws.

## 6. Opciones

| Opción | Qué es | Esfuerzo | Riesgo | HUD |
|---|---|---|---|---|
| **(a) Solo proyección + estirar** | El runtime escribe el valor en `0x089E9728` al arrancar (o al cargar `mgp_main`), según `--aspect` o el aspecto de la ventana; el presentador estira la imagen al aspecto de la ventana. Es el cheat con "Stretch" de PPSSPP. | Pequeño (una sesión corta) | Bajo. La dirección va en `profiles/mpo` y se valida con el SHA-256 del ELF. Arrastra el cambio del rango de profundidad (§2.2). | Estirado |
| **(a') Proyección limpia** | Lo mismo, pero solo la escala x: una pista `patch` de `config/hints/mgp_main.txt` cambia las 4 instrucciones `mov.s $f13, $f20` (`0x088451B4` y sus copias) para leer otra constante que el runtime escriba al arrancar, sin tocar `f17`. | Pequeño, pero regenera `mgp_main` | Bajo | Estirado |
| **(b) HUD centrado en el renderer** | En Vulkan, el 3D (modo transform) y lo 2D que cubre el ancho entero (clears, fundidos, fondos) van a todo el ancho, `x·N·k`; el resto del modo through va a una zona 480 × 272 centrada, `(W − 480·N)/2 + x·N`. El video, centrado con barras. | Medio (1-2 sesiones) | Medio: la regla "cubre el ancho" se puede equivocar (un fondo de 470 px, un menú con scissor parcial). Como todo el HUD se mueve igual, el texto no se separa. | Correcto, en el centro 16:9 |
| **(b') Marcas 2D sobre el 3D** | Lo que coloca `0x0881877C` quedaría metido hacia el centro con (b). Se arregla con una pista o un reemplazo de esa función que devuelva x' = 240 + k·(x − 240), o dibujando esos draws con la transformación del 3D. | Pequeño una vez identificadas | Medio: hay que encontrar todos los caminos que proyectan a pantalla (las 6 llamadas y lo que haya en las etapas) | Correcto |
| **(c) Anclar cada elemento a su borde** | El radar a la derecha, la vida abajo a la izquierda: hay que saber de qué elemento es cada draw. Por posición del draw no sirve (un texto centrado que cruza un tercio se partiría; los glifos van uno por draw); hace falta enganchar el motor de layouts (`.la2`) o las funciones que dibujan cada parte del HUD. | Grande e incierto (RE de `.la2` y de cada pantalla) | Alto | Ideal |
| (d) Un juego más ancho | Cambiar el ancho de la cámara (`cam+0x1B0`) o el viewport del GE | — | **Descartado**: el aspecto entra en la escala y (`B[1][1]`), así que solo achica el FOV vertical (Vert−); el GE no dibuja más allá de x = 511 y la VRAM no tiene sitio (§4). | — |

- **`register_native_fast_path`** existe en el framework (`runtime.hpp`), pero solo lo usa el código
  generado que lo llama: para (a') o (b') es más directo una pista `patch` (como las de §6.6 de
  `mgp_main.md`) que cambie instrucciones antes de recompilar, o escribir datos al arrancar.
- **Capturas**: `--capture` es la VRAM a 480 × 272 en todos los casos; para ver el ultrawide hay que
  usar `--capture-window 1` (lo que muestra la ventana).
- **Paridad softgpu/Vulkan en ultrawide**: con (a) se mantiene (las dos dibujan lo mismo en la
  VRAM). Con (b), si el readback sale del target ancho, la VRAM tendría el HUD movido y ya no sería
  lo de softgpu. Hay dos salidas, a decidir en la fase:
  - dibujar cada draw dos veces, en el target normal (readback, hashes, paridad intactos) y en un
    target ancho solo para presentar: duplica el trabajo de la GPU, que aquí sobra;
  - un solo target ancho y aceptar que en ultrawide la VRAM no se compara con softgpu.

## 7. Recomendación

1. **W1 — ultrawide "como PPSSPP"** (primera fase, corta):
   - `--aspect <ancho/alto>` en `mpo_boot` (y en el launcher), que escriba
     `(480/272)/aspecto` en `0x089E9728` al cargar `mgp_main`, después de validar el SHA-256 del
     ELF y que el valor sea 1,0 (la dirección y el valor van en `profiles/mpo/config/`, regla 3);
   - el presentador estira al aspecto pedido, en softgpu y en Vulkan, y el video va con barras;
   - en 16:9 no se escribe nada (0,9926 frente a 1,0 no se ve);
   - criterio de hecho: `tramo3` con 21:9 deja el mismo save que sin él y las capturas de la
     ventana muestran el 3D bien proporcionado. Es lo que ya hace la comunidad con el cheat, así
     que sirve de punto de partida aunque el HUD salga estirado.
2. **W2 — HUD centrado** en Vulkan (opción b + b'): factor `k` por eje en el target, la regla de
   "cubre el ancho", las marcas de `0x0881877C` y la decisión de §6 sobre la paridad. Antes de
   empezar, una medición barata: teñir en el renderer los draws transform y through de un frame
   de misión para ver si todo el HUD es through (cierra el [?] de §3).
3. **W3 — anclar a los bordes** (opción c): solo si el usuario lo pide después de ver W2. Empieza
   por decodificar `.la2` y ver si los layouts tienen anclas o solo posiciones.
4. **Qué dejar fuera**: (d) un framebuffer más ancho de verdad, y anclar por posición del draw en
   el renderer (rompe el texto). La versión limpia (a') solo si el cambio del rango de profundidad
   da algún problema visible.

Para cerrar los [?] que quedan:

- el HUD en modo transform: el teñido del paso 2, o un frame dump con `through` y la matriz por
  draw;
- la causa de §5.3: un hash del MT19937 (`0x08A13798`) en cada check de la grabación;
- las marcas 2D: seguir las 6 llamadas a `0x0881877C` en una misión con alertas, y buscar en las
  etapas otros caminos que proyecten a pantalla;
- la `.la2`: decodificar el formato.
