# SmallTV dashboard emulator

A host-side 240x240 emulator for the dashboard renderer. It compiles the real
firmware `ViewModel`, `ViewRender`, `ViewIcons`, usage store and anti-aliased
font against a fake `Arduino_GFX` framebuffer, so the pixels and text metrics
match the device.

The emulator also starts a local device manager at `http://127.0.0.1:8788/`:

- discovers SmallTV devices on the local subnet
- serves a full copy of the device web UI through a local proxy
- supports the device web UI password (HTTP Digest)
- serves the visual dashboard editor at `/editor`
- proxies dashboard, overlay, usage, settings, export/import and firmware upload
- accepts local `.bin` firmware uploads and the device's GitHub self-update

The emulator does not emulate Wi-Fi/OTA in the firmware itself; it talks to real
devices over the network. Usage data in the SDL preview comes from a local JSON
fixture.

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
emulator/build/smalltv-emulator --view examples/view.json \
  --usage daemon/usage.example.json --scale 2
```

It opens the dashboard window and the local device manager. Use **Scan** to find
the device, select it, then use the full web UI or **Open dashboard editor**.

Options:

- `--view` draw-list JSON for the emulator window
- `--usage` normalized usage JSON for the emulator window
- `--time HH:MM` fixed clock; omit for the local clock
- `--scale` integer window scale, 1-8
- `--port` local manager port (default 8788)
- `--assets` directory containing `manager.html`, `device-ui.html` and `editor/`
- `--no-browser` do not open the manager automatically
- `--screenshot PATH` write a 240x240 P6 PPM and exit
- `Esc` quits

The local manager uses the device URL selected in its toolbar. The visual editor
is served locally so mixed-content blocking does not apply. Firmware upload,
GitHub update, settings, WiFi scan, export/import and factory reset all go
through the device proxy.

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
