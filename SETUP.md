# Spin up everything

This is the end-to-end guide for this repository: build the firmware, flash any
supported board, set up the dashboard, run the external editor, feed Claude /
Codex / DeepSeek usage, use the webhook overlay, and publish updates.

All commands assume you are at the repository root:

```sh
cd /path/to/smalltv-mod-custom
```

## 1. What you are spinning up

| Piece | Where | Purpose |
|---|---|---|
| Firmware | `src/` | One 240x240 dashboard, a transient webhook overlay, usage cards |
| Release builder | `tools/build_release.py` | Builds and stages all release images in the fewest framework rebuilds |
| Web UI | `src/webui.html` -> `src/webui.h` | Device settings, status, overlay JSON fallback |
| Visual editor | `editor/` | Static dashboard editor, deployable to GitHub Pages |
| Usage aggregator | `daemon/usage_daemon.py` | Claude + Codex + DeepSeek -> `/api/usage` |
| DeepSeek proxy | `daemon/deepseek_proxy.py` | Counts DeepSeek OpenAI/Anthropic token usage (the API has no aggregate endpoint) |
| Usage pusher | `daemon/push_usage.py` | Push an already-normalized document |
| Example dashboard | `examples/view.json` | Clock + Claude/Codex/DeepSeek cards |

There is no ticker, radar, HA/MQTT, theme engine, notification-preset mode or
carousel anymore. The dashboard is the only display mode; the webhook overlay is
transient above it.

## 2. Requirements

- Linux, macOS or WSL. These instructions use Linux paths; replace `/dev/ttyUSB0`
  with your port (`/dev/ttyACM0`, `COM3`, ...).
- Python 3.12 for PlatformIO. PlatformIO 6.1.19 does not reliably run on newer
  Pythons.
- PlatformIO 6.1.19.
- `esptool` for USB flashing and backups.
- Node 22+ only if you want to run the editor syntax check. The editor itself
  has no build step.
- Claude Code and/or Codex credentials to paste into the setup UI from any
  machine; no local login is required on the daemon host.
- A DeepSeek API key for the balance collector.
- Optionally, a local
  [clawdmeter-daemon](https://github.com/giovi321/clawdmeter-daemon) if you want
  the legacy Claude payload.

Create a local toolchain venv:

```sh
python3.12 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install 'platformio==6.1.19' 'esptool'
```

If `python3.12` is not installed, use `mise` (already used on this machine):

```sh
mise install python@3.12
mise exec python@3.12 -- python3.12 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install 'platformio==6.1.19' 'esptool'
```

The build script also pins `pioarduino` to 6.1.19, which is the SCons that works
with the ESP32 framework.

## 3. Build the firmware

The fast path builds every release target in framework-friendly order, stages
the eight assets in `dist/`, and runs the 8 KB OTA-slot gate:

```sh
python tools/build_release.py
```

Useful variants:

```sh
python tools/build_release.py --envs smalltv,smalltv_c2
python tools/build_release.py --clean       # wipe .pio/build, keep packages
python tools/build_release.py --pio /path/to/pio --jobs 8
python tools/build_release.py --dry-run
```

Expected output in `dist/`:

```text
smalltv-mod-firmware.bin                  # ESP8266 / Ultra app image
smalltv-mod-loader.bin                    # Ultra first-install loader
smalltv-mod-firmware-c2.bin               # ESP32-C2 app
smalltv-mod-firmware-c2.factory.bin       # ESP32-C2 first install
smalltv-mod-firmware-esp32.bin            # NM-TV-154 app
smalltv-mod-firmware-esp32.factory.bin    # NM-TV-154 first install
smalltv-mod-firmware-esp32-pro.bin        # SmallTV Pro app / stock-UI install
smalltv-mod-firmware-esp32-pro.factory.bin# SmallTV Pro first install / recovery
```

Manual per-env builds still work:

```sh
pio run -e smalltv
pio run -e smalltv_c2
pio run -e smalltv_esp32
pio run -e smalltv_esp32_8mb
pio run -e smalltv_loader
```

After editing `src/webui.html`, regenerate the served page and rebuild:

```sh
python tools/gzip_webui.py
```

The release builder uses the current `.pio` caches; only switching between the
C2, classic-ESP32 custom, classic-ESP32 stock and ESP8266 framework states costs
a framework rebuild.

## 4. Flash by board

Back up the stock firmware before overwriting it. The factory files are written
at offset `0x0`; the non-factory files are app images for OTA.

### SmallTV / SmallTV-ultra (ESP8266)

Standard SmallTV, from the stock web UI:

1. Browse to `http://<device-ip>/update`.
2. Upload `dist/smalltv-mod-firmware.bin`.
3. The device reboots into this firmware.

SmallTV-ultra, whose stock updater rejects the full image with `Not Enough
Space`:

1. Browse to `http://<device-ip>/update`, upload `dist/smalltv-mod-loader.bin`.
2. Join the open `SmallTV-Loader` AP, browse to `http://192.168.4.1/update`.
3. Upload `dist/smalltv-mod-firmware.bin`.

UART recovery on either ESP8266 board:

```sh
PORT=/dev/ttyUSB0
python -m esptool --port "$PORT" read_flash 0x0 0x400000 stock-backup.bin
python -m esptool --port "$PORT" erase_flash
python -m esptool --port "$PORT" write_flash 0x0 dist/smalltv-mod-firmware.bin
```

Hold `GPIO0` to GND while powering on to enter flash mode, then release it.

### SmallTV ESP32-C2 / ESP8684

```sh
PORT=/dev/ttyUSB0
python -m esptool --chip esp32c2 --port "$PORT" read_flash 0x0 0x400000 stock-backup.bin
python -m esptool --chip esp32c2 --port "$PORT" --baud 921600 \
  write_flash 0x0 dist/smalltv-mod-firmware-c2.factory.bin
```

The onboard CH340C handles auto-reset, so no button is needed. Use the system
`esptool`, not the bundled PlatformIO one (the C2 env is already configured this
way).

### NM-TV-154 (classic ESP32, 4 MB)

```sh
PORT=/dev/ttyUSB0
python -m esptool --chip esp32 --port "$PORT" read_flash 0x0 0x400000 stock-backup.bin
python -m esptool --chip esp32 --port "$PORT" --baud 921600 \
  write_flash 0x0 dist/smalltv-mod-firmware-esp32.factory.bin
```


### SmallTV Pro (classic ESP32, 8 MB)

The USB-C port is power only. First install over the air from the stock web UI:

1. Browse to `http://<device-ip>/update`.
2. Upload `dist/smalltv-mod-firmware-esp32-pro.bin` (the app image, not the
   factory image).

The partition layout matches stock, so the stock updater accepts it.

UART backup/recovery:

```sh
PORT=/dev/ttyUSB0
python -m esptool --chip esp32 --port "$PORT" read_flash 0x0 0x800000 stock-backup.bin
python -m esptool --chip esp32 --port "$PORT" --baud 921600 \
  write_flash 0x0 dist/smalltv-mod-firmware-esp32-pro.factory.bin
```

After the first install, every board updates from the web UI's **System** tab.

## 5. First boot and WiFi

1. The device shows `SETUP MODE` and opens `SmallTV-Setup`.
2. Join it; if no captive portal appears, browse to `http://192.168.4.1`.
3. Open **WiFi**, scan, select your 2.4 GHz network, enter the password, save.
4. The device reboots and shows the network, its IP, and
   `http://<hostname>.local`.

The default hostname is unique, usually `smalltv-<chipid>`. Set a friendly
hostname in **WiFi** if you want, then use the shown IP or the exact mDNS name.
`smalltv.local` only works if you named the device `smalltv`.

Optional on this screen:

- **WiFi password** for the web UI, under **System**. Off by default. There is
  no recovery other than reflashing.
- **Night mode** and timezone under **Display**.
- **Button** actions under **Display** on classic-ESP32 boards.

## 6. Dashboard and editor

The device stores one draw list at `/view.json`. You can edit it with the
external visual editor or paste JSON into the device's **View** tab.

### Option A: visual editor on GitHub Pages

Publish the editor from this PC:

```sh
tools/deploy_pages.sh
```

It copies `editor/` to the `gh-pages` branch, force-pushes that branch, and
configures GitHub Pages to deploy from it. Run it again after editor changes.
The URL is:

```text
https://jphultra.github.io/smalltv-mod-custom/
```

Direct push from that HTTPS page to an HTTP device is blocked by mixed content.
Use the editor for design/export, or run a local HTTP copy for direct push.

### Option B: visual editor locally, direct push

```sh
cd editor
python3 -m http.server 8000
```

Open `http://localhost:8000`, then:

1. **Device URL**: `http://<device-ip>` or the exact `http://<hostname>.local`.
2. **Editor token**: the token you set in the device's **View** tab.
3. **Load example** to start from the sample dashboard.
4. **Load** to pull the installed dashboard, **Save dashboard** to install it,
   **Test overlay** to show it for its TTL.
5. **Fetch usage** to preview live provider data.

The editor can load/push `/api/dashboard`, push/clear `/api/view`, and read
`/api/usage`. It supports drag/resize handles, undo/redo (Ctrl/Cmd+Z), alignment
and z-order controls, real icon previews, and a clickable bitmap pixel grid. The
token is in the query string; it is a write credential, not the web UI password.

### Option C: device-only JSON fallback

1. Open the device web UI -> **View**.
2. Set an **Editor token** and save.
3. Open `examples/view.json` and paste it into the JSON textarea.
4. **Save dashboard**, then **Test overlay**.

### The draw list

```json
{
  "bg": "#0b1420",
  "ttl": 15,
  "priority": 0,
  "draw": [
    {"t":"text","x":120,"y":6,"s":5,"c":"#e9edf4","a":"c","v":"{HH}:{MM}"},
    {"t":"text","x":120,"y":48,"s":2,"c":"#86e2bf","a":"c","v":"{WEEKDAY} {YYYY}-{MON}-{DD}"},
    {"t":"usage","x":8,"y":68,"w":224,"h":54,"c":"#d97757","provider":"claude","metric":-1},
    {"t":"usage","x":8,"y":126,"w":224,"h":62,"c":"#10a37f","provider":"chatgpt","metric":-1},
    {"t":"usage","x":8,"y":192,"w":224,"h":44,"c":"#4d6bfe","provider":"deepseek","metric":-1}
  ]
}
```

Primitives: `fill`, `rect`, `rrect`, `circle`, `line`, `text`, `icon`, `bitmap`,
`usage`. Text tokens: `{HH}`, `{hh}`, `{MM}`, `{SS}`, `{DD}`, `{MON}`,
`{MONTH}`, `{WD}`, `{WEEKDAY}`, `{YYYY}`. A usage `metric` of `-1` (or omitted)
stacks all metrics; `0`-`4` selects one. `priority` is the overlay priority (0-3).

## 7. Usage: Claude, Codex, DeepSeek

### 7.1 Configure the aggregator and credentials

```sh
cp daemon/usage-daemon.example.json daemon/usage-daemon.json
```

`daemon/usage-daemon.json` is git-ignored because it may contain local URLs. Set
its `device` to your device URL:

```json
"device": "http://192.168.1.50"
```

The example stores provider credentials in
`~/.config/smalltv/credentials.json`. Enter them with the local setup UI:

```sh
python3 daemon/setup_ui.py --config daemon/usage-daemon.json
```

It prints a URL and a generated setup token:

```text
SmallTV usage setup: http://127.0.0.1:8790/
Setup token: <generated-on-startup>
```

Open the URL, enter the token, and fill the provider cards. The page:

- never reads `~/.claude`, `~/.codex` or environment variables on this host;
- accepts pasted Claude/Codex credentials JSON from any machine, or manual
  access/refresh/account fields;
- saves only what you submit to `~/.config/smalltv/credentials.json` (mode
  `0600`); blank fields keep the previous value;
- never displays saved secrets back and never writes them to the daemon log;
- has a **Test** button for each provider, which does not push to the device.

Use **Save** after filling a provider and **Clear** to remove all of its
credentials. `--credentials PATH` or `credentialsFile` in the daemon config
changes the store location. The setup UI is localhost-only by default; use
`--bind 0.0.0.0` only for a VM/container and keep the setup token private.

The device's `/api/usage` push endpoint is open by design (the daemon cannot
carry the web UI password).

### 7.2 Claude

Add the native Claude provider; no credential file path is required:

```json
{"id":"claude","name":"Claude","adapter":"claude"}
```

In the setup UI, either paste the JSON from a machine where Claude Code is
logged in:

```json
{"claudeAiOauth":{"accessToken":"...","refreshToken":"...","expiresAt":0}}
```

or enter the access token, refresh token and optional expiry manually. The JSON
is parsed in your browser; only the fields you save reach the daemon. If you only
have a long-lived `claude setup-token` value, paste it as the access token. The
adapter refreshes an expired access token with the refresh token and the public
Claude Code OAuth client id.

The old `claude-legacy` adapter is still available for the old
[clawdmeter-daemon](https://github.com/giovi321/clawdmeter-daemon) payload; if
that daemon pushes to `/api/usage` directly, remove the aggregator's Claude entry
to avoid duplicate updates. `authFile` and `tokenEnv` are optional explicit
legacy fallbacks; the setup UI never reads them.

### 7.3 Codex / ChatGPT subscription

Add the Codex provider; no `codex login` is required on the daemon host:

```json
{"id":"chatgpt","name":"Codex","adapter":"codex",
 "includePlan":true,"includeCredits":false,"includeAdditional":false}
```

In the setup UI, paste the Codex auth JSON from any machine:

```json
{"tokens":{"access_token":"...","refresh_token":"...","account_id":"..."}}
```

or enter the access token, refresh token and account id manually. The JSON is
parsed in the browser; only the saved fields reach the daemon. The adapter calls
`https://chatgpt.com/backend-api/wham/usage`, maps the 5-hour and weekly windows
plus the plan name, and refreshes an expired access token through
`https://auth.openai.com/oauth/token`.

`includePlan` is on by default. `includeCredits` adds the credits balance when
present; `includeAdditional` adds other rate-limit windows. The mapper caps a
provider at five metrics (`USAGE_MAX_METRICS`), so a card can show all of them
when it has enough height. This is the endpoint the Codex CLI itself uses; it is
not a public documented API, so it can change. The daemon does not send the
email, account id or tokens to the device. `authFile` is an optional explicit
legacy fallback.

### 7.4 DeepSeek balance

Add the DeepSeek provider and enter its API key in the setup UI:

```json
{"id":"deepseek","name":"DeepSeek","adapter":"deepseek"}
```

Balance comes from `https://api.deepseek.com/user/balance`. The key is stored in
the same `0600` credentials file; no environment variable is required.
`apiKeyEnv` / `apiKeyFile` are optional explicit legacy fallbacks.

### 7.5 DeepSeek token usage

DeepSeek's public API returns usage per response but has no aggregate endpoint.
Run the local proxy and route the DeepSeek client through it:

```sh
python daemon/deepseek_proxy.py
```

```text
OpenAI-compatible base URL: http://127.0.0.1:8789
Anthropic-compatible:       http://127.0.0.1:8789/anthropic
```

The proxy forwards to `https://api.deepseek.com`, adds
`stream_options.include_usage` to streaming JSON requests, counts OpenAI
`usage` objects and Anthropic streaming usage, and serves:

```sh
curl http://127.0.0.1:8789/usage
curl -X POST http://127.0.0.1:8789/usage/reset
```

State lives in `~/.local/state/smalltv/deepseek-usage.json`. The API key is
forwarded per request from the DeepSeek client and never written there. `/usage`
includes the running total, `date`, `today` and per-day `days` buckets, so you
can show today's tokens instead of the all-time count.

Add the usage source to the provider:

```json
{
  "id":"deepseek","name":"DeepSeek","adapter":"deepseek",
  "usageUrl":"http://127.0.0.1:8789/usage",
  "usagePath":"today.total_tokens",
  "usageLabel":"tokens today",
  "usageBudget":0
}
```

Use `"usagePath":"total_tokens"` for the all-time counter. Set `usageBudget` to a
token budget if you want a fill bar; `0` shows the count only. If the proxy is
down, balance still appears and the daemon logs the usage source error.

### 7.6 Run the aggregator

```sh
python daemon/usage_daemon.py --config daemon/usage-daemon.json
```

The daemon re-reads the credentials store each cycle, so saving in the setup UI
takes effect without a restart. Useful checks:

```sh
python daemon/usage_daemon.py --config daemon/usage-daemon.json --once --dry-run
python daemon/usage_daemon.py --config daemon/usage-daemon.json --once
curl http://<device-ip>/api/status
```

A metric can carry a `pct` (0-100), a `value` (for example `110 CNY` or
`1.2M`), or both. The dashboard's usage cards show the value; `pct > 0` adds a
fill bar.

### 7.7 Optional: run them at login

Create user units. Adjust `%h/Projects/smalltv-mod-custom` if your checkout is
elsewhere.

`~/.config/systemd/user/smalltv-deepseek-proxy.service`:

```ini
[Unit]
Description=SmallTV DeepSeek usage proxy
After=network-online.target

[Service]
WorkingDirectory=%h/Projects/smalltv-mod-custom
ExecStart=%h/Projects/smalltv-mod-custom/.venv/bin/python daemon/deepseek_proxy.py
Restart=on-failure

[Install]
WantedBy=default.target
```

`~/.config/systemd/user/smalltv-usage.service`:

```ini
[Unit]
Description=SmallTV usage aggregator
After=network-online.target smalltv-deepseek-proxy.service

[Service]
WorkingDirectory=%h/Projects/smalltv-mod-custom
ExecStart=%h/Projects/smalltv-mod-custom/.venv/bin/python daemon/usage_daemon.py --config daemon/usage-daemon.json
Restart=on-failure

[Install]
WantedBy=default.target
```

Provider credentials are read from the setup UI store; the service needs no
`EnvironmentFile`. Enable:

```sh
systemctl --user daemon-reload
systemctl --user enable --now smalltv-deepseek-proxy.service smalltv-usage.service
journalctl --user -u smalltv-usage -f
```

## 8. Notifications over the webhook

The overlay is a full 240x240 draw list. Push one:

```sh
curl -X POST 'http://<device-ip>/api/view' \
  -H 'Content-Type: text/plain' \
  --data '{"ttl":20,"priority":1,"bg":"#000000","draw":[
    {"t":"text","x":120,"y":100,"s":3,"c":"#ffffff","a":"c","v":"Build finished"},
    {"t":"icon","x":120,"y":150,"s":3,"c":"#00ff88","a":"c","v":"check"}
  ]}'
```

`priority` (0-3, default 0) decides which overlay owns the panel. A higher
priority interrupts the current one; an equal or lower priority waits behind it.
One overlay can wait, so a burst of notifications does not leave a stale queue.
When the visible overlay expires, the waiting one starts with its own full TTL.

Clear it:

```sh
curl -X POST 'http://<device-ip>/api/view/clear'
```

If the web UI password is enabled, add the editor token instead of digest:

```sh
curl -X POST 'http://<device-ip>/api/view?token=<editor-token>' \
  -H 'Content-Type: text/plain' --data @view.json
```

Or use digest auth:

```sh
curl --digest -u admin:'<password>' -X POST 'http://<device-ip>/api/view' ...
```

### Polling instead of pushing

In **View**, set:

- **Poll URL**: a URL returning the same draw list JSON.
- **Poll every (s)**: e.g. `15`; `0` disables.
- **Default hold (s)**: e.g. `20`.
- **Auth header** / **Auth value**: optional, for a protected poll URL. Leave the
  value blank to keep the saved secret; clear the header to remove it.

The poller feeds the same priority queue as `POST /api/view`, so a higher
priority push can interrupt a polled view and the polled view can wait behind it.

A `204` or an empty body clears the visible overlay and anything waiting;
invalid JSON leaves both alone. A static file server works:

```sh
python3 -m http.server 8788 --directory /path/to/views
# Poll URL: http://<pc-ip>:8788/view.json
```

The overlay is never persisted; reboot clears it.

## 9. Web UI and API

The UI is at `http://<device-ip>/` or `http://<hostname>.local/`.

| Route | Purpose |
|---|---|
| `GET/POST /api/config` | settings |
| `GET /api/status` | device, providers, overlay, poller |
| `GET/POST /api/dashboard` | installed dashboard JSON |
| `POST /api/view`, `POST /api/view/clear` | transient overlay |
| `GET/POST /api/usage` | usage status / daemon push |
| `POST /update` | firmware upload (System tab) |
| `GET /api/export`, `POST /api/import` | settings backup |

Common checks:

```sh
curl http://<device-ip>/api/status
curl http://<device-ip>/api/dashboard
curl http://<device-ip>/api/usage
```

A provider appears as `valid`, `stale`, or `error`; the **Usage** tab in the web
UI shows the same data.

## 10. Updates and releases

The device self-update is pointed at `JPHultra/smalltv-mod-custom` in
`src/config.h`. Releases are built and published from this PC; there are no
GitHub Actions.

1. Edit `FW_VERSION` in `src/config.h` (currently `4.0.6-custom`) to the new version.
2. Build all targets and stage the assets:

```sh
python3 tools/build_release.py --pio /path/to/pio --jobs 8
python3 tools/ci_checks.py assets
```

3. Commit the version change and tag it, for example `v4.0.1`:

```sh
git add src/config.h
git commit -m "Bump FW_VERSION to 4.0.1"
git push origin main
git tag v4.0.1
git push origin v4.0.1
```

4. Attach the eight `dist/` assets to the tag:

```sh
gh release create v4.0.1 dist/* --title "v4.0.1" --notes "Release notes"
```

5. If the editor changed, publish it separately:

```sh
tools/deploy_pages.sh
```

The firmware's `UPDATE_ASSET` names must match the release assets; do not rename
the files in `dist/`. On the device, **System -> Update from GitHub** checks the
latest release and downloads the asset matching its variant. Manual OTA uses the
non-factory app image from `dist/` or the release page.

If the repo moves, update:

- `REPO_URL`, `REPO_OWNER`, `REPO_NAME` in `src/config.h`.
- `EDITOR_URL` in `src/WebPortal.cpp`.
- `DAEMON_URL` in `src/config.h` if you document a different daemon location.

## 11. Optional features

### Physical button

Classic-ESP32 boards expose a capacitive button. Configure short/long actions
and sensitivity under **Display -> Button**. Actions include repaint, dismiss
overlay, network info, wake and screen off.

### Night mode

**Display -> Clock & night mode** sets the timezone, start/end, and night
brightness. `0` blanks the backlight. It only dims after a fresh NTP sync.

### Web UI password

**System -> Password** enables HTTP digest auth for the page, API and firmware
upload. The daemon push endpoint `/api/usage` stays open. There is no recovery.

### Settings backup

**System -> Export settings** writes `config.json`, including WiFi passwords,
editor token and other secrets. **Import** applies and reboots.

## 12. Verify the repository

Host tests:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc \
  -I.pio/libdeps/smalltv/ArduinoJson/src tests/test_storage.cpp -o /tmp/storage && /tmp/storage
c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc tests/test_webguard.cpp -o /tmp/webguard && /tmp/webguard
c++ -std=c++11 -Wall -Wextra -Werror -g -DSMALLTV_ESP8266 \
  -Itests/shim -Isrc -Isrc/features/view -I.pio/libdeps/smalltv/ArduinoJson/src \
  tests/test_view.cpp src/features/view/ViewModel.cpp -o /tmp/view && /tmp/view
c++ -std=c++11 -Wall -Wextra -Werror -g -DSMALLTV_ESP8266 \
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_USE_DOUBLE=0 \
  -Itests/shim -Isrc -Isrc/features/usage -I.pio/libdeps/smalltv/ArduinoJson/src \
  tests/test_usage.cpp src/features/usage/UsageClient.cpp -o /tmp/usage && /tmp/usage
c++ -std=c++11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DSMALLTV_ESP8266 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_USE_DOUBLE=0 \
  -Itests/shim -Isrc -Isrc/features/view -Isrc/features/usage \
  -I.pio/libdeps/smalltv/ArduinoJson/src tests/test_parsers_hostile.cpp \
  src/features/view/ViewModel.cpp src/features/usage/UsageClient.cpp -o /tmp/hostile && /tmp/hostile
python -m unittest discover -s daemon -p 'test_*.py'
```

Other checks:

```sh
python tools/ci_checks.py assets
python tools/ci_checks.py size --pio .venv/bin/pio --min-free 8192
python3 - <<'PY'
import pathlib, re
html = pathlib.Path("editor/index.html").read_text()
pathlib.Path("/tmp/editor-check.js").write_text(
    re.findall(r"<script>(.*?)</script>", html, re.S)[-1])
PY
node --check /tmp/editor-check.js
```

The build script already runs `assets`, the per-env size gate and all firmware
builds.

## 13. Troubleshooting

| Symptom | Fix |
|---|---|
| `pio: command not found` | Activate `.venv`, or pass `--pio /path/to/pio` |
| `No module named 'SCons.Tool.FortranCommon'` | Use PlatformIO 6.1.19; the build script sets the constraints. Do not build with a newer pioarduino core |
| Editor **Load/Save** fails at `https://...` | Mixed content. Use a local HTTP copy of the editor or the device **View** tab |
| Editor 403 / authentication | Set **Editor token** in the device **View** tab and enter the same token; use the device IP or exact hostname |
| Device shows “Push a view from the editor” | No `/view.json`. Push a dashboard or paste `examples/view.json` |
| Claude provider is `unavailable` | Open `setup_ui.py`, paste a Claude Code credentials JSON or an access/refresh token, save, then Test |
| Codex provider is `unavailable` | Open `setup_ui.py`, paste the Codex auth JSON or access/refresh/account fields, save, then Test |
| Setup UI asks for a token | Restart `python3 daemon/setup_ui.py --config ...` and copy the newly printed token; it is not stored |
| Codex card is short a metric | Set `includeCredits` / `includeAdditional` if you want them; the mapper caps a provider at five metrics |
| Poll URL returns 401 | Set **Auth header** and **Auth value** in **View**; leave the value blank to keep the saved secret |
| DeepSeek balance is `unavailable` | Enter the API key in the setup UI and press Save, then Test |
| DeepSeek token usage is missing/0 | The client is not using `http://127.0.0.1:8789`; start the proxy and set the provider's `usageUrl`; use `usagePath: "today.total_tokens"` for a daily number |
| Usage card shows 0% and no value | The daemon is not sending a `value`/`pct`; check `/api/status` and `--dry-run` |
| `smalltv.local` does not resolve | Use the IP or the actual `smalltv-xxxx.local` shown on screen |
| Web UI password lost | Reflash over USB/UART; there is no recovery |
| ESP8266 HTTPS view poll fails | TLS is tight. Use an HTTP poll URL or do not poll |

## 14. Repository map

```text
src/                  firmware
  features/view/      dashboard draw list, renderer, overlay, poller
  features/usage/     usage store and parser
  config.h            identity, repo/update target, limits
  WebPortal.cpp       web/API routes, auth, editor token
  webui.html          web UI source (regenerate webui.h)
  board_*.h           board pin maps
tools/
  build_release.py    grouped release builder
  gzip_webui.py       web UI generator
  ci_checks.py        asset and size gates
  deploy_pages.sh     manual editor publishing to gh-pages
editor/               static visual dashboard editor
daemon/
  usage_daemon.py     Claude/Codex/DeepSeek aggregator
  setup_ui.py         local manual-entry credential UI
  setup.html          setup page
  credentials.py      atomic 0600 credential store
  deepseek_proxy.py   token-usage proxy
  push_usage.py       one-shot JSON pusher
examples/view.json    sample dashboard
partitions/           ESP32 partition layouts
case/                 3D-printed case
```

## 15. Security notes

- The web UI is plain HTTP. Keep it on the LAN; do not forward port 80 to the
  internet.
- The settings export contains WiFi passwords, the editor token and other
  secrets.
- Claude/Codex/DeepSeek credentials stay on the daemon host in
  `~/.config/smalltv/credentials.json` (mode 0600). The setup UI never reads
  local auth files or environment variables; the daemon sends only usage
  numbers to the device.
- The setup UI binds `127.0.0.1` and requires a generated setup token. If you
  bind it beyond localhost, treat the token like a password.
- The DeepSeek proxy binds `127.0.0.1` by default. If you bind it to `0.0.0.0`,
  anyone on the LAN can use the proxy and read the usage totals.
- The editor token is a write credential for the view endpoints; treat it like
  a password.
