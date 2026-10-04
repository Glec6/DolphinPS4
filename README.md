# 🐬 Dolphin for PS4

**Play GameCube and Wii games on a jailbroken PS4**, powered by the [Dolphin emulator](https://dolphin-emu.org) with a PS4-style menu, per-game settings and a Vulkan renderer that runs on the PS4's own GPU.

Ported to PS4 by **ShiroKlein**.

> ⚠️ This project does not include any games, BIOS files or firmware. Please dump your own discs.

---

## ✨ Features

- 🎮 **GameCube and Wii games**, with the JIT CPU emulation, dual core and fast memory access running on the PS4.
- 🖥️ **Vulkan renderer**: Mesa's RADV driver talking straight to the PS4 GPU (GNM), with async shader compiling and hybrid ubershaders.
- 📺 **PSP-style menu (XMB)**: games tagged 🟣 GameCube / ⚪ Wii, box art downloaded automatically from GameTDB, colour themes, your own wallpapers and menu sounds.
- ⏸️ **In-game menu (L3 + R3)**: save/load states (8 slots), change settings while playing, cheats, Wii Remote options, quit back to the menu.
- ⚙️ **Settings for all games or per game**: resolution, aspect ratio, widescreen hack, frame-rate limit (Auto 60/30, 60, 45, 30), texture filtering, anti-aliasing, shader compilation and more. Changes made in-game are saved.
- 🧠 **Auto CPU clock**: lowers the emulated GameCube CPU clock in heavy scenes so games don't go into slow motion.
- 🕹️ **Remappable controls** for GameCube and Wii, with DualShock 4 button icons.
- 💾 **Cheats** (Action Replay / Gecko) from the menu or in-game.
- 🐞 **Diagnostics you can switch on/off**: performance log, freeze reports, emulator log, profiler.

---

## 📋 Requirements

- A **jailbroken PS4** (developed and tested on a base "fat" PS4) with a homebrew package installer.
- An FTP client to copy your games to the console.
- Your own GameCube / Wii game dumps.

## 📥 Install

1. Install the `.pkg` from the [Releases](../../releases) page with your package installer.
2. Copy your games to **`/data/DolphinPS4/games/`** on the PS4 (over FTP).
   - Supported: `.rvz` `.iso` `.nkit.iso` `.gcm` `.gcz` `.ciso` `.wia` `.wbfs`
   - Multi-disc games: name them `Game (Disc 1).rvz`, `Game (Disc 2).rvz`.
   - 💡 **`.rvz` is recommended**: smallest files, no speed cost. Convert with Dolphin on PC (right-click a game → *Convert File*).
3. Open **Dolphin** from the PS4 home screen. The version is shown under **Settings → About**.

---

## 🎮 Controls

### Menu (XMB)
| Button | Action |
|---|---|
| D-pad / left stick ← → | Switch category (Settings · Games · Themes) |
| D-pad / left stick ↑ ↓ | Move |
| L1 / R1 | Jump 5 items |
| ✕ | Start game / select |
| ○ | Back |
| △ (on a game) | Start · Game Settings · Cheats · Information |

### GameCube controller
| GameCube | DualShock 4 |
|---|---|
| A / B / X / Y | ✕ / □ / ○ / △ |
| Z | R1 |
| Start | OPTIONS |
| L / R | L2 / R2 (analog) |
| Control Stick / C-Stick | Left stick / Right stick |
| D-Pad | D-pad |

### Wii Remote + Nunchuk
| Wii | DualShock 4 |
|---|---|
| A / B | ✕ / R2 |
| 1 / 2 | □ / △ |
| − / + | L1 / OPTIONS |
| Home | Touchpad click |
| Pointer | Right stick |
| Shake | R3 |
| D-Pad | D-pad |
| Nunchuk stick / C / Z | Left stick / L1 / L2 |

Remap anything in **Settings → Controls**: press ✕ on a row, then the new button.
Games that need the Wii Remote alone (Wii Sports, Wii Play) or held sideways (New Super Mario Bros. Wii, Mario Kart Wii, Kirby's Return to Dream Land, Smash Bros. Brawl) are set up automatically.

### ⏸️ In-game menu: press **L3 + R3** together
- **Resume**
- **Save State / Load State**: ← → picks the slot (1–8)
- **Wii Extension** (Nunchuk / None) and **Sideways Wii Remote**: Wii games only
- **Settings**: the top row, **Apply To**, chooses *This Game* (its own settings) or *All Games*
- **Cheats**
- **Quit Game**: back to the menu

---

## 💡 Tips

- **Shader stutter:** the first time a new effect appears, it can hitch while its shader compiles. The cache makes it smooth next time.
- **Frame Rate Limit → Auto (60/30):** if a game hovers around 45–55 FPS and feels juddery, this gives a steady 30 instead. Avoid 45 on a 60 Hz TV.
- **Shader Compilation:** *Asynchronous* (default) never stutters but effects can appear a moment late; *Hybrid Ubershaders* also draws everything right away, at some GPU cost.
- **Speed Features → Compatible:** try it if a game glitches or freezes.
- **Internal Resolution:** most games are limited by the CPU, not the GPU, so 2x often costs nothing.
- **Reset All Settings** (in the XMB Settings) goes back to the tuned defaults.
- **Wallpapers:** put PNG images in `/data/DolphinPS4/themes/` and pick them under Themes.

## 📊 Tested games (base PS4)

| Game | FPS |
|---|---|
| Super Mario Sunshine | 30 (native 30) |
| Resident Evil 4 | 30 (native 30) |
| The Legend of Zelda: Twilight Princess | 30 (native 30) |
| Mortal Kombat: Deadly Alliance | 60 |
| Crash Nitro Kart | 60 |
| Crash Tag Team Racing | 60 |
| Sonic Adventure DX | 60 |
| Shadow the Hedgehog | 30–60 |
| Worms 3D | ~55 |
| Super Smash Bros. Melee | 36–60 |
| Crash Bandicoot: The Wrath of Cortex | 20–60 |
| Super Mario Galaxy | 40–57 |
| Wii Sports | ~40 |
| Wii Sports Resort | ~37 |
| FIFA Street 2 | ~34 |

Ranges depend on the scene: light scenes reach the high number, heavy scenes drop to the low one. The PS4's CPU is the usual limit.

## 🐞 Reporting a problem

1. Turn on **Settings → Diagnostics → All Diagnostics** (with *Performance Log* and *Freeze Reports*), then reproduce the problem.
2. Copy these from `/data/DolphinPS4/` over FTP: `boot-trace.log`, `dolphin.log`, `crash.log` (if it crashed), and any `stall-stacks-*.txt` / `hang-stacks.txt`.
3. Open an [issue](../../issues) with the game and region, the app version (Settings → About), what you did and when it happened.

**Where things are saved:** GameCube saves in `/data/DolphinPS4/User/GC/`, Wii saves in `/data/DolphinPS4/User/Wii/`, save states in `/data/DolphinPS4/User/StateSaves/`.

---

## 🛠️ Building from source

This repo holds the PS4 port: patches against Dolphin and Mesa, the PS4 runtime, toolchain files, the XMB assets and the build scripts.

| Part | Base |
|---|---|
| Dolphin | `771fb154059c5812d6a715a23226249cc42877a2` + [`patches/dolphin-ps4.patch`](patches/dolphin-ps4.patch) |
| Mesa (RADV) | 26.0.8 + [`patches/mesa-ps4.patch`](patches/mesa-ps4.patch) and [`port/mesa`](port/mesa) (the GNM winsys) |

Built in **WSL (Ubuntu)** with **clang 21** and libc++ on top of the [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) C runtime from [PacBrew](https://github.com/PacBrew). The build tree lives in `~/dolphinps4-build` (see [`toolchain/env.sh`](toolchain/env.sh)).

```bash
# 1. Sysroot: libc++ (also prepares the header overlay) and the runtime for the PS4 target
toolchain/build-libcxx.sh && toolchain/build-runtime.sh
# 2. Dolphin source with the PS4 patch
git clone https://github.com/dolphin-emu/dolphin ~/dolphinps4-build/src/dolphin
cd ~/dolphinps4-build/src/dolphin && git checkout 771fb154059c5812d6a715a23226249cc42877a2 \
  && git submodule update --init --recursive && git apply /path/to/DolphinPS4/patches/dolphin-ps4.patch
# 3. Mesa RADV for the PS4 GPU, then Dolphin
scripts/build-mesa.sh setup
scripts/configure-dolphin.sh && scripts/build-dolphin.sh
# 4. Package (DOLPHIN_PS4_VERSION sets the version shown in About)
DOLPHIN_PS4_VERSION=02.95 scripts/package-dolphin.sh
```

The scripts' comments explain each step and the options they take.

### Project layout
- `patches/`: changes to Dolphin and Mesa
- `port/`: PS4 runtime (startup, memory allocator, crash log, profiler) and the Mesa GNM winsys
- `toolchain/`: compiler wrappers, CMake toolchain files, linker script
- `scripts/`: build, package and asset scripts (icon, menu sounds, intro animation)
- `xmb/`: menu assets: fonts, sounds, built-in default settings, GameTDB certificate bundle
- `sce_sys/`: app icon

---

## 🙏 Credits

- The [Dolphin Emulator](https://dolphin-emu.org) team: the emulator this is built on
- [Mesa](https://mesa3d.org) and its RADV driver
- [OpenOrbis](https://github.com/OpenOrbis) and [PacBrew](https://github.com/PacBrew): PS4 homebrew toolchain
- The love-ps4 port, used as a reference for the PS4 setup
- [GameTDB](https://www.gametdb.com): game titles and box art
- Fonts: Nunito, Questrial and Michroma (SIL Open Font License)

## 📄 License

GPL-2.0-or-later, like Dolphin (see [`LICENSE`](LICENSE)). Bundled third-party code keeps its own license (dlmalloc: public domain; Mesa: MIT).

Not affiliated with Nintendo, Sony or the Dolphin Emulator project.
