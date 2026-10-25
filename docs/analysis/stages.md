# Inventario de etapas (`mgp_stage`) — `ULUS-10202` 1.02

Este es el inventario de los 106 overlays de etapa (`USRDIR/stage/<s>/<s>.prx` más el `<s>.rlc` que
va dentro de `_zar`). Solo tiene números y nombres. Todo es **[V]**: se midió en F04 (2026-10-25)
sobre los archivos validados con `supported.toml` y sobre el `mgp_main.elf` de F02. El formato del
`.rlc` está en [`docs/formats/rlc.md`](../formats/rlc.md).

## Lo que importa para el port

- **Hay 91 PRX distintos** para 106 etapas. F13 tendrá que recompilar como mucho 91 overlays.
- **Las etapas son casi el mismo programa.**
  - Entre los 91 PRX suman 54.2 MB de `.text`, 28 veces el `.text` de `mgp_main`.
  - Sin repetir, las funciones suman solo 2.0 MB (el 3.7 %).
  - Hay 57 PRX de misión "grandes" (más de 900 KB). Cualquier par de ellos comparte al menos el
    76 % de los bytes de `.text` (el peor caso es `st09` contra `st11_boss`). El 84 % del código de
    `st01` está en las 57. Entre `st01` y `st02`, el 96.7 % del código de `st01` está idéntico
    (salvo las direcciones) en `st02`.
  - Las 6 etapas `st` chicas (`st10_boss`, `st17_boss`, `st18_boss`, `st19_boss`,
    `st19_boss_demo` y `st20`) son otra cosa y comparten menos con las misiones (por ejemplo, el
    21.5 % de `st01` está en `st20`). Este dato y los anteriores salen de un script de prueba con
    el mismo método, no de `stage_inventory.py`.
  - Recompilar cada overlay entero sería, en tamaño, como recompilar 28 veces `mgp_main`. Hay que
    tenerlo en cuenta en el diseño de F06 y F13.
- **Prácticamente no copian código de `mgp_main`**: solo 270 funciones pequeñas (9 004 bytes). Lo
  llaman a través del `.rlc`.
- **Las etapas llaman a 1 493 funciones distintas de `mgp_main`.** 468 de ellas no son destino de
  ningún `jal`/`j` dentro de `mgp_main`, así que sirven como **semillas de funciones** para el
  análisis de `mgp_main` (F03 y F06).
- **La imagen más grande ocupa 0xFC400 bytes** (`st09`, con `.bss`). Es lo mínimo que hay que reservar
  para una etapa de misión.
- **No usan `$gp`**, ni las etapas ni `mgp_main`.
- **`mgp_main` entra a la etapa por punteros**. Los datos de cada etapa empiezan con una tabla
  ordenada de pares `(clave, puntero)`, con entre 3 y 232 entradas (16 531 en total). De ellas,
  8 114 apuntan a la propia etapa y 8 417 a `mgp_main`; esas 8 417 son todos los `R_MIPS_32` del
  `.rlc`. Ver `docs/formats/rlc.md`.
- **En Ghidra** (PRX enlazado en `0x08B8F900`, con el bloque de relleno de `mgp_main`):
  - `title`: 1 851 funciones, 100 % de `.text` decodificado, 0 `jr` sin resolver y 68 `jalr` sin
    resolver;
  - `st01`: 4 379 funciones, 99.99 %, 1 `jr` y 108 `jalr` sin resolver.
- **Los imports son pocos y todos de Sony.** Ninguna etapa importa de `mgp_main`, `kjfs`, `sound2` ni
  `zlibdec`. Las de misión usan solo `ThreadManForUser` (6 NIDs) y `sceSuspendForUser` (2). Las de
  red y las de video traen `sceNet*`, `sceHttp`, `sceUsbGps` o `sceMpeg`.

## Cómo se regenera

```bash
python -I profiles/mpo/tools/stage_inventory.py \
    --stages game/disc/PSP_GAME/USRDIR/stage --main game/elf/mgp_main.elf \
    --supported profiles/mpo/config/supported.toml
```

- Hace falta que `game/` tenga los `_zar`. `mpo --install` los extrae desde F04.
- Todo lo que hay a partir de "Resumen" es la salida del script, sin tocar.

## Cómo se mide el código compartido

1. Se toma el `.text` de cada PRX distinto (91; los repetidos cuentan una vez).
2. Se corta en "funciones". Cada función empieza en el inicio de `.text` o en un destino de llamada
   o puntero a código que aparece en las relocaciones propias, y termina donde empieza la siguiente.
3. Se enmascaran los campos que parchea una relocación, propia o del `.rlc`: el destino de
   `jal`/`j`, el inmediato de `lui`/`addiu` y las palabras de `R_MIPS_32`. Así, el código deja de
   depender de dónde se carga.
4. Se hashea cada función.

Dos funciones con el mismo hash son **idénticas salvo por las direcciones**: pueden llamar a otras
funciones o usar otros globales, pero tienen las mismas instrucciones. Los cortes son aproximados,
porque no se usó ningún análisis de flujo. La columna "compartido" de la tabla de etapas es el
porcentaje de bytes de `.text` que también aparecen en otro PRX distinto.

## Resumen

- Etapas: 106; PRX distintos (por SHA-256): 91.
- Hashes de `<s>.prx` y `_zar` iguales a `supported.toml`: 106 de 106.
- Nombre de módulo: mgp_stage.
- Bytes de PRX: 89822112 en total; 73551412 sin repetir.
- Entradas `.rlc`: 569819 en total; relocaciones propias (PT_PRXRELOC): 2470676.
- Imagen en memoria más grande: `st09`, 0xFC400 bytes (con `.bss`, redondeada a 0x100).
- Grupos de etapas con el mismo PRX: 11 (r_main01 = r_net01; st01t1 = st01t2; st0204 = st0205; st0602 = st0604; st07tn = st07ts; st08 = st0801 = st0802; st09 = st0902 = st0903; st13t0 = st13t1; st16 = st1601 = st1602; st17 = st1702 = st1703; st1803 = st1805).

### Imports y exports

- Imports `ThreadManForUser (6), sceSuspendForUser (2)`: 77 etapas.
- Imports `sceUtility (5)`: 12 etapas.
- Imports `—`: 4 etapas.
- Imports `sceUtility (5), sceSuspendForUser (2)`: 2 etapas.
- Imports `sceUtility (11), sceNet (1), sceNetAdhoc (8), sceNetAdhocDiscover (6), ThreadManForUser (4), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceNetApctl (3), sceUtility (11), sceNet (1), sceNetAdhoc (8), sceNetAdhocDiscover (6), ThreadManForUser (4), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceNetApctl (3), sceUsb (5), sceUtility (15), sceNet (1), sceNetAdhoc (8), sceRtc (1), sceNetAdhocDiscover (6), sceUsbGps (4), ThreadManForUser (10), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceUtility (15), sceNet (1), sceNetAdhoc (8), sceNetInet (3), sceRtc (7), sceHttp (22), sceParseUri (3), sceNetAdhocDiscover (6), ThreadManForUser (6), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceMpeg (23), sceUtility (9), sceRtc (1), ThreadManForUser (6), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceMpeg (23), ThreadManForUser (12), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceUsb (5), sceUtility (11), sceNet (1), sceNetAdhoc (8), sceNetAdhocDiscover (6), sceUsbGps (4), ThreadManForUser (5), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceUtility (9), sceNetAdhocMatching (2), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceHttp (21), sceParseUri (1), ThreadManForUser (1), sceSuspendForUser (2)`: 1 etapas.
- Imports `sceUtility (11), sceNet (1), sceNetAdhoc (8), sceNetInet (3), sceRtc (6), sceHttp (22), sceParseUri (3), sceNetAdhocDiscover (6), ThreadManForUser (5)`: 1 etapas.
- Imports `sceMpeg (23), sceUtility (13), sceRtc (1), IoFileMgrForUser (4), ThreadManForUser (12), sceSuspendForUser (2)`: 1 etapas.
- Exports `syslib: module_start module_stop module_info`: 106 etapas.

### Referencias a `mgp_main` (de los PRX distintos)

- Destinos de `jal`/`j` en `mgp_main`: 1493 direcciones distintas; 1025 también son destino de algún `jal`/`j` de `mgp_main` y 468 no lo son (semillas de funciones para el análisis de `mgp_main`).
- R_MIPS_32 hacia el segmento 0 de `mgp_main`: 7050 entradas.
- R_MIPS_HI16 hacia el segmento 0 de `mgp_main`: 130 entradas.
- R_MIPS_HI16 hacia el segmento 1 de `mgp_main`: 78707 entradas.
- R_MIPS_LO16 hacia el segmento 0 de `mgp_main`: 135 entradas.
- R_MIPS_LO16 hacia el segmento 1 de `mgp_main`: 103157 entradas.

### Código compartido entre etapas (`.text` de los PRX distintos)

- `.text` total: 54226588 bytes en 586080 funciones (28.1 veces el `.text` de `mgp_main`).
- Funciones distintas: 18561, que suman 2003576 bytes (3.7 % del total).
- Funciones idénticas a una de `mgp_main`: 270 (9004 bytes).
- Instrucciones con base `$gp` en `.text`: 0 en las etapas y 0 en `mgp_main`.

| Familia | Etapas | PRX distintos | `.text` (bytes) | Compartido con otra etapa | Exclusivo |
|---|---:|---:|---:|---:|---:|
| menús y demos | 13 | 13 | 2761508 | 92.3 % | 7.7 % |
| n (versus/red) | 12 | 12 | 2271340 | 100.0 % | 0.0 % |
| r_ e init (residentes) | 4 | 3 | 3332 | 94.0 % | 6.0 % |
| st (misiones) | 77 | 63 | 49190408 | 99.6 % | 0.4 % |

## Etapas

| Etapa | PRX | `.text` | seg 0 | seg 1 (file/mem) | imagen | gp | relocs PRX | `.rlc` | imports | compartido |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| `addscvr` | 253628 | 0x2E578 | 0x2FFCC | 0x1470/0x17F0 | 0x31800 | 0x9460 | 6285 | 1844 | 6 libs, 32 NIDs | 100 % |
| `apscan` | 269836 | 0x31718 | 0x332B8 | 0x14D8/0x1860 | 0x34C00 | 0x94D0 | 6667 | 2032 | 7 libs, 35 NIDs | 100 % |
| `briefing` | 440780 | 0x51300 | 0x54150 | 0x2144/0x2DD0 | 0x57000 | 0xA140 | 10787 | 4042 | 10 libs, 55 NIDs | 82 % |
| `cyberval2` | 569792 | 0x61034 | 0x6901C | 0x2F78/0x11500 | 0x7A600 | 0xAF70 | 15730 | 3744 | 10 libs, 73 NIDs | 98 % |
| `ending` | 203836 | 0x25584 | 0x266D0 | 0x10B4/0x2340 | 0x28B00 | 0x90B0 | 5073 | 1865 | 5 libs, 41 NIDs | 97 % |
| `flashdemo` | 216452 | 0x27410 | 0x286A0 | 0xDF8/0xDF80 | 0x36700 | 0x8DF0 | 5722 | 1835 | 3 libs, 37 NIDs | 95 % |
| `frvsmenu` | 176876 | 0x20014 | 0x20DA0 | 0x1010/0x1290 | 0x22100 | 0x9000 | 4581 | 1425 | 2 libs, 7 NIDs | 99 % |
| `gpstest` | 275620 | 0x3255C | 0x34358 | 0x150C/0x1890 | 0x35C00 | 0x9500 | 6852 | 2127 | 8 libs, 42 NIDs | 100 % |
| `gsclimenu` | 171100 | 0x1ED74 | 0x1FAF0 | 0xF8C/0x1210 | 0x20D00 | 0x8F80 | 4473 | 1288 | 2 libs, 7 NIDs | 98 % |
| `gsmenu` | 177844 | 0x201C0 | 0x21000 | 0x1030/0x12B0 | 0x22300 | 0x9020 | 4622 | 1421 | 3 libs, 13 NIDs | 98 % |
| `init` | 2556 | 0x294 | 0x320 | 0xF0/0xF0 | 0x500 | 0x80E0 | 28 | 52 | 0 libs, 0 NIDs | 70 % |
| `n01` | 248100 | 0x2E81C | 0x2F200 | 0x2E00/0x3380 | 0x32600 | 0xADF0 | 5218 | 3399 | 1 libs, 5 NIDs | 100 % |
| `n02` | 265780 | 0x3211C | 0x32B80 | 0x3060/0x35E0 | 0x36200 | 0xB050 | 5512 | 3632 | 1 libs, 5 NIDs | 100 % |
| `n03` | 250260 | 0x2EF80 | 0x2F96C | 0x2E18/0x33A0 | 0x32E00 | 0xAE10 | 5246 | 3453 | 1 libs, 5 NIDs | 100 % |
| `n05` | 244132 | 0x2DD20 | 0x2E678 | 0x2DCC/0x3350 | 0x31A00 | 0xADC0 | 5096 | 3389 | 1 libs, 5 NIDs | 100 % |
| `n09` | 242516 | 0x2D768 | 0x2E0B0 | 0x2DEC/0x3370 | 0x31500 | 0xADE0 | 5076 | 3357 | 1 libs, 5 NIDs | 100 % |
| `n11` | 245636 | 0x2E188 | 0x2EADC | 0x2DEC/0x3370 | 0x31F00 | 0xADE0 | 5140 | 3448 | 1 libs, 5 NIDs | 100 % |
| `n12` | 251924 | 0x2F460 | 0x2FED8 | 0x2E04/0x3390 | 0x33300 | 0xAE00 | 5282 | 3463 | 1 libs, 5 NIDs | 100 % |
| `n14` | 241700 | 0x2D47C | 0x2DDCC | 0x2DCC/0x3350 | 0x31200 | 0xADC0 | 5070 | 3324 | 1 libs, 5 NIDs | 100 % |
| `n15` | 239932 | 0x2CEC4 | 0x2D804 | 0x2D8C/0x3310 | 0x30C00 | 0xAD80 | 5041 | 3289 | 1 libs, 5 NIDs | 100 % |
| `n16` | 233060 | 0x2B77C | 0x2C074 | 0x2D64/0x32F0 | 0x2F400 | 0xAD60 | 4940 | 3163 | 1 libs, 5 NIDs | 100 % |
| `n17` | 242476 | 0x2D768 | 0x2E0B0 | 0x2DCC/0x3350 | 0x31400 | 0xADC0 | 5075 | 3354 | 1 libs, 5 NIDs | 100 % |
| `n18` | 251548 | 0x2F320 | 0x2FD98 | 0x2DEC/0x3370 | 0x33200 | 0xADE0 | 5279 | 3451 | 1 libs, 5 NIDs | 100 % |
| `ntconnect` | 158236 | 0x1C7F4 | 0x1D6B0 | 0xBBC/0xE40 | 0x1E500 | 0x8BB0 | 4147 | 1065 | 4 libs, 25 NIDs | 99 % |
| `ntmenu2` | 573296 | 0x5E9C4 | 0x6661C | 0x62A8/0x1D8B0 | 0x83F00 | 0xE2A0 | 15874 | 3657 | 9 libs, 65 NIDs | 73 % |
| `r_main01` | 3148 | 0x538 | 0x5CC | 0x28/0x30 | 0x600 | 0x8020 | 40 | 35 | 0 libs, 0 NIDs | 100 % |
| `r_main02` | 3132 | 0x538 | 0x5CC | 0x20/0x20 | 0x600 | 0x8010 | 40 | 34 | 0 libs, 0 NIDs | 100 % |
| `r_net01` | 3148 | 0x538 | 0x5CC | 0x28/0x30 | 0x600 | 0x8020 | 40 | 35 | 0 libs, 0 NIDs | 100 % |
| `st01` | 1138436 | 0xCD708 | 0xD2148 | 0x62B8/0x98C0 | 0xDBB00 | 0xE2B0 | 31388 | 7036 | 2 libs, 8 NIDs | 100 % |
| `st0100` | 1030828 | 0xB7F30 | 0xBC4B8 | 0x5A68/0x8E70 | 0xC5400 | 0xDA60 | 29357 | 5544 | 2 libs, 8 NIDs | 100 % |
| `st01t1` | 1138012 | 0xCD5A4 | 0xD1FE8 | 0x62B8/0x98C0 | 0xDB900 | 0xE2B0 | 31379 | 7022 | 2 libs, 8 NIDs | 100 % |
| `st01t2` | 1138012 | 0xCD5A4 | 0xD1FE8 | 0x62B8/0x98C0 | 0xDB900 | 0xE2B0 | 31379 | 7022 | 2 libs, 8 NIDs | 100 % |
| `st02` | 1171684 | 0xD2D30 | 0xD7CB8 | 0x65D8/0x9C60 | 0xE1A00 | 0xE5D0 | 32518 | 7125 | 2 libs, 8 NIDs | 100 % |
| `st0204` | 1171660 | 0xD2D30 | 0xD7CB8 | 0x65C8/0x9C50 | 0xE1A00 | 0xE5C0 | 32517 | 7125 | 2 libs, 8 NIDs | 100 % |
| `st0205` | 1171660 | 0xD2D30 | 0xD7CB8 | 0x65C8/0x9C50 | 0xE1A00 | 0xE5C0 | 32517 | 7125 | 2 libs, 8 NIDs | 100 % |
| `st03t1` | 1167420 | 0xD1D58 | 0xD6D68 | 0x64D8/0x9AE0 | 0xE0900 | 0xE4D0 | 32507 | 7011 | 2 libs, 8 NIDs | 100 % |
| `st03t2` | 1167396 | 0xD1D58 | 0xD6D68 | 0x64C8/0x9AD0 | 0xE0900 | 0xE4C0 | 32506 | 7011 | 2 libs, 8 NIDs | 100 % |
| `st04` | 1078100 | 0xC1348 | 0xC5B98 | 0x5E58/0x9260 | 0xCEE00 | 0xDE50 | 30312 | 6179 | 2 libs, 8 NIDs | 100 % |
| `st04m1` | 1132300 | 0xCAC18 | 0xCFA88 | 0x62E8/0x96F0 | 0xD9200 | 0xE2E0 | 31855 | 6466 | 2 libs, 8 NIDs | 100 % |
| `st04m3` | 1166628 | 0xD13A8 | 0xD66B8 | 0x6230/0x9630 | 0xDFD00 | 0xE220 | 32708 | 6817 | 2 libs, 8 NIDs | 100 % |
| `st04vo` | 1099556 | 0xC5630 | 0xCA1A8 | 0x5E08/0x9210 | 0xD3400 | 0xDE00 | 30762 | 6244 | 2 libs, 8 NIDs | 100 % |
| `st05` | 1147292 | 0xCEE18 | 0xD3C98 | 0x5F78/0x9380 | 0xDD100 | 0xDF70 | 31725 | 6944 | 2 libs, 8 NIDs | 100 % |
| `st0501` | 1147340 | 0xCEB94 | 0xD3A18 | 0x6108/0x9510 | 0xDD000 | 0xE100 | 31761 | 6930 | 2 libs, 8 NIDs | 100 % |
| `st0502` | 1099692 | 0xC5C64 | 0xCA578 | 0x5F48/0x9350 | 0xD3900 | 0xDF40 | 30617 | 6473 | 2 libs, 8 NIDs | 100 % |
| `st0503` | 1100444 | 0xC5C64 | 0xCA578 | 0x60D8/0x94E0 | 0xD3B00 | 0xE0D0 | 30661 | 6480 | 2 libs, 8 NIDs | 100 % |
| `st0504` | 1143052 | 0xCD0BC | 0xD1F28 | 0x6478/0x9880 | 0xDB800 | 0xE470 | 31977 | 6664 | 2 libs, 8 NIDs | 100 % |
| `st0505` | 1169580 | 0xD0DEC | 0xD5DB8 | 0x6C68/0xA070 | 0xDFF00 | 0xEC60 | 33037 | 6714 | 2 libs, 8 NIDs | 100 % |
| `st0506` | 1169452 | 0xD0DC0 | 0xD5D88 | 0x6C38/0xA040 | 0xDFE00 | 0xEC30 | 33033 | 6710 | 2 libs, 8 NIDs | 100 % |
| `st06` | 1090316 | 0xC3EB4 | 0xC8718 | 0x5ED8/0x94E0 | 0xD1C00 | 0xDED0 | 30431 | 6474 | 2 libs, 8 NIDs | 100 % |
| `st0601` | 1090460 | 0xC3EB4 | 0xC8718 | 0x5F48/0x9550 | 0xD1D00 | 0xDF40 | 30435 | 6483 | 2 libs, 8 NIDs | 100 % |
| `st0602` | 1134524 | 0xCB570 | 0xD0328 | 0x6498/0x9AA0 | 0xD9E00 | 0xE490 | 31803 | 6698 | 2 libs, 8 NIDs | 100 % |
| `st0604` | 1134524 | 0xCB570 | 0xD0328 | 0x6498/0x9AA0 | 0xD9E00 | 0xE490 | 31803 | 6698 | 2 libs, 8 NIDs | 100 % |
| `st0605` | 1160956 | 0xCF274 | 0xD4198 | 0x6C68/0xA270 | 0xDE500 | 0xEC60 | 32859 | 6746 | 2 libs, 8 NIDs | 100 % |
| `st07m1` | 1109076 | 0xC7770 | 0xCC098 | 0x6218/0x9620 | 0xD5700 | 0xE210 | 30832 | 6552 | 2 libs, 8 NIDs | 100 % |
| `st07m2` | 1107692 | 0xC73F0 | 0xCBD18 | 0x61A8/0x95B0 | 0xD5300 | 0xE1A0 | 30785 | 6529 | 2 libs, 8 NIDs | 100 % |
| `st07tn` | 1103948 | 0xC6640 | 0xCAF18 | 0x6218/0x9620 | 0xD4600 | 0xE210 | 30751 | 6489 | 2 libs, 8 NIDs | 100 % |
| `st07ts` | 1103948 | 0xC6640 | 0xCAF18 | 0x6218/0x9620 | 0xD4600 | 0xE210 | 30751 | 6489 | 2 libs, 8 NIDs | 100 % |
| `st08` | 1194884 | 0xD6148 | 0xDB238 | 0x6E08/0xA210 | 0xE5500 | 0xEE00 | 33444 | 7168 | 2 libs, 8 NIDs | 100 % |
| `st0801` | 1194884 | 0xD6148 | 0xDB238 | 0x6E08/0xA210 | 0xE5500 | 0xEE00 | 33444 | 7168 | 2 libs, 8 NIDs | 100 % |
| `st0802` | 1194884 | 0xD6148 | 0xDB238 | 0x6E08/0xA210 | 0xE5500 | 0xEE00 | 33444 | 7168 | 2 libs, 8 NIDs | 100 % |
| `st09` | 1311996 | 0xEBECC | 0xF1C08 | 0x7188/0xA790 | 0xFC400 | 0xF180 | 36393 | 8128 | 2 libs, 8 NIDs | 100 % |
| `st0902` | 1311996 | 0xEBECC | 0xF1C08 | 0x7188/0xA790 | 0xFC400 | 0xF180 | 36393 | 8128 | 2 libs, 8 NIDs | 100 % |
| `st0903` | 1311996 | 0xEBECC | 0xF1C08 | 0x7188/0xA790 | 0xFC400 | 0xF180 | 36393 | 8128 | 2 libs, 8 NIDs | 100 % |
| `st10` | 1209692 | 0xD79D0 | 0xDCF28 | 0x6E94/0xA4A0 | 0xE7400 | 0xEE90 | 34351 | 6920 | 2 libs, 8 NIDs | 100 % |
| `st10_boss` | 375364 | 0x45A0C | 0x47690 | 0x2850/0x36D0 | 0x4AE00 | 0xA840 | 8874 | 3598 | 2 libs, 8 NIDs | 80 % |
| `st10m1` | 1139948 | 0xCC610 | 0xD1498 | 0x6318/0x9920 | 0xDAE00 | 0xE310 | 31971 | 6666 | 2 libs, 8 NIDs | 100 % |
| `st11_boss` | 1130140 | 0xCB914 | 0xD0378 | 0x650C/0xA690 | 0xDAB00 | 0xE500 | 31231 | 6550 | 2 libs, 8 NIDs | 93 % |
| `st11t0` | 1166652 | 0xD1DE4 | 0xD6D38 | 0x6638/0x9C40 | 0xE0A00 | 0xE630 | 32373 | 7159 | 2 libs, 8 NIDs | 100 % |
| `st11t1` | 1166628 | 0xD1DE4 | 0xD6D38 | 0x6628/0x9C30 | 0xE0A00 | 0xE620 | 32372 | 7159 | 2 libs, 8 NIDs | 100 % |
| `st12tn` | 1124948 | 0xCAA50 | 0xCF478 | 0x6158/0x9760 | 0xD8C00 | 0xE150 | 31180 | 6828 | 2 libs, 8 NIDs | 100 % |
| `st12tnf` | 1124948 | 0xCAA50 | 0xCF478 | 0x6158/0x9760 | 0xD8C00 | 0xE150 | 31180 | 6828 | 2 libs, 8 NIDs | 100 % |
| `st12ts` | 1168228 | 0xD210C | 0xD7088 | 0x64F8/0x9B00 | 0xE0C00 | 0xE4F0 | 32504 | 7034 | 2 libs, 8 NIDs | 100 % |
| `st12tsf` | 1124948 | 0xCAA50 | 0xCF478 | 0x6158/0x9760 | 0xD8C00 | 0xE150 | 31180 | 6828 | 2 libs, 8 NIDs | 100 % |
| `st13t0` | 1122596 | 0xCA280 | 0xCEC78 | 0x6228/0x9830 | 0xD8500 | 0xE220 | 31116 | 6770 | 2 libs, 8 NIDs | 100 % |
| `st13t1` | 1122596 | 0xCA280 | 0xCEC78 | 0x6228/0x9830 | 0xD8500 | 0xE220 | 31116 | 6770 | 2 libs, 8 NIDs | 100 % |
| `st14` | 1118748 | 0xC99B8 | 0xCE3F8 | 0x5F48/0x9550 | 0xD7A00 | 0xDF40 | 30999 | 6757 | 2 libs, 8 NIDs | 100 % |
| `st1401` | 1118908 | 0xC9754 | 0xCE198 | 0x6108/0x9710 | 0xD7900 | 0xE100 | 31039 | 6744 | 2 libs, 8 NIDs | 100 % |
| `st1402` | 1188572 | 0xD4B14 | 0xD9C28 | 0x6C58/0xA260 | 0xE3F00 | 0xEC50 | 33415 | 6997 | 2 libs, 8 NIDs | 100 % |
| `st1403` | 1118812 | 0xC9754 | 0xCE198 | 0x60C8/0x96D0 | 0xD7900 | 0xE0C0 | 31035 | 6741 | 2 libs, 8 NIDs | 100 % |
| `st1404` | 1118828 | 0xC9754 | 0xCE198 | 0x60D8/0x96E0 | 0xD7900 | 0xE0D0 | 31035 | 6743 | 2 libs, 8 NIDs | 100 % |
| `st1405` | 1118060 | 0xC9754 | 0xCE198 | 0x5F38/0x9540 | 0xD7700 | 0xDF30 | 30991 | 6735 | 2 libs, 8 NIDs | 100 % |
| `st15` | 1117036 | 0xC90E0 | 0xCDB18 | 0x61D8/0x95E0 | 0xD7100 | 0xE1D0 | 30987 | 6684 | 2 libs, 8 NIDs | 100 % |
| `st1501` | 1117124 | 0xC910C | 0xCDB48 | 0x61E8/0x95F0 | 0xD7200 | 0xE1E0 | 30990 | 6684 | 2 libs, 8 NIDs | 100 % |
| `st1502` | 1186852 | 0xD44CC | 0xD95C8 | 0x6D68/0xA170 | 0xE3800 | 0xED60 | 33370 | 6938 | 2 libs, 8 NIDs | 100 % |
| `st16` | 1151828 | 0xCD62C | 0xD2538 | 0x6AA0/0x9EA0 | 0xDC400 | 0xEA90 | 32684 | 6540 | 2 libs, 8 NIDs | 100 % |
| `st1601` | 1151828 | 0xCD62C | 0xD2538 | 0x6AA0/0x9EA0 | 0xDC400 | 0xEA90 | 32684 | 6540 | 2 libs, 8 NIDs | 100 % |
| `st1602` | 1151828 | 0xCD62C | 0xD2538 | 0x6AA0/0x9EA0 | 0xDC400 | 0xEA90 | 32684 | 6540 | 2 libs, 8 NIDs | 100 % |
| `st17` | 1093220 | 0xC46E8 | 0xC8FA8 | 0x5F50/0x9550 | 0xD2500 | 0xDF40 | 30506 | 6505 | 2 libs, 8 NIDs | 100 % |
| `st1701` | 1093252 | 0xC46E8 | 0xC8FA8 | 0x5F60/0x9560 | 0xD2600 | 0xDF50 | 30508 | 6505 | 2 libs, 8 NIDs | 100 % |
| `st1702` | 1093220 | 0xC46E8 | 0xC8FA8 | 0x5F50/0x9550 | 0xD2500 | 0xDF40 | 30506 | 6505 | 2 libs, 8 NIDs | 100 % |
| `st1703` | 1093220 | 0xC46E8 | 0xC8FA8 | 0x5F50/0x9550 | 0xD2500 | 0xDF40 | 30506 | 6505 | 2 libs, 8 NIDs | 100 % |
| `st17_boss` | 431972 | 0x4F994 | 0x51ABC | 0x21E4/0x42F0 | 0x55E00 | 0xA1E0 | 10900 | 3536 | 2 libs, 8 NIDs | 100 % |
| `st18` | 1096076 | 0xC517C | 0xC9AA8 | 0x5CF8/0x9300 | 0xD2E00 | 0xDCF0 | 30585 | 6479 | 2 libs, 8 NIDs | 100 % |
| `st1801` | 1101556 | 0xC6058 | 0xCA9D8 | 0x5F08/0x9510 | 0xD3F00 | 0xDF00 | 30718 | 6518 | 2 libs, 8 NIDs | 100 % |
| `st1802` | 1122028 | 0xCA4C8 | 0xCEEE8 | 0x5F30/0x9530 | 0xD8500 | 0xDF20 | 31063 | 6791 | 2 libs, 8 NIDs | 100 % |
| `st1803` | 1092956 | 0xC4424 | 0xC8D38 | 0x5E68/0x9470 | 0xD2200 | 0xDE60 | 30579 | 6419 | 2 libs, 8 NIDs | 100 % |
| `st1804` | 1091836 | 0xC42E4 | 0xC8BF8 | 0x5CB8/0x92C0 | 0xD1F00 | 0xDCB0 | 30533 | 6400 | 2 libs, 8 NIDs | 100 % |
| `st1805` | 1092956 | 0xC4424 | 0xC8D38 | 0x5E68/0x9470 | 0xD2200 | 0xDE60 | 30579 | 6419 | 2 libs, 8 NIDs | 100 % |
| `st1806` | 1092820 | 0xC4424 | 0xC8D38 | 0x5E18/0x9420 | 0xD2200 | 0xDE10 | 30572 | 6417 | 2 libs, 8 NIDs | 100 % |
| `st1807` | 1093596 | 0xC4688 | 0xC8F98 | 0x5E58/0x9460 | 0xD2400 | 0xDE50 | 30585 | 6439 | 2 libs, 8 NIDs | 100 % |
| `st18_boss` | 462460 | 0x55C38 | 0x57EBC | 0x2264/0x4370 | 0x5C300 | 0xA260 | 11495 | 3862 | 2 libs, 8 NIDs | 100 % |
| `st19` | 1106148 | 0xC711C | 0xCBA28 | 0x5F28/0x9530 | 0xD5000 | 0xDF20 | 30766 | 6643 | 2 libs, 8 NIDs | 100 % |
| `st19_boss` | 347780 | 0x3F014 | 0x41020 | 0x35D8/0x44E0 | 0x45500 | 0xB5D0 | 8270 | 3154 | 2 libs, 8 NIDs | 85 % |
| `st19_boss_demo` | 236388 | 0x2AE3C | 0x2C140 | 0x1238/0x1F40 | 0x2E100 | 0x9230 | 6202 | 1999 | 2 libs, 8 NIDs | 100 % |
| `st20` | 383300 | 0x46A6C | 0x48940 | 0x1F18/0x3AA0 | 0x4C400 | 0x9F10 | 9562 | 3179 | 2 libs, 8 NIDs | 79 % |
| `title` | 301244 | 0x37370 | 0x38C80 | 0x1CB0/0x3940 | 0x3C600 | 0x9CA0 | 7467 | 2880 | 6 libs, 55 NIDs | 92 % |
