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
                {"label":"7d","pct":45,"resetMin":6372}]},
    {"id":"deepseek","name":"DeepSeek","ok":true,
     "metrics":[{"label":"balance","value":"110 CNY","pct":0,"resetMin":0},
                {"label":"tokens","value":"1.2M","pct":0,"resetMin":0}]}
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

## Aggregate providers

`usage_daemon.py` fetches each provider, maps it to the contract above, and
pushes all three together. Copy and edit the example, then run it:

```sh
cp daemon/usage-daemon.example.json daemon/usage-daemon.json
python3 daemon/usage_daemon.py --config daemon/usage-daemon.json
```

`daemon/usage-daemon.json` is ignored by git because it may contain API keys or
local URLs. Use `--once` for one cycle and `--dry-run` to print the normalized
document without pushing.

| Adapter | Source |
|---|---|
| `claude-legacy` | the old `{s,sr,w,wr,st,ok}` payload |
| `codex` | the ChatGPT/Codex subscription usage endpoint, using `~/.codex/auth.json` |
| `deepseek` | the DeepSeek balance API, plus optional token usage from a local proxy |
| `generic` | any JSON object, with dotted `statusPath`, `pctPath`, `resetPath`, `valuePath` |
| `normalized` | a provider object already shaped like the contract |

A failed provider is sent as `{"id":...,"name":...,"ok":false}` so the device
keeps the previous values and shows the error state.

## Codex / ChatGPT subscription

The `codex` adapter reads the ChatGPT tokens that the Codex CLI already stores
in `~/.codex/auth.json`, calls
`https://chatgpt.com/backend-api/wham/usage`, and maps the 5-hour and weekly
windows. If the access token has expired it uses the stored refresh token
against `https://auth.openai.com/oauth/token`, then writes the refreshed tokens
back to the auth file (mode 0600). The email, account id and tokens are never
sent to the device.

```json
{"id":"chatgpt","name":"Codex","adapter":"codex","authFile":"~/.codex/auth.json"}
```

Run Codex at least once so the auth file exists. If Codex rotates the token
while the daemon runs, the daemon re-reads the file before refreshing and uses
the newer token.

## DeepSeek

The `deepseek` adapter reads an API key from `DEEPSEEK_API_KEY` (or
`apiKeyEnv`/`apiKeyFile`) and fetches the official balance:

```json
{"id":"deepseek","name":"DeepSeek","adapter":"deepseek","apiKeyEnv":"DEEPSEEK_API_KEY"}
```

DeepSeek's public API returns balance but no aggregate token usage. To show token
usage, run the local proxy and point the DeepSeek client at it:

```sh
python3 daemon/deepseek_proxy.py
# client base URL: http://127.0.0.1:8789        (OpenAI-compatible)
# or:              http://127.0.0.1:8789/anthropic
```

The proxy forwards to `https://api.deepseek.com`, adds
`stream_options.include_usage` to streaming JSON requests, counts the usage
fields, and serves `GET /usage` (and `POST /usage/reset`). The token counter
persists in `~/.local/state/smalltv/deepseek-usage.json`; the API key is
forwarded per request and is never written there. Only OpenAI-compatible usage
objects are counted.

Add the usage source to the DeepSeek provider:

```json
{
  "id":"deepseek","name":"DeepSeek","adapter":"deepseek",
  "apiKeyEnv":"DEEPSEEK_API_KEY",
  "usageUrl":"http://127.0.0.1:8789/usage",
  "usagePath":"total_tokens",
  "usageLabel":"tokens",
  "usageBudget":0
}
```

`usageBudget` is optional: set it to a token budget and the card gets a fill bar;
leave it at `0` to show the count only. If the proxy is not running, the balance
still appears and the daemon logs the usage-source error.

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
