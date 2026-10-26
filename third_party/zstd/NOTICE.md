# Zstandard (decompressor only)

- Upstream: <https://github.com/facebook/zstd>, release **v1.5.7**
  (`zstd-1.5.7.tar.gz`, SHA-256
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`).
- License: zstd is dual-licensed BSD-3-Clause or GPL-2.0-only. This project uses
  it under **BSD-3-Clause** (`LICENSE`, copied unmodified from the release),
  which is compatible with GPL-3.0-or-later.
- Files:
  - `zstd.h`: `lib/zstd.h` from the release, unmodified (SHA-256
    `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b`).
  - `zstd_errors.h`: `lib/zstd_errors.h`, unmodified (SHA-256
    `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a`), which
    `zstd.h` includes.
  - `zstddeclib.c`: the single-file decompressor generated from the release
    with its own script, unmodified (SHA-256
    `5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0`):

    ```sh
    cd build/single_file_libs
    python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
    ```

    It has no legacy-format support, no multithreading and no assembly
    (`ZSTD_DISABLE_ASM`), so it builds the same on Linux and Windows.
- Use: decompressing PPSSPP frame dumps (`.ppdmp` versions 5 and 6) in the
  `mpo_ge_replay` library (F11a). The runtime does not link it.
