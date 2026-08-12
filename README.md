<div align=center><img src="./extras/logo.png" width=250></div>
<h1 align="center">Streets of Rage X · Nintendo Switch port</h1>

A wrapper/loader that runs the **Android release of Streets of Rage X**
(the OpenBOR fan-game) on the Nintendo Switch. It loads the Android
`arm64-v8a` binaries straight from `switch/sorx_nx/` (`libhidapi.so`,
`libSDL2.so`, `libopenbor.so`), provides a minimal Android-like environment
(fake JNI, a bionic→newlib libc shim, an EGL/GLES import bridge, and a
JNI-driven audio bridge over `audout`), and drives SDL2's real
`org.libsdl.app.SDLActivity` native lifecycle so OpenBOR's own SDL2 game loop
runs unmodified.

### How to install

On your SD card, the app lives in `/switch/sorx_nx/`.

```
/switch/sorx_nx/sorx.nro       <- the port (this repo builds it)
/switch/sorx_nx/libhidapi.so   <- from .apk (Android arm64-v8a build)
/switch/sorx_nx/libSDL2.so     <- from .apk
/switch/sorx_nx/libopenbor.so  <- from .apk
/switch/sorx_nx/bor.pak        <- from .apk, the game data; OpenBOR opens "bor.pak"
                                   relative to its CWD, NOT under an assets/
                                   subfolder (that layout only existed inside
                                   the APK)
```

Launch it through a **game/title override** (hold R on a game and open it) or
a forwarder, so the port runs with full RAM. It will not work in applet/album
mode.

Saves and `config.txt` live in `/switch/sorx_nx/save/`.

`config.txt` keys:
* `screen_width` / `screen_height` — `-1` = auto (1080p docked, 720p handheld).


### Known Limitations

- At the first run, the game will extract the assets from .pak in the directory folder. (It may take a while)
- Videos are stubbed due to decode limitation. (You can skip pressing Start)
- Game loadings may take a while, but after loaded a game, you can play all stages with no load time.

### How to build

You need devkitA64 (devkitPro) with these packages:

* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-zlib`

### Credits
- <b>elliencode</b> — the original Android so-loader
- <b>fgsfds</b>, <b>Andy Nguyen</b> and <b>NaGaa95</b> — the Switch so-loader
  groundwork (`so_util.c`, the fake-JNI object model, the libc shim) this
  wrapper is built on, via the NBA Jam Switch wrapper lineage.
- <b>Kratus</b> — for  Streets of Rage X Game.

### Legal

This project has no affiliation with SEGA. Streets of Rage X is a fan-game;
this repo contains no game assets beyond what you provide yourself in
`dist/sorx_nx/`. Source under the MIT License; see LICENSE.
