# Análisis de `mgp_main` (ULUS-10202, disco 1.02)

Resultado de la fase F03. Aquí van **solo números, nombres y direcciones**: nada de código del juego,
desensamblado ni texto extraído. Todo se reproduce con las herramientas de `profiles/mpo/tools/` (ver
`profiles/mpo/README.md`). Las etiquetas siguen `CLAUDE.md` §11: **[V]** verificado en el binario,
**[S]** de una fuente externa y **[?]** sin verificar o inferido.

## 1. Cómo se obtuvo

- **Entrada**: `game/elf/mgp_main.elf` (2 693 412 bytes, SHA-256 `ca700ea5…6185cadf8`), que deja
  `mpo --install`.
- **Herramientas**: Ghidra 12.1.3 con ghidra-allegrex v21.4 (loader `PspElfLoader`, "PSP Executable
  (ELF)"), sobre Temurin JDK 21.0.13+9, en modo headless.
- **Importación**: relocado en **`0x08804000`**, con las relocaciones PRX (`PT_PRXRELOC`) aplicadas
  por el loader: 76 371 relocaciones.
- **Pipeline** (`profiles/mpo/tools/ghidra/run_analysis.sh`):
  1. auto-análisis de Ghidra con las opciones por defecto;
  2. siembra de funciones desde las relocaciones (`MpoSeedFunctions.java relocations`);
  3. siembra desde prólogos `addiu sp, sp, -N` en bytes sin decodificar (`prologues`);
  4. siembra desde el inicio de cada hueco y del código sin función (`gaps`);
  5. nombres de imports con la tabla de NIDs de PPSSPP `v1.20.4` (`MpoModuleTables.java`);
  6. métricas, export para el recompilador y anclajes (`MpoMetrics.java`,
     `MpoExportRecompInput.java`, `MpoFindAnchors.java`).
- **Tabla de NIDs**: `build_nid_table.py` sobre `Core/HLE/*.cpp` de PPSSPP `v1.20.4` (commit
  `fa50bb19…`): 1 884 entradas, 1 629 NIDs distintos, 104 librerías (SHA-256 del TSV
  `b4b455d9…2f5317`).
- Los módulos de Konami (`sound2.prx`, `kjfs.prx`, `zlibdec.prx`) se analizaron igual, en las bases
  que muestra PPSSPP (§7).

## 2. Identidad y bloques [V]

- `sceModuleInfo`: nombre `mgp_main`, atributo `0x0000`, versión 1.1, `gp` = `0x08819150`
  (`0x15150` relativo).
- `module_start` (`0x08804128`) declara **SDK `0x02080010`** (`sceKernelSetCompiledSdkVersion`) y
  **compilador `0x30306`** (`sceKernelSetCompilerVersion`, o sea GCC 3.3.6) [V].
- No exporta funciones propias: solo `module_start` (`0x08804128`), `module_stop` (`0x088044C0`),
  `module_info`, `module_start_thread_parameter` y `module_stop_thread_parameter`.

| Bloque | Inicio | Tamaño | Flags |
|---|---|---|---|
| `.init` | `0x08804000` | `0x24` | r-x |
| `.text` | `0x08804040` | `0x1D6828` (1 927 208 bytes) | r-x |
| `.fini` | `0x089DA868` | `0x1C` | r-x |
| `.sceStub.text` | `0x089DA884` | `0x548` | r-x |
| `.lib.ent` | `0x089DADD4` | `0x10` | r-- |
| `.lib.stub` | `0x089DADEC` | `0x280` | r-- |
| `.rodata.sceModuleInfo` | `0x089DB070` | `0x34` | r-- |
| `.rodata.sceResident` | `0x089DB0A4` | `0x36C` | r-- |
| `.rodata.sceNid` | `0x089DB410` | `0x2A4` | r-- |
| `.ctors` / `.dtors` / `.jcr` | `0x089DB6B4` | `0x8` / `0x8` / `0x4` | rw- |
| `.eh_frame` | `0x089DB6C8` | `0x64` | r-- |
| `.rodata` | `0x089DB730` | `0x175E4` | rw- |
| `.data` | `0x089F2D40` | `0xD154` | rw- |
| `.bss` | `0x089FFEC0` | `0xFB900` | rw- (sin inicializar) |

## 3. Cobertura del desensamblado [V]

`.text` mide 1 927 208 bytes. "Decodificado" es la fracción de bytes que quedan como instrucciones;
"sin relleno" descuenta los bytes en cero que no se decodificaron (alineación); "en funciones" es la
fracción que cae dentro del cuerpo de alguna función.

| Paso | Funciones (total) | Funciones en `.text` | Instrucciones en `.text` | Decodificado | Sin relleno | En funciones | Bytes ≠0 sin decodificar | Huecos ≠0 |
|---|---|---|---|---|---|---|---|---|
| Ghidra (auto-análisis) | 5 835 | 5 676 | 431 151 | 89.49 % | 91.68 % | 72.62 % | 156 478 | 823 |
| + relocaciones | 6 799 | 6 635 | 433 636 | 90.00 % | 92.10 % | 83.61 % | 148 751 | 748 |
| + prólogos | 7 404 | 7 234 | 467 259 | 96.98 % | 97.70 % | 90.43 % | 44 054 | 443 |
| **+ huecos (final)** | **8 484** | **8 313** | **481 653** | **99.97 %** | **99.98 %** | **99.80 %** | **312** | **3** |

- **Ghidra solo se queda en el 89.5 %.** Sigue el flujo desde las entradas, y una buena parte del
  código solo se alcanza por punteros (tablas de `jalr`) o no se alcanza desde `mgp_main` (código
  muerto, o código que llaman las etapas a través del `.rlc`, ver §8).
- **Relocaciones**: de 2 251 candidatos, 944 se sembraron en la primera ronda (567 por punteros en
  datos y 377 por pares `lui`/`addiu` en código) y 3 en la segunda. Se descartaron 946 que caen
  dentro de una función (las etiquetas de los `switch`) y 103 que no parecen el inicio de una
  función.
- **Prólogos**: 536 funciones más.
- **Huecos**: 1 078 funciones más en 16 rondas (los huecos que siguen a un `jr ra` o a relleno, y el
  código sin función, típicamente el destino de un `j` de tail call). Dos candidatos se rechazaron
  porque su desensamblado daba error.
- Los 3 huecos que quedan suman 312 bytes no nulos: `0x089500B4` (16 bytes), `0x0899156C`
  (372 bytes) y `0x089CFF78` (48 bytes). Este último es una de las funciones que el juego parchea al
  arrancar (§6.6).
- 275 funciones tienen nombre de análisis (`ANALYSIS`) y 240 son stubs de import (`IMPORTED`).
- El objetivo de gow-psp-recomp (99.88 % de `.text`) se alcanza, pero **solo con los tres pasos de
  siembra**. Las funciones sembradas por los pasos 3 y 4 son heurísticas: F06 tiene que compararlas
  con `psp_analyze`.

## 4. Instrucciones [V]

Clasificadas por los bits del opcode (no por el mnemónico de Ghidra), sobre todos los bloques
ejecutables al final del pipeline.

| Clase | Cantidad |
|---|---|
| Instrucciones | 482 007 |
| Funciones | 8 484 |
| **VFPU (total)** | **27 657** (5.7 % de las instrucciones) |
| VFPU: loads/stores (`lv.s`/`lv.q`/`sv.s`/`sv.q`/`lvl`/`lvr`/`svl`/`svr`) | 18 225 |
| VFPU: prefijos `vpfxs` / `vpfxt` / `vpfxd` | 47 / 23 / 279 |
| VFPU: `viim` / `vfim` | 5 / 2 |
| Funciones con al menos una instrucción VFPU | 1 114 |
| Mnemónicos VFPU distintos | 89 |
| FPU (COP1, con `lwc1`/`swc1`) | 51 431 |
| Funciones con FPU | 1 932 |
| **Branch-likely (total)** | **7 709** |
| — `beql` / `bnel` / `blezl` / `bgtzl` | 6 443 |
| — REGIMM (`bltzl` / `bgezl` / `bltzall` / `bgezall`) | 209 |
| — `bc1fl` / `bc1tl` | 1 055 |
| — `bvfl` / `bvtl` | 2 |
| `jr ra` (retornos) | 8 427 |
| `jr <reg>` resueltos (tablas de saltos) | 176 |
| **`jr <reg>` sin resolver** | **1** (`0x088A77C8`) |
| `jalr` resueltos | 2 |
| `jalr` sin resolver (punteros a función, vtables) | 256 |
| `syscall` | 0 (los imports van por stubs) |
| `break` | 252 |
| `cache` | 3 (§6.6) |
| `sync`, `ll`/`sc`, COP0, SPECIAL2 (`halt`/`mfic`/`mtic`) | 0 |
| `lwl`/`lwr`/`swl`/`swr` | 1 420 |
| SPECIAL3 (`ext`/`ins`/`seb`/`seh`/`wsbh`/`wsbw`/`bitrev`) | 3 142 |
| `movz`/`movn` | 1 449 |
| `min`/`max` | 202 |
| `clz`/`clo` | 12 |
| `madd(u)`/`msub(u)` | 0 |

- **El único `jr` sin resolver** (`0x088A77C8`, en `0x088A77B4`) es un `switch` que Ghidra partió en
  dos funciones: la comprobación de rango queda en `0x088A7780` y el salto indexa la tabla
  `0x089EA590`. Se resuelve con una entrada en el perfil.
- **VFPU**: las más usadas son `lv.q` (9 392), `sv.q` (6 769), `lv.s` (1 397), `vidt.q` (927),
  `vscl.q` (748), `vdot.t` (734), `sv.s` (667), `vrot.q` (628), `vzero.s` (512), `vsub.q` (499),
  `vmul.s` (489), `vmmul.q` (433), `vcmp.s` (410), `vcmovt.s` (379), `vadd.q` (374) y `vcst.s`
  (312).
  - Funciones transcendentes, que tienen que copiar las tablas o aproximaciones de PPSSPP:
    `vrsq.s` (280), `vsqrt.s` (236), `vcos.s` (98), `vsin.s` (92), `vdiv.s` (19), `vasin.s` (2),
    `vrcp.s`/`vrcp.t` (2 y 2), `vsin.t` (2), `vcos.t` (1) y `vsin.q` (1).
  - Otras: `mtv` (75), `mfvc` (26), `vflush` (31), `bvt` (28) y `bvf` (21).
  - Los prefijos son pocos (349) y casi todos `vpfxd` (saturación del destino).
  - La VFPU **sí importa**: hay unas 23 veces más instrucciones que en God of War (~1 200 en 81
    funciones, `RESEARCH.md` §3). El emisor necesita la VFPU completa desde F07.

## 5. Imports y exports [V]

`mgp_main` importa **240 funciones de 32 librerías**, con 532 llamadas en total.

- 169 NIDs son de Sony y están en la tabla de PPSSPP `v1.20.4`. **PPSSPP implementa 165**; los otros
  4 son `sceUtilityHtmlViewer*`, que en PPSSPP están en la tabla pero sin implementar.
- Los otros **71 NIDs son de los módulos de Konami** y no están en ninguna tabla: 20 de `kjfs`, 48
  de `sound2` y 3 de `zlibdec`. Coinciden uno a uno con los exports de esos PRX (§7).
- Ninguna variable importada.
- Los stubs de los imports de Konami **no están en `.sceStub.text` sino dentro de `.text`**
  (`0x089C9A18–0x089C9C50`). Las herramientas los localizan desde la tabla `.lib.stub`.

| Librería | Funciones | PPSSPP: impl. | PPSSPP: sin impl. | NID desconocido | Llamadas |
|---|---|---|---|---|---|
| `ThreadManForUser` | 22 | 22 | 0 | 0 | 100 |
| `sound2` (Konami) | 48 | 0 | 0 | 48 | 74 |
| `sceRtc` | 3 | 3 | 0 | 0 | 43 |
| `sceGe_user` | 11 | 11 | 0 | 0 | 31 |
| `sceUtility` | 13 | 9 | 4 | 0 | 26 |
| `UtilsForUser` | 7 | 7 | 0 | 0 | 26 |
| `sceNetInet` | 14 | 14 | 0 | 0 | 24 |
| `Kernel_Library` | 2 | 2 | 0 | 0 | 24 |
| `kjfs` (Konami) | 20 | 0 | 0 | 20 | 18 |
| `sceLibFont` | 8 | 8 | 0 | 0 | 18 |
| `StdioForUser` | 3 | 3 | 0 | 0 | 15 |
| `ModuleMgrForUser` | 7 | 7 | 0 | 0 | 13 |
| `sceNetApctl` | 7 | 7 | 0 | 0 | 12 |
| `sceUmdUser` | 2 | 2 | 0 | 0 | 12 |
| `IoFileMgrForUser` | 7 | 7 | 0 | 0 | 11 |
| `sceNetAdhoc` | 7 | 7 | 0 | 0 | 11 |
| `sceDisplay` | 5 | 5 | 0 | 0 | 10 |
| `sceNetAdhocctl` | 11 | 11 | 0 | 0 | 10 |
| `sceNet` | 5 | 5 | 0 | 0 | 9 |
| `SysMemUserForUser` | 6 | 6 | 0 | 0 | 7 |
| `sceNetResolver` | 5 | 5 | 0 | 0 | 7 |
| `sceHttp` | 7 | 7 | 0 | 0 | 7 |
| `sceWlanDrv` | 2 | 2 | 0 | 0 | 4 |
| `InterruptManager` | 3 | 3 | 0 | 0 | 3 |
| `sceOpenPSID` | 1 | 1 | 0 | 0 | 3 |
| `sceSuspendForUser` | 3 | 3 | 0 | 0 | 3 |
| `zlibdec` (Konami) | 3 | 0 | 0 | 3 | 3 |
| `sceCtrl` | 2 | 2 | 0 | 0 | 2 |
| `LoadExecForUser` | 2 | 2 | 0 | 0 | 2 |
| `sceSsl` | 2 | 2 | 0 | 0 | 2 |
| `sceImpose` | 1 | 1 | 0 | 0 | 1 |
| `scePower` | 1 | 1 | 0 | 0 | 1 |
| **Total** | **240** | **165** | **4** | **71** | **532** |

Versión de librería `0x0011` en todas las de Sony y `0x0000` en las de Konami. Atributos: `0x4001`
(la mayoría), `0x4000` (`InterruptManager`, `SysMemUserForUser`, `sceSuspendForUser`), `0x0009`
(red, fuentes y Konami) y `0x0001` (`Kernel_Library`).

### Funciones importadas por librería (Sony)

| Librería | Funciones |
|---|---|
| `ThreadManForUser` | `sceKernelSetEventFlag`, `sceKernelCreateSema`, `sceKernelAllocateFpl`, `sceKernelCreateCallback`, `sceKernelDeleteFpl`, `sceKernelDeleteEventFlag`, `sceKernelStartThread`, `sceKernelFreeFpl`, `sceKernelDeleteSema`, `sceKernelSignalSema`, `sceKernelWaitEventFlag`, `sceKernelCreateThread`, `sceKernelWaitSema`, `sceKernelCreateEventFlag`, `sceKernelPollSema`, `sceKernelDelayThreadCB`, `sceKernelWaitSemaCB`, `sceKernelExitDeleteThread`, `sceKernelClearEventFlag`, `sceKernelGetSystemTimeWide`, `sceKernelExitThread`, `sceKernelCreateFpl` |
| `UtilsForUser` | `sceKernelDcacheWritebackAll`, `sceKernelDcacheWritebackInvalidateRange`, `sceKernelGetGPI`, `sceKernelDcacheWritebackRange`, `sceKernelLibcGettimeofday`, `sceKernelLibcClock`, `sceKernelLibcTime` |
| `ModuleMgrForUser` | `sceKernelUnloadModule`, `sceKernelGetModuleIdByAddress`, `sceKernelStartModule`, `sceKernelLoadModuleByID`, `sceKernelStopModule`, `sceKernelGetModuleId`, `sceKernelStopUnloadSelfModuleWithStatus` |
| `SysMemUserForUser` | `sceKernelSetCompiledSdkVersion`, `sceKernelSetCompilerVersion`, `sceKernelPrintf`, `sceKernelAllocPartitionMemory`, `sceKernelGetBlockHeadAddr`, `sceKernelFreePartitionMemory` |
| `IoFileMgrForUser` | `sceIoWrite`, `sceIoDevctl`, `sceIoClose`, `sceIoOpen`, `sceIoRead`, `sceIoRename`, `sceIoLseek` |
| `InterruptManager` | `sceKernelRegisterSubIntrHandler`, `sceKernelReleaseSubIntrHandler`, `sceKernelEnableSubIntr` |
| `Kernel_Library` | `sceKernelCpuSuspendIntr`, `sceKernelCpuResumeIntr` |
| `LoadExecForUser` | `sceKernelExitGame`, `sceKernelRegisterExitCallback` |
| `StdioForUser` | `sceKernelStdin`, `sceKernelStdout`, `sceKernelStderr` |
| `sceSuspendForUser` | `sceKernelPowerTick`, `sceKernelVolatileMemLock`, `sceKernelVolatileMemUnlock` |
| `sceGe_user` | `sceGeListSync`, `sceGeUnsetCallback`, `sceGeListEnQueueHead`, `sceGeEdramGetSize`, `sceGeContinue`, `sceGeSetCallback`, `sceGeListEnQueue`, `sceGeDrawSync`, `sceGeBreak`, `sceGeListUpdateStallAddr`, `sceGeEdramGetAddr` |
| `sceDisplay` | `sceDisplaySetMode`, `sceDisplayGetAccumulatedHcount`, `sceDisplaySetFrameBuf`, `sceDisplayWaitVblankStartCB`, `sceDisplayGetFrameBuf` |
| `sceCtrl` | `sceCtrlSetSamplingMode`, `sceCtrlReadBufferPositive` |
| `sceUmdUser` | `sceUmdWaitDriveStatCB`, `sceUmdGetDriveStat` |
| `sceRtc` | `sceRtcGetAccumulativeTime`, `sceRtcGetCurrentTick`, `sceRtcGetCurrentClockLocalTime` |
| `sceLibFont` | `sceFontFindOptimumFont`, `sceFontGetFontInfo`, `sceFontClose`, `sceFontDoneLib`, `sceFontNewLib`, `sceFontOpen`, `sceFontGetCharGlyphImage_Clip`, `sceFontGetCharInfo` |
| `sceUtility` | `sceUtilityLoadAvModule`, `sceUtilityLoadNetModule`, `sceUtilityUnloadNetModule`, `sceUtilityGetSystemParamString`, `sceUtilityGetSystemParamInt`, `sceUtilityNetconfInitStart`, `sceUtilityNetconfGetStatus`, `sceUtilityNetconfUpdate`, `sceUtilityNetconfShutdownStart`, `sceUtilityHtmlViewerInitStart`\*, `sceUtilityHtmlViewerUpdate`\*, `sceUtilityHtmlViewerGetStatus`\*, `sceUtilityHtmlViewerShutdownStart`\* |
| `sceImpose` | `sceImposeSetLanguageMode` |
| `scePower` | `scePowerRegisterCallback` |
| `sceOpenPSID` | `sceOpenPSIDGetOpenPSID` |
| `sceWlanDrv` | `sceWlanGetEtherAddr`, `sceWlanGetSwitchState` |
| `sceNet` | `sceNetInit`, `sceNetTerm`, `sceNetFreeThreadinfo`, `sceNetEtherNtostr`, `sceNetThreadAbort` |
| `sceNetApctl` | `sceNetApctlInit`, `sceNetApctlTerm`, `sceNetApctlAddHandler`, `sceNetApctlDelHandler`, `sceNetApctlGetState`, `sceNetApctlGetInfo`, `sceNetApctlDisconnect` |
| `sceNetAdhoc` | `sceNetAdhocInit`, `sceNetAdhocTerm`, `sceNetAdhocPdpCreate`, `sceNetAdhocPdpDelete`, `sceNetAdhocPdpSend`, `sceNetAdhocPdpRecv`, `sceNetAdhocSetSocketAlert` |
| `sceNetAdhocctl` | `sceNetAdhocctlInit`, `sceNetAdhocctlTerm`, `sceNetAdhocctlScan`, `sceNetAdhocctlGetScanInfo`, `sceNetAdhocctlAddHandler`, `sceNetAdhocctlDelHandler`, `sceNetAdhocctlGetState`, `sceNetAdhocctlJoin`, `sceNetAdhocctlCreate`, `sceNetAdhocctlDisconnect`, `sceNetAdhocctlGetPeerList` |
| `sceNetInet` | `sceNetInetInit`, `sceNetInetTerm`, `sceNetInetSocket`, `sceNetInetClose`, `sceNetInetBind`, `sceNetInetConnect`, `sceNetInetSelect`, `sceNetInetSend`, `sceNetInetSendto`, `sceNetInetRecv`, `sceNetInetRecvfrom`, `sceNetInetSetsockopt`, `sceNetInetInetAton`, `sceNetInetGetErrno` |
| `sceNetResolver` | `sceNetResolverInit`, `sceNetResolverTerm`, `sceNetResolverCreate`, `sceNetResolverDelete`, `sceNetResolverStartNtoA` |
| `sceHttp` | `sceHttpInit`, `sceHttpEnd`, `sceHttpsInit`, `sceHttpsEnd`, `sceHttpsLoadDefaultCert`, `sceHttpLoadSystemCookie`, `sceHttpSaveSystemCookie` |
| `sceSsl` | `sceSslInit`, `sceSslEnd` |

\* En la tabla de PPSSPP, sin implementar.

**Lo que no está en `mgp_main`** [V]:

- **Nada de audio de Sony** (`sceAudio`, `sceSas`, `sceAtrac`): todo el audio pasa por `sound2`.
- **Nada de `sceMpeg`/`scePsmf`, savedata, OSK ni diálogos** (`sceUtilitySavedata`, `Osk`,
  `MsgDialog`). Esos imports están en los PRX de etapa (§8).
- **Ni `sceKernelLoadModule` ni `sceKernelIcacheInvalidate*`.** Los módulos se cargan con
  `sceKernelLoadModuleByID` sobre un descriptor abierto antes.
- **Ni el escaneo de APs** (`sceNetApctlScanUser`, `GetBSSDescIDListUser`, `GetBSSDescEntryUser`):
  está en la etapa `apscan` (§6.5).

## 6. Anclajes

Las direcciones son de `mgp_main` relocado en `0x08804000`. Los nombres entre comillas son el rol
que se dedujo, no símbolos del juego.

### 6.1 Arranque, hilos y bucle principal [V]

| Qué | Dirección | Notas |
|---|---|---|
| `module_start` | `0x08804128` | Fija las versiones de SDK y compilador y crea el hilo **`user_main`** (entrada `0x088041FC`, prioridad `0x20`, pila `0x40000`, atributo `0x80000000` = usuario). |
| Entrada de `user_main` | `0x088041FC` | Inicializa el heap `UserSbrk` (`0x089DA428`), el runtime de C (constructores) y llama a `0x08804834`. |
| Lanzador del hilo principal | `0x08804834` | **Parchea tres funciones** (§6.6), hace `sceKernelDcacheWritebackAll` y crea el hilo **`Main`** (entrada `0x088045A0`, prioridad `0x30`, pila `0x20000`, atributo `0x4000` = VFPU). |
| Entrada de `Main` | `0x088045A0` | Callback de salida, inicialización de memoria (`0x0883E098`), `sceKernelGetGPI` (interruptores de desarrollo), carga de módulos (`0x08804510`), `kjfs`, GE y vídeo (`0x08843BF0`), GCL (`0x0884340C`), sonido (`0x088109F0`) y por último el bucle. |
| **Bucle principal** | `0x0883F464` | `Main` lo llama en un `while (1)`. Recorre 16 listas de tareas por prioridad y llama al `update` de cada una. La espera de frame (§6.2) es una de esas tareas. |
| Inicialización de memoria | `0x0883E098` | Crea la FPL `mgp_main` (partición 2, atributo `0x4000` = memoria alta, un bloque de casi 16 MiB) y el semáforo `memsema`, y fija el modo de frame inicial a 1 (30 fps). |

### 6.2 Ritmo de frame: el divisor de FPS y el timestep [V]

**El código CWCheat `0x201F5210` está verificado**: escribe `0x089F5210`, una variable de `.data`
con el **mínimo de vblanks por frame**.

- **Lectores y escritores**: lo leen 3 funciones y lo escriben 2:
  - `0x08843A20`, la **espera de frame**, lo lee. Llama a `sceDisplayWaitVblankStartCB` hasta que el
    contador de vblanks llega al objetivo del frame y lo pone a cero. El objetivo siguiente es el
    mayor entre este mínimo y los vblanks que tardó el frame, con un tope de 6.
  - `0x08843BF0` (inicialización del GE y del vídeo) lo escribe con 2.
  - `0x08843DD8(modo)` lo escribe con 1, 2, 3 o 4 para los modos 0 a 3.
- **El contador de vblanks** (`0x08A312FC`) lo incrementa `0x08843A04`, un handler de
  sub-interrupción que registra `0x08843BF0`. Por eso hacen falta `sceKernelRegisterSubIntrHandler`
  y `sceKernelEnableSubIntr` con el vblank en F09.
- **Selector del modo de frame** (`0x0883DBB0(modo)`). Fija todo el ritmo a la vez:

  | Modo | Vblanks por frame (`0x089F5210`) | Timestep (`0x089F4FD8`) | `0x08A18E6C` (fps) | `0x089F4FDC` |
  |---|---|---|---|---|
  | 0 | 1 | 5 | 60 | 1 |
  | 1 | 2 | 10 | 30 | 2 |
  | 2 | 3 | 15 | 20 | 2 |
  | 3 | 4 | 20 | 15 | 4 |

  - El modo 2 es el **gameplay a 20 fps** (divisor 3) y el modo 1 es el de arranque (30 fps).
  - Cuadra con las tres opciones del cheat: 3, 2 o 1 en el divisor y `0xF`, `0xA` o 5 en el
    timestep.
- **El timestep `0x089F4FD8`** (CWCheat `0x201F4FD8`, también verificado) vale 5 por cada vblank del
  frame. Lo leen **233 funciones** (412 lecturas) y lo escriben 10: el selector del modo y otras 9,
  que lo cambian por su cuenta [?].
  - Es el "delta" que se resta a los contadores de la lógica, y de ahí sale que la lógica dependa
    del framerate (`RESEARCH.md` §4, Timing).
- **El aspect ratio del cheat de widescreen** (`0x201E9728` = `0x089ED728`) cae en `.rodata`. Ghidra
  solo encuentra una referencia, en `0x08926E5C`, que lo usa como constante float dentro de un
  cálculo [?]. Queda para M7.

### 6.3 Intérprete GCL [V en las tablas, ? en los roles]

El intérprete se localizó por sus **tablas de comandos**: pares (`strcode24(nombre)`, puntero al
handler). Los hashes coinciden con el `strcode()` de `Jayveer/Gcx` (`mgs/common/strcode.h`).

| Tabla | Dirección | Entradas | Comandos con nombre conocido | Descriptor |
|---|---|---|---|---|
| Comandos base | `0x089F51D4` | 6 | `if`, `switch`, `eval`, `call`, `return`, `print` | `{6, 0x089F51D4}` en `0x089F5208` |
| Comandos del juego | `0x089F2F34` | 7 | `load`, `command`, `chara`, `restart`, `trap`, `mesg` (y otro con hash `0x2BF16E`) | `{7, 0x089F2F34}` en `0x089F2F70` |

- Los handlers de los comandos base están en `0x08843764–0x088438C8`. Llaman a rutinas del rango
  `0x08841800–0x088439FF`, que es el **núcleo del intérprete** [?]:
  - `0x088418E0` lee una opción por letra, como el `GCL_GetOption` de MGS [?];
  - `0x08841BB8` y `0x08841BF4` leen valores, y `0x08841C78`, cadenas [?];
  - `0x088423DC` ejecuta un bloque [?].
- **Inicialización del GCL** [?]: `0x0884340C`, que `Main` llama justo después del GE.
- Ghidra no encuentra referencias directas a los descriptores. Se registran de forma indirecta y
  hay que seguirlos con más calma cuando haga falta.
- Hay **65 tablas más con la misma forma** (hash de 24 bits y puntero a código, con 6 entradas o
  más) en `.rodata`/`.data`. La de `0x089EC0B4` tiene 213 entradas, y probablemente es la de
  actores, `chara` [?].

### 6.4 Sistema de archivos, `data.cnf` y módulos [V]

| Qué | Dirección | Notas |
|---|---|---|
| Cargador de módulos residentes | `0x08805F30(índice)` | Arma `<disco>modules/<nombre>.prx` y lo abre con `sceIoOpen` (o con `kjfs_93E9297F` cuando `kjfs` está activo). Espera al UMD y hace `sceKernelLoadModuleByID` + `sceKernelStartModule`. Lleva un contador de referencias por módulo. |
| Tabla de módulos | `0x089F2DF4` | 22 entradas de 12 bytes (nombre, ID, refcount): `audiocodec`, `sc_sascore`, `libatrac3plus`, `sound2`, `videocodec`, `mpegbase`, `mpeg`, `usbpspcm`, `ifhandle`, `pspnet`, `pspnet_adhoc`, `memab`, `pspnet_adhoc_auth`, `pspnet_adhocctl`, `kjnet`, `pspnet_adhoc_matching`, `pspnet_inet`, `pspnet_apctl`, `pspnet_ap_dialog_dummy`, `libfont`, `kjfs`, `zlibdec`. |
| Carga al arrancar | `0x08804510` | `sceUtilityLoadAvModule(0..3)` y después los módulos 21, 3, 20 y 19: **`zlibdec`, `sound2`, `kjfs` y `libfont`**, en ese orden. Coincide con el orden de direcciones de PPSSPP. |
| Inicialización de `kjfs` | `0x088045A0` | `kjfs_2FCD7551` o `kjfs_C78E5271`, con el callback `0x08840230`. Elige una u otra según `sceKernelGetGPI` y los dispositivos disponibles [?]. |
| Llamadas a `kjfs` | `0x08804A58–0x08805200`, `0x089A83BC`, `0x089A8450`, `0x088573EC` | 13 de los 20 exports tienen llamadas. |
| `data.cnf` | cadenas en `0x089E94B0` y `0x089EFE5C` | Las usan `0x0883DA20` y `0x089A83BC`; esta última también usa `stage/` y `kjfs_813E06AD`. |
| Rutas | `0x089F2DB4–0x089F2DE4` | Tabla de prefijos de dispositivo: `host0:`, `disc0:/PSP_GAME/USRDIR/`, `disc0:` y `umd1:`. `fatms0:` se usa con `sceIoDevctl`. |
| Archivos con nombre | — | `slot.dat`, `codec.dat`, `face.dat`, `movie.dat`, `demo.dat` y `SHDEstage.dat`. |
| `zlibdec` | `0x0884C60C` | La única función que llama a los 3 exports de `zlibdec`. |

### 6.5 Red y AP Scan [V]

- **El AP Scan no está en `mgp_main`.** Las tres funciones del escaneo (`sceNetApctlScanUser`,
  `sceNetApctlGetBSSDescIDListUser` y `sceNetApctlGetBSSDescEntryUser`) las importan **dos PRX de
  etapa: `apscan` y `briefing`** (`prx_imports.py`).
  - `apscan` también importa `sceUtilityOsk*` y `sceUtilityMsgDialog*`, `sceNetAdhocPtp*`,
    `sceNetAdhocDiscover*` y `sceNetGetLocalEtherAddr`.
- **`mgp_main` tiene la parte común de `sceNetApctl`**:
  - `0x0885D27C`: `sceNetInit`, `sceUtilityLoadNetModule`, `sceNetApctlInit`,
    `sceNetApctlAddHandler` y `sceKernelCreateFpl`;
  - `0x0885D5F0`: el cierre, con `Disconnect`, `DelHandler`, `Term`, `sceNetTerm` y
    `UnloadNetModule`;
  - `0x0885D9A4`: `sceNetApctlGetInfo` y `Netconf`;
  - `0x0885C868`: `sceNetApctlGetState`.
- `sceWlanGetSwitchState` se llama en `0x08866370` y `sceWlanGetEtherAddr` en `0x08838CA8`,
  `0x0883914C` y `0x0885D7A4`.
- `sceUsbGps` lo importan las etapas `briefing` y `gpstest`.

### 6.6 Caché de instrucciones y código que se modifica [V]

- **`mgp_main` no importa `sceKernelIcacheInvalidate*`.** Pero hay dos casos que el recompilador
  tiene que tratar:
  1. **Parche al arrancar**. `0x08804834` escribe `jr ra; nop` sobre las dos primeras instrucciones
     de tres funciones: `0x089CFE80` (con argumentos variables), `0x089D002C` y `0x089CFF78`.
     Parecen de salida de depuración [?]. Después hace `sceKernelDcacheWritebackAll`.
     - El perfil tiene que tratarlas como **retorno inmediato**. Si no, el código recompilado
       ejecutaría su cuerpo original.
  2. **Enlace de la etapa**. `0x08805E1C` hace lo siguiente:
     - arranca la etapa con `sceKernelStartModule(id, 4, &info)`;
     - aplica una tabla de relocaciones de 8 bytes por entrada contra las bases de `mgp_main`
       (`0x08804000`) y de su `.data` (`0x089F2D40`), en `0x08805C0C`;
     - hace `sceKernelDcacheWritebackAll` e **invalida toda la caché de instrucciones** con
       `cache 0x04` cada 64 bytes durante 16 KiB (`0x08805EC0`);
     - y salta a un puntero que deja la etapa.
     - Es el `.rlc`: **el juego parchea el código de la etapa al cargarla**, y eso es trabajo de F04
       (§8).
- Los otros `cache`:
  - `0x089C5E24` usa `cache 0x1B` (Hit Writeback Invalidate D), en el código del GE;
  - PPSSPP trata `0x04` como no-op y solo invalida su JIT con `0x08`.
- `sound2` sí importa `sceKernelIcacheInvalidateRange`, pero solo en un helper que lee un archivo a
  un buffer (`0x08B24AD0`/`0x08B24BD0`). Para datos de sonido no tiene efecto [?].

### 6.7 GE, vídeo y entrada [V]

- El GE y el vídeo se inicializan en `0x08843BF0`: `0x089C5AD8`, que pone `sceGeSetCallback` y
  `ListEnQueue`, y `sceDisplaySetMode` en `0x089C622C`.
- El envoltorio de las display lists está en `0x089C5AD8–0x089C6D50` (`sceGeListEnQueue`,
  `EnQueueHead`, `ListSync` y `DrawSync`).
- `sceDisplaySetFrameBuf` se llama en `0x089C622C`, `0x089C6308` y `0x089C6BB4`.
- `sceCtrlSetSamplingMode` se llama en `0x08840334` y `sceCtrlReadBufferPositive` en `0x08840390`.

### 6.8 Sonido (lado de `mgp_main`) [V]

- `0x088109F0` inicializa `sound2`. Le pasa **tres callbacks de `mgp_main`** (`0x088109E8`,
  `0x088109A0` y `0x088109C4`) con `sound2_206AEA6D`, y después fija volúmenes y canales.
- El resto de las llamadas a `sound2` se concentran en `0x08810700–0x08812A64`, más `0x088073E4` y
  `0x08996418`/`0x089964F8`.

## 7. Módulos de Konami (`USRDIR/modules/`) [V]

| | `sound2.prx` | `kjfs.prx` | `zlibdec.prx` |
|---|---|---|---|
| Módulo | `KCEJ_SOUND2` | `KCEJ_FS` | `ZLIB` |
| Base en PPSSPP | `0x08B24100` | `0x08B75500` | `0x08B20000` |
| Código + datos (memsz) | `0x18580` + `0x38E08` | `0xDD40` + `0x3C30` | `0x2F50` + `0x116C` |
| Funciones (Ghidra → final) | 420 → 531 | 187 → 230 | 18 → 24 |
| Instrucciones | 24 027 | 13 491 | 2 655 |
| Cobertura final (decodificado / en funciones) | 99.95 % / 99.95 % | 99.94 % / 99.94 % | 100 % / 100 % |
| VFPU / FPU | 75 (5 funciones) / 288 | 0 / 0 | 0 / 0 |
| Branch-likely | 389 | 249 | 42 |
| `jr` resueltos / sin resolver | 12 / 0 | 2 / 0 | 3 / 0 |
| `jalr` sin resolver | 16 | 14 | 3 |
| Exports | librería `sound2`, 48 funciones | librería `kjfs`, 20 funciones | librería `zlibdec`, 3 funciones |
| Imports | 82 de 11 librerías, **todos implementados en PPSSPP** | `zlibdec` (3) y 53 de Sony, todos implementados | ninguno |

- **`sound2` importa**:
  - `sceSasCore` (21: `__sceSasInit`, `__sceSasCore`, `__sceSasCoreWithMix`, `SetVoice`,
    `SetPitch`, `SetVolume`, `SetADSR`, `SetADSRmode`, `SetSimpleADSR`, `SetSL`, `SetKeyOn`,
    `SetKeyOff`, `SetPause`, `SetNoise`, `GetEndFlag`, `GetPauseFlag`, `GetEnvelopeHeight`,
    `RevType`, `RevParam`, `RevEVOL` y `RevVON`);
  - `sceAtrac3plus` (9: `SetDataAndGetID`, `DecodeData`, `GetSoundSample`, `GetMaxSample`,
    `SetLoopNum`, `GetLoopStatus`, `ResetPlayPosition`, `Reinit` y `ReleaseAtracID`);
  - `sceAudio` (9: `ChReserve`, `ChRelease`, `OutputBlocking`, `OutputPanned`,
    `OutputPannedBlocking`, `ChangeChannelConfig`, `ChangeChannelVolume`, `SetChannelDataLen` y
    `GetChannelRestLength`);
  - además, `IoFileMgrForUser` (8, incluido `sceIoOpenAsync`), `ThreadManForUser` (17, con VPL
    `SndHeapVPL` y los semáforos `SndSema`, `SndAtracDecodeSema`, `SndLoadSema` y `SndSeqSema`),
    `sceUmdUser` y `UtilsForUser` (con `sceKernelIcacheInvalidateRange`).
- **`kjfs` importa** I/O asíncrono (`sceIoOpenAsync`, `ReadAsync`, `LseekAsync`, `IoctlAsync`,
  `PollAsync` y `WaitAsyncCB`), `sceIoDopen`/`Dread`, `sceUmdActivate` y FPL. **No importa
  `sceKernelLoadModule`**: solo `Stop`/`Unload` para sí mismo.
- Los tamaños cuadran con la lista de módulos de PPSSPP, redondeados a `0x100`.
- **El hueco de `0x24800` bytes entre `mgp_main` y `ZLIB`** (F02) queda explicado:
  - `0x800` de alineación;
  - **`UserSbrk`**: `sceKernelAllocPartitionMemory(2, "UserSbrk", LowAligned, 0x10000, 0x1000)`, en
    `0x089DA428` desde `user_main`;
  - los bloques que PPSSPP reserva para `sceUtilityLoadAvModule`: `av_atrac3plus` `0x8000` y
    `av_mpegbase` `0xC000` (`ModuleLoadInfo` en `sceUtility.cpp`; `av_avcodec` y `av_sascore` miden
    0).
  - En total, `0x800 + 0x10000 + 0x8000 + 0xC000 = 0x24800`. Para obtener las mismas direcciones que
    PPSSPP, el kernel HLE (F08) tiene que reproducir ese orden de reservas.

## 8. Lo que F04 necesita (no se hizo aquí)

- **Carga de la etapa**:
  - `0x08804CC4` descarga la etapa anterior (`sceKernelStopModule` + `UnloadModule`) y carga la
    nueva con `sceKernelLoadModuleByID` sobre el descriptor `0x08A00030`;
  - el archivo se lee por trozos con `0x08804C3C` → `0x08804A90`, que usa `kjfs`;
  - la ruta sale de `%sstage/%s/` (`0x089E311C`, en `0x08804B0C`).
- **Enlace y arranque**: `0x08805E1C`, desde `0x0883D6E4` (§6.6). La tabla de 8 bytes por entrada,
  con su tamaño en `0x08A00154`/`0x08A00158`, es la candidata a ser el `.rlc`. La aplica
  `0x08805C0C` con las bases `0x08804000` y `0x089F2D40`.
- **Las etapas no importan nada de `mgp_main`** (`prx_imports.py` sobre las 106: solo exportan
  `module_start`/`module_stop`/`module_info`). Todo enlace con `mgp_main` pasa por esa tabla.
- **Imports de las 106 etapas** (cuántas importan cada librería):
  - `sceSuspendForUser` 89, `ThreadManForUser` 87 y `sceUtility` 23;
  - `sceNet`, `sceNetAdhoc` y `sceNetAdhocDiscover` 6 cada una, y `sceRtc` 5;
  - `sceMpeg` 3 (`title`, `ending` y `flashdemo`), `sceHttp` 3 y `sceParseUri` 3;
  - `sceUsbGps` 2, `sceUsb` 2, `sceNetInet` 2 y `sceNetApctl` 2;
  - `sceNetAdhocMatching` 1 (`gsmenu`) e `IoFileMgrForUser` 1 (`title`).

## 9. Decisión sobre `KCEJ_SOUND2`: **recompilar** [V en los datos]

**Se recompila `sound2.prx` como un módulo más, igual que `kjfs.prx` y `zlibdec.prx`, y se hace HLE
de lo que hay debajo (`sceSasCore`, `sceAtrac3plus` y `sceAudio`) en F16.**

**Por qué:**

- **Es pequeño y limpio**:
  - 531 funciones y 24 027 instrucciones, el 5 % de `mgp_main`;
  - 0 `jr` sin resolver y 16 `jalr`;
  - 75 instrucciones VFPU.
- **Todo lo que importa ya tiene referencia**: sus 82 imports son de Sony y PPSSPP implementa los
  82. PPSSPP ejecuta este mismo código, no lo emula con HLE, y el juego queda "Perfect".
- **Hacer HLE de su API sería RE a ciegas**:
  - Son 48 entradas con NIDs propios de Konami, sin nombre en ninguna base de datos.
  - Detrás hay un secuenciador, streaming ATRAC y bancos de SAS cuyo formato no conocemos.
  - Además, `mgp_main` le pasa **callbacks propios** (§6.8) que el HLE tendría que respetar.
  - No hay implementación de referencia contra la que comparar.
- **Recompilar mantiene la lógica de sonido bit a bit** y concentra el trabajo de F16 en una capa
  documentada y con código de referencia (el `sceSas` y el `sceAtrac` de PPSSPP).

**Qué exige** (brecha para F06):

- El runtime tiene que **cargar varios módulos recompilados residentes** (`mgp_main`, `zlibdec`,
  `sound2`, `kjfs` y `libfont`; el último es de Sony y va por HLE).
- Tiene que **resolver los imports entre ellos por NID**: los 71 imports de Konami de `mgp_main`, y
  los 3 de `zlibdec` que importa `kjfs`.
- Hay que fijar las bases para que sean deterministas, idealmente las mismas que en PPSSPP (§7).
- Es la misma maquinaria que necesitan los overlays de etapa, pero sin descarga.

**Alternativas descartadas:**

- **HLE de `sound2`**: por lo anterior.
- **Reemplazar solo `zlibdec` por zlib nativa**: sus 3 exports son, casi seguro, `inflateInit_`,
  `inflate` e `inflateEnd` [S: PPSSPP #18904]. Es una optimización posible, pero no hace falta.
  Recompilarlo cuesta 24 funciones.

## 10. Implicaciones para el recompilador (F06–F07)

- **Semillas**: los pasos 3 y 4 (`prologues` y `gaps`) añaden 1 614 funciones que Ghidra no
  encuentra. `MpoExportRecompInput.java` las exporta con sus rangos.
- **Tablas de saltos**: 176 resueltas. Falta el `switch` partido de `0x088A77C8` (tabla
  `0x089EA590`).
- **`jalr`**: 256 sin resolver. Van por el dispatch `addr→fn`, que tiene que conocer las 8 484
  entradas.
- **Overrides del perfil**:
  - tratar `0x089CFE80`, `0x089D002C` y `0x089CFF78` como retorno inmediato (§6.6);
  - el `cache 0x04` es un no-op.
- **VFPU**: 27 657 instrucciones en 1 114 funciones y 89 mnemónicos. Los prefijos son raros (349).
  Hay que cubrir las transcendentes con la semántica de PPSSPP.
- **FPU**: 51 431 instrucciones (10.7 %), con 1 055 `bc1fl`/`bc1tl`.
- **Branch-likely**: 7 709 (1.6 %). La regla de delay slot de `CLAUDE.md` §5 aplica en todas.
