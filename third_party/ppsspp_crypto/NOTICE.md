# ppsspp_crypto — descifrado de módulos `~PSP` (solo en el instalador)

Este directorio **no contiene código ni claves de terceros**. Solo tiene la receta de CMake
(`ppsspp_crypto.cmake`) que, al configurar, descarga de PPSSPP estos archivos **sin modificar** y
verifica el SHA-256 de cada uno.

- Origen: <https://github.com/hrydgard/ppsspp>, tag `v1.20.4`
  (commit `fa50bb1976065c4f8b1b47af227d367fe9771555`).
- Destino: `<build>/_deps/ppsspp_crypto/`, fuera del repo.
- Se compila solo en `mpo_prx_decrypter`, que enlaza únicamente `mpo_host` (el instalador `mpo` y
  sus tests).

## Por qué no se vendoriza

`kirk_engine.c` trae la keyvault KIRK completa, `kirk1_key` y las claves ECDSA. `PrxDecrypter.cpp`
trae las tablas de claves de los tags de PRX. `CLAUDE.md` §1.1 prohíbe subir al repo claves KIRK o
tablas de claves, así que estos archivos nunca se versionan. Lo decidió el usuario en F02 (ver
`docs/sessions/F02.md`).

Hay que tener en cuenta que el binario `mpo` sí lleva esas claves compiladas, igual que PPSSPP.
Ningún otro target (framework, runtime ni código generado) las enlaza.

## Archivos y licencias

| Archivo | Autor u origen | Licencia |
|---|---|---|
| `ext/libkirk/kirk_engine.c`, `kirk_engine.h` | Draan (KIRK engine), con cambios de PPSSPP | GPL-3.0-or-later |
| `ext/libkirk/bn.c`, `ec.c` | Segher Boessenkool | GPL-2.0 o GPL-3.0 (a elección) |
| `ext/libkirk/AES.c`, `AES.h` | Rijmen, Bosselaers y Barreto (rijndael-alg-fst) | Dominio público |
| `ext/libkirk/SHA1.c`, `SHA1.h` | David Ireland, sobre A.M. Kuchling | Sin restricciones (dominio público) |
| `ext/libkirk/kirk_common.h` | PPSSPP | GPL-2.0-or-later (licencia del proyecto) |
| `Core/ELF/PrxDecrypter.cpp` | PPSSPP (claves de PSARDUMPER y JPCSP) | GPL-2.0-or-later: el archivo no tiene cabecera, y el README de PPSSPP dice "released under the GPL 2.0 (and later)" |

- **No se usa `Core/ELF/PrxDecrypter.h` de PPSSPP**: es el único archivo de `Core/` y `GPU/` cuya
  cabecera dice "version 2.0" sin "or later". En su lugar,
  `profiles/mpo/host/crypto/ppsspp_shim/` tiene cabeceras propias (GPL-3.0-or-later), que declaran
  solo lo que `PrxDecrypter.cpp` necesita: los tipos enteros, el tamaño de la cabecera `~PSP`, un
  `INFO_LOG` vacío y `pspDecryptPRX`.
- Todo es compatible con GPL-3.0-or-later, que es la licencia del proyecto. El texto de la GPL-3.0
  está en `LICENSE`, en la raíz.

## Compilar sin red

- `-DMPO_PPSSPP_SOURCE_DIR=<checkout de PPSSPP en ese commit>` toma los archivos de una copia local.
  Los hashes se verifican igual.
- `-DMPO_ENABLE_PRX_DECRYPT=OFF` compila sin descifrado. En ese caso, `mpo --install` rechaza las
  versiones que necesitan descifrado y no escribe nada.
