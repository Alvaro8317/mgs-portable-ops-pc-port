# Investigación — Port nativo de MGS: Portable Ops

Fecha: 2026-10-24 (con hechos añadidos en F01 y F02). Las etiquetas indican de dónde sale cada dato:

- **[V]**: verificado en la fuente primaria (redump, código fuente, README o strings de un binario).
- **[S]**: viene de una fuente secundaria o de prensa.
- **[?]**: inferencia o dato sin verificar.

> La página de pcmrace.com (lista de ports nativos, actualizada el 20/10/2026) bloquea el acceso
> automatizado (403). El contexto de §1 se reconstruyó con otras fuentes y con un fragmento que pegó
> el usuario. Según ese fragmento, la lista cuenta **4 proyectos PSP en Windows y 1 en Linux**, y
> menciona a PSPRecomp entre los frameworks base de la escena, junto a N64Recomp, XenonRecomp,
> ReXGlue, PS1Recomp, PS2Recomp y SNESRecomp.

---

## 1. Contexto: los ports de octubre de 2026 y qué técnica usan

| Proyecto | Origen | Técnica | ¿Sirve para PSP? |
|---|---|---|---|
| **Bloodborne** (deadinside28 `bbport`, y los forks para Windows de Supermedo y Mrsuss60) | PS4 | Ejecuta el código x86-64 original directamente en la CPU, con un runtime que reemplaza las librerías del sistema y un renderer Vulkan derivado de shadPS4. No es decompilación ni JIT. Hay "cierta zona gris" entre recomp y emulador. | **No.** Funciona porque la PS4 es x86-64. |
| **Uncharted 1–3** (Nathan Drake Collection) | PS4 | El mismo autor y el mismo enfoque (runtime y Vulkan de shadPS4). Va por la v0.3.1 (19/10). | No, por la misma razón. |
| **Uncharted 3** (`snozzy8z/U3-PS3Recomp`) | PS3 | Recompilación estática del ejecutable de PS3. En desarrollo. | Sí: el concepto es el mismo que buscamos. |
| **CTR Nitro-Fueled** (SacredFire y los forks `GetLostIn/CTR-NitroFueled-PC` y `Ryu7w7/OCTR-…`) | PS4 | Basado en bbport y shadPS4. Tiene un fork con online en servidor comunitario. | No. |
| **RE Outbreak File #1 y #2** (Outbreak·Web, de "lucas") | PS2 | Según la prensa usa **PS2Recomp + WebGPU** y corre en el navegador, con co-op online. **No hay código fuente.** Hay polémica por un posible uso de "vibe coding" con IA. | Sí, la idea: recompilar un juego de una consola MIPS y llevarlo al navegador. |
| **Yakumo** (MHP3rd HD) | **PSP** | Recompilación estática sobre **PSPRecomp**. Ver §3. | **Es la referencia principal.** |
| **gow-psp-recomp** (God of War CoO y GoS) | **PSP** | Recompilador propio, Ghidra y C. Ver §3. | **Es la referencia principal.** |

**Conclusión**: lo que hizo posibles los ports de PS4 de este mes es que esa consola usa x86-64, y
eso no se repite en la PSP. Para la PSP lo que funciona es la recompilación estática de MIPS más un
runtime HLE, como hacen N64Recomp, PS2Recomp y PSPRecomp.

Fuentes:
- Bloodborne: [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc), [Mrsuss60/bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port), [NielsL92/bloodborne-pc-port](https://github.com/NielsL92/bloodborne-pc-port), [Wccftech](https://wccftech.com/fromsoftwares-bloodborne-pc-port-11-years-ps4/), [ixbt](https://ixbt.games/en/news/2026/10/04/bloodborne-portirovali-a-pk-igra-zapuskaetsia-bez-emuliatora.html), [tech-insider](https://tech-insider.org/bloodborne-pc-port-100-fps-rx-6650-xt-2026/)
- Uncharted: [VideoCardz](https://videocardz.com/newz/uncharted-1-2-and-3-are-now-playable-on-pc), [DSOGaming](https://www.dsogaming.com/news/uncharted-the-nathan-drake-collection-unofficial-pc-port-released/), [U3-PS3Recomp](https://github.com/snozzy8z/U3-PS3Recomp)
- CTR: [GetLostIn/CTR-NitroFueled-PC](https://github.com/GetLostIn/CTR-NitroFueled-PC), [Ryu7w7/OCTR-NitroFueled-PC](https://github.com/Ryu7w7/OCTR-NitroFueled-PC)
- RE Outbreak: [OpenCritic](https://opencritic.com/news/38247/you-can-play-resident-evil-outbreak-completely-free-from-your-browser-right-now), [GamesRadar](https://www.gamesradar.com/hardware/retro/you-can-now-play-my-favorite-resident-evil-multiplayer-romp-for-free-in-a-web-browser-without-a-ps2-but-wretched-ai-vibecoding-could-be-lurking/), [pro-gamer.gg](https://pro-gamer.gg/resident-evil-outbreak-file-1-2-free-to-play-web-browser-webgpu-ai/)

---

## 2. Versiones de MGS: Portable Ops

### MPO (redump) [V]

| Redump | Seriales | Región / idiomas | Versión | Edición |
|---|---|---|---|---|
| [4536](http://redump.org/disc/4536/) | ULUS-10202 | USA / En | 1.02 | Original (PVD 2006-10-29) |
| [3605](http://redump.org/disc/3605/) | ULES-00645 | EU / En, Fr, De, Es, It | 1.01 | Original, Promo y Double Pack (PVD 2007-02-21) |
| [24767](http://redump.org/disc/24767/) | ULJM-05193, ULAS-42087, ULJM-05573, ULJM-08016 | JP y Asia / Ja | 1.02 | Original, PSP the Best y Dendou Selection |
| [126612](http://redump.org/disc/126612/) | ULJM-05227 | JP | 1.02 | Premium Pack (hash distinto) |
| [3478](http://redump.org/disc/3478/) | ULJM-05284 | JP | 1.00 | Deluxe Pack |
| [36711](http://redump.org/disc/36711/) | ULKS-46119 | Corea / Ja | 1.00 | Requiere firmware 2.82 |

### MPO+ [V]

| Redump | Serial | Región | Versión |
|---|---|---|---|
| [11277](http://redump.org/disc/11277/) | ULUS-10290 | USA / En | 1.01 |
| [19424](http://redump.org/disc/19424/) | ULES-01003 | EU (5 idiomas) | 1.00 |
| [3479](http://redump.org/disc/3479/) | ULJM-05261 y 05285 | JP | 1.03 |
| [3247](http://redump.org/disc/3247/) | ULAS-42123 | Asia / En | 1.01 |

### Otros datos de versiones

- **No hay parches oficiales** [V]. El servidor de parches de Sony responde 404 o un cuerpo vacío
  para todos los seriales.
- **La versión EU de MPO es un build posterior** [S]:
  - Trae 2 mapas multijugador más, unos 12 reclutables más y misiones extra.
  - **No tiene GPS.**
  - La contraseña de Raikov cambia: en US es `IVN =RV` y en EU es `SKAZKA`.
- PSN US reutiliza los IDs de los discos: `UP0101-ULUS10202_00-…` y `UP0101-ULUS10290_00-…` [V].
- El servidor ad hoc de PPSSPP trata ULES00645 y ULJM05193 como si fueran ULUS10202, así que el
  multijugador funciona entre regiones [V,
  [proAdhocServer.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/proAdhocServer.cpp)].

---

## 3. Proyectos de recompilación de PSP (referencias técnicas)

| Proyecto | Juego | Herramienta | Licencia | Sistemas | Estado |
|---|---|---|---|---|---|
| [TeamGDB/Yakumo](https://github.com/TeamGDB/Yakumo) | MHP3rd HD (NPJB-40001) | [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) | MIT | Windows, Linux, macOS y Android | v0.7.5 (13/10/2026), jugable |
| [DrummingBird1/gow-psp-recomp](https://github.com/DrummingBird1/gow-psp-recomp) | GoW CoO y GoS (UCUS98653 y 98737) | Propia (Python, Ghidra y C) | GPL-3.0+ | Windows (Linux "planeado") | Preview v1.2.1 |
| [gasparegas/tkdr-recomp](https://github.com/gasparegas/tkdr-recomp) | Tekken DR (ULUS-10139) | Propia (Python y JS a WASM) | MIT | Nativo (SDL2) y Web | Arranca hasta Quick Battle |
| [elprogramadorloco-arch/fuc-recomp](https://github.com/elprogramadorloco-arch/fuc-recomp) | Fate/unlimited codes | Fork de PSPRecomp | MIT | Windows | v0.0.5 |
| [sal063/Ace-Combat-X-Recompilation](https://github.com/sal063/Ace-Combat-X-Recompilation) | Ace Combat X | ? | GPL-2.0 | ? | V1.5 |
| [zmbkilla/PSP2I_Recomp](https://github.com/zmbkilla/PSP2I_Recomp) | Phantasy Star Portable 2 Infinity | "psprecomp" (Rust → C++17, SDL2 y GL 3.3) [S] | ? | ? | ? (hecho con mucha ayuda de IA) |

Hay más proyectos que no se revisaron: Mango77x/MHForever, erreib/pure-recomp (Wipeout Pure),
tclamb/mhp2g-decomp y "PSP Web Recomp". Los trackers donde aparecen son
[recompsoftware.com](https://recompsoftware.com/platforms/playstation-portable/) y
[decomps.net](https://decomps.net/?platform=psp).

### PSPRecomp / Yakumo: lo relevante [V]

**Estructura y pipeline:**
- El framework `psprecomp_core` se separa de los perfiles por juego en `profiles/<id>/`, que tienen
  `config/ data/ generated/ host/ scripts/ tests/ tools/ third_party/ progress/`.
- El host registra sus funciones con `Runtime::register_function()`, los imports con
  `register_hle()` y los atajos nativos con `register_native_fast_path()`.
- El pipeline es: `psp_analyze` → `psp_recomp` (o un generador propio del perfil) → host → CMake con
  `-DPSPRECOMP_PROFILE=<id>`.
- **El descifrado queda fuera del framework.**

**Intérprete de respaldo:**
- Se usa cuando el dispatch no encuentra una función generada. Corre en porciones acotadas, para que
  el scheduler pueda expropiar.
- Es unas 20 veces más lento que el código recompilado.
- Con `PSPRECOMP_NO_INTERPRETER=1` se hace una parada estricta, y al final se reportan las
  direcciones que se interpretaron.

**Overlays:**
- Se usan `set_runtime_missing_function_hook`, `set_runtime_unsupported_hook` y
  `Runtime::unregister_functions(start, end)`.
- Se compila **una librería compartida por overlay**, identificada por el hash de la memoria del
  invitado.
- Yakumo tiene 355 overlays, que se instalan entre frames.

**Build y testing:**
- El build se hace en 5 etapas: bootstrap, `--install image.iso` (valida el disc ID y el SHA-256 del
  EBOOT cifrado), `generate.sh` (89 unidades de C++), compilación y `build_overlays.sh`.
  - Tarda unas 2 h en un M1 y entre 5 y 6 h en una Steam Deck.
  - Se compila **un trabajo por cada 4 GiB de RAM** (`PSPRECOMP_GENERATED_JOBS`).
  - Se necesitan **64 MiB de pila** en Linux.
- Testing:
  - Toggles A/B por variable de entorno, trazas, `PSPRECOMP_HLE_HISTOGRAM=1` y capturas con F12.
  - `ctest` sin el juego, ASan/UBSan y clang-tidy.
  - Un smoke test manual que hashea los saves.

**Problemas abiertos en Yakumo:**
- Las superficies curvas (issue #10, o sea el bezier/spline del GE) **no están**.
- Los diálogos de savedata no dibujan nada (#33).

**Renderer:**
- Vulkan (MoltenVK en macOS).
- Interpolación de frames con la simulación fija a 30 fps.
- Texturas en formato de PPSSPP.

### gow-psp-recomp: lo relevante [V]

- **Pipeline**:
  1. `pspelf.py` carga el ELF con relocaciones PSP en la base `0x08804000`.
  2. En Ghidra (12.1.3 + ghidra-allegrex), `ExportRecompInput.java` genera `<ID>.recomp.json` con
     funciones, rangos y tablas de saltos.
  3. `recomp.py` (con `allegrex.py` y `emitc.py`) genera C.
  4. El runtime en C se compila con CMake.
- **Reglas de codegen**: son las que adoptamos en `CLAUDE.md` §5 (delay slots, branch-likely,
  `switch` para tablas de saltos y `rt_call` para el resto de indirectos).
- **Memoria y VFPU**:
  - La memoria es una arena de 512 MB indexada con `addr & 0x1FFFFFFF`.
  - Las instrucciones VFPU se emiten como `rt_vfpu(c, word, pc)`, con la semántica de PPSSPP
    (prefijos incluidos).
- **Difftest**: `difftest.py` compara contra Unicorn (MIPS32 24Kf) con entradas aleatorias.
- **Runtime**:
  - `rt_thread.c` (scheduler) y `rt_io.c` (lee CHD e ISO con libchdr; descifra PGD con libkirk).
  - `rt_ge.c` y `rt_gesoft.c` (GE en software) más `rt_gpu.c` (hardware con HLSL, SDL3 GPU, D3D12 y
    Vulkan).
  - `rt_audio`, `rt_sas`, `rt_atrac` (`at3_standalone`), `rt_mpeg` y `rt_h264dec` (OpenH264).
- **Herramientas**: `--record-input` y `--input` (reproducción determinista), `--frames-dir` con
  `check_frames.py`, `audio_report.py`, `--texture-dump`/`--texture-pack` y `RT_GE_TRACE_LIST`.
- **Factibilidad** (traducida del hebreo,
  [doc](https://github.com/DrummingBird1/gow-psp-recomp/blob/main/docs/feasibility-static-recomp.md)):
  - Cobertura de la CPU:
    - 99.88 % de `.text` desensamblado.
    - 317 de 319 saltos computados resueltos.
    - Los `jalr` son casi siempre vtables o punteros a función, así que van por una tabla de búsqueda
      "como en N64Recomp".
  - FPU: ronda el 14 % del código, en precisión simple. Ojo con flush-to-zero, FCR31, `cvt.w.s` y NaN.
  - VFPU: unas 1.200 instrucciones en 81 funciones. Para `vsin` y `vcos` hay que copiar las tablas de
    PPSSPP.
  - No hay código automodificable, y los codecs de Sony cargados como PRX se resuelven con HLE.
  - Imports: entre 199 y 206, y PPSSPP los implementa todos.
  - Hilos: fibras.
  - GE: hay 3 opciones (reutilizar la GPU de PPSSPP, escribir una propia, o hooks a nivel de motor).
  - **Riesgos principales**: los efectos de framebuffer (render-to-texture y lecturas de VRAM), el
    timing, la deriva numérica de FPU y VFPU, y la licencia.
  - Plan:

    | Paso | Duración estimada |
    |---|---|
    | MVP del recompilador | — |
    | Kernel | 3 a 6 semanas |
    | GE | 1.5 a 4 meses |
    | Audio, video y saves | 1 a 2 meses |
    | Estabilidad | — |
    | **Total** | **6 a 12 meses** |

---

## 4. Internos de MGS: Portable Ops

### Código

- **`mgp_main`** es el módulo principal [V]. Los plugins de TheFloW y savempo lo enganchan, y
  [MGSRemastered](https://github.com/TheOfficialFloW/RemasteredControls/blob/master/MetalGearSolid/main.c)
  también engancha `mgp_stage`. Peace Walker comparte esa convención de nombres.
- **Identidad de los módulos, vista en el ELF descifrado (F02) [V]**:
  - `EBOOT.BIN` es un `~PSP` con módulo `mgp_main`, tag `0xC0CB167C` y `decrypt_mode` 9. Está sin
    comprimir y mide `psp_size` 2 693 760 bytes. `PrxDecrypter` de PPSSPP lo descifra con la entrada
    `g_keyEBOOT2xx` (código KIRK `0x5D`) de su tabla `g_tagInfo`, la que usan los tipos 0 y 1.
  - El ELF de `mgp_main` mide 2 693 412 bytes, con SHA-256
    `ca700ea516a98949763410da537a75a08f9723718fab302bcd7ef1e6185cadf8`. Coincide con los primeros
    `elf_size` bytes del "Dump decrypted EBOOT.BIN" de PPSSPP.
    - PPSSPP vuelca el buffer entero de `max(elf_size, psp_size)` bytes, así que su archivo tiene 348
      bytes más de residuo del descifrado. El buffer completo también coincide byte a byte.
  - `mgp_main` es un **PRX relocatable** (`e_type 0xFFA0`), no un `ET_EXEC` de dirección fija:
    - `e_entry` es `0x128` y `e_flags` es `0x10A23001`.
    - Tiene dos `LOAD`: el primero con vaddr `0x0`, filesz y memsz `0x1EED14` y flags RWX; el
      segundo con vaddr `0x1EED40`, filesz `0xD154`, memsz `0x108A80` y flags RW, que es casi todo
      `.bss`.
    - Tiene un segmento `0x700000A0`, que son las relocaciones PRX del formato clásico (`PT_PRXRELOC`; no es el
      `0x700000A1` comprimido), de `0x95298` bytes, y
      34 secciones con nombre (`.text`, `.sceStub.text`, `.lib.stub`, `.rodata.sceNid`, `.rel.*`,
      etc.).
  - Su `sceModuleInfo` está en el offset de archivo `0x1D7130` (vaddr relativa `0x1D7070`):
    - nombre `mgp_main`, atributo `0x0000` (modo usuario) y versión 1.1;
    - `gp` en `0x15150`;
    - exports en `0x1D6DD4–0x1D6DE4` (16 bytes) e imports en `0x1D6DEC–0x1D706C` (640 bytes).
  - **Base de carga**: el debugger de PPSSPP (en el Windows del usuario, con el juego ya en marcha)
    lista estos módulos:

    | Módulo | Dirección | Tamaño |
    |---|---|---|
    | `mgp_main` | `0x08804000` | `0x2F7800` |
    | `ZLIB` | `0x08B20000` | `0x4100` |
    | `KCEJ_SOUND2` | `0x08B24100` | `0x51400` |
    | `KCEJ_FS` | `0x08B75500` | `0x11A00` |
    | `mgp_stage` | `0x08B8F900` | `0x3C600` |

    - El tamaño de `mgp_main` cuadra con el ELF: `0x1EED40 + 0x108A80 = 0x2F77C0`, redondeado a
      0x100.
    - Entre el final de `mgp_main` (`0x08AFB800`) y `ZLIB` quedan `0x24800` bytes que reserva otra
      cosa [?].
    - Como el módulo es relocatable, la recompilación tiene que **aplicar sus relocaciones en
      `0x08804000`**, igual que el `pspelf.py` de gow-psp-recomp.
  - Los módulos de Konami en `USRDIR/modules/` son PRX relocatables sin cifrar:
    - `kjfs.prx` es `KCEJ_FS` (`gp` `0x87A0`);
    - `sound2.prx` es `KCEJ_SOUND2` (`gp` `0x8B00`);
    - `zlibdec.prx` es `ZLIB`.
  - Los 106 `stage/<s>/<s>.prx` son `mgp_stage` (`0xFFA0`).
  - Se obtiene con `python -I profiles/mpo/tools/prx_info.py <elf>`.
- Hay **overlays de código por etapa** [V]: cada etapa trae un **PRX con su tabla de relocación
  `.rlc`**.
  - **F04 [V]**: el `.rlc` es la última entrada del DAR que va dentro de `stage/<s>/_zar` (y la
    última línea de su `data.cnf`), no un archivo suelto. Tiene el formato de `PT_PRXRELOC`, pero
    enlaza la etapa con `mgp_main`: `ofs_base` es un segmento de la etapa y `addr_base`, uno de
    `mgp_main`. Ver [`docs/formats/rlc.md`](formats/rlc.md) y
    [`docs/analysis/stages.md`](analysis/stages.md).
  - **F04 [V]**: lo aplica `mgp_main` en `0x08805C0C`, después de `sceKernelStartModule` y antes de
    invalidar la icache y entrar a la etapa. Coincide con `rlc.py`, y un savestate de PPSSPP con
    `st02` en `0x08B8F900` coincide byte a byte en todo `.text`.
  - Las strings `%sstage/%s/%s.rlc`, `kjfs: stage module %s -> %s` y `rlc: preloaded %s.rlc ...
    entries` **no son del juego** [V]. Son del plugin homebrew `savempo_plugin.prx`
    ([savempo](https://github.com/snakeswiss/MPO-Guide)), que en MPO+ sirve las etapas desde la
    Memory Stick.
  - Hay un `title.prx` que es un ELF
    ([XeNTaX](https://github.com/XeNTaXBackup/XeNTaXBackup.github.io/blob/master/markdown/MGSportable%20Ops%20texture%20format._2560.md)).
  - Se cargan con `sceKernelLoadModule`
    ([logs](https://report.ppsspp.org/logs/game/ULJM05193_1.02)). **F04 [V]**: en `ULUS-10202`,
    `mgp_main` importa `sceKernelLoadModuleByID` (no `sceKernelLoadModule`), además de
    `sceKernelStartModule`, `sceKernelStopModule` y `sceKernelUnloadModule`.
- Otros nombres que aparecen [V, strings]:
  - Los módulos `KCEJ_FS` (FS) y `KCEJ_SOUND2` (sonido).
  - Los hooks `ntmenu2` (menú de red), `mpogate` y `mpostun`.
  - El manifiesto de datos `data.cnf` ("datacnf: detached streaming fd…").
- zlib embebida [S]: `inflate`, `inflateEnd` e `inflateInit_` con NIDs ofuscados (en MPO+ JP,
  [#18904](https://github.com/hrydgard/ppsspp/issues/18904)).
- En MPO+ quedan restos de desarrollo: carga de etapas desde `host0:` y "GPI dev switches"
  ([#20191](https://github.com/hrydgard/ppsspp/issues/20191)).
- **No hay fuente propia** [V] (F17a): el disco no trae `MGP.PGF` ni ningún `.pgf`, `mgp_main` no
  nombra ninguno y el juego solo importa `sceFontFindOptimumFont`/`sceFontOpen`, nunca
  `OpenUserFile`/`OpenUserMemory`. Usa las fuentes del firmware (`flash0:/font/*.pgf`). Ver
  `docs/formats/pgf.md`.
- Scripts: **GCL/GCX en la variante MGS3**, compartida con Acid 1 y 2. GCL está basado en Tcl y el
  GCX "puede o no estar cifrado" ([Jayveer/Gcx](https://github.com/Jayveer/Gcx)) [V].
- Linaje del motor: el formato de modelos MDP es común a Acid, MPO y Peace Walker. El TXP de MPO es
  distinto del de PW: en MPO la paleta va por textura y en PW por frame.
- **Análisis de `mgp_main` en Ghidra (F03) [V]**. El detalle está en
  [`docs/analysis/mgp_main.md`](analysis/mgp_main.md).
  - Son 8 484 funciones y 482 007 instrucciones, y el 99.97 % de `.text` queda decodificado. Ghidra
    solo llega al 89.49 %; el resto se alcanza sembrando desde relocaciones, prólogos y huecos.
  - Importa 240 funciones de 32 librerías:
    - 169 de Sony, de las que PPSSPP `v1.20.4` implementa 165; las 4 que no son
      `sceUtilityHtmlViewer*`;
    - 71 de los módulos de Konami (`kjfs` 20, `sound2` 48 y `zlibdec` 3), cuyos stubs están dentro
      de `.text`.
  - `module_start` declara SDK `0x02080010` y compilador `0x30306` (GCC 3.3.6). Crea el hilo
    `user_main`, que a su vez crea `Main` (con VFPU), y el bucle principal es `0x0883F464`.
  - Al arrancar carga `zlibdec`, `sound2`, `kjfs` y `libfont` desde `USRDIR/modules/`, con
    `sceKernelLoadModuleByID`. Antes llama a `sceUtilityLoadAvModule(0..3)`.
  - **El hueco de `0x24800` bytes** antes de `ZLIB` es la alineación, más el heap `UserSbrk`
    (`0x10000`), más los bloques que PPSSPP reserva para `av_atrac3plus` y `av_mpegbase`.
  - **No importa `sceKernelLoadModule` ni `sceKernelIcacheInvalidate*`**, pero modifica código en dos
    casos:
    - al arrancar, convierte tres funciones de depuración en `jr ra`;
    - al cargar una etapa, le aplica una tabla de relocaciones contra sus propias bases (el candidato
      a ser el `.rlc`) e invalida la caché de instrucciones con `cache 0x04`. **F04 [V]**: es el
      `.rlc` (ver `docs/formats/rlc.md`).
  - **El intérprete GCL** se localizó por sus tablas de comandos (`strcode24` y handler):
    - `0x089F51D4`, con `if`, `switch`, `eval`, `call`, `return` y `print`;
    - `0x089F2F34`, con `load`, `command`, `chara`, `restart`, `trap` y `mesg`.
- **Los módulos de Konami (F03) [V]**:
  - `sound2.prx` tiene 531 funciones y 24 027 instrucciones, y exporta 48 funciones. Importa
    `sceSasCore` (21), `sceAtrac3plus` (9) y `sceAudio` (9), todas implementadas en PPSSPP.
  - `kjfs.prx` tiene 230 funciones y exporta 20. Usa I/O asíncrono e importa `zlibdec`.
  - `zlibdec.prx` tiene 24 funciones y exporta 3.
  - **Decisión: los tres se recompilan como módulos residentes**, y la capa de Sony que tienen debajo
    va por HLE (ver `docs/analysis/mgp_main.md` §9).

### Formatos de datos

Fuente principal: [waspothegreat/MGS-MPO-Noesis](https://github.com/waspothegreat/MGS-MPO-Noesis)
[V]. Todo es little-endian.

**Archivos contenedores:**

- **`USRDIR/slot.dat`**:
  - Una cabecera de 0x800 bytes: `u32 time, u16 1, u16 1, u16 pages, u16 groups`.
  - Por cada página, un TOC de entradas `(u32 hash, u32 offset & 0x3FFFFFFF)` rellenado a 0x800, y
    después los datos.
  - **No necesita `.key`**, a diferencia del de PW.
  - Los nombres no se guardan: solo hay hashes.
- **`stage/<s>/_zar`**: un `u32` con el tamaño descomprimido seguido de zlib que contiene un DAR.
  Dentro van `cache.dar/qar` y `resident.dar/qar`. **F04 [V]**: el DAR también trae `data.cnf`,
  `scenerio.gcx` y el `<s>.rlc` (ver [`docs/formats/rlc.md`](formats/rlc.md)).
- **DAR**: un `u32 count`, y por cada archivo un nombre (terminado en NUL y alineado a 4), un
  `u32 size` y los datos alineados a 16. **F04 [V]**: después de los datos de cada entrada va un
  byte NUL, y la siguiente entrada empieza justo después, sin alinear.
- **QAR**: el último `u32` es el offset de la lista. La lista es `count` seguido de `(hash, size)` y
  los nombres, con los datos alineados a 0x80.

**Hash de recursos:**

- `hash = (type<<24) | strcode24(name)`, con
  `strcode24: v = ((v>>19 | v<<5) + c) & 0xFFFFFF` (si el resultado es 0, pasa a ser 1).
- Tipos: `bin, gcx, tri, mdh, mds, lt2, cv2, mtar, mtsq, mtfa, mtcm, geom, nav, cvd, eft, zon,
  mdp/mdb/mdc/mdl, txp, kms, rpd, fcx, mtst, mdpb, mgm, prx (0x31), rlc (0x32), ptcp, cddl, cap,
  pcmp, sep, bgp, la2, slot, png, img, vib, rat, rcm, ola, row, mtra, psq`.

**Recursos:**

- **MDP** (modelos):
  - Posiciones en s16 multiplicadas por una escala.
  - Pesos en u8 (128 = 1.0).
  - Triangle strips con paleta de huesos.
  - La unidad es de unos 1 mm.
- **TXP** (texturas):
  - De 4 u 8 bits con CLUT RGBA8888, siempre swizzled y a veces con zlib.
  - Sus flags se traducen a estado del GE: TFX_REPLACE o MODULATE (con doble de color), blending
    alpha, aditivo o resta inversa, env-map y alpha ref 0x7F.
- **MTAR** (animación): claves de rotación eje-ángulo empaquetadas en bits y traslaciones en un
  formato tipo half-float, a 60 Hz.
- Otras herramientas de formatos:
  [Joy-Division/tools-mgs](https://github.com/Joy-Division/tools-mgs),
  [Zoft123/Codec](https://github.com/Zoft123/Codec) (su lector de `slot.dat` pide `.key`, así que
  probablemente no sirve para MPO) y MGPSP Redkit v1.2 de RedCode Interactive (cerrado, abril de
  2026, [anuncio](https://x.com/redcode_team/status/2031778567210562019)).

### Timing y FPS [V]

Fuente: [CWCheat-Database-Plus](https://github.com/Saramagrean/CWCheat-Database-Plus-).

- **El gameplay corre a 20 fps** con un divisor de vsync de 3.
- En ULUS-10202:
  - `0x201F5210` es el divisor (3, 2 o 1).
  - `0x201F4FD8` es el timestep (0xF, 0xA o 5).
- El código de 60 fps tiene que parchear los timers de wall-sneak, chaff, stun y granadas, y aun así
  deja efectos secundarios: misiles y Cunningham más rápidos, y chaff más corto
  ([notas JP](https://github.com/SIEBEN5106/MGS-Portable-Ops-60fps-code-JP)).
  **La lógica depende del framerate.**
- Widescreen en US: `0x201E9728` es un float de aspect ratio (16:9, 21:9 o 32:9) y se usa con
  "Stretch".
- El revival online solo permite 20 o 30 fps.
- **Verificado en el binario (F03) [V]**:
  - `0x089F5210` (`0x201F5210`) es el mínimo de vblanks por frame. Lo lee la espera de frame
    (`0x08843A20`, que usa `sceDisplayWaitVblankStartCB` y un contador de vblanks que alimenta una
    sub-interrupción).
  - `0x0883DBB0(modo)` fija el modo de frame:

    | Modo | Vblanks por frame | Timestep (`0x089F4FD8`) | fps (`0x08A18E6C`) |
    |---|---|---|---|
    | 0 | 1 | 5 | 60 |
    | 1 | 2 | 10 | 30 |
    | 2 | 3 | 15 | 20 |
    | 3 | 4 | 20 | 15 |

  - El arranque usa el modo 1 y el gameplay a 20 fps es el modo 2.
  - El timestep lo leen 233 funciones.
  - El float del cheat de widescreen (`0x089ED728`) está en `.rodata`, y Ghidra solo le encuentra una
    referencia [?].
  - Detalle en `docs/analysis/mgp_main.md` §6.2.

### Compatibilidad con PPSSPP y lo que revela del uso del GE

- En escritorio está **"Perfect"** para US 1.02 y EU 1.01
  ([US](https://report.ppsspp.org/game/ULUS10202_1.02),
  [EU](https://report.ppsspp.org/game/ULES00645_1.01)).
- **Líneas**: se pensaba que `compat.ini` tenía `[CenteredLines]` para ULUS10202 y ULES00645, por el
  radar y el mapa ([#20142](https://github.com/hrydgard/ppsspp/issues/20142)). **No es así en
  v1.20.4**: la sección solo lista Echochrome y un homebrew, y el flag solo se lee en
  `GPU/Common/SoftwareTransformCommon.cpp`, la ruta de los renderers por hardware; softgpu no lo
  usa [V] (F12). Si #20142 lo añadió después, es posterior al tag fijado [?].
- **Splines y beziers del GE**: se usan para subdividir el suelo cerca de la cámara, con skinning
  ("vtype 12000780, bones: 1") ([#1884](https://github.com/hrydgard/ppsspp/issues/1884),
  [#3343](https://github.com/hrydgard/ppsspp/issues/3343)).
- **Clamp de profundidad y guard-band**: un bug de PPSSPP aquí hacía desaparecer el suelo
  ([#12348](https://github.com/hrydgard/ppsspp/issues/12348)).
- **Render-to-texture**: el juego texturiza desde el render target actual y usa `fbo_blit_color` y
  `sceGeEdramSetAddrTranslation`. Hubo bugs históricos con flashes negros
  ([#2438](https://github.com/hrydgard/ppsspp/issues/2438),
  [#1202](https://github.com/hrydgard/ppsspp/issues/1202)).
- **Medición directa** (F05, 11 frame dumps, [`docs/analysis/ge-usage.md`](analysis/ge-usage.md)).
  Lo de arriba son reportes de terceros; esto está medido.
  - **[V]** El framebuffer del juego es **5551** con doble buffer.
  - **[V]** La intro y el "Press Start" no usan el GE: se muestran en tres buffers 8888.
  - **[V]** Los splines son el 49 % de las draws, sin skinning y siempre con depth clamp.
  - **[V]** Las texturas son siempre CLUT4/8 con swizzle.
  - **[V]** No se vieron render-to-texture, logic ops, stencil, beziers ni mipmaps.
- **Borrado de saves**: el diálogo de borrar save podía vaciar SAVEDATA
  ([#6600](https://github.com/hrydgard/ppsspp/issues/6600)). Hay que implementar bien el modo delete.
  **[V]** MPO borra con `LISTALLDELETE` y `gameName` vacío, justo el caso del bug
  ([`docs/analysis/utility.md`](analysis/utility.md), F18a).

### Hardware y funciones del sistema

**Confirmado [V]:**

- **Gráficos y texto**: GE completo (incluidos splines, beziers y líneas) y `sceFont`.
- **Audio y video**: `sceAtrac` (ATRAC3), `sceSas` (ADSR) y `sceMpeg`. Muchas cinemáticas son
  in-engine ([#3659](https://github.com/hrydgard/ppsspp/issues/3659)), pero **no todas**: el disco
  trae **30 videos PMF** en `USRDIR/mpg/` (342 MB), con el título (`MPO_OP*.pmf`), los créditos
  (`staffroll.pmf`) y cinemáticas `sNN_MM*.pmf` [V, F01, `docs/formats/disc-layout.md`].
- **Sistema**: `sceUtility` Savedata y OSK, y `sceKernelLoadModule`.
- **Red**: `sceNetAdhoc*` (con Matching y Discover), `sceNetApctl*`, `sceNetInet`,
  `sceNetResolver`, `sceHttp*` (`…WithURL`), `sceUri*` y `sceNpAuth`.
- **Dónde está cada cosa (imports, F03)** [V]:
  - `mgp_main` no importa audio de Sony, `sceMpeg`, savedata, OSK ni diálogos.
  - El audio lo importa `sound2.prx`: `sceSasCore`, `sceAtrac3plus` y `sceAudio`.
  - Los PRX de etapa importan:
    - `sceMpeg`: `title`, `ending` y `flashdemo`;
    - `sceUtility`: 23 etapas;
    - `sceUsbGps`: `briefing` y `gpstest`;
    - el escaneo de `sceNetApctl`: `apscan` y `briefing`.
  - `mgp_main` sí importa `sceUtilityNetconf*` y `sceUtilityHtmlViewer*` (este último sin
    implementar en PPSSPP).
  - Los módulos se cargan con `sceKernelLoadModuleByID`, no con `sceKernelLoadModule`.
- **VFPU (F03)** [V]: `mgp_main` tiene 27 657 instrucciones VFPU (el 5.7 %) en 1 114 funciones.
  - Usa 89 mnemónicos y 349 prefijos.
  - Entre las transcendentes: `vrsq` 280, `vsqrt` 236, `vcos` 99 y `vsin` 95.

**Sin verificar [?]:**

- Si `sceUsbGps` se usa de verdad (solo en US y JP). En US lo importan las etapas `briefing` y
  `gpstest` [V, F03].
- `PSP_SYSTEM_VER` de cada región. US 1.02 trae `2.81` [V, F01].

### AP Scan: reclutar con APs de Wi-Fi

**Cómo lo ve el jugador** [S]:
- Cada AP da un soldado aleatorio, y cuenta también si tiene contraseña.
- Se llena una barra de señal presionando Círculo.
- Hay un cooldown por AP (24 h según un foro).
- Algunas unidades (GRU, KGB, Ocelot) solo se consiguen así
  ([WP](https://en.wikipedia.org/wiki/Metal_Gear_Solid:_Portable_Ops),
  [GameFAQs](https://gamefaqs.gamespot.com/boards/932978-metal-gear-solid-portable-ops/42848892)).

**Qué hace por dentro** [V]:
- La secuencia es `sceNetInit` → `sceNetApctlInit` → `sceNetApctlAddHandler` →
  `sceNetApctlScanUser`.
- Después espera `SCAN_COMPLETE` y lee los resultados con `sceNetApctlGetBSSDescIDListUser` y
  `sceNetApctlGetBSSDescEntryUser`.
- La API da por AP el BSSID, el SSID, el canal, la intensidad de la señal y el tipo de seguridad
  ([sceNet.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceNet.cpp)), pero **MPO
  solo pide la señal, el BSSID y el SSID**: nunca pide el canal ni la seguridad (F20a) [V].
- **Dónde está (F03) [V]**:
  - `sceNetApctlScanUser`, `GetBSSDescIDListUser` y `GetBSSDescEntryUser` los importan **los PRX de
    etapa `apscan` y `briefing`**, no `mgp_main`.
  - `mgp_main` tiene el resto:
    - `Init` y `AddHandler` en `0x0885D27C`;
    - el cierre en `0x0885D5F0`;
    - `GetInfo` y `Netconf` en `0x0885D9A4`;
    - `GetState`.

**Contrato completo (F20a)** [V], en [`docs/analysis/apscan.md`](analysis/apscan.md):
- El código del escaneo está en las etapas `apscan` y `briefing`; `mgp_main` solo pone la red y los
  stubs.
- El handler solo actúa con el estado nuevo `DISCONNECTED`: `SCAN_COMPLETE` (2) marca resultados y
  `ERROR` (6) guarda el código.
- Al ver `SCAN_COMPLETE`, el juego lee los resultados y llama **otra vez y en el acto** a `ScanUser`,
  sin mirar lo que devuelve. Salir de la pantalla y reclutar esperan al siguiente `SCAN_COMPLETE`.
- Se queda con el AP de señal más fuerte y saca la semilla del recluta del BSSID y el SSID. En US 1.02
  la tabla de SSID especiales está vacía (la función devuelve -1).
- **El cooldown que se ve en el código es global y depende del tiempo desde el último recluta**, no
  es por AP. El "24 h por AP" de los foros puede estar en `0x0883754C` [?].

**En los emuladores:**
- PPSSPP finge 4 APs, y eso basta para Peace Walker
  ([PR #14345](https://github.com/hrydgard/ppsspp/pull/14345)). **Pero MPO y MPO+ se quedan colgados
  escaneando** ([#20703](https://github.com/hrydgard/ppsspp/issues/20703), abierto). El maintainer
  sospecha que falta algún evento de Apctl. Causa probable, leyendo el código sin ejecutarlo (F20a)
  [?]: PPSSPP deja el estado en `SCANNING` 100 ms después de `SCAN_COMPLETE`, así que el reescaneo
  inmediato del juego recibe `NOT_DISCONNECTED`, el juego lo ignora y espera para siempre. Además,
  `__NetApctlCallbacks` usa referencias al frente de la cola después de `pop_front()`.
- **JPCSP funciona.** Su implementación es la que hay que estudiar.

**Plan para el port** [?]:
- Generar los soldados con una función determinista de (BSSID, SSID, señal, tiempo), guardando el
  historial en el save.
- Usar APs virtuales con semilla, y opcionalmente el escaneo real del sistema operativo (WlanAPI o
  nl80211).

### GPS (PSP-290)

- Solo existe en US y JP; EU lo quitó [S].
- Usaría `sceUsbGps`, que está implementado en
  [PPSSPP sceUsbGps.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceUsbGps.cpp).
- El port lo deja como stub.

### Online (revival de SaveMGO) [V]

- Hosts originales:
  - `mpoweb.konamionline.com` y `info.service.konamionline.com` (HTTP).
  - `mpostun{us,jp,eu}.konamionline.com` (STUN).
- Hosts de reemplazo: `mpo.savemgo.com` y `stun.mgo2pc.com`.
- El P2P va por UDP 5730.
- PPSSPP lo soporta desde el
  [PR #20670](https://github.com/hrydgard/ppsspp/pull/20670), con `infra-dns.json`.
- Guía: [snakeswiss/MPO-Guide](https://github.com/snakeswiss/MPO-Guide/blob/main/FAQ/faq.md).
- Modos: VR es casual. RC es ranked, y otros jugadores pueden capturarte soldados.

### Otros recursos útiles para el RE

- [NickRPGreen autosplitter](https://github.com/NickRPGreen/Metal-Gear-Solid-Portable-Ops-Autosplitter):
  direcciones de RAM del IGT, el área y el estado (US, PAL y JP).
- [TheFloW RemasteredControls](https://github.com/TheOfficialFloW/RemasteredControls): hooks de la
  cámara con el stick derecho.
- [Freakler camera_patch_lite](https://github.com/Freakler/psp-camera_patch_lite).
- Referencias de la VFPU y del hardware:
  [pspdev/vfpu-docs](https://github.com/pspdev/vfpu-docs),
  [psdevwiki CPU](https://playstationdev.wiki/pspdevwiki/index.php?title=CPU) y
  [Copetti: PSP](https://www.copetti.org/writings/consoles/playstation-portable/).

---

## 5. Huecos que quedan para M0

1. Confirmar que `EBOOT.BIN` es `mgp_main`, si está cifrado (casi seguro que sí, con ~PSP) y cuál es
   su base de carga. **F01 [V]**: está cifrado (`~PSP`) y el nombre de módulo en la cabecera es
   `mgp_main`. **F02 [V]**: es un PRX relocatable y PPSSPP lo carga en `0x08804000` (ver §4,
   Código). Resuelto.
2. Ver si los `stage/*.prx` están cifrados y entender el formato `.rlc`: ¿es una relocación propia
   de Konami además de la de PRX? **F01 [V]**: los 106 `stage/<s>/<s>.prx` son **ELF sin cifrar**, y
   **no hay ningún `.rlc` en el disco** (cada etapa trae solo `<s>.prx` y `_zar`). Hay que averiguar
   de dónde sale el `.rlc` (F04). **F04 [V]**: va dentro del `_zar`. Es una relocación propia de
   Konami, con el formato de `PT_PRXRELOC`, que enlaza la etapa con `mgp_main` y complementa las
   relocaciones del PRX (ver `docs/formats/rlc.md`). La aplica `mgp_main` (`0x08805C0C`). Resuelto.
3. Hacer el inventario de imports por librería y versión de firmware (`PSP_SYSTEM_VER`).
   **F03 [V]**: inventario completo de `mgp_main` y de los módulos de Konami en
   `docs/analysis/mgp_main.md` §5 y §7, y resumen de las 106 etapas en el §8. Resuelto para
   `mgp_main`.
4. Medir la VFPU, los branch-likely, los `jr`/`jalr` y los saltos sin resolver en `mgp_main` y en una
   etapa. **F03 [V]**: medido en `mgp_main` y en los tres módulos de Konami
   (`docs/analysis/mgp_main.md` §4 y §7). Las etapas quedan para F04 y F06.
5. Averiguar si `KCEJ_SOUND2` es un PRX aparte o va dentro del ejecutable. **F01 [V]**: hay un
   `USRDIR/modules/sound2.prx` aparte (ELF sin cifrar), junto a `kjfs.prx` y `zlibdec.prx`, y 22
   librerías de Sony cifradas (`~SCE`/`~PSP`). **F02 [V]**: `sound2.prx` es `KCEJ_SOUND2` y
   `kjfs.prx` es `KCEJ_FS`. Resuelto.
6. Ver si los GCX de MPO están cifrados.
7. Ver por qué el AP Scan se cuelga en PPSSPP, comparando contra JPCSP.
8. Revisar el repo y la versión de ghidra-allegrex. No se encontró en búsqueda; gow-psp-recomp usa
   "v21.4". **F03 [V]**: es [kotcrab/ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex)
   (Apache-2.0). La release `v21.4` (2026-08-25) es para Ghidra 12.1.3, la última versión de Ghidra
   que soporta. Resuelto.
9. Fuentes que fallaron en esta investigación: TCRF (sirvió contenido trampa), el wiki de fandom
   (402), GameFAQs y serialstation (bloqueados) y el hilo 4799 del foro de PPSSPP (404).
