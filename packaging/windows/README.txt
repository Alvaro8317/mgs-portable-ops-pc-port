Metal Gear Solid: Portable Ops - unofficial PC port
===================================================

This is not a game you can play by itself, and it holds nothing of the game.
You need your own copy of Metal Gear Solid: Portable Ops (USA, ULUS-10202) as
an ISO file. The port is compiled on this computer, from your ISO.

1. Unpack this whole folder somewhere with about 10 GB free (not inside
   Program Files).
2. Run mpo_launcher.exe.
3. Choose your ISO and press Install. The first time it downloads the
   compiler (llvm-mingw, CMake and Ninja, about 250 MB) and compiles the game:
   20 to 40 minutes. Then press Play. The stages keep compiling in the
   background while you play.

If the install says that an antivirus may have removed a file, allow it in
Windows Security > Virus & threat protection > Protection history, add an
exclusion for the folder %LOCALAPPDATA%\mpo-pc-port and press Install again.
The full guide is source\README.md (in Spanish: source\README.es.md).

What is in this folder:
- mpo_launcher.exe: the launcher (GPL-3.0-or-later).
- source/: the port's source code (GPL-3.0-or-later, see LICENSE.txt).
- python/: the embeddable Python 3.14.10 (PSF license, python/LICENSE.txt),
  which runs source/profiles/mpo/scripts/setup.py.
- ffmpeg/: FFmpeg 7.1.5 (LGPL-2.1-or-later) compiled for the videos, with its
  source code (ffmpeg-7.1.5.tar.xz) and how it was built (BUILD.txt).

Everything the install makes (the decrypted game, the generated code and the
build) goes to %LOCALAPPDATA%\mpo-pc-port\install and must not be shared.
