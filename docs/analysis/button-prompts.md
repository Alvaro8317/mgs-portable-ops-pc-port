# Controles en pantalla: de dónde salen y cómo cambiarlos según el dispositivo — `ULUS-10202` 1.02

Spike de investigación (2026-11-03) para la mejora de F23+ "Controles en pantalla según el
dispositivo" de `docs/PLAN.md`: que el juego muestre ×/A/Espacio, START/Menu/Esc, etc. según lo
último que se usó. Aquí no hay código de producción: solo qué dibuja el juego, dónde vive, qué se
probó y el plan recomendado. Ningún asset del juego va al repo; las texturas y capturas que se
miraron quedaron en `analysis/spike-prompts/` (ignorado por git) y aquí se describen con palabras,
nombres, tamaños y hashes.

## Resumen

1. **Los íconos de botón del HUD y de los menús son texturas propias** (`.txp`), no glifos de una
   fuente [V]. Cada ícono es **una textura aparte** (una página de 32 × 16 en T4, con su CLUT de 16
   colores y un frame de 16 × 16 o 12 × 12), dentro de la TXP del layout que lo usa
   (`lay_keyconfig_0.txp`, `lay_pausemenu_0.txp`, `lay_partyedit_0.txp`…). No son atlas: se pueden
   cambiar por hash del contenido sin recortes. El mismo ícono se repite **byte a byte** en varios
   layouts y etapas.
2. **Los botones dentro del texto** (tutoriales, códec, briefing) son **glifos de 18 × 18 a 2 bpp
   que trae cada script** [V]: el texto es ASCII/UTF-8 y el botón es un código de dos bytes
   `0x8C nn` que apunta al bloque de glifos del propio archivo (`scenerio.gcx` de la etapa, o
   `codec.dat`). Las palabras ("START button", "L button", "directional buttons", "analog stick") son
   texto normal. **`sceFont` no dibuja nada de esto**: solo el aviso del Memory Stick [V].
3. **Los diálogos de PPGe** (los nuestros) ponen el símbolo en `button_symbol()` de
   `host/hle/hle_utility.cpp` y los textos ("Enter", "Back", "Cancel", "Start", "Select") a mano en
   tres funciones [V]. Es el cambio más fácil.
4. **Probado en el juego** [V]: cambiar el glifo × del `scenerio.gcx` de `st04vo` por una "B" hace que
   el diálogo de la celda diga "Press the B button", y cambiar los texels del ícono △ de
   `lay_pausemenu_0.txp` por una "Y" hace que la pausa diga "Y : FLOOR CHANGE". La repetición de
   `play1` coincide en todos los frames salvo en esos dos, así que el juego no cambia de
   comportamiento (§6).
5. **Recomendación**: primero el "dispositivo actual" en el frontend y PPGe (con un registro nuevo en
   las grabaciones); después el reemplazo de texturas por hash para los íconos (es la base de las
   texturas HD); después la sustitución de los 4 glifos de botón en los bloques de glifos; y las
   palabras, al final o nunca (§8).

## 1. Método (reproducible)

Todo se hizo con datos locales del checkout principal (`<main>` =
`/home/alvaro8317/dev/games/mgs-portable-ops-pc-port`), sin compilar nada:

- **Texturas**: `profiles/mpo/tools/txp.py` (nuevo en este spike, solo lectura) lee las QAR y las
  TXP, con el formato de `waspothegreat/MGS-MPO-Noesis`
  ([fmt_mgs_portableops.py](https://github.com/waspothegreat/MGS-MPO-Noesis)) [S], comprobado
  contra el disco:

  ```bash
  T=profiles/mpo/tools/txp.py; Z=analysis/f04/zar     # las etapas ya extraídas en F04
  python -I $T qar  $Z/r_main01/resident.qar                     # entradas: hash, tamaño, nombre
  python -I $T info $Z/r_main01/resident.qar lay_keyconfig_0.txp # páginas y frames
  python -I $T sheet $Z/r_main01/resident.qar lay_keyconfig_0.txp analysis/spike-prompts/kc.png
  python -I $T inventory $Z                                      # cada TXP, en cuántas etapas
  ```

  Para buscar íconos en todo el disco se decodificaron las 25 143 páginas de las 101 `cache.qar` y
  las 3 `resident.qar`, se agruparon las de hasta 64 × 64 por el SHA-256 del RGBA (1 810 distintas;
  130 de ellas en TXP de interfaz) y se miraron en hojas de contacto.
- **Texto**: se separaron las cadenas de los 106 `scenerio.gcx` (cadenas terminadas en NUL, con más
  de un 90 % imprimible) y se contaron los bytes ≥ 0x80. La cabecera del GCX es la de
  [Jayveer/Gcx](https://github.com/Jayveer/Gcx) (`GcxBlockHeader`) [S]: después de la tabla de
  procs (terminada en `-1`) van `procOffset`, `resourceTableOffset`, `stringResources`,
  `fontDataOffset` y `seed`.
- **En el juego**: dos copias del ISO con un `_zar` cambiado (§6), repitiendo `analysis/f19/play1.input`
  con el `mpo_boot` de `build-f09-release` (sin ventana, softgpu, `--interpreter 0`,
  `TZ='<-05>5'`, una copia de `play1.input.ms0` con `cp -a`, `--until-us 236000000`,
  `--capture-every 60`).
- **`sceFont`**: las llamadas a `sceFontGetCharInfo` de `analysis/f21/tramo1.trace.txt` (512 s de
  juego con una misión entera).

## 2. Íconos del HUD y de los menús: texturas

### 2.1 Formato y ejemplos [V]

- Los layouts de interfaz son pares `lay_<nombre>.la2` (en el DAR) + `lay_<nombre>_0.txp` (en la
  QAR). La `.la2` es del editor de layouts ("DESIGNED BY: TEAMFULLMECHA") y referencia los frames de
  la TXP.
- **Cada ícono es su propia página**: 32 × 16 (potencia de 2: 32 × 16), formato GE **T4** (CLUT de
  4 bits), CLUT de 16 entradas RGBA8888, texels **comprimidos con zlib dentro de la TXP** (flag de
  textura `0x0002`) y swizzled. El frame usa 16 × 16 (o 12 × 12) en `0,0`. Como la textura entera es
  el ícono, un reemplazo por hash no tiene que recortar nada.
- `lay_keyconfig_0.txp` (`r_main01/resident.qar`, recurso `0x147C1E07`, 10 560 bytes, 20 páginas): las
  fuentes del layout (3 páginas), la cruceta, los 4 botones, el stick, y una página por botón: □,
  →, ←, ↓, R, ○, L, ×, ↑, △ (páginas 9 a 18) y un △ pequeño de 8 × 16 (página 19). Es la pantalla
  KEY CONFIG del menú de pausa.
- `lay_pausemenu_0.txp` (`r_main01`, `0x14EB5398`, 22 páginas): △, ⊗ y □ de 12 × 12 (páginas 19 a 21).
  El △ es el de "△ : FLOOR CHANGE" del mapa de la pausa (captura `analysis/f19/cap-rp1/flip-013440`).
- Hay más íconos de botón (○, ×, □, ⊗, △, L, R, combinaciones como "○×") en `lay_partyedit_0`,
  `lay_property_0`, `lay_itemlist_0`, `lay_missiontop_0`, `lay_multiindex_0`, `lay_password_0`,
  `lay_gps_0`, `lay_launcher_0`, `lay_bottommenu_0`, `lay_multitable_0`, `lay_gameovermenu_0` y
  `lay_demotalk3d_0`. El inventario exacto (qué página es qué botón) es trabajo de la fase que los
  reemplace; con `txp.py` es mecánico.

### 2.2 Se comparten entre layouts y etapas [V]

Por el SHA-256 del RGBA decodificado (los 12 primeros dígitos):

| Ícono | Hash | Dónde |
|---|---|---|
| □ de KEY CONFIG | `c2cff2e599a2` | `lay_keyconfig_0` de `r_main01`, `r_net01` y `ntmenu2` |
| ○ de KEY CONFIG | `31e0f33d234d` | ídem |
| × de KEY CONFIG | `331f1b27776f` | ídem |
| △ de KEY CONFIG | `28c71d5b21ad` | ídem |
| △ pequeño (8 × 16) | `6a4b8cb4e30a` | 16 TXP: `lay_keyconfig_0`, `lay_optindex_0`, `lay_multitable*_0`, `lay_rule_0`, `lay_pausetable_0`, `lay_resulttable_0` (`r_main01`, `r_net01`, `ntmenu2`, `cyberval2`) |
| ⊗ | `190b16d699f1` | `lay_partyedit_0` de 5 etapas, `lay_gameovermenu_0` y `lay_pausemenu_0` |

- Así que **un juego de reemplazos por dispositivo cubre todas las etapas** con un número pequeño de
  texturas: no hace falta nada por etapa.
- Los íconos de botón son pocos frente a todo lo demás: de las 130 texturas pequeñas distintas de los
  layouts, unas 25 parecen botones (a ojo, en las hojas de contacto) [?].

### 2.3 Lo que no es textura de botón

- **Las palabras de los menús** ("PRESS START BUTTON" del título, "PRESS SELECT BUTTON FOR HELP" de la
  pausa, "PRESS SELECT BUTTON TO EXIT" en `slot.dat`) se escriben con las **fuentes bitmap de cada
  layout**: 2 o 3 páginas con el ASCII y el Latin-1 (`menu.txp`, `fontprint.txp`, `db_font.txp` de
  `init`, y las primeras páginas de cada `lay_*_0.txp`) [V]. Son atlas: cambiar "START" por "Esc" no
  se puede hacer con el reemplazo de texturas, hace falta cambiar la cadena. Las cadenas están en las
  `.la2` (`lay_title.la2`, `lay_pausemenu.la2`, `lay_missiontop.la2`), en `slot.dat` y en
  `gsclimenu.prx` [V].
- **`sceFont`**: en los 512 s de `tramo1` (una misión entera) dibujó 296 glifos, todos de "Checking
  Memory Stick™... Do not remove the Memory Stick™ or turn off power." (U+0020…U+007A y U+2122) [V].
  El juego no usa las fuentes del firmware para el HUD, los menús ni los diálogos.

## 3. Botones dentro del texto: glifos del script

### 3.1 La codificación [V]

- Los textos de diálogo están en las cadenas de `scenerio.gcx` (106 etapas, 19 986 cadenas de texto)
  y de `USRDIR/codec.dat` (252 800 bytes, mismo sello de tiempo `0x4531E19C` que los GCX, así que es
  el mismo formato). Son ASCII con etiquetas (`<S=1>`, `<P=WIN_NAME_SET,ROY>`, `<V=DW>`…) y UTF-8
  para `™`/`®`. No están cifradas (`seed = 0`).
- **Un botón es `0x8C nn`**: el glifo `nn` (desde 1) del **bloque de glifos del mismo archivo**, en
  `blockStart + fontDataOffset`: un `u32` con el tamaño y `n × 81` bytes. Cada glifo es de **18 × 18 a
  2 bpp, de izquierda a derecha y con el píxel más significativo primero** (la "B" del §6 salió
  derecha, no en espejo).
- Los códigos son **locales a cada archivo**: el mismo botón tiene otro código en otra etapa, según el
  orden en que aparece. Por ejemplo, × es `8C 02` en `st04vo` y `8C 03` en `st04m1`.
- Los bloques de glifos de las 106 etapas solo tienen **38 glifos distintos**: 33 kanji (de cadenas
  en japonés que no salen en la versión US), `・` y los cuatro botones:

  | Etapa | Glifos del bloque, en orden |
  |---|---|
  | `st04vo` (la celda) | ○ × □ △ |
  | `st04m1` | ・ ○ × □ △ |
  | `briefing` | × ○ 漢 字 交 |
  | `r_main01`, `addscvr`, `apscan`, `cyberval2`, `gpstest` | 33 kanji (`出撃除名部隊…`) |
  | las demás etapas jugables | ・ |

  Así que **en los GCX, solo `st04vo`, `st04m1` y `briefing` tienen botones en el texto** [V].
- `codec.dat` tiene sus propios glifos: ○, □ y △ aparecen byte a byte iguales a los de las etapas
  (en `0xA88`, `0xAD9`, `0x14F84`, `0x24F9C` y `0x2A134`); el × de las etapas no aparece, así que el
  del códec es otro dibujo o no está [V]. La estructura completa de `codec.dat` (cuántos bloques, dónde empieza cada uno) no se
  analizó [?].

### 3.2 Botones con palabras [V]

Menciones en los GCX (cuenta / etapas):

| Texto | Veces | Etapas |
|---|---:|---|
| `<glifo> button` | 24 | `briefing`, `st04m1`, `st04vo` |
| "the START button" | 12 | `st04m1`, `st04vo` |
| "L button" / "R button" | 6 / 5 | `briefing`, `st04m1`, `st04vo` |
| "the SELECT button" | 4 | ídem |
| "directional button(s)" (con "left and right", "down") | 7 | ídem |
| "analog stick" | 3 | `st04m1`, `st04vo` |
| "Action/Attack/Lock-On/Sneaking/First Person/Crawl/Equip Button" | 27 | `st04m1`, `st04vo` |

y en `codec.dat`: "the Attack button" 12, `<glifo> button` 9, "the L button" 4, "the Equip button"
3, "the START button" 2, "Lock-On Button" 2, "Action button" 1, "R button" 1.

- **Cada línea de tutorial está dos veces**: una con el glifo ("Press the × button") y otra con el
  nombre de la acción ("Press the Crawl Button") [V]. Lo más probable es que el juego elija la
  segunda cuando la configuración de botones no es la de fábrica (KEY CONFIG tiene los tipos A a D)
  [?]. Esa variante ya no depende del dispositivo.
- "START", "SELECT", "L", "R", la cruceta y el stick siempre van en palabras: no hay glifo para ellos.

## 4. Los diálogos del sistema (PPGe, nuestros) [V]

En `profiles/mpo/host/hle/hle_utility.cpp`:

- `button_symbol(button)` devuelve "○", "×", "△" o "□" como texto (se dibuja con `ltn0`, y `jpn0`
  para los símbolos que `ltn0` no tiene, `hle/ppge.hpp`).
- `Utility::display_buttons()` (savedata y mensajes, como `PSPDialog::DisplayButtons`): símbolo +
  "Enter"/"Back" (o el texto que pida el juego), y `button_swap` cambia de lado los dos.
- El diálogo de mensaje: el símbolo de cancelar + "Cancel".
- El OSK: □ + "Space", el de OK + "Select", el de cancelar + "Delete", y las palabras "Start"
  ("Finish") y "Select" ("Shift").

Para que dependa del dispositivo hace falta:

- un `PromptSet` que traduzca un botón de la PSP a lo que se dibuja: "A", "B", "X", "Y", "Menu",
  "View" (Xbox); los cuatro símbolos y "START"/"SELECT" (PlayStation, el actual); y el nombre de la
  tecla o del botón del mouse que tenga asignado en `input.toml` (teclado);
- que `button_symbol()` y las palabras "Start"/"Select" del OSK lo consulten;
- que la fuente tenga esos caracteres (con la fuente libre de F23c también).

**Hecho en F24b** [V]: `host/frontend/prompts.*` (`DeviceTracker`, `PromptSet`, `--prompts`),
`host/hle/prompt_draw.*` (las teclas se dibujan como un recuadro con texto ASCII, así que no hacen
falta glifos nuevos) y el registro `p` de las grabaciones. Con el juego de la PSP los diálogos
dibujan lo mismo que antes, byte a byte (`docs/sessions/F24b.md`).

## 5. El "último dispositivo" en el frontend

- **Hoy no se guarda** [V]: `SdlInput::HandleEvent` (`host/frontend/sdl_input.cpp`) recibe cada evento
  de teclado, mouse y mando y lo pasa a `InputTranslator`, pero no recuerda de dónde vino el último.
  La nota de `docs/PLAN.md` ("el frontend ya sabe qué dispositivo mandó el último input") vale para
  el evento, no para un estado.
- Qué añadir: un `last_device` que cambie con una pulsación o un eje que pase el umbral (no con el
  ruido del stick), con dos valores, teclado/mouse o un mando concreto, y del mando su
  `SDL_GetGamepadType()` (Xbox 360/One, PS3/PS4/PS5, Switch…). Para la etiqueta de cada botón,
  `SDL_GetGamepadButtonLabelForType()` da A/B/X/Y o cruz/círculo/cuadrado/triángulo para el botón
  posicional (`SDL_GAMEPAD_BUTTON_SOUTH`…) [S]
  ([SDL3: SDL_GetGamepadButtonLabel](https://wiki.libsdl.org/SDL3/SDL_GetGamepadButtonLabel)).
- **El prompt sale del mapeo activo, no de una tabla fija**: el juego habla de botones de la PSP, y
  `input.toml` dice qué tecla o botón del mando da cada uno. Con el mapeo por defecto, × es Espacio /
  A (sur), ○ es el clic derecho / B, □ el clic izquierdo / X, △ Ctrl / Y, START Esc / Menu, SELECT
  Tab / View, L y R son Q/E y LB/RB (y los gatillos). Si el jugador reasigna, el prompt sigue a su
  mapeo (buscar al revés en `InputMap`).
- La configuración A-D del propio juego (qué acción va en qué botón de la PSP) la resuelve el juego,
  que ya cambia el texto; el port solo traduce botones de la PSP.

## 6. Experimentos en el juego [V]

Las dos pruebas se hicieron sobre una copia del ISO (`analysis/spike-prompts/iso/patched.iso`) con el
`_zar` cambiado en su sitio: se descomprimió, se cambiaron los bytes, se volvió a comprimir con zlib
nivel 9 (sale más chico) y se rellenó con ceros hasta el tamaño original. El juego acepta los ceros
después del stream.

1. **El glifo × de `st04vo` por una "B"** (glifo 2 del bloque, en `scenerio.gcx + 0xF2F8 + 81`):
   - la captura `flip-013920` dice "What? You don't even know how to crouch? / Press the B but…" en
     vez de "Press the × but…", con la B al derecho;
   - la repetición de `play1`: `frame checks: 234 matched, 1 differed (first at flip 13920)`. **Solo
     cambia el frame donde se ve el glifo**; los de antes y los de después (13980, 14040, 14100)
     coinciden, así que el juego hizo lo mismo;
   - o sea: el juego **lee el glifo del bloque del GCX en el momento de dibujar el diálogo** (o al
     cargar la etapa, que también es después del cambio): cambiar esos 81 bytes cambia el botón sin
     tocar la cadena.
2. **El ícono △ de la pausa por una "Y"** (página 19 de `lay_pausemenu_0.txp`, en
   `r_main01/resident.qar`; texels nuevos comprimidos en los 64 bytes que tenía), en la misma copia:
   - la captura `flip-013440` (el mapa del menú de pausa) dice "Y : FLOOR CHANGE" en vez de
     "△ : FLOOR CHANGE";
   - la repetición: `frame checks: 233 matched, 2 differed (first at flip 13440)`: los dos frames con
     algo cambiado (13440 y 13920) y ninguno más;
   - o sea: el ícono del HUD es la textura de la TXP tal cual, y el GE la lee de los texels que el
     juego descomprime en RAM. Un reemplazo por contenido en el renderer la encuentra.

**Las comprobaciones de las grabaciones ven los prompts**: el hash de cada frame (FNV-1a del RGBA
que se muestra, F19) cambia si cambia un prompt. Ver §7.5.

## 7. Caminos posibles

### 7.1 PPGe (diálogos del sistema)

- `PromptSet` + `button_symbol()` + las palabras del OSK. Si hacen falta íconos en vez de letras, un
  atlas pequeño propio en `hle/ppge`.
- Esfuerzo: **media sesión**. Riesgo: bajo (es código nuestro; los tests de `mpo_hle_test` ya dibujan
  los diálogos).

### 7.2 Reemplazo de texturas por hash (íconos del HUD y de los menús)

- **Cómo lo hace PPSSPP `v1.20.4`** (`GPU/Common/TextureReplacer.cpp`, `TextureCacheCommon.h`) [V]:
  - el nombre del archivo es `%016llx%08x`: un `cachekey` de 64 bits y un hash de 32;
  - `cachekey = (dirección & 0x3FFFFFFF) << 32 | dim`, y con CLUT `cachekey ^= cluthash`, donde
    `cluthash = clutHash ^ clutformat`, con `clutHash` un XXH32 o la mitad baja de un XXH3_64 de los
    bytes de la CLUT (backends de Vulkan y D3D11);
  - el hash es `quick`, `xxh32` o `xxh64` (en `textures.ini`) sobre los bytes de los texels en RAM
    (por filas si `bufw > w`), truncado a 32 bits;
  - `ignoreAddress = true` deja solo la parte baja (`dim ^ cluthash`), y pide `xxh32` o `xxh64`.
- Para MPO, **sin la dirección**: las TXP se cargan en la memoria del juego donde toque y los mismos
  íconos están en varias etapas. Con `ignoreAddress = true` y `hash = xxh64`, la clave queda en
  contenido puro (texels + CLUT + tamaño).
- **Dónde va en el port**: una capa de "texturas sustituidas" que consultan los dos renderers al leer
  una textura (softgpu en `host/ge/sampler.cpp`, Vulkan en su caché de texturas), con el mismo
  RGBA y el mismo tamaño para los prompts. Así softgpu y Vulkan siguen dando lo mismo
  (`capture_compare.py`). Las texturas HD (más grandes) serían solo de Vulkan.
- **Cambio en caliente**: un juego de reemplazos por dispositivo (`prompts/xbox/`, `prompts/ps/`,
  `prompts/keyboard/`, cada uno con su `textures.ini` y PNG), y al cambiar de dispositivo se
  invalidan en la caché solo las entradas sustituidas.
- **Los PNG tienen que ser nuestros** (dibujados para el port, con licencia libre), no derivados de
  las texturas del juego, y sin logos de marca: letras A/B/X/Y, las cuatro formas, teclas con texto.
- Esfuerzo: **1 o 2 sesiones** (la capa, los dos renderers, el formato `textures.ini` mínimo, el
  inventario de los ~25 íconos y los PNG). Riesgo: medio-bajo; lo delicado es invalidar la caché de
  Vulkan y que la textura del ícono no sea parte de un atlas (no lo es, §2.1). Sirve tal cual para las
  texturas HD de M7+.

### 7.3 Glifos de botón en los bloques de glifos

- Hay solo 4 glifos (○ × □ △) en 3 GCX (`st04vo`, `st04m1`, `briefing`) y unos pocos en
  `codec.dat`. Se reconocen por su contenido (81 bytes fijos), así que no hacen falta tablas por
  etapa.
- Opciones:
  - **a) Parchear la copia en RAM**: cuando el juego termina de cargar un GCX o un bloque del códec,
    buscar los 81 bytes de cada glifo de botón y escribir los del dispositivo. Para cambiar en
    caliente, se reescriben. Falta saber **cuándo** está el GCX descomprimido en RAM y dónde
    (lo carga el propio juego: `kjfs` + `zlibdec`, recompilados); un gancho después de la carga de
    la etapa en `OverlayManager` o en el intérprete GCL es candidato [?]. Si el juego copia el
    glifo a una caché propia al abrir el diálogo, un cambio a mitad de un diálogo se vería al
    siguiente.
  - **b) Interceptar la función que lee el glifo** (en `mgp_main`): exacto y en caliente, pero hay
    que encontrarla. Pistas: lee `fontDataOffset` del bloque del GCX y multiplica por 81
    (`0x51`); las búsquedas de inmediatos `0x8C` y `0x51` en `.text` dan 14 y 18 candidatos
    (`0x088F4A00`, `0x0894F690`… no se revisaron todos) [?]. `mpo_boot --watch-call` sirve para
    confirmarla.
  - **c) Por GPU**: solo si el juego dibuja cada glifo como una textura aparte [?]. No se comprobó
    cómo compone el texto (lo que lo decidiría: un frame dump del GE de PPSSPP en el diálogo de la
    celda, leído con `ppdmp.py`).
- **El glifo es de 18 × 18 a 2 bpp**: A/B/X/Y y las formas caben bien; para el teclado hace falta un
  dibujo de tecla con texto corto ("Spc", "Ctrl") o un mouse con el botón marcado.
- Esfuerzo: **1 sesión** con (a), **1 o 2** con (b) por el RE. Riesgo: medio (la parte de RE); el
  experimento del §6 ya prueba que el cambio de bytes llega a la pantalla y no al juego.

### 7.4 Palabras ("START button", "L button", "analog stick"…)

- Cambiar una cadena cambia su largo: el texto se reparte en líneas, el diálogo se escribe letra a
  letra y algunas cadenas se cortan en varias partes. **Podría cambiar cuánto dura un diálogo y con
  eso el timing del juego**, lo que rompería las repeticiones; no se comprobó [?].
- Haría falta enganchar la función que maqueta el texto (RE de `mgp_main`) o reescribir los
  recursos de cadenas del GCX y su tabla de offsets al cargarlos, más las `.la2` y `slot.dat` para
  los menús.
- Son unas 50 frases en 3 etapas más el códec y 3 o 4 cadenas de menú. **Esfuerzo alto para poco**:
  se puede quedar fuera, o limitarse a sustituciones del mismo largo cuando quepan.

### 7.5 Grabaciones y repeticiones

- Las grabaciones guardan las muestras de `sceCtrl`, así que **el prompt no cambia lo que hace el
  juego** (el §6 lo confirma: los frames sin el glifo coinciden). Pero **el hash del frame sí cambia**
  [V], y una grabación hecha con el teclado mostraría prompts de teclado.
- Propuesta: un registro nuevo en `input_recording` (`p <muestra> <juego de prompts>`) cada vez que
  cambia el dispositivo, y que el cambio se aplique justo en esa muestra. La repetición pone los
  mismos prompts en el mismo momento. Las grabaciones viejas (sin `p`) se repiten con los de PSP,
  que es lo que tenían.
- Sin ventana y sin grabación, por defecto los de PSP (los tests y las capturas de referencia no
  cambian).

## 8. Plan recomendado

| Orden | Fase | Qué | Por qué en este orden |
|---|---|---|---|
| 1 | Prompts I | `last_device` en el frontend (con `SDL_GetGamepadType` y las etiquetas de SDL), `PromptSet` sacado de `input.toml`, el registro `p` de las grabaciones, y PPGe (§4, §7.1). Una opción para fijarlo (`--prompts auto\|psp\|xbox\|keyboard`). | Es la base de todo lo demás, es código nuestro y se prueba sin el juego. |
| 2 | Texturas sustituidas | La capa de reemplazo por hash en softgpu y Vulkan, con `textures.ini` estilo PPSSPP (`ignoreAddress`, `xxh64`), el inventario de los íconos de botón con `txp.py` y los PNG propios por dispositivo, con cambio en caliente (§7.2). | Cubre el HUD y los menús de todas las etapas, y es lo mismo que necesitan las texturas HD de M7+. |
| 3 | Glifos de botón | Sustituir ○ × □ △ en los bloques de glifos de los GCX y de `codec.dat` (§7.3, empezando por la opción a y bajando a b si hace falta). | Pocos glifos, pocas etapas (tutoriales y briefing), y el experimento ya mostró que funciona. |
| — | Palabras | Fuera por ahora (§7.4). Como mucho, sustituciones del mismo largo. | Caro, con riesgo para el timing, y solo afecta a ~50 frases de tutorial. |

Lo primero que hay que hacer en la fase 2 es el inventario completo de íconos (qué página de qué TXP
es qué botón, con su hash de contenido) y en la fase 3 confirmar dónde está el GCX en RAM después de
cargar la etapa.

## 9. Lo que no se resolvió

- **Cómo compone el juego el texto** (glifo a glifo como sprites o un bitmap por línea) [?]. Lo
  decidiría un frame dump del GE de PPSSPP en el diálogo de la celda.
- **Cuándo usa el juego las frases con nombre de acción** ("Crawl Button") en vez del glifo [?].
  Se vería cambiando el tipo en KEY CONFIG y mirando el tutorial.
- **La estructura de `codec.dat`** (bloques y su tabla de glifos) [?].
- **La función de `mgp_main` que dibuja `0x8C nn`** [?] (§7.3 b).
- **Qué son los nombres de los frames** de los íconos (sus hashes son `strcode24` de un nombre, por
  ejemplo `0x0090368B` para el △ de KEY CONFIG) [?]: no hacen falta para reemplazar por contenido.
