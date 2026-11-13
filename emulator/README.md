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
- Native usage collection for Claude, Codex/ChatGPT and DeepSeek
- In-app Usage setup page with OS secure credential storage
- Native DeepSeek token-usage proxy
- Headless usage worker mode for background/autostart use
- Native settings for WiFi, display, clock, button, view/overlay and security
- Live status and usage
- Native visual dashboard editor:
  - text, rectangle, rounded rectangle, circle, line, icon, bitmap, image, usage
    and fill components
  - drag, resize, multi-select, explicit Z order, duplicate/delete and groups
  - copy/paste, keyboard nudging, snapping, rulers, guides and alignment
  - component names, visibility/lock state and starter templates
  - usage preview states (live, waiting, error and long values)
  - usage style presets plus copy/paste style
  - device image asset manager and local autosave recovery
  - dashboard bundles containing the view and referenced image assets
  - distribute horizontally/vertically and measurement guides
  - properties, icon picker, bitmap pixel grid
  - configurable usage-card text, alignment, colours, bars, panels and labels
  - numeric and draggable text/usage geometry with padding and margins
  - pixel-based size controls with automatic legacy-view migration
  - collapsible Components, Document and Inspector drawers; Inspector opens only with selection
  - empty-canvas/background deselection and live background colour preview
  - PNG/JPEG/BMP and animated GIF import with RGB565 stills and indexed GIF frames
  - full-screen image/GIF backgrounds with cover cropping
  - undo/redo, JSON apply/copy
  - load/save from a file or the device
  - overlay test and clear
- Firmware upload (`.bin`) with the in-app file browser
- App self-update, firmware release install, settings export/import, reboot and factory reset
- Native file browser; no zenity/kdialog dependency
- Diagnostic log viewer for the current and previous boot
- Connection heartbeat with online/offline/update status and last-seen details
- Phase and progress display for firmware updates
- App self-update checks against GitHub at launch, with selectable firmware releases
- Connection-first startup with saved-device auto-connect
- OS-secure saved device passwords and a "Continue offline" design/usage mode
- Connection recovery banner that preserves the current workspace
- Flat graphite/amber desktop styling with vendored Inter fonts

Image/GIF assets are imported only in the desktop app. They are converted to a
compact device asset, uploaded to `/img`, and referenced by name from the
dashboard JSON. The web editor does not support image assets.

Windows app self-update writes its progress and failure details to
`%TEMP%\smalltv-update.log`. If an update cannot be applied, the updater restores
the previous folder and restarts the old app.

The app does not emulate Wi-Fi/OTA in firmware. Fonts live in
`emulator/assets/` and are included in the release packages. It talks to real devices over
the network. The local preview uses a JSON fixture until you edit the dashboard.

## Build

Prerequisites: a C++17 compiler, `pkg-config`, SDL2 development files, `make`,
and a firmware build once so `.pio/libdeps/.../ArduinoJson` exists.

Linux:

```sh
# Debian/Ubuntu: sudo apt install build-essential pkg-config libsdl2-dev libcurl4-openssl-dev libsecret-1-dev
# Arch:          sudo pacman -S base-devel pkgconf sdl2 curl libsecret
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

The app starts on a connection screen. It scans the local network, offers
saved-device auto-connect, and stores optional device passwords in DPAPI on
Windows or the OS keyring on Linux. If no device is available, **Continue
offline** opens Design and Usage; Device and Settings stay connection-only.
After a successful connection, Device shows discovery, a scaled live preview,
status/usage telemetry and maintenance actions; Design is the three-pane
dashboard editor; Usage holds provider credentials, fetch controls and the
DeepSeek proxy; Settings uses section navigation with two-column forms.
Settings > System can fetch the device's current and previous diagnostic logs.
A lost connection keeps the current workspace and shows a reconnect banner.

The native Usage tab stores Claude/Codex/DeepSeek credentials in the OS secure
store (DPAPI on Windows, Secret Service on Linux) and can push the normalized
usage document directly to the connected device. DeepSeek token usage still
requires pointing the DeepSeek client at `http://127.0.0.1:8789`.

The app checks GitHub for a newer desktop release when it opens. It does not
install automatically; the update prompt can defer or skip the version. The
Settings > System page also lists firmware releases and installs the selected
release through the device's normal OTA endpoint, avoiding the ESP8266
boot-time TLS/heap path.

The same executable has a headless worker mode:

```sh
emulator/build/smalltv-emulator --usage-worker --usage-device http://smalltv.local
```

Install it as a Linux user service or Windows logon task with:

```sh
tools/install_usage_worker.sh /path/to/smalltv-emulator http://smalltv.local
```

```bat
tools\install_usage_worker.bat C:\path	o\smalltv-emulator.exe http://smalltv.local
```

Options:

- `--view PATH` draw-list JSON loaded into the editor
- `--usage PATH` normalized usage JSON for the local preview
- `--time HH:MM` fixed clock; omit for the local clock
- `--tab NAME` start on `device`, `design` or `settings`
- `--settings-tab NAME` start Settings on `wifi`, `display`, `view`, `system` or `json`
- `--url URL` connect to a device at startup
- `--offline` start in Design/Usage without connecting to a device
- `--user USER` / `--pass PASS` device web UI credentials
- `--screenshot PATH` write a 240x240 P6 PPM and exit
- `Esc` quits

## Native editor

The editor has three panes: components on the left, a scaled real firmware
render in the centre, and component/view properties on the right. Device and
file actions live in the inspector. Click a component to select it, shift/ctrl
click or drag a selection box for multiple components, drag to move, and drag
the yellow corner handle to resize. **Add component** inserts a new component;
the toolbar also exposes Z order, groups, copy/paste, snapping, rulers, guides,
distribution and usage preview states. Usage components have style presets,
style copy/paste and custom title/metric labels. The JSON panel lets you
inspect or replace the draw list directly. Dashboard bundles include the view
and any referenced image assets; autosave keeps a local recovery copy.

## Release packages

- Linux: `smalltv-emulator-linux-x86_64.tar.gz` (needs the system SDL2, libcurl and libsecret runtimes)
- Windows: `smalltv-emulator-windows-x86_64.zip` (portable; includes `SDL2.dll`; uses built-in WinHTTP/DPAPI)
- Windows installer: `smalltv-emulator-windows-x86_64-setup.exe` (per-user NSIS installer; the ZIP remains the self-update payload)

Build the Windows installer with `makensis` by passing `--windows-installer` to
`tools/package_emulator.py`.

## Screenshot

```sh
emulator/build/smalltv-emulator --view examples/view.json \
  --usage daemon/usage.example.json --time 10:36 --screenshot /tmp/smalltv.ppm
magick /tmp/smalltv.ppm /tmp/smalltv.png
```
