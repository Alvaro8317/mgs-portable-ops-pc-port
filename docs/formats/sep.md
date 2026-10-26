# Formato `.sep` (streams de sonido)

Los 242 archivos `PSP_GAME/USRDIR/oss/*.sep` (85 MB, ver `disc-layout.md`) llevan la música
(`oss_bgm_*`) y otros sonidos. Esta página resume lo que se verificó en F16a (`docs/sessions/F16a.md`).
Solo tiene la estructura; nunca se suben datos del juego.

## Estructura

- **[V]** Empieza con la firma `SP` y una etiqueta de build (`062247 MPO_EN` en la versión US).
- **[V]** Después vienen los streams uno tras otro. Cada uno es un `u32` little-endian con el
  tamaño, seguido de un archivo RIFF/WAVE completo.
- **[V]** El primer stream de cada archivo es un ATRAC3plus mono de 0.27 s en silencio. Los demás son
  ATRAC3 estéreo.
- **[?]** La cabecera tiene una tabla con entradas de 0x20 bytes (aparecen valores como `0x407F` y
  `0xC9`). Falta interpretar sus campos.

## Contenido medido (versión US, `ULUS-10202`)

**[V]** Los 242 archivos tienen 1 655 streams en total. Todos se decodificaron sin frames malos y
dieron PCM idéntico al de PPSSPP:

| Codec | Streams | Duración total | Con loop | En silencio |
|---|---:|---:|---:|---:|
| ATRAC3 estéreo, frames de 192 bytes (66 kbps, joint stereo) | 1 514 | 9 812 s (2.7 h) | 2 | 3 |
| ATRAC3plus mono, frames de 376 bytes | 141 | 38 s (0.27 s cada uno) | 0 | 141 |

El ATRAC del juego pasa por `sceAtrac3plus`, que importa `sound2.prx` (`KCEJ_SOUND2`). La
librería que lo decodifica es `mpo_audio` (`profiles/mpo/host/audio/`).
