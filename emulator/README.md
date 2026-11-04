# SmallTV desktop app

A native desktop application for the SmallTV dashboard. It runs the real
firmware `ViewModel`, `ViewRender`, `ViewIcons`, usage store and anti-aliased
font against a fake `Arduino_GFX` framebuffer, and adds a native device manager
and visual dashboard editor.

The app is **fully native**: SDL2 + Dear ImGui, no web server, no browser and no
HTML/WebView assets.

## Features

- Local live preview using the firmware renderer
- Subnet discovery of SmallTV devices
- Device web-UI password (HTTP Digest)
- Native settings for WiFi, display, clock, button, view/overlay and security
- Live status and usage
- Native visual dashboard editor:
  - text, rectangle, rounded rectangle, circle, line, icon, bitmap, usage and
    fill layers
  - drag, resize, layer ordering, duplicate/delete
  - properties, icon picker, bitmap pixel grid
  - undo/redo, alignment, JSON apply/copy
  - load/save from a file or the device
  - overlay test and clear
- Firmware upload (`.bin`) with the in-app file browser
- GitHub self-update, settings export/import, reboot and factory reset
- Native file browser; no zenity/kdialog dependency
- Diagnostic log viewer for the current and previous boot
- Connection heartbeat with online/offline/update status and last-seen details
- Phase and progress display for firmware updates
- Flat graphite/amber desktop styling with vendored Inter fonts

The app does not emulate Wi-Fi/OTA in firmware. Fonts live in
`emulator/assets/` and are included in the release packages. It talks to real devices over
the network. The local preview uses a JSON fixture until you edit the dashboard.

## Build

Prerequisites: a C++17 compiler, `pkg-config`, SDL2 development files, `make`,
and a firmware build once so `.pio/libdeps/.../ArduinoJson` exists.

Linux:

```sh
# Debian/Ubuntu: sudo apt install build-essential pkg-config libsdl2-dev
# Arch:          sudo pacman -S base-devel pkgconf sdl2
make -C emulator
```

Windows: use MSYS2 with `g++`, `pkg-config`, SDL2 and `make`:

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 make
make -C emulator
```

Dear ImGui is vendored under `emulator/third_party/imgui/` (MIT), using the SDL2
and SDL_Renderer2 backends. Inter is bundled under `emulator/assets/fonts/`
(OFL).

## Run

```sh
emulator/build/smalltv-emulator --view examples/view.json \
  --usage daemon/usage.example.json
```

Select or enter a device in the Device workspace and connect. Device shows
discovery, a scaled live preview, status/usage telemetry and maintenance
actions; Design is the three-pane dashboard editor; Settings uses section
navigation with two-column forms. Settings > System can fetch the device's
current and previous diagnostic logs.

Options:

- `--view PATH` draw-list JSON loaded into the editor
- `--usage PATH` normalized usage JSON for the local preview
- `--time HH:MM` fixed clock; omit for the local clock
- `--tab NAME` start on `device`, `design` or `settings`
- `--settings-tab NAME` start Settings on `wifi`, `display`, `view`, `system` or `json`
- `--url URL` connect to a device at startup
- `--user USER` / `--pass PASS` device web UI credentials
- `--screenshot PATH` write a 240x240 P6 PPM and exit
- `Esc` quits

## Native editor

The editor has three panes: layers on the left, a scaled real firmware render
in the centre, and layer/view properties on the right. Device and file actions
live in the inspector. Click a layer to select it, drag to move, and drag the
yellow corner handle to resize. **Add layer** inserts a new component at the
canvas centre. The JSON panel lets you inspect or replace the draw list
directly.

## Release packages

- Linux: `smalltv-emulator-linux-x86_64.tar.gz` (needs the system SDL2 runtime)
- Windows: `smalltv-emulator-windows-x86_64.zip` (includes `SDL2.dll`)

## Screenshot

```sh
emulator/build/smalltv-emulator --view examples/view.json \
  --usage daemon/usage.example.json --time 10:36 --screenshot /tmp/smalltv.ppm
magick /tmp/smalltv.ppm /tmp/smalltv.png
```
