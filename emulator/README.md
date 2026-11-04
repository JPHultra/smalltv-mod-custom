# SmallTV dashboard emulator

A host-side 240x240 emulator for the dashboard renderer. It compiles the real
firmware `ViewModel`, `ViewRender`, `ViewIcons`, usage store and anti-aliased
font against a fake `Arduino_GFX` framebuffer, so the pixels and text metrics
match the device.

It does not emulate Wi-Fi, the web UI, OTA, buttons or provider fetching. Usage
data comes from a local JSON file.

## Build

Prerequisites: a C++17 compiler, `pkg-config`, SDL2 development files, and a
firmware build once so `.pio/libdeps/.../ArduinoJson` exists.

Linux:

```sh
# Debian/Ubuntu: sudo apt install build-essential pkg-config libsdl2-dev
# Arch:          sudo pacman -S base-devel pkgconf sdl2
make -C emulator
```

Windows: use WSL, or MSYS2 with `g++`, `pkg-config` and the SDL2 package:

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 make
make -C emulator
```

## Run

```sh
emulator/build/smalltv-emulator \
  --view examples/view.json \
  --usage daemon/usage.example.json \
  --time 10:36 \
  --scale 2
```

- `--view` draw-list JSON
- `--usage` normalized usage JSON
- `--time HH:MM` fixed clock; omit for the local clock
- `--scale` integer window scale, 1-8
- `--screenshot PATH` write a 240x240 P6 PPM and exit
- `Esc` quits

The default files are `examples/view.json` and `daemon/usage.example.json`.

## Release packages

The GitHub release includes ready-to-run packages:

- Linux: `smalltv-emulator-linux-x86_64.tar.gz` (needs the system `libSDL2` runtime)
- Windows: `smalltv-emulator-windows-x86_64.zip` (includes `SDL2.dll`)

## Screenshot

```sh
emulator/build/smalltv-emulator \
  --view examples/view.json --usage daemon/usage.example.json \
  --time 10:36 --screenshot /tmp/smalltv.ppm
magick /tmp/smalltv.ppm /tmp/smalltv.png
```

Use the matching usage JSON from your daemon/device to preview real values.
