# Usage daemon

The device does not call provider APIs. One local process normalizes Claude,
Codex/ChatGPT and DeepSeek usage and POSTs it to the device:

```json
{
  "providers": [
    {"id":"claude","name":"Claude","ok":true,"status":"normal",
     "metrics":[{"label":"5h","pct":29,"resetMin":142},
                {"label":"7d","pct":4,"resetMin":9876}]},
    {"id":"chatgpt","name":"Codex","ok":true,"status":"plus",
     "metrics":[{"label":"5h","pct":1,"resetMin":292},
                {"label":"7d","pct":45,"resetMin":6372},
                {"label":"plan","value":"plus","pct":0,"resetMin":0}]},
    {"id":"deepseek","name":"DeepSeek","ok":true,
     "metrics":[{"label":"balance","value":"110 CNY","pct":0,"resetMin":0},
                {"label":"tokens today","value":"1.2M","pct":0,"resetMin":0}]}
  ]
}
```

`pct` is 0-100, `resetMin` is minutes until reset (`0` means now/unknown), and
`value` is an optional display string. A metric with a value and `pct: 0` has no
fill bar; give it a `pct` (for example a budget percentage) to add one.
Providers are optional; a missing provider keeps its last snapshot. `ok:false`
marks the provider as errored while keeping its previous metrics.

The legacy Claude-only shape is still accepted:

```json
{"s":29,"sr":142,"w":4,"wr":9876,"st":"normal","ok":true}
```

## Configure and run

```sh
cp daemon/usage-daemon.example.json daemon/usage-daemon.json
python3 daemon/setup_ui.py --config daemon/usage-daemon.json
```

The setup UI opens your browser automatically and prints the same URL, with a
one-time setup token already attached. If no browser opens, use the printed URL
or start it with `--no-open` and copy the token. Paste the Claude, Codex and/or
DeepSeek credentials, then use **Save all credentials** (or a card's Save
button). Each card's **Test** button saves that card first. On Windows, use
`daemon\setup_windows.bat` (or `py -3 daemon\setup_ui.py --config
daemon\usage-daemon.json`). The page:

- never reads `~/.claude`, `~/.codex` or environment variables;
- accepts pasted credentials JSON from any machine, or individual fields;
- saves to `~/.config/smalltv/credentials.json` on Linux/macOS, or
  `%APPDATA%\smalltv\credentials.json` on Windows, with mode `0600`;
- never displays saved secrets back, logs them, or puts them in `/api/status`;
- has a **Test** button for each provider (the test does not push to the device).

Use Save for a provider after filling its fields. A blank field keeps the
previous value; Clear removes all credentials for that provider. Change the
store with `--credentials PATH` or the config's `credentialsFile`.

Run the daemon after setup:

```sh
python3 daemon/usage_daemon.py --config daemon/usage-daemon.json
```

`daemon/usage-daemon.json` is git-ignored because it may contain local URLs. Use
`--once` for one cycle and `--dry-run` to print the normalized document without
pushing. The daemon re-reads the credentials store each cycle, so saving in the
setup UI takes effect without restarting it.

The setup UI is localhost-only by default. `--bind 0.0.0.0` is available for a
daemon in a VM/container, but then anyone who can reach the port and knows the
setup token can update credentials.

### Windows

From PowerShell or Command Prompt in the repository root:

```bat
daemon\setup_windows.bat
```

or, to set the device while creating the config:

```bat
py -3 daemon\setup_ui.py --config daemon\usage-daemon.json --device http://smalltv.local
```

The browser is only for the initial credential setup. To start the daemon
automatically at login without a browser, install a Task Scheduler job:

```bat
daemon\install_windows.bat -DeviceUrl http://smalltv.local
```

For a machine that should start it before anyone logs in, run an elevated
Command Prompt:

```bat
daemon\install_windows.bat -DeviceUrl http://smalltv.local -AtStartup
```

`-AtStartup` asks for the Windows account password so Task Scheduler can run the
job before login; the password is stored by Windows, not in this repository.
Remove the job with `daemon\install_windows.bat -Uninstall`. Both modes run
`pythonw.exe` with no console and no browser. The Linux systemd service below is
not used on Windows.

| Adapter | Source |
|---|---|
| `claude` | Claude Code's OAuth usage endpoint, using setup-UI credentials |
| `claude-legacy` | the old `{s,sr,w,wr,st,ok}` payload from a local clawdmeter |
| `codex` | the ChatGPT/Codex subscription usage endpoint, using setup-UI credentials |
| `deepseek` | the DeepSeek balance API, plus optional token usage from a local proxy |
| `generic` | any JSON object, with dotted `statusPath`, `pctPath`, `resetPath`, `valuePath` |
| `normalized` | a provider object already shaped like the contract |

A failed provider is sent as `{"id":...,"name":...,"ok":false}` so the device
keeps the previous values and shows the error state.

## Native Claude

The `claude` adapter calls the Claude Code OAuth usage endpoint and maps the
five-hour and weekly limits. Enter the credentials in the setup UI. You can
paste the JSON from a machine where Claude Code is logged in, for example:

```json
{"claudeAiOauth":{"accessToken":"...","refreshToken":"...","expiresAt":0}}
```

The JSON is parsed in your browser when you press **Fill from JSON**; it is only
sent to the daemon when you press **Save**. The refresh token is used to renew
an expired access token. The public Claude Code OAuth client id is built in and
can be overridden in the Client id field.

If you only have a long-lived `claude setup-token` value, paste it as the access
token. No `claude` installation is required on the daemon host.

`authFile` and `tokenEnv` remain explicit legacy fallbacks for an existing
config; neither is used implicitly, and the setup UI never reads them.

## Codex / ChatGPT subscription

The `codex` adapter calls `https://chatgpt.com/backend-api/wham/usage` and maps
the 5-hour and weekly windows. The plan name is included by default as a `plan`
metric. The setup UI accepts pasted Codex auth JSON or individual fields:

```json
{"tokens":{"access_token":"...","refresh_token":"...","account_id":"..."}}
```

The JSON is parsed in the browser; only saved fields go to the daemon. An
expired access token is renewed with the refresh token and account id. The
public Codex OAuth client id is built in and can be overridden.

```json
{"id":"chatgpt","name":"Codex","adapter":"codex",
 "includePlan":true,"includeCredits":false,"includeAdditional":false}
```

`includeCredits` adds the credits balance when the endpoint reports one, and
`includeAdditional` adds any other rate-limit windows. The device shows at most
five metrics per provider (`USAGE_MAX_METRICS`); the mapper caps the list at
five. The explicit `authFile` config remains an optional legacy fallback; it is
not used by default.

## DeepSeek

Enter the DeepSeek API key in the setup UI. The `deepseek` adapter uses it for
the official balance request:

```json
{"id":"deepseek","name":"DeepSeek","adapter":"deepseek"}
```

DeepSeek's public API returns balance but no aggregate token usage. To show token
usage, run the local proxy and point the DeepSeek client at it:

```sh
python3 daemon/deepseek_proxy.py
# client base URL: http://127.0.0.1:8789        (OpenAI-compatible)
# or:              http://127.0.0.1:8789/anthropic
```

The proxy forwards to `https://api.deepseek.com`, adds
`stream_options.include_usage` to streaming JSON requests, counts OpenAI
`usage` objects and Anthropic streaming `message_start`/`message_delta` usage,
and serves `GET /usage` (and `POST /usage/reset`). The token counter persists in
`~/.local/state/smalltv/deepseek-usage.json`; the API key is forwarded per
request from the DeepSeek client and is never written there. The `/usage`
document includes the running total, `date`, `today` and per-day `days` buckets,
so the dashboard can show a daily number instead of the all-time count.

Add the usage source to the DeepSeek provider:

```json
{
  "id":"deepseek","name":"DeepSeek","adapter":"deepseek",
  "usageUrl":"http://127.0.0.1:8789/usage",
  "usagePath":"today.total_tokens",
  "usageLabel":"tokens today",
  "usageBudget":0
}
```

Use `"usagePath":"total_tokens"` if you prefer the all-time counter.
`usageBudget` is optional: set it to a token budget and the card gets a fill bar;
leave it at `0` to show the count only. If the proxy is not running, the balance
still appears and the daemon logs the usage-source error.

The explicit `apiKeyEnv` / `apiKeyFile` config remains an optional legacy
fallback; the setup UI does not read the environment or those files.

## Push a JSON file

If something else already produces the normalized document, skip the aggregator:

```sh
python3 daemon/push_usage.py --device http://smalltv.local --file usage.json
python3 daemon/push_usage.py --device http://smalltv.local --demo --once
python3 daemon/push_usage.py --device http://smalltv.local --url http://127.0.0.1:8787/usage
```

`--interval` (default 60) repeats the push. All scripts use only the Python
standard library.

## Tests

```sh
python3 -m unittest discover -s daemon -p 'test_*.py'
```
