# at3_standalone (ATRAC3 / ATRAC3plus decoder)

- **Origen**: PPSSPP `v1.20.4` (commit `fa50bb1976065c4f8b1b47af227d367fe9771555`),
  directorio `ext/at3_standalone/`. Es el decodificador ATRAC3/ATRAC3plus de FFmpeg que PPSSPP
  separó del resto de FFmpeg.
- **Licencia**: los archivos derivados de FFmpeg llevan su cabecera **LGPL-2.1-or-later** (texto en
  `LICENSE`). `at3_decoders.h`, `compat.h` y `compat.cpp` no tienen cabecera: son pegamento escrito
  por PPSSPP (proyecto GPL-2.0-or-later). Todo es compatible con la GPL-3.0-or-later del port.
- **Copia sin modificar**: los archivos de `src/` son idénticos a los de PPSSPP (se compararon con
  `diff -r`; solo faltan `CMakeLists.txt` y un `README.txt` vacío). `aac_defines.h` no se compila
  (incluye cabeceras de libavutil que no existen aquí), pero se conserva para que la copia sea
  completa.
- **Shims** (`shim/`, LGPL-2.1-or-later, escritos para este port): sustituyen las tres cabeceras de
  PPSSPP que incluye el código (`ppsspp_config.h`, `Common/Log.h` y `Common/MemoryUtil.h`) y dan
  la reserva de memoria alineada y el log (desactivado salvo con `MPO_AT3_LOG=1..4`).
- **Build**: `at3_standalone.cmake` crea la librería estática `mpo_at3_standalone`.

| Archivo | SHA-256 |
|---|---|
| `aac_defines.h` | `bed26cf5fedd3bbeabe16ee7e33e437b7b27f29ee7cfd96b26e843ec861ae407` |
| `at3_decoders.h` | `ed0bfaf3f6e5e952a6c292ca45b5699e19baf76d0110a1e185a0626d3e5d642f` |
| `atrac.cpp` | `909bae48dcf366f1fc5f17e6cf49e996076f780e2c1fe147de5fb2b06ea59dd0` |
| `atrac.h` | `d3af304148aac3c638ca66414f15288ae74719a5e931da8ba927e1814766df19` |
| `atrac3.cpp` | `cb620e9d80c8093cfef598e4df3ea339a02c1dc644c2f63b769575e4d8bf2465` |
| `atrac3data.h` | `7f333b5652e24d6234469745b2b2ac7cb6df50343d9c7c9af009b5c6eb3fb8c9` |
| `atrac3plus.cpp` | `109c0329b47278be638f02426a562d43351366ef464d63253dc73cdd10534422` |
| `atrac3plus.h` | `555b9a0b5c176a9364b29bfc7761476ef1189c5a7bbba9907d7ae8e71b46325b` |
| `atrac3plus_data.h` | `d6aaff5c1bb5351f5294fbc288ad1b199534c99621c68c587b1254566631a018` |
| `atrac3plusdec.cpp` | `fb38cb1b2f0b67aa79e536fcf26371f83529b30622496889fd9bf1846a94a7e9` |
| `atrac3plusdsp.cpp` | `3c89356b15dd4bab3b3a4897dd213d720a7d3b8348dda06c88bcd55cf8b61294` |
| `compat.cpp` | `8d379eb3a64f6148761cd18331ab27ba337ddc4531e82e440b9fe402ff39237d` |
| `compat.h` | `790717da221d524a1f1490b500703b26b399b10173696787bc88545fd126d1e0` |
| `fft.cpp` | `1f356f7f583806cf1a925aa96db1d9248ce828b4e7340565aedc45cc2f62e3ce` |
| `fft.h` | `261e06b21bce15c4ace6b5b915ab0205f541fff5987c6d7e1706653cbf298850` |
| `float_dsp.h` | `a422fd31275acaaca2f5dec122a8d9aa1f8ece507e45f6b1433463a668598903` |
| `get_bits.cpp` | `52d330db740ceb3f409f0624e5f4f3f7b1dc2bec614a696da315fbdff3fd1e5b` |
| `get_bits.h` | `a7b7607394aad6af221c01c2b37ad1d12b64d15bf6554cdcbaf8510dcb4dc747` |
| `intreadwrite.h` | `554b87abca170dcf3bf0d56d12112aefd7be61f1218dc6190f59bf5dfef00a72` |
| `mem.cpp` | `5899087c84397db946fa91d737a70ba6589449ea74115b0e438f48bebd030cad` |
| `mem.h` | `3b4c78a4ba926548300f67d2a860417d5c2ef6a4d8d13c92c320205f07223602` |
