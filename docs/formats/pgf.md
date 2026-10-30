# PGF: fuentes de `sceFont`

Lo lee y rasteriza `mpo_font` (`profiles/mpo/host/font/`, F17a). La referencia es PPSSPP `v1.20.4`
`Core/Font/PGF.cpp`, que a su vez viene de JPCSP [S]. Etiquetas: **[V]** verificado, **[S]** de
PPSSPP, **[?]** sin verificar.

## Qué fuente usa MPO

- **El disco US no trae ningún `MGP.PGF`** [V], aunque `docs/RESEARCH.md` §4 lo afirma con [V]:
  - no está en el árbol ISO9660 (`USRDIR/` solo tiene `codec.dat`, `slot.dat` y los directorios);
  - la firma `PGF0` no aparece en ningún offset del ISO con una cabecera válida;
  - `mgp_main.elf` no contiene la cadena `.pgf` ni `MGP`.
- `mgp_main` importa de `sceLibFont` solo `NewLib`, `DoneLib`, `FindOptimumFont`, `Open`, `Close`,
  `GetFontInfo`, `GetCharInfo` y `GetCharGlyphImage_Clip`, sin `OpenUserFile` ni `OpenUserMemory`
  ([`mgp_main.md`](../analysis/mgp_main.md)). Por tanto **usa las fuentes del firmware**
  (`flash0:/font/*.pgf`) [V].
- **Qué fuentes pide** (F17b, traza de `mpo_boot` con las 18 fuentes del firmware) [V]:
  - `FindOptimumFont` con familia 1 (sans serif), idioma 2 (latín) y 10 × 10 puntos → `ltn0.pgf`;
    con idioma 1 (japonés) → `jpn0.pgf`. El estilo va a 0 (no lo pide);
  - al arrancar (2,4 s) abre `ltn0` en modo 1 (`FONT_OPEN_INTERNAL_FULL`) y `jpn0` en modo 0
    (`STINGY`); al pasar por NEW GAME cierra las dos, hace `DoneLib` y crea otra librería;
  - dibuja cada carácter en un búfer de 480 × 96 píxeles de 4 bits (formato 0, 240 bytes por línea),
    con la esquina superior izquierda del glifo en `(xPos64, yPos64)`, y lo usa de textura. El
    primer texto es "Checking Memory Stick™... Do not remove the Memory Stick™ or turn off power."
    (74 caracteres), que coincide al píxel en posición con la grabación de PPSSPP.
- Las fuentes que aportó el usuario no son las mismas versiones que la tabla de PPSSPP: `ltn0.pgf`
  pesa 38 236 bytes y tiene 341 glifos (el registro dice 69 108 bytes), y `jpn0.pgf` pesa 4 316 284
  (registro: 1 581 700) [V]. Como en PPSSPP, la memoria que se le pide al juego al abrir una fuente
  sale del registro y no del archivo.
- Las fuentes del firmware tienen copyright (Sony/Fontworks) y no van al repo. PPSSPP las distribuye en
  `assets/flash0/font/` (`ltn0`–`ltn15`, `jpn0` y `kr0`). Son las originales [V]:
  - `ltn0` es "FTT-NewRodin Pro Latin";
  - `jpn0` es "FTT-NewRodin Pro DB";
  - `kr0` es "AsiaKNHH-SONY-uni".

## Estructura [S], comprobada con las fuentes del firmware [V]

- Cabecera de `0x188` bytes, little-endian:
  - firma `PGF0` en el offset 4;
  - revisión (2 o 3) y versión (6 en las del firmware [V]);
  - longitudes y bits por entrada del mapa de caracteres y de la tabla de punteros;
  - tamaño (10 pt) y resolución (128 dpi), en 26.6;
  - nombre y tipo de la fuente;
  - primer y último carácter;
  - máximos de métricas, en 26.6;
  - longitudes de las cuatro tablas de métricas y del mapa de sombras.
- Las revisiones 3 añaden 20 bytes y dos tablas de compresión del mapa de caracteres. Las fuentes
  del firmware probadas son todas de la revisión 2 [V].
- Siguen, en este orden:
  1. cuatro tablas de pares `u32` (dimensión, ajuste X, ajuste Y y avance; H y V);
  2. el mapa de sombras;
  3. las tablas de compresión (solo en la revisión 3);
  4. el mapa de caracteres;
  5. la tabla de punteros a glifos;
  6. los datos de los glifos.
- Las tablas de 2 a 4 y la 5 están empaquetadas en bits: el bit *i* es el bit *i* % 32 de la
  palabra LE *i* / 32. Cada tabla se rellena hasta 32 bits.
- Los punteros de glifo cuentan en unidades de 4 bytes.
- Cada glifo empieza con:
  - 14 bits de tamaño (el desplazamiento al glifo de sombra);
  - 7 bits por cada uno de ancho, alto, *left* y *top* (con signo);
  - 6 de flags (la dirección del bitmap, H o V, y qué métricas van indexadas);
  - 7 de flags de sombra y 9 de id de sombra;
  - las métricas: un índice de 8 bits o dos valores de 32.
- El bitmap es RLE por nibbles de 4 bits por píxel:
  - un nibble *n* < 8 repite el valor siguiente *n* + 1 veces;
  - *n* ≥ 8 va seguido de 16 − *n* valores literales.
- Con el rasterizador de F17a se decodificaron sin errores todos los caracteres del mapa que tienen
  bitmap; ninguno falló [V]:
  - `ltn0`: 338 de 341 glifos, caracteres del 0x20 al 0x2265;
  - `jpn0`: 30 735 de 30 754;
  - `kr0`: 13 960 de 13 969.

  Los glifos que faltan son de tamaño cero (espacios) o no los referencia el mapa de caracteres; la
  herramienta no los distingue [?].
