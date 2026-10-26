# Estructura del disco — `ULUS-10202` (disco 1.02)

Lista de archivos del UMD de **MGS: Portable Ops (USA, disco 1.02)**: solo nombres y tamaños, nada
del contenido. Todo lo de esta página es **[V]**: se leyó con `mpo --list` del ISO del usuario,
validado con `mpo --install` contra `profiles/mpo/config/supported.toml` (F01, 2026-10-24).

## Identidad de la imagen

| Dato | Valor |
|---|---|
| `PARAM.SFO` | `DISC_ID = ULUS10202`, `DISC_VERSION = 1.02`, `TITLE = METAL GEAR SOLID PORTABLE OPS`, `PSP_SYSTEM_VER = 2.81` |
| PVD | System ID `PSP GAME`, volume ID vacío, creado `2006-10-29 18:04:02 +09:00` (igual que [redump 4536](http://redump.org/disc/4536/)) |
| Tamaño | 1 393 426 432 bytes = 680 384 sectores de 2048 (igual que redump) |
| Entradas | 637 (523 archivos y 114 directorios) |

- **El ISO del usuario no es idéntico al de redump.** Tiene el mismo tamaño y la misma fecha del PVD,
  pero distinto MD5 y SHA-1. El ISO del usuario tiene MD5 `5df4c19724ea51e904ee175f5118a5d9` y
  redump `843867e814664dedc82252fda1d05e8c`. La diferencia que se ve es que
  `SYSDIR/UPDATE/{DATA.BIN, EBOOT.BIN, PARAM.SFO}` y `SYSDIR/BOOT.BIN` están **llenos de ceros**,
  algo habitual en los ISOs de internet.
  - Por eso `supported.toml` **ignora** `PSP_GAME/SYSDIR/UPDATE/` (el actualizador de firmware de
    Sony) y `PSP_GAME/SYSDIR/BOOT.BIN`. El juego no usa ninguno de los dos.
  - [?] Falta confirmar con un volcado limpio que los otros 519 archivos coinciden con el disco de
    redump.

## Lo que importa para el port

- **`PSP_GAME/SYSDIR/EBOOT.BIN`** (2 693 760 bytes) está cifrado, con cabecera `~PSP`, y el nombre
  de módulo es **`mgp_main`**.
- **No hay archivos `.rlc` sueltos en el disco.** F04 [V]: cada `<etapa>.rlc` va dentro del
  `_zar` de su etapa (es la última entrada del DAR). Ver [`rlc.md`](rlc.md).
- **106 etapas** en `USRDIR/stage/<etapa>/`, cada una con **`<etapa>.prx` y `_zar`**, nada más.
  - Los `.prx` de etapa son **ELF sin cifrar** (`\x7fELF`). Ocupan entre 2 556 y 1 311 996 bytes
    (mediana 1 103 948) y suman 89 822 112 bytes.
- **25 módulos** en `USRDIR/modules/`:
  - `kjfs.prx` (seguramente `KCEJ_FS`), `sound2.prx` (seguramente `KCEJ_SOUND2`) y `zlibdec.prx` son
    **ELF sin cifrar**. Responde en parte a la pregunta de `RESEARCH.md` §5.5: el driver de sonido es
    un PRX aparte. Falta confirmar el nombre interno del módulo en F02 o F03.
  - Los otros 22 (`lib*.prx`, `psmf.prx`, `libpsmfplayer.prx`, `scan.prx`, `show_macaddr.prx` y
    `pspnet_ap_dialog_dummy.prx`) son librerías de Sony con cabecera `~SCE` seguida de `~PSP`
    (cifradas). `scan.prx` es candidato para el AP Scan (F20).
- **30 videos PMF** en `USRDIR/mpg/` (342 MB), entre ellos `MPO_OP*.pmf`, `staffroll.pmf` y
  `sNN_MM*.pmf`. Hay **cinemáticas en video además del título**, así que `sceMpeg`/`scePsmf` es más
  importante de lo que suponía `CLAUDE.md` §4 (afecta a F17).
- **242 streams `.sep`** en `USRDIR/oss/` (85 MB): música (`oss_bgm_*`) y otros sonidos. El formato
  está en [`sep.md`](sep.md) (F16a).
- `USRDIR/slot.dat` (209 MB) y `USRDIR/codec.dat` (252 800 bytes).

## Cómo se regenera

```bash
mpo --list game/ULUS-10202.iso | python -I profiles/mpo/tools/disc_layout.py
```

El script genera la sección "Lista de archivos". El resto de la página se escribe a mano.

## Lista de archivos

### Resumen por directorio

| Directorio | Archivos | Bytes |
|---|---:|---:|
| `/` | 1 | 48 |
| `PSP_GAME` | 6 | 485 929 |
| `PSP_GAME/SYSDIR` | 2 | 5 387 172 |
| `PSP_GAME/SYSDIR/UPDATE` | 3 | 20 538 176 |
| `PSP_GAME/USRDIR` | 2 | 209 628 032 |
| `PSP_GAME/USRDIR/modules` | 25 | 411 196 |
| `PSP_GAME/USRDIR/mpg` | 30 | 341 575 680 |
| `PSP_GAME/USRDIR/oss` | 242 | 85 356 544 |
| `PSP_GAME/USRDIR/stage/*` | 212 | 363 198 803 |
| **Total** | **523** | **1 026 581 580** |

### `/`

| Archivo | Bytes |
|---|---:|
| `UMD_DATA.BIN` | 48 |

### `PSP_GAME`

| Archivo | Bytes |
|---|---:|
| `ICON0.PNG` | 5 804 |
| `ICON1.PMF` | 389 120 |
| `PARAM.SFO` | 472 |
| `PIC0.PNG` | 14 963 |
| `PIC1.PNG` | 7 350 |
| `SND0.AT3` | 68 220 |

### `PSP_GAME/SYSDIR`

| Archivo | Bytes |
|---|---:|
| `BOOT.BIN` | 2 693 412 |
| `EBOOT.BIN` | 2 693 760 |

### `PSP_GAME/SYSDIR/UPDATE`

| Archivo | Bytes |
|---|---:|
| `DATA.BIN` | 15 456 672 |
| `EBOOT.BIN` | 5 079 424 |
| `PARAM.SFO` | 2 080 |

### `PSP_GAME/USRDIR`

| Archivo | Bytes |
|---|---:|
| `codec.dat` | 252 800 |
| `slot.dat` | 209 375 232 |

### `PSP_GAME/USRDIR/modules`

| Archivo | Bytes |
|---|---:|
| `kjfs.prx` | 77 744 |
| `libadler.prx` | 1 088 |
| `libbase64.prx` | 1 472 |
| `libccc.prx` | 3 520 |
| `libdeflt.prx` | 4 416 |
| `libfont.prx` | 23 456 |
| `libfpu.prx` | 4 192 |
| `libheap.prx` | 3 392 |
| `libmd5.prx` | 2 400 |
| `libmp3.prx` | 4 000 |
| `libmt19937.prx` | 1 312 |
| `libpsmfplayer.prx` | 35 904 |
| `libqpcode.prx` | 1 952 |
| `libsha0.prx` | 2 576 |
| `libsha1.prx` | 2 576 |
| `libsha224.prx` | 2 928 |
| `libsha256.prx` | 2 928 |
| `libsha384.prx` | 4 672 |
| `libsha512.prx` | 4 672 |
| `psmf.prx` | 7 440 |
| `pspnet_ap_dialog_dummy.prx` | 2 448 |
| `scan.prx` | 31 792 |
| `show_macaddr.prx` | 29 520 |
| `sound2.prx` | 134 912 |
| `zlibdec.prx` | 19 884 |

### `PSP_GAME/USRDIR/mpg`

| Archivo | Bytes |
|---|---:|
| `MPO_OP.pmf` | 1 464 320 |
| `MPO_OP_LONG.pmf` | 18 708 480 |
| `MPO_OP_LOOP.pmf` | 17 285 120 |
| `MPO_loop.pmf` | 1 527 808 |
| `oce_tel.pmf` | 3 237 888 |
| `s01_01.pmf` | 1 843 200 |
| `s04_06.pmf` | 3 414 016 |
| `s05_05.pmf` | 5 625 856 |
| `s06_02.pmf` | 5 468 160 |
| `s09_02.pmf` | 4 280 320 |
| `s09_02b_02.pmf` | 3 999 744 |
| `s10_03a.pmf` | 1 400 832 |
| `s10_03b.pmf` | 10 676 224 |
| `s10_03c.pmf` | 14 538 752 |
| `s12_02_demo1.pmf` | 3 608 576 |
| `s12_02_demo2.pmf` | 10 348 544 |
| `s14_01_demo1a.pmf` | 2 367 488 |
| `s14_01_demo1b.pmf` | 9 304 064 |
| `s14_04.pmf` | 28 596 224 |
| `s14_05a.pmf` | 4 726 784 |
| `s14_05b.pmf` | 12 732 416 |
| `s14_05c.pmf` | 17 600 512 |
| `s15_09.pmf` | 21 630 976 |
| `s16_03a.pmf` | 2 299 904 |
| `s16_03b.pmf` | 6 373 376 |
| `s17_02.pmf` | 23 783 424 |
| `s17_03.pmf` | 5 898 240 |
| `s18_01.pmf` | 46 264 320 |
| `s19_01.pmf` | 8 271 872 |
| `staffroll.pmf` | 44 298 240 |

### `PSP_GAME/USRDIR/oss`

| Archivo | Bytes |
|---|---:|
| `oss_bgm_bgm01_01_01m.sep` | 4 096 |
| `oss_bgm_bgm01_01_02.sep` | 1 116 160 |
| `oss_bgm_bgm01_01_04.sep` | 663 552 |
| `oss_bgm_bgm01_01b.sep` | 4 096 |
| `oss_bgm_bgm01_03_a.sep` | 1 429 504 |
| `oss_bgm_bgm04_06_02.sep` | 929 792 |
| `oss_bgm_bgm04_06_04.sep` | 798 720 |
| `oss_bgm_bgm04_06_06.sep` | 950 272 |
| `oss_bgm_bgm06_02_a01.sep` | 485 376 |
| `oss_bgm_bgm06_02_a03m.sep` | 4 096 |
| `oss_bgm_bgm06_02_a_01.sep` | 4 096 |
| `oss_bgm_bgm06_02_b.sep` | 573 440 |
| `oss_bgm_bgm06_04_01.sep` | 266 240 |
| `oss_bgm_bgm06_04_02.sep` | 886 784 |
| `oss_bgm_bgm06_04_04.sep` | 612 352 |
| `oss_bgm_bgm06_04_05.sep` | 800 768 |
| `oss_bgm_bgm09_02_a01.sep` | 1 175 552 |
| `oss_bgm_bgm09_02_b03.sep` | 116 736 |
| `oss_bgm_bgm09_02_b04.sep` | 1 196 032 |
| `oss_bgm_bgm10_03_01.sep` | 1 024 000 |
| `oss_bgm_bgm10_03_04m.sep` | 940 032 |
| `oss_bgm_bgm12_03a.sep` | 4 096 |
| `oss_bgm_bgm12_03a_01.sep` | 890 880 |
| `oss_bgm_bgm12_03a_02.sep` | 219 136 |
| `oss_bgm_bgm12_03a_03.sep` | 851 968 |
| `oss_bgm_bgm12_03a_04.sep` | 1 245 184 |
| `oss_bgm_bgm12_03b1.sep` | 813 056 |
| `oss_bgm_bgm12_03b2.sep` | 409 600 |
| `oss_bgm_bgm12_03b5.sep` | 219 136 |
| `oss_bgm_bgm12_03b5_05.sep` | 219 136 |
| `oss_bgm_bgm12_03b_01.sep` | 813 056 |
| `oss_bgm_bgm12_03b_02.sep` | 409 600 |
| `oss_bgm_bgm13_01_01.sep` | 4 096 |
| `oss_bgm_bgm13_01_02.sep` | 763 904 |
| `oss_bgm_bgm13_01_03.sep` | 4 096 |
| `oss_bgm_bgm14_01a_1.sep` | 290 816 |
| `oss_bgm_bgm14_01a_2.sep` | 536 576 |
| `oss_bgm_bgm14_01a_4.sep` | 4 096 |
| `oss_bgm_bgm14_01b_01.sep` | 1 624 064 |
| `oss_bgm_bgm14_01b_03.sep` | 811 008 |
| `oss_bgm_bgm14_01b_04.sep` | 667 648 |
| `oss_bgm_bgm14_05_01m.sep` | 872 448 |
| `oss_bgm_bgm14_05_04.sep` | 643 072 |
| `oss_bgm_bgm14_05_05m.sep` | 931 840 |
| `oss_bgm_bgm14_05_08.sep` | 4 096 |
| `oss_bgm_bgm15_09_02.sep` | 327 680 |
| `oss_bgm_bgm15_09a.sep` | 4 096 |
| `oss_bgm_bgm15_09b.sep` | 327 680 |
| `oss_bgm_bgm16_03_01m.sep` | 1 185 792 |
| `oss_bgm_bgm16_03_05.sep` | 518 144 |
| `oss_bgm_bgm16_03_06.sep` | 616 448 |
| `oss_bgm_bgm16_03_08m.sep` | 4 096 |
| `oss_bgm_bgm17_02_01.sep` | 1 017 856 |
| `oss_bgm_bgm17_02_02.sep` | 376 832 |
| `oss_bgm_bgm17_02_03.sep` | 626 688 |
| `oss_bgm_bgm17_02_04.sep` | 1 200 128 |
| `oss_bgm_bgm17_02_06.sep` | 4 096 |
| `oss_bgm_bgm19_01_a1.sep` | 892 928 |
| `oss_bgm_bgm19_01_b1.sep` | 1 007 616 |
| `oss_bgm_bgm19_01tel.sep` | 1 210 368 |
| `oss_s01_01_01m.sep` | 2 048 |
| `oss_s01_01_02.sep` | 624 640 |
| `oss_s01_01_03.sep` | 497 664 |
| `oss_s01_01_04.sep` | 436 224 |
| `oss_s01_01_05.sep` | 233 472 |
| `oss_s01_01_b.sep` | 1 808 384 |
| `oss_s01_03_a.sep` | 794 624 |
| `oss_s01_03_b.sep` | 743 424 |
| `oss_s01_03_c.sep` | 548 864 |
| `oss_s04_06_01m.sep` | 2 048 |
| `oss_s04_06_02.sep` | 575 488 |
| `oss_s04_06_03.sep` | 122 880 |
| `oss_s04_06_04.sep` | 608 256 |
| `oss_s04_06_05.sep` | 174 080 |
| `oss_s04_06_06.sep` | 272 384 |
| `oss_s04_06_07.sep` | 526 336 |
| `oss_s06_02_a01.sep` | 360 448 |
| `oss_s06_02_a02.sep` | 49 152 |
| `oss_s06_02_a03m.sep` | 2 048 |
| `oss_s06_02_b.sep` | 530 432 |
| `oss_s06_04_01.sep` | 206 848 |
| `oss_s06_04_02.sep` | 395 264 |
| `oss_s06_04_03.sep` | 411 648 |
| `oss_s06_04_04.sep` | 591 872 |
| `oss_s06_04_05.sep` | 307 200 |
| `oss_s06_04_06.sep` | 411 648 |
| `oss_s09_02_a01.sep` | 436 224 |
| `oss_s09_02_a02m.sep` | 2 048 |
| `oss_s09_02_b01.sep` | 311 296 |
| `oss_s09_02_b02.sep` | 215 040 |
| `oss_s09_02_b02m.sep` | 2 048 |
| `oss_s09_02_b03.sep` | 165 888 |
| `oss_s09_02_b04.sep` | 784 384 |
| `oss_s09_02_b05.sep` | 378 880 |
| `oss_s09_02_b06.sep` | 59 392 |
| `oss_s10_03_01.sep` | 290 816 |
| `oss_s10_03_02.sep` | 245 760 |
| `oss_s10_03_03.sep` | 600 064 |
| `oss_s10_03_04m.sep` | 2 048 |
| `oss_s10_03_05.sep` | 288 768 |
| `oss_s10_03_06.sep` | 702 464 |
| `oss_s12_03a.sep` | 3 055 616 |
| `oss_s12_03a_01.sep` | 903 168 |
| `oss_s12_03a_02.sep` | 190 464 |
| `oss_s12_03a_03.sep` | 788 480 |
| `oss_s12_03a_04.sep` | 1 171 456 |
| `oss_s12_03b_01.sep` | 630 784 |
| `oss_s12_03b_02.sep` | 208 896 |
| `oss_s12_03b_03.sep` | 2 048 |
| `oss_s12_03b_04.sep` | 77 824 |
| `oss_s12_03b_05.sep` | 202 752 |
| `oss_s12_03b_1.sep` | 602 112 |
| `oss_s12_03b_2.sep` | 208 896 |
| `oss_s12_03b_3.sep` | 2 048 |
| `oss_s12_03b_4.sep` | 77 824 |
| `oss_s12_03b_5.sep` | 202 752 |
| `oss_s13_01_01.sep` | 546 816 |
| `oss_s13_01_02.sep` | 688 128 |
| `oss_s13_01_03.sep` | 225 280 |
| `oss_s14_01a_1.sep` | 266 240 |
| `oss_s14_01a_2.sep` | 2 048 |
| `oss_s14_01a_3.sep` | 364 544 |
| `oss_s14_01a_4.sep` | 2 048 |
| `oss_s14_01b_01.sep` | 350 208 |
| `oss_s14_01b_02.sep` | 325 632 |
| `oss_s14_01b_03.sep` | 751 616 |
| `oss_s14_01b_04.sep` | 598 016 |
| `oss_s14_05_01m.sep` | 2 048 |
| `oss_s14_05_02.sep` | 366 592 |
| `oss_s14_05_03.sep` | 282 624 |
| `oss_s14_05_04.sep` | 583 680 |
| `oss_s14_05_05m.sep` | 2 048 |
| `oss_s14_05_06.sep` | 106 496 |
| `oss_s14_05_07.sep` | 53 248 |
| `oss_s14_05_08m.sep` | 2 048 |
| `oss_s15_09_02.sep` | 290 816 |
| `oss_s15_09_a.sep` | 2 048 |
| `oss_s15_09_b.sep` | 290 816 |
| `oss_s16_03_01m.sep` | 2 048 |
| `oss_s16_03_02.sep` | 315 392 |
| `oss_s16_03_03.sep` | 233 472 |
| `oss_s16_03_04.sep` | 509 952 |
| `oss_s16_03_05.sep` | 479 232 |
| `oss_s16_03_06.sep` | 303 104 |
| `oss_s16_03_06a.sep` | 86 016 |
| `oss_s16_03_07.sep` | 126 976 |
| `oss_s16_03_08m.sep` | 2 048 |
| `oss_s17_02_01.sep` | 1 054 720 |
| `oss_s17_02_02.sep` | 389 120 |
| `oss_s17_02_03.sep` | 677 888 |
| `oss_s17_02_04.sep` | 755 712 |
| `oss_s17_02_05.sep` | 471 040 |
| `oss_s17_02_06m.sep` | 2 048 |
| `oss_s19_01_a1.sep` | 118 784 |
| `oss_s19_01_a2.sep` | 260 096 |
| `oss_s19_01_a3m.sep` | 2 048 |
| `oss_s19_01_b1.sep` | 174 080 |
| `oss_s19_01_b2.sep` | 315 392 |
| `oss_s19_01_b3.sep` | 131 072 |
| `oss_s19_01_b4.sep` | 22 528 |
| `oss_s19_01tel.sep` | 786 432 |
| `oss_se_se01_01_01m.sep` | 4 096 |
| `oss_se_se01_01_02.sep` | 133 120 |
| `oss_se_se01_01_03.sep` | 196 608 |
| `oss_se_se01_01_04.sep` | 315 392 |
| `oss_se_se01_01_05.sep` | 223 232 |
| `oss_se_se01_01b.sep` | 4 096 |
| `oss_se_se01_03_a.sep` | 270 336 |
| `oss_se_se01_03_b.sep` | 430 080 |
| `oss_se_se01_03_c.sep` | 313 344 |
| `oss_se_se04_06_01m.sep` | 4 096 |
| `oss_se_se04_06_02.sep` | 444 416 |
| `oss_se_se04_06_03.sep` | 258 048 |
| `oss_se_se04_06_05.sep` | 272 384 |
| `oss_se_se04_06_07.sep` | 98 304 |
| `oss_se_se06_02_a01.sep` | 296 960 |
| `oss_se_se06_02_a03m.sep` | 4 096 |
| `oss_se_se06_02_a_01.sep` | 4 096 |
| `oss_se_se06_02_b.sep` | 61 440 |
| `oss_se_se06_04_01.sep` | 329 728 |
| `oss_se_se06_04_03.sep` | 157 696 |
| `oss_se_se06_04_04.sep` | 409 600 |
| `oss_se_se06_04_05.sep` | 12 288 |
| `oss_se_se06_04_06.sep` | 94 208 |
| `oss_se_se09_02_a01.sep` | 45 056 |
| `oss_se_se09_02_b01.sep` | 149 504 |
| `oss_se_se09_02_b03.sep` | 26 624 |
| `oss_se_se09_02_b04.sep` | 28 672 |
| `oss_se_se09_02_b05.sep` | 153 600 |
| `oss_se_se10_03_01.sep` | 96 256 |
| `oss_se_se10_03_02.sep` | 32 768 |
| `oss_se_se10_03_03.sep` | 51 200 |
| `oss_se_se10_03_04m.sep` | 4 096 |
| `oss_se_se10_03_05.sep` | 4 096 |
| `oss_se_se10_03_06.sep` | 110 592 |
| `oss_se_se12_03a.sep` | 4 096 |
| `oss_se_se12_03a_01.sep` | 661 504 |
| `oss_se_se12_03a_02.sep` | 126 976 |
| `oss_se_se12_03a_03.sep` | 122 880 |
| `oss_se_se12_03a_04.sep` | 790 528 |
| `oss_se_se12_03b1.sep` | 296 960 |
| `oss_se_se12_03b5.sep` | 215 040 |
| `oss_se_se12_03b_01.sep` | 296 960 |
| `oss_se_se12_03b_05.sep` | 215 040 |
| `oss_se_se13_01_01.sep` | 530 432 |
| `oss_se_se14_01a_1.sep` | 94 208 |
| `oss_se_se14_01a_2.sep` | 4 096 |
| `oss_se_se14_01a_3.sep` | 30 720 |
| `oss_se_se14_01a_4.sep` | 4 096 |
| `oss_se_se14_01b_01.sep` | 108 544 |
| `oss_se_se14_01b_02.sep` | 4 096 |
| `oss_se_se14_01b_03.sep` | 81 920 |
| `oss_se_se14_05_01m.sep` | 4 096 |
| `oss_se_se14_05_02.sep` | 237 568 |
| `oss_se_se14_05_03.sep` | 32 768 |
| `oss_se_se14_05_04.sep` | 253 952 |
| `oss_se_se14_05_05m.sep` | 4 096 |
| `oss_se_se14_05_06.sep` | 65 536 |
| `oss_se_se14_05_08.sep` | 4 096 |
| `oss_se_se15_09_02.sep` | 143 360 |
| `oss_se_se15_09a.sep` | 4 096 |
| `oss_se_se15_09b.sep` | 143 360 |
| `oss_se_se16_03_01m.sep` | 4 096 |
| `oss_se_se16_03_02.sep` | 108 544 |
| `oss_se_se16_03_03.sep` | 190 464 |
| `oss_se_se16_03_04.sep` | 53 248 |
| `oss_se_se16_03_06.sep` | 274 432 |
| `oss_se_se16_03_06a.sep` | 274 432 |
| `oss_se_se16_03_08m.sep` | 4 096 |
| `oss_se_se17_02_01.sep` | 141 312 |
| `oss_se_se17_02_02.sep` | 221 184 |
| `oss_se_se17_02_03.sep` | 4 096 |
| `oss_se_se17_02_04.sep` | 20 480 |
| `oss_se_se17_02_05.sep` | 8 192 |
| `oss_se_se17_02_06.sep` | 4 096 |
| `oss_se_se19_01_a1.sep` | 182 272 |
| `oss_se_se19_01_a2.sep` | 40 960 |
| `oss_se_se19_01_a3.sep` | 32 768 |
| `oss_se_se19_01_b2.sep` | 4 096 |
| `oss_se_se19_01_b3.sep` | 16 384 |
| `oss_se_se19_01_b4.sep` | 4 096 |
| `oss_se_se19_01tel.sep` | 321 536 |

### `PSP_GAME/USRDIR/stage/<etapa>/` (106 etapas)

Cada etapa tiene `<etapa>.prx` y `_zar`. Bytes de cada uno:

| Etapa | `<etapa>.prx` | `_zar` | Otros |
|---|---:|---:|---|
| `addscvr` | 253 628 | 75 208 |  |
| `apscan` | 269 836 | 120 637 |  |
| `briefing` | 440 780 | 2 207 257 |  |
| `cyberval2` | 569 792 | 1 546 441 |  |
| `ending` | 203 836 | 202 323 |  |
| `flashdemo` | 216 452 | 78 778 |  |
| `frvsmenu` | 176 876 | 26 764 |  |
| `gpstest` | 275 620 | 129 738 |  |
| `gsclimenu` | 171 100 | 9 880 |  |
| `gsmenu` | 177 844 | 6 685 680 |  |
| `init` | 2 556 | 34 779 |  |
| `n01` | 248 100 | 1 168 672 |  |
| `n02` | 265 780 | 1 164 662 |  |
| `n03` | 250 260 | 1 284 520 |  |
| `n05` | 244 132 | 1 271 487 |  |
| `n09` | 242 516 | 1 005 834 |  |
| `n11` | 245 636 | 474 720 |  |
| `n12` | 251 924 | 1 295 208 |  |
| `n14` | 241 700 | 1 343 820 |  |
| `n15` | 239 932 | 716 118 |  |
| `n16` | 233 060 | 685 343 |  |
| `n17` | 242 476 | 774 827 |  |
| `n18` | 251 548 | 1 164 069 |  |
| `ntconnect` | 158 236 | 49 148 |  |
| `ntmenu2` | 573 296 | 999 281 |  |
| `r_main01` | 3 148 | 1 962 809 |  |
| `r_main02` | 3 132 | 35 408 |  |
| `r_net01` | 3 148 | 2 706 367 |  |
| `st01` | 1 138 436 | 3 388 072 |  |
| `st0100` | 1 030 828 | 1 474 139 |  |
| `st01t1` | 1 138 012 | 3 237 141 |  |
| `st01t2` | 1 138 012 | 3 255 651 |  |
| `st02` | 1 171 684 | 3 385 061 |  |
| `st0204` | 1 171 660 | 3 332 965 |  |
| `st0205` | 1 171 660 | 3 545 612 |  |
| `st03t1` | 1 167 420 | 3 504 590 |  |
| `st03t2` | 1 167 396 | 3 573 644 |  |
| `st04` | 1 078 100 | 2 895 796 |  |
| `st04m1` | 1 132 300 | 3 579 849 |  |
| `st04m3` | 1 166 628 | 3 267 510 |  |
| `st04vo` | 1 099 556 | 2 989 873 |  |
| `st05` | 1 147 292 | 3 578 195 |  |
| `st0501` | 1 147 340 | 3 614 644 |  |
| `st0502` | 1 099 692 | 3 419 437 |  |
| `st0503` | 1 100 444 | 3 434 341 |  |
| `st0504` | 1 143 052 | 3 399 648 |  |
| `st0505` | 1 169 580 | 3 670 656 |  |
| `st0506` | 1 169 452 | 3 417 957 |  |
| `st06` | 1 090 316 | 3 184 566 |  |
| `st0601` | 1 090 460 | 3 184 327 |  |
| `st0602` | 1 134 524 | 3 187 030 |  |
| `st0604` | 1 134 524 | 3 189 682 |  |
| `st0605` | 1 160 956 | 3 186 451 |  |
| `st07m1` | 1 109 076 | 2 750 311 |  |
| `st07m2` | 1 107 692 | 3 076 277 |  |
| `st07tn` | 1 103 948 | 2 991 464 |  |
| `st07ts` | 1 103 948 | 2 987 922 |  |
| `st08` | 1 194 884 | 3 251 734 |  |
| `st0801` | 1 194 884 | 3 256 577 |  |
| `st0802` | 1 194 884 | 3 256 585 |  |
| `st09` | 1 311 996 | 3 304 733 |  |
| `st0902` | 1 311 996 | 3 418 753 |  |
| `st0903` | 1 311 996 | 3 504 384 |  |
| `st10` | 1 209 692 | 3 345 665 |  |
| `st10_boss` | 375 364 | 2 769 324 |  |
| `st10m1` | 1 139 948 | 3 129 348 |  |
| `st11_boss` | 1 130 140 | 2 245 771 |  |
| `st11t0` | 1 166 652 | 2 920 442 |  |
| `st11t1` | 1 166 628 | 3 216 356 |  |
| `st12tn` | 1 124 948 | 3 300 877 |  |
| `st12tnf` | 1 124 948 | 3 344 684 |  |
| `st12ts` | 1 168 228 | 3 266 334 |  |
| `st12tsf` | 1 124 948 | 3 289 527 |  |
| `st13t0` | 1 122 596 | 3 419 708 |  |
| `st13t1` | 1 122 596 | 3 429 036 |  |
| `st14` | 1 118 748 | 3 483 889 |  |
| `st1401` | 1 118 908 | 3 596 130 |  |
| `st1402` | 1 188 572 | 3 566 121 |  |
| `st1403` | 1 118 812 | 3 575 357 |  |
| `st1404` | 1 118 828 | 3 566 351 |  |
| `st1405` | 1 118 060 | 3 558 038 |  |
| `st15` | 1 117 036 | 3 377 821 |  |
| `st1501` | 1 117 124 | 3 587 821 |  |
| `st1502` | 1 186 852 | 3 648 410 |  |
| `st16` | 1 151 828 | 3 294 120 |  |
| `st1601` | 1 151 828 | 3 294 133 |  |
| `st1602` | 1 151 828 | 3 294 286 |  |
| `st17` | 1 093 220 | 3 196 288 |  |
| `st1701` | 1 093 252 | 3 210 762 |  |
| `st1702` | 1 093 220 | 3 413 536 |  |
| `st1703` | 1 093 220 | 3 217 241 |  |
| `st17_boss` | 431 972 | 2 251 072 |  |
| `st18` | 1 096 076 | 3 171 889 |  |
| `st1801` | 1 101 556 | 2 897 579 |  |
| `st1802` | 1 122 028 | 3 218 645 |  |
| `st1803` | 1 092 956 | 3 183 769 |  |
| `st1804` | 1 091 836 | 3 171 204 |  |
| `st1805` | 1 092 956 | 3 183 769 |  |
| `st1806` | 1 092 820 | 3 184 638 |  |
| `st1807` | 1 093 596 | 3 173 128 |  |
| `st18_boss` | 462 460 | 2 146 779 |  |
| `st19` | 1 106 148 | 2 578 251 |  |
| `st19_boss` | 347 780 | 1 758 700 |  |
| `st19_boss_demo` | 236 388 | 1 309 862 |  |
| `st20` | 383 300 | 2 441 853 |  |
| `title` | 301 244 | 232 822 |  |
