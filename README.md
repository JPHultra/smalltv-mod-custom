# smalltv-dash

Personal dashboard firmware for the GeekMagic SmallTV family, built from the
upstream `smalltv-mod` codebase. It keeps one declarative 240x240 dashboard, a
transient webhook overlay, and a normalized Claude/ChatGPT/DeepSeek usage feed.
The stock ticker, plane radar, theme engine, notification presets and
Home Assistant/MQTT screens have been removed.

New here? Follow [SETUP.md](SETUP.md) for the complete build, flash, dashboard, editor and usage setup.

## Hardware targets

| Env | Board |
|---|---|
| `smalltv` | SmallTV / SmallTV-ultra, ESP8266 |
| `smalltv_c2` | SmallTV ESP32-C2 / ESP8684 |
| `smalltv_esp32` | NM-TV-154, classic ESP32 |
| `smalltv_esp32_8mb` | SmallTV Pro, 8 MB classic ESP32 |
| `smalltv_loader` | one-time ESP8266 loader for the Ultra |

## Build

The fast path builds every release image in the order that minimizes pioarduino
framework rebuilds, stages the eight assets in `dist/`, and runs the local 8 KB
slot gate:

```sh
pip install platformio==6.1.19
python3 tools/build_release.py
```

Useful options:

```sh
python3 tools/build_release.py --envs smalltv,smalltv_c2
python3 tools/build_release.py --pio /path/to/pio --jobs 8
python3 tools/build_release.py --clean       # wipe .pio/build, keep packages
python3 tools/build_release.py --dry-run
```

The script pins `platformio`/`pioarduino` to 6.1.19 (the SCons that works with
the ESP32 framework), and groups the ESP8266, C2, classic-custom, and
classic-stock builds. The individual commands still work:

```sh
pio run -e smalltv
pio run -e smalltv_c2
pio run -e smalltv_esp32
pio run -e smalltv_esp32_8mb
```

After editing `src/webui.html`, regenerate the served copy:

```sh
python tools/gzip_webui.py
```

Host checks:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc \
  -I.pio/libdeps/smalltv/ArduinoJson/src tests/test_storage.cpp -o /tmp/storage && /tmp/storage
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc tests/test_webguard.cpp -o /tmp/webguard && /tmp/webguard
c++ -std=c++11 -Wall -Wextra -Werror -g -DSMALLTV_ESP8266 \
  -Itests/shim -Isrc -Isrc/features/view -I.pio/libdeps/smalltv/ArduinoJson/src \
  tests/test_view.cpp src/features/view/ViewModel.cpp -o /tmp/view && /tmp/view
g++ -std=c++17 -O2 -Wall -Wextra -Iemulator -Iemulator/image -Isrc \
  tests/test_image_asset.cpp emulator/image/HostImageSource.cpp \
  emulator/image/ImageCodec.cpp emulator/image/GifDecoder.cpp \
  -o /tmp/image-asset && /tmp/image-asset
c++ -std=c++11 -Wall -Wextra -Werror -g -DSMALLTV_ESP8266 \
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_USE_DOUBLE=0 \
  -Itests/shim -Isrc -Isrc/features/usage -I.pio/libdeps/smalltv/ArduinoJson/src \
  tests/test_usage.cpp src/features/usage/UsageClient.cpp -o /tmp/usage && /tmp/usage
python3 -m unittest discover -s daemon -p 'test_*.py'
```

Flashing is unchanged from upstream: OTA from the stock web UI on the ESP8266,
esptool `write_flash 0x0 firmware.factory.bin` on the ESP32 boards, and the
two-step loader on the Ultra. Self-update pulls release assets from
`JPHultra/smalltv-mod-custom`, so tag releases as `vX.Y.Z`.

Release locally, without GitHub Actions:

```sh
# edit FW_VERSION in src/config.h first
python3 tools/build_release.py --pio /path/to/pio --jobs 8
python3 tools/package_emulator.py \
  --windows-bin /path/to/smalltv-emulator.exe \
  --sdl2-dll /path/to/SDL2.dll
python3 tools/ci_checks.py assets
python3 tools/verify_update.py --local-only --from-version <previous-version>
git add src/config.h
git commit -m "Bump FW_VERSION to X.Y.Z-custom"
git push origin main
git tag vX.Y.Z-custom
git push origin vX.Y.Z-custom
gh release create vX.Y.Z-custom dist/* --title "vX.Y.Z-custom" \
  --notes "Release notes" --prerelease
python3 tools/verify_update.py --tag vX.Y.Z-custom \
  --from-version <previous-version>
gh release edit vX.Y.Z-custom --prerelease=false --latest
```

The eight firmware assets plus the two emulator packages are attached to the
release by `gh`; keep the firmware names unchanged because the firmware's
`UPDATE_ASSET` values point at them. Verify the prerelease before promoting it
to latest so a broken asset list never becomes a release target.

## Dashboard emulator

A native SDL2 + Dear ImGui desktop app runs the real view renderer and
anti-aliased font. It includes device discovery, native settings, firmware
upload and a visual dashboard editor. There is no local web server and no
browser/WebView dependency:

```sh
make -C emulator
emulator/build/smalltv-emulator --view examples/view.json --usage daemon/usage.example.json
```

Use the **Device**, **Design**, **Usage** and **Settings** tabs. The Design
editor calls its draw-list entries **components** and supports multi-select,
copy/paste, keyboard nudging, snapping, rulers, guides, groups, visibility,
locking, explicit Z order, distribute controls, usage preview states, device
image management, portable dashboard bundles and autosave recovery. Components,
Document and Inspector are collapsible drawers; the Inspector appears only when
something is selected and can be closed. The device web UI password uses HTTP
Digest; app self-update, firmware release selection, manual firmware upload and
settings export/import are handled directly by the app. See
[`emulator/README.md`](emulator/README.md) for details and Windows/MSYS2
instructions.

## Dashboard

The device stores one draw list at `/view.json` and paints it as the main
screen. It is edited with the static visual editor in [`editor/`](editor/) or
the JSON box in the device's **View** tab. The firmware never includes the
editor.

```json
{
  "bg": "#0b1420",
  "ttl": 15,
  "draw": [
    {"t":"text","x":120,"y":30,"s":6,"c":"#e9edf4","a":"c","v":"{HH}:{MM}"},
    {"t":"usage","x":8,"y":120,"w":224,"h":82,"c":"#d97757","provider":"claude","metric":-1}
  ]
}
```

Primitives: `fill`, `rect`, `rrect`, `circle`, `line`, `text`, `icon`,
`bitmap`, `image`, `usage`. A usage component with `metric` omitted or set to `-1`
stacks every metric the provider sends; `0`-`4` picks one. Text supports `{HH}`, `{hh}`, `{MM}`, `{SS}`, `{DD}`,
`{MON}`, `{MONTH}`, `{WD}`, `{WEEKDAY}`, `{YYYY}`. `ttl` is used by the overlay;
the stored dashboard ignores it. A sample lives in
[`examples/view.json`](examples/view.json).

Each component may also carry `id`, `name`, `z`, `hidden`, `locked` and `gid`.
`z` is drawn back-to-front (higher is in front); omitted values use array order.
`hidden` is omitted by the renderer, `locked` is editor-only, and groups are
editor metadata stored in a top-level `groups` array. Groups support move,
duplicate, delete, hide, lock and Z order; they are not sent as render commands
and do not add recursive/nested group support.

Text components support an optional box with `w`, `h`, `padX`, `padY`,
`mx`, `my`, `va` and `lineGap`, so the text can be anchored inside a fixed
region. Sizes are exposed as pixels through `sizePx`. Usage components can add
an independent `style.layout` geometry for the card title, reset line and every
metric row's label, value and bar. Usage text sizes are exposed as `namePx`,
`labelPx`, `valuePx` and `resetPx`. Bitmap/image display sizes use
`displayW`/`displayH` in pixels. The desktop inspector exposes numeric pixel
fields and draggable sub-element boxes; legacy scale fields remain in saved
views for older firmware compatibility.

Image and GIF backgrounds are imported by the native desktop app, not the web
editor. The app converts PNG/JPEG/BMP to RGB565 and animated GIF to indexed
256-color frames, uploads the asset to the device, and stores only a reference
in the dashboard:

```json
{
  "bg": "#0b1420",
  "bgImage": "aurora-1a2b3c4d",
  "draw": [
    {"t":"image","src":"aurora-1a2b3c4d","x":0,"y":0,"s":1,"a":"l"}
  ]
}
```

`bgImage` is drawn first with a 240x240 cover crop; `t:"image"` is a draggable
asset component. The legacy hex `bitmap` primitive remains supported. Assets live
under `/img` and are managed through `/api/images` and `/api/image`; they are
never embedded in the view JSON or the small RAM bitmap pool. The ESP8266 and
4 MB ESP32 boards cap an asset at about 700 KB, while the 8 MB Pro board allows
up to 3 MB. Animated backgrounds run at a capped frame rate.

### Usage card style

Usage components accept a `style` object so text sizing, alignment, visibility,
colours, bar layout and custom labels can be changed without rebuilding the
firmware:

```json
{
  "t": "usage",
  "provider": "claude",
  "metric": -1,
  "style": {
    "nameSize": 2, "labelSize": 1, "valueSize": 4, "resetSize": 1,
    "nameAlign": "l", "labelAlign": "r", "valueAlign": "l",
    "showName": true, "showDot": true, "showLabel": true, "showValue": true,
    "showBar": true, "showReset": true, "showPanel": true,
    "barHeight": 8, "padding": 12, "radius": 8,
    "title": "Claude",
    "labels": ["5 hour", "Weekly"]
  }
}
```

Sizes of `0` mean auto-fit. `labelAlign` and `valueAlign` default to the
layout-appropriate alignment when omitted. The default style reproduces the
previous card appearance.

Publish the editor to GitHub Pages from this PC with
`tools/deploy_pages.sh`; it force-pushes `editor/` to the `gh-pages` branch and
sets Pages to deploy from that branch. The URL is
`https://jphultra.github.io/smalltv-mod-custom/`. The editor has drag/resize
handles, undo/redo (Ctrl/Cmd+Z), alignment and z-order controls, real icon
previews, and a clickable bitmap pixel grid. A page served over HTTPS cannot
fetch an HTTP device (mixed-content blocking), so direct push only works from a
local HTTP copy or the device's own **View** tab.

## Webhook overlay

Push a draw list and the panel shows it above the dashboard for its `ttl`.
`priority` (0-3) lets a more important message interrupt the current overlay; an
equal or lower priority waits behind it (one item).

```sh
curl -X POST http://smalltv.local/api/view \
  -H 'Content-Type: text/plain' \
  --data '{"ttl":20,"priority":1,"bg":"#000000","draw":[{"t":"text","x":120,"y":110,"s":3,"c":"#ffffff","a":"c","v":"Build finished"}]}'
```

The **View** tab can also poll a URL on a schedule (`view.pollUrl`,
`view.pollSec`), optionally with `view.pollAuthHeader` / `view.pollAuthValue`.
`204` or an empty body clears the overlay and its queue; invalid JSON leaves
them alone. The overlay is never persisted.

## Usage collection

The native desktop app can collect Claude, Codex/ChatGPT and DeepSeek usage
itself, store credentials in the OS secure store, and push directly to
`/api/usage`. It also includes a headless worker mode for background/autostart
use. The Python daemon remains available as an alternative.

The external daemon normalizes the three providers and POSTs the document to
`/api/usage`:

```json
{"providers":[
  {"id":"claude","name":"Claude","ok":true,"metrics":[{"label":"5h","pct":71,"resetMin":142}]},
  {"id":"chatgpt","name":"Codex","ok":true,"metrics":[{"label":"5h","pct":99,"resetMin":292},
                                                       {"label":"7d","pct":55,"resetMin":6372},
                                                       {"label":"plan","value":"plus","pct":0,"resetMin":0}]},
  {"id":"deepseek","name":"DeepSeek","ok":true,"metrics":[{"label":"balance","value":"110 CNY","pct":0,"resetMin":0},
                                                          {"label":"tokens today","value":"1.2M","pct":0,"resetMin":0}]}
]}
```

`pct` is the **percentage remaining** (0-100), not the percentage consumed.
The daemon converts provider utilization/used percentages before sending them.
`daemon/usage_daemon.py` aggregates the providers:

- **Claude** via the native `claude` adapter. Paste a Claude Code credentials
  JSON or access/refresh tokens in the setup UI; the legacy
  `{s,sr,w,wr,st,ok}` adapter also works.
- **Codex / ChatGPT subscription** via the `codex` adapter. Paste the Codex auth
  JSON or access/refresh/account fields in the setup UI. It refreshes tokens,
  maps the 5-hour and weekly windows plus the plan name, and optionally adds
  credits/additional windows with `includeCredits` / `includeAdditional`.
- **DeepSeek** via the `deepseek` adapter: official balance API plus optional
  token usage from `daemon/deepseek_proxy.py`. Enter the API key in the setup UI.
  DeepSeek has no aggregate usage endpoint, so the proxy forwards API requests,
  counts OpenAI and Anthropic usage, and serves running, daily and per-day totals
  to the daemon.
- **Generic / normalized** sources via dotted JSON paths or a pre-shaped object.

One-step setup installs auto-start and opens the credential page:

```sh
daemon/setup_linux.sh --device http://smalltv.local
```

```bat
daemon\setup_windows.bat -DeviceUrl http://smalltv.local
```

Add `--linger` on Linux or `-AtStartup` on Windows for start-before-login. The
manual setup UI is:

```sh
python3 daemon/setup_ui.py --config daemon/usage-daemon.json
```

On Windows use `daemon\setup_windows.bat`. It opens a browser and setup token
automatically; the page never reads `~/.claude`, `~/.codex` or
environment variables, and saves only submitted fields to
`~/.config/smalltv/credentials.json` on Linux/macOS or
`%APPDATA%\smalltv\credentials.json` on Windows (mode `0600`). The daemon
re-reads that store each cycle. A metric may carry a `value` string (for example `110 CNY` or
`1.2M tokens`) instead of a percentage. `daemon/push_usage.py` remains for a
single pre-built document, a URL, or demo data. The firmware only needs the
normalized contract; provider credentials stay on the daemon host.

## HTTP API

| Route | Purpose |
|---|---|
| `GET/POST /api/config` | settings |
| `GET /api/status` | device and provider status |
| `GET/POST /api/dashboard` | installed dashboard JSON |
| `POST /api/view`, `POST /api/view/clear` | transient overlay |
| `GET/POST /api/usage` | usage status / daemon push |
| `GET /api/logs`, `GET /api/logs/previous` | current / previous boot diagnostic log |
| `GET/POST /api/images`, `GET/DELETE /api/image?name=...` | image/GIF asset list, upload, download and delete |
| `POST /update` | firmware upload |

The optional web-UI password is digest auth. The editor token is an alternative
for `/api/dashboard`, `/api/view`, `/api/view/clear` and `/api/usage`, and
enables the CORS headers the GitHub Pages editor needs. `/api/usage` stays open
for the daemon, as before.

`/api/status` also reports firmware update progress in an `update` object
(`phase`, `progress`, `downloaded`, `total`, `target`, `message`, `error`).
Progress is live for uploads started by the desktop app or the manual web upload.

## Notes

- The ESP8266 image is about 814 KB flash / 60 KB static RAM in this build,
  down from the upstream full image, so the old `smalltv_lean` variant is gone.
- `src/config.h` points the desktop updater at `JPHultra/smalltv-mod-custom`; change it
  if the repository moves.
- Credits and license are unchanged from upstream: WTFPL, with the hardware and
  design credits in the upstream project.
