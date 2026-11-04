<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Metal Gear Solid: Portable Ops — unofficial PC port

[Español](README.es.md)

An unofficial native port of **Metal Gear Solid: Portable Ops** (PSP, 2006) to **Windows and
Linux** (Steam Deck included). The game's code is translated to C++ and compiled for your PC, so it
does not run inside an emulator.

**This repository holds nothing from the game, and the port can't be played on its own.** It needs
your own copy of the game, as an ISO file. The port is built on your computer from that ISO: the
translated game is never downloaded from anywhere and must never be shared (see
[Legal notice](#legal-notice)).

> **Status: in development, no release yet.** On Linux, the first missions play from the start of
> the game and all 92 stages compile. A full playthrough, on Linux and on Windows, is still in
> progress. The game has also run on Windows from a developer build. The Windows ZIP and its
> launcher are built and checked automatically, but they haven't been tested on a player's PC yet.

## Contents

- [What you need](#what-you-need)
- [Install on Windows](#install-on-windows)
- [Install on Linux and Steam Deck](#install-on-linux-and-steam-deck)
- [Playing](#playing)
- [Settings](#settings)
- [Controls](#controls)
- [Saves](#saves)
- [Texture packs](#texture-packs)
- [If something goes wrong](#if-something-goes-wrong)
- [Uninstall](#uninstall)
- [Legal notice](#legal-notice)
- [For developers](#for-developers)

## What you need

| | |
|---|---|
| **The game** | An ISO of the **USA** release, **ULUS-10202** (disc version 1.02), made from your own UMD. Other regions and versions are rejected with a clear message. |
| **PSP fonts** | The firmware fonts, `ltn0.pgf` and `jpn0.pgf` (from `flash0:/font` of a PSP). If PPSSPP is installed, its `assets/flash0/font` folder works too. **Recommended**: without them the game runs, and the save screens and the on-screen keyboard (soldier names, passwords) show their text with a free substitute font (FreeSans) instead of the PSP's. The game's own brief "Checking Memory Stick" notice at start does not show without them. |
| **Disk space** | About **15 GB** free while the game compiles, and about 10 GB after. |
| **Memory** | 8 GB of RAM or more. |
| **Graphics** | A GPU with **Vulkan 1.2** (any GPU from the last ~8 years with up-to-date drivers). |
| **Time** | The first install compiles the game: **20 to 40 minutes** on a recent PC, longer on an older one. After that you can play, and the rest keeps compiling in the background (see [Playing](#playing)). |
| **Internet** | Only during the install. It downloads the compiler and some pinned libraries, never anything from the game. |

The easiest way to get the fonts: copy the `font` folder from your PSP's `flash0` (or from PPSSPP)
into a `flash0` folder next to your ISO, so you get:

```
Games/
  ULUS-10202.iso
  flash0/
    font/
      ltn0.pgf
      jpn0.pgf
      ...
```

The installer finds them there without being asked. You can also pick the folder in the launcher.

## Install on Windows

1. Download `mpo-pc-port-<version>-windows-x86_64.zip` from the
   [Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases) page.
2. Unpack the **whole** ZIP into a folder you own, **not** inside `Program Files`, for example
   `C:\Games\mpo-pc-port`.
3. Open `mpo_launcher.exe`. If Windows SmartScreen warns about an unknown app, choose
   *More info › Run anyway*. The launcher isn't signed.
4. Choose your ISO (and the fonts folder, if the launcher asks) and press **Install**. The first time
   it downloads the compiler (llvm-mingw, CMake and Ninja, about 250 MB) into its own folder. You
   don't need Visual Studio or administrator rights.
5. When it says the game is ready, press **Play**.

The launcher works with the mouse, the keyboard or a gamepad. It speaks English, or Spanish when
your system is in Spanish; **Language** in its settings changes it.

**Updating**: unpack the new ZIP (in the same folder or another one) and open its
`mpo_launcher.exe`. It shows **Update** above **Play**: press it to compile the game again with the
new version. Only what changed is compiled, the stages already compiled are kept, and your saves
and settings stay where they are.

### If your antivirus removes a file

Some antivirus programs, **Avast** in particular, flag part of the compiler as a threat and
quarantine it (usually `clang-23.exe`). **It's a false positive**: the compiler is the official
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) release, and the installer checks its SHA-256
before using it. The install then stops with "the compiler does not run" or "an antivirus may have
removed or blocked it".

To fix it, restore the file and add an exception for the port's folder,
`C:\Users\<your user>\AppData\Local\mpo-pc-port`, so nothing else it compiles gets removed:

- **Avast**: *Protection › Virus Chest*, restore the file; then *Menu › Settings › General ›
  Exceptions › Add exception* with that folder.
- **Windows Security** (Microsoft Defender): *Virus & threat protection › Protection history*, allow
  the file; then *Manage settings › Exclusions › Add an exclusion › Folder* with that folder.

Menu names can change between versions. Then press **Install** again: it unpacks the compiler again
by itself. Doing this before the first install avoids the problem.

## Install on Linux and Steam Deck

### With Flatpak (recommended, and the only way on a Steam Deck)

1. Download `mpo-pc-port.flatpak` from the
   [Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases) page.
2. Install and open it:

   ```bash
   flatpak install --user mpo-pc-port.flatpak
   flatpak run io.github.alvaro8317.MpoPcPort
   ```

   It also shows up in your applications menu. On a Steam Deck, do this in Desktop Mode. Then you can
   add it to Steam (*Add a Non-Steam Game*) and play it in Game Mode with the gamepad.
3. Choose your ISO, press **Install**, and **Play** when it's ready.

The Flatpak builds the game with its own compiler (the `org.freedesktop.Sdk` runtime, which Flatpak
downloads the first time), so you don't need to install anything else. It can read your ISO from
your home folder, from `/media`, from `/run/media` and from a Steam Deck's microSD card.

### Without Flatpak (from the source code)

You need some packages from your distribution: CMake 3.22 or later, Ninja, Clang (or GCC) with
C++20, Python 3.11 or later, `make`, `pkg-config`, the Vulkan loader, and the development headers
for sound (ALSA, PulseAudio and PipeWire), for the window (X11 and Wayland) and for gamepads
(udev). **Without the sound headers the game builds, but it has no sound.** On Debian and Ubuntu
(24.04 or later):

```bash
sudo apt install cmake ninja-build clang python3 make pkg-config ca-certificates libvulkan1 \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libegl-dev libdecor-0-dev libudev-dev libdbus-1-dev
```

On other distributions, install the packages with the same names (usually ending in `-devel`
instead of `-dev`), or use the Flatpak.

Download `mpo-pc-port-<version>-source.tar.gz` from the
[Releases](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/releases) page and unpack it.
Then, from that folder:

```bash
python3 -I profiles/mpo/scripts/setup.py install --iso ~/Games/ULUS-10202.iso
python3 -I profiles/mpo/scripts/setup.py play
```

In a second terminal, while you play (or before), compile the stages:

```bash
python3 -I profiles/mpo/scripts/setup.py stages --low-priority
```

`setup.py status` tells you what's done. Every step picks up where it stopped, so if something
fails or you close the terminal, just run the same command again.

## Playing

- **You can play as soon as the install ends.** The game is split into 92 stages (the missions, the
  menus, the cutscenes), and those keep compiling in the background, in story order:
  - first, all of them in a quick version (about 40 more minutes);
  - then in an optimized version (several hours).

  A stage that isn't compiled yet still works, but it runs slower, with FPS drops in busy scenes.
  The launcher shows how many are done. You can stop the compile with **Stop** and it picks up
  where it left off next time.
- **The game runs at 20 FPS during gameplay and 30 FPS in the menus.** That's how it was on the
  PSP, and its logic is tied to that speed. It isn't a bug.
- **Recruiting by AP Scan works**, with virtual Wi-Fi access points that change every day.
- **GPS** answers "no receiver", as on a PSP without one.
- **Multiplayer** (ad hoc and online) isn't supported yet: the game finds nobody around.

## Settings

The launcher's **Settings** section (with the mouse or the gamepad) chooses how the game runs. Each
change is saved at once and applies the next time you press **Play** (the language, at once):

- **Renderer**: Vulkan (the GPU), or Software, which is slower and is there to find out whether a
  problem comes from the GPU.
- **Resolution**: the game's internal resolution, from 1x (the PSP's 480x272) to 8x (3840x2176).
  Vulkan only. A higher one is sharper and asks more of the GPU.
- **Window**: the size of the window, from 1x (480x272) to 8x. You can still resize it by hand.
- **Fullscreen**: borderless, at your desktop's resolution. **F11** or **Alt+Enter** also switches
  while you play.
- **Aspect**: 16:9 (the PSP's own screen, the default), or ultrawide: 21:9 (2560x1080 or
  3440x1440) or 32:9. Wider than 16:9, the 3D shows more at the sides, like PPSSPP's widescreen
  cheat with "Stretch"; the HUD and the menus are stretched to fit, and the videos keep 16:9 with
  black bars at the sides. It doesn't change how the game plays.
- **Texture pack**: **Use it if there is one** (the default) or **The game's textures**. An HD pack
  looks better and asks more of the GPU and the memory; this turns it off without deleting it (see
  [Texture packs](#texture-packs)).
- **Language**: the launcher's, English or Spanish. **System** follows your system's language
  (Spanish if it's Spanish, English otherwise); before installing, the choice is at the top of the
  window. The messages of the install steps and the logs stay in English, the same for everyone
  who reports a problem.

They're kept in `settings.toml`, next to `input.toml` (see [Changing the
controls](#changing-the-controls)):

```toml
renderer = "vulkan"   # or "soft"
render_scale = 1      # 1 to 8
window_scale = 2      # 1 to 8
fullscreen = false
aspect = "native"     # or "<width>:<height>", such as "64:27" (2560x1080) or "32:9"
texture_pack = true   # false: the game's own textures, even with a pack
language = "auto"     # or "en", "es"
```

If the file is damaged or a value isn't valid, the game starts with the default for it, and the
launcher says what was wrong. Without the launcher, `setup.py settings` shows what `setup.py play`
will use. Options after `--` win over the file (for example
`setup.py play -- --render-scale 2`), and so do `MPO_RENDERER` and `MPO_RENDER_SCALE`.

## Controls

The game's menus confirm with **Circle** and go back with **Cross**, as on the PSP.

### Keyboard and mouse

| PSP | Keyboard / mouse |
|---|---|
| Analog stick (move) | W A S D |
| D-pad (the camera, and menus) | Arrow keys |
| Cross | Space |
| Circle | Right click |
| Square | Left click |
| Triangle | Ctrl |
| L | Q |
| R | E |
| Start | Esc or Enter |
| Select | Tab |

**The mouse can turn the camera**: click in the game window to capture the mouse, then move it: left
and right turn the camera, up and down work as the d-pad's up and down. **F1** (or switching to
another window) releases it.

**F11** or **Alt+Enter** switches between the window and fullscreen while you play. The game doesn't
see those keys: Alt+Enter doesn't press Start, even though Enter is Start. To start in fullscreen,
use **Fullscreen** in [Settings](#settings).

When the game asks you to type something (a name, a password), type it on the keyboard. Backspace
deletes, Enter finishes and Esc cancels.

### Gamepad

Xbox, PlayStation and most other gamepads work, with the same layout as PPSSPP:

| PSP | Xbox | PlayStation |
|---|---|---|
| Analog stick | Left stick | Left stick |
| D-pad (the camera, and menus) | D-pad | D-pad |
| Cross | A | Cross |
| Circle | B | Circle |
| Square | X | Square |
| Triangle | Y | Triangle |
| L | LB or LT | L1 or L2 |
| R | RB or RT | R1 or R2 |
| Start | Start / Menu | Options |
| Select | Back / View | Share / Create |

**The right stick moves the camera**: left and right turn it, up and down work as the d-pad's up and
down. The further you push it, the faster the camera moves. Switching to another window stops it until you come back. To turn it off or change
how it reacts, see `[pad_camera]` in `input.toml` (below).

### Buttons on screen

The save, load and message screens and the on-screen keyboard show the buttons of what you're
playing with, and switch as soon as you use something else: the keys and mouse buttons from your
`input.toml` (Space, Esc, Right click...) on the keyboard, A/B/X/Y on an Xbox pad, the shapes on a
PlayStation pad, and the labels of Nintendo and other pads.

The game's own button icons (in the menus and the HUD, such as "△ : FLOOR CHANGE" on the pause map)
switch too, with icons drawn by the port: a letter in a circle on a gamepad (A, B...), a key with
its name on the keyboard (SPC, Q...), a mouse with the button marked. The shapes stay as they are on
a PlayStation pad. For now only a few icons are known; the rest still show the PSP's buttons, and
the tutorials and the codec too. With a texture pack, these icons win over the pack's on those
textures only.

- To always show the same ones, start the game with `--prompts`, for example
  `setup.py play -- --prompts psp` (also `xbox`, `playstation`, `nintendo` or `keyboard`).
- To keep the game's icons (and your pack's), add `--prompt-icons off`.
- Your own icons go in the `prompts` folder of the data folder (next to `textures`, see
  [Texture packs](#texture-packs)): a pack per style in `prompts/xbox/`, `prompts/playstation/`,
  `prompts/nintendo/`, `prompts/generic/` or `prompts/keyboard/`, and more icons for the port to
  draw in `prompts/inventory.toml` (the format is in
  [`profiles/mpo/config/prompt_icons.toml`](profiles/mpo/config/prompt_icons.toml)).

### Changing the controls

Copy [`profiles/mpo/config/input.toml`](profiles/mpo/config/input.toml) (in the ZIP, under `source\`)
to:

- Windows: `%APPDATA%\mpo-pc-port\input.toml`
- Linux: `~/.config/mpo-pc-port/input.toml`
- Flatpak: `~/.var/app/io.github.alvaro8317.MpoPcPort/config/mpo-pc-port/input.toml`

Then edit it. The comments at the top say how. You don't need to reinstall anything.

## Saves

Your saves are kept apart from the install, so reinstalling or updating doesn't touch them:

| | Save folder |
|---|---|
| Windows | `%APPDATA%\mpo-pc-port\ms0\PSP\SAVEDATA` |
| Linux | `~/.local/share/mpo-pc-port/ms0/PSP/SAVEDATA` |
| Flatpak | `~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/ms0/PSP/SAVEDATA` |

**Bringing your saves from a PSP or from PPSSPP**: in the launcher, press **Import a save** and pick
either a save folder (`ULUS10202…`) or a whole `SAVEDATA` folder. PPSSPP keeps them in
`Documents\PPSSPP\PSP\SAVEDATA` on Windows and in `~/.config/ppsspp/PSP/SAVEDATA` on Linux. A
PSP keeps them in `PSP/SAVEDATA` on its Memory Stick. If PPSSPP is installed, the folder browser
has a **PPSSPP saves** button, and you can also type a path or tick **Show hidden folders**. Your
original files are only read, never changed. If the port already has a save with the same name,
nothing is replaced and the launcher tells you.

**Taking your saves to a PSP or to PPSSPP**: press **Export my saves** and pick a folder (with the
Flatpak, your Downloads folder). The exported saves are encrypted the way a PSP expects. Copy them
into the `SAVEDATA` folder of PPSSPP or of the PSP.

Without the launcher, use `setup.py import-save <folder>` and `setup.py export-save <folder>`
(add `--overwrite` to replace a save that is already there).

> Back up your save folder before trying a new version of the port.

## Texture packs

The port can draw your own PNG images instead of the game's textures (for example, other button
icons or HD textures). Put a pack in the `textures` folder next to your saves:

| | Texture pack folder |
|---|---|
| Windows | `%APPDATA%\mpo-pc-port\textures` |
| Linux | `~/.local/share/mpo-pc-port/textures` |
| Flatpak | `~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/textures` |

A pack is a `textures.ini` plus PNG files, in the same format as PPSSPP's texture packs, with
`hash = xxh64` and `ignoreAddress = true` under `[options]`. A PNG named after a texture's key
(`00000000a1b2c3d4e5f60718.png`) or listed under `[hashes]` replaces that texture. A PNG larger
than the original works too: the Vulkan renderer uses it at its own size. Textures in video memory
(most of the ones the game uses) are replaced too, but never what the game draws itself (the screen, render
targets) nor the frames of the videos (title, comics and credits). Without a `textures.ini` in
that folder the game looks exactly as before.

To make a pack, start the game with `setup.py play -- --dump-textures <folder>`: it writes every
texture it draws into `<folder>`, named by its key, plus a `textures.ini`. Edit the PNGs you want
to change and copy them, with the `textures.ini`, into the pack folder. The dumped textures are the
game's art: keep them for yourself and only share PNGs you made.

### Making an HD pack

The dump also writes a `dump.csv`: for each texture, its size, format, the first stage it showed up
in and how often it was drawn. Dump into the same folder over several play sessions and the numbers
add up, so play the parts you want the pack to cover. Then use `texture_pack.py`, which comes with
the port (`source\profiles\mpo\tools\` in the Windows ZIP). On Windows run it with the port's
Python, `python\python.exe -I source\profiles\mpo\tools\texture_pack.py ...`; on Linux,
`python3 -I profiles/mpo/tools/texture_pack.py ...`:

1. **See where to start**: `texture_pack.py report <dump>` lists the textures drawn the most first
   (the walls and floors you see all the time). `--sort stage` groups them by the stage where they
   first appeared, `--sort size` puts the largest first, `--stage st02` shows only one stage and
   `--limit 50` the first 50.
2. **Scale them**: install an upscaler yourself, for example
   [realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) (unpack it, its models
   come with it), and run
   `texture_pack.py upscale <dump> <pack> --scale 4 --realesrgan <path to realesrgan-ncnn-vulkan>`.
   `--top 200`, `--stage st04vo` or `--min-uses 1000` scale only part of the dump; `--model` picks
   another Real-ESRGAN model (`realesr-animevideov3` also does `--scale 2` and `3`). Any other tool
   works too: put its command after `--`, with `{input}`, `{output}` and `{scale}` where it takes
   them, for example `texture_pack.py upscale <dump> <pack> --scale 2 -- mytool -i {input} -o
   {output} -s {scale}`. The PNGs keep their names and their transparency, and the pack gets the
   dump's `textures.ini`. PNGs already in the pack are kept (`--overwrite` redoes them), so you can
   stop and go on later.
3. **Check the pack**: `texture_pack.py check <pack> <dump>` says if a PNG isn't a texture of the
   dump, isn't a whole multiple of the original size (2×, 3×, 4×…, the same in both directions, at
   most 4096), or if a name or the `textures.ini` is wrong.
4. Copy the pack into the `textures` folder (or start the game with `--textures <pack>`).

The scaled textures are still made from the game's art: don't share them either.

## If something goes wrong

| What you see | What to do |
|---|---|
| The ISO is rejected ("Unsupported disc" or "Rejected: this image does not match") | The port only takes the USA release ULUS-10202 (disc 1.02), dumped whole and unchanged. Make sure it's that version and not a patched, translated or compressed (CSO) copy. |
| "no PSP firmware fonts found", or the save screens and the on-screen keyboard use a different font from the PSP's | Without the firmware fonts, those screens use a substitute font: the game works the same. To get the PSP's look, press **Add the fonts...** in the launcher and choose the `font` folder (see [What you need](#what-you-need)). It only copies them; you don't need to install again. |
| Windows: "the compiler does not run" or "an antivirus may have removed" a file | Your antivirus quarantined part of the compiler. It's a false positive: see [If your antivirus removes a file](#if-your-antivirus-removes-a-file). |
| Windows: the game stops at once and the launcher says "a DLL it needs is missing", or "generated files are missing" | Also the antivirus, this time on the port's files. Add the exception from [If your antivirus removes a file](#if-your-antivirus-removes-a-file) and press Install again: it redoes only what's missing. |
| The install stops with an error | The launcher shows the last lines. Press Install again: it picks up where it stopped, and a network hiccup is often all it was. The full logs are in the install folder's `logs` folder (see [Uninstall](#uninstall)). |
| The game stops and says a function is "unimplemented" | You reached a part of the game the port can't handle yet. It stops on purpose rather than risk damaging your save. Please [open an issue](https://github.com/Alvaro8317/mgs-portable-ops-pc-port/issues) with the lines the launcher shows and say where you were in the game. |
| Low FPS in some scenes | That stage may not be compiled yet. Look at the launcher's count and let the background compile finish. |
| No window, or a Vulkan error | Update your GPU drivers. Under WSL or in a virtual machine there's often no real GPU. There, choose **Software** as the renderer in the launcher's [Settings](#settings) (slower), or start the game with `MPO_RENDERER=soft`. |
| No sound | The game uses your system's default audio device. Pick it before starting the game. If you built it from the source code, check that the sound packages are installed. |

When you report a problem, **never attach the ISO, the install folder or files from the game**:
the text of the error and your system (Windows or Linux, GPU) are enough.

## Uninstall

Delete the port's folder (the ZIP folder on Windows), or run
`flatpak uninstall io.github.alvaro8317.MpoPcPort`. Then delete the install folder, which holds the
decrypted game, the generated code and the compiled game, about 10 GB:

- Windows: `%LOCALAPPDATA%\mpo-pc-port\install`
- Linux: `~/.local/share/mpo-pc-port/install`
- Flatpak: `~/.var/app/io.github.alvaro8317.MpoPcPort/data/mpo-pc-port/install`

Your saves are in another folder (see [Saves](#saves)), so delete that one only if you don't want
them anymore.

## Legal notice

- This is a **fan project**. It isn't made, endorsed or supported by Konami, Kojima Productions or
  Sony. *Metal Gear Solid* and *Portable Ops* are trademarks of Konami Digital Entertainment.
  *PSP* and *PlayStation* are trademarks of Sony Interactive Entertainment.
- **No part of the game is distributed**, not even translated or compiled: no ISO, executable,
  stage file, graphics, sound, video, text, encryption key or PSP firmware file. The repository and
  the releases contain only the port's own code, a list of addresses inside the game's code (to
  translate it without analysis tools) and free third-party tools. Every release is scanned
  automatically for game files.
- **You must own the game** and make your ISO from your own UMD. The installer checks that the ISO
  is exactly the USA disc 1.02 and builds the port from it, on your computer only.
- What the install creates (the decrypted game, the generated code and the compiled game) is
  derived from the game and is for your own use. **Never share it.**
- The port is free software, under the [GNU GPL, version 3 or later](LICENSE). It includes code
  from PPSSPP (GPL-2.0-or-later) and other projects, listed with their licenses in
  [`THIRD_PARTY.md`](THIRD_PARTY.md). It comes with **no warranty**.

## For developers

- [`CLAUDE.md`](CLAUDE.md): the technical approach (static recompilation and an HLE runtime), the
  rules and the decisions taken.
- [`docs/PLAN.md`](docs/PLAN.md) and [`docs/sessions/`](docs/sessions/): the plan by phases and what
  each phase did.
- [`docs/RESEARCH.md`](docs/RESEARCH.md): the research, with its sources.
- [`profiles/mpo/README.md`](profiles/mpo/README.md): the game's profile and its tools.
- [`docs/analysis/distribution.md`](docs/analysis/distribution.md): how the install and the releases
  work.
- [`docs/smoke-test.md`](docs/smoke-test.md): the checklist followed before every release.

The project documentation is in Spanish. Code and comments are in English.
