# smalltv-dash

Personal dashboard firmware for the GeekMagic SmallTV family, built from the
upstream `smalltv-mod` codebase. It keeps one declarative 240x240 dashboard, a
transient webhook overlay, and a normalized Claude/ChatGPT/DeepSeek usage feed.
The stock ticker, plane radar, theme engine, notification presets and
Home Assistant/MQTT screens have been removed.

## Hardware targets

| Env | Board |
|---|---|
| `smalltv` | SmallTV / SmallTV-ultra, ESP8266 |
| `smalltv_c2` | SmallTV ESP32-C2 / ESP8684 |
| `smalltv_esp32` | NM-TV-154, classic ESP32 |
| `smalltv_esp32_wg` | NM-TV-154 with WireGuard |
| `smalltv_esp32_8mb` | SmallTV Pro, 8 MB classic ESP32 |
| `smalltv_loader` | one-time ESP8266 loader for the Ultra |

## Build

```sh
pip install platformio==6.1.19
pio run -e smalltv
pio run -e smalltv_c2
pio run -e smalltv_esp32 -e smalltv_esp32_wg
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
```

Flashing is unchanged from upstream: OTA from the stock web UI on the ESP8266,
esptool `write_flash 0x0 firmware.factory.bin` on the ESP32 boards, and the
two-step loader on the Ultra. Self-update pulls release assets from
`JPHultra/smalltv-mod-custom`, so tag releases as `vX.Y.Z` and let
`.github/workflows/build.yml` attach the images.

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
    {"t":"usage","x":8,"y":120,"w":224,"h":82,"c":"#d97757","provider":"claude","metric":0}
  ]
}
```

Primitives: `fill`, `rect`, `rrect`, `circle`, `line`, `text`, `icon`,
`bitmap`, `usage`. Text supports `{HH}`, `{hh}`, `{MM}`, `{SS}`, `{DD}`,
`{MON}`, `{MONTH}`, `{WD}`, `{WEEKDAY}`, `{YYYY}`. `ttl` is used by the overlay;
the stored dashboard ignores it. A sample lives in
[`examples/view.json`](examples/view.json).

The editor deploys to GitHub Pages from `editor/`; open it at
`https://jphultra.github.io/smalltv-mod-custom/`. A page served over HTTPS cannot
fetch an HTTP device (mixed-content blocking), so direct push only works from a
local HTTP copy or the device's own **View** tab.

## Webhook overlay

Push a draw list and the panel shows it above the dashboard for its `ttl`:

```sh
curl -X POST http://smalltv.local/api/view \
  -H 'Content-Type: text/plain' \
  --data '{"ttl":20,"bg":"#000000","draw":[{"t":"text","x":120,"y":110,"s":3,"c":"#ffffff","a":"c","v":"Build finished"}]}'
```

The **View** tab can also poll a URL on a schedule (`view.pollUrl`,
`view.pollSec`). `204` or an empty body clears the overlay; invalid JSON leaves
it alone. The overlay is never persisted.

## Usage daemon

One local process normalizes the three providers and POSTs the document to
`/api/usage`:

```json
{"providers":[
  {"id":"claude","name":"Claude","ok":true,"metrics":[{"label":"5h","pct":29,"resetMin":142}]},
  {"id":"chatgpt","name":"ChatGPT","ok":true,"metrics":[{"label":"5h","pct":62,"resetMin":88}]},
  {"id":"deepseek","name":"DeepSeek","ok":true,"metrics":[{"label":"balance","pct":81,"resetMin":0}]}
]}
```

`daemon/push_usage.py` pushes a file, a URL, or demo data. Provider collectors
are intentionally separate. The legacy Claude-only `{s,sr,w,wr,st,ok}` payload
is still accepted.

## HTTP API

| Route | Purpose |
|---|---|
| `GET/POST /api/config` | settings |
| `GET /api/status` | device and provider status |
| `GET/POST /api/dashboard` | installed dashboard JSON |
| `POST /api/view`, `POST /api/view/clear` | transient overlay |
| `GET/POST /api/usage` | usage status / daemon push |
| `POST /update` | firmware upload |

The optional web-UI password is digest auth. The editor token is an alternative
for `/api/dashboard`, `/api/view`, `/api/view/clear` and `/api/usage`, and
enables the CORS headers the GitHub Pages editor needs. `/api/usage` stays open
for the daemon, as before.

## Notes

- The ESP8266 image is about 608 KB flash / 47 KB static RAM in this build,
  down from the upstream full image, so the old `smalltv_lean` variant is gone.
- `src/config.h` points the updater at `JPHultra/smalltv-mod-custom`; change it
  if the repository moves.
- Credits and license are unchanged from upstream: WTFPL, with the hardware and
  design credits in the upstream project.
