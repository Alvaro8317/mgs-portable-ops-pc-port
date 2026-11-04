# xxHash

- Upstream: <https://github.com/Cyan4973/xxHash>, release **v0.8.3**
  (`v0.8.3.tar.gz`, SHA-256
  `aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80`).
- License: **BSD-2-Clause** (`LICENSE`, copied unmodified from the release; the
  library files carry the same notice), compatible with GPL-3.0-or-later.
- Files, copied unmodified from the release:
  - `xxhash.h` (SHA-256
    `17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`);
  - `xxhash.c` (SHA-256
    `5c3591fe6e6c86a619eb26760e9520e37a6fd5152882ab5ad93f912e2a855966`), which
    only defines `XXH_IMPLEMENTATION` and includes the header.
- Use: the content key of the texture replacement (F24c, `mpo_texreplace`):
  `XXH64` of the texels and `XXH32` of the CLUT, as PPSSPP v1.20.4's
  `TextureReplacer` (`hash = xxh64`) computes them, so the file names match.
  XXH32 and XXH64 are fixed algorithms: every release gives the same values.
- A local `.clang-format` keeps the upstream files out of the formatter.
