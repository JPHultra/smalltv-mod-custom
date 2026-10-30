# Usage daemon

The device does not call provider APIs. One local process normalizes Claude,
ChatGPT and DeepSeek usage and POSTs it to the device:

```json
{
  "providers": [
    {"id":"claude","name":"Claude","ok":true,"status":"normal",
     "metrics":[{"label":"5h","pct":29,"resetMin":142},
                {"label":"7d","pct":4,"resetMin":9876}]},
    {"id":"chatgpt","name":"ChatGPT","ok":true,
     "metrics":[{"label":"5h","pct":62,"resetMin":88}]},
    {"id":"deepseek","name":"DeepSeek","ok":true,
     "metrics":[{"label":"balance","pct":81,"resetMin":0}]}
  ]
}
```

`pct` is 0-100, `resetMin` is minutes until reset (`0` means now/unknown).
Providers are optional; a missing provider keeps its last snapshot. `ok:false`
marks the provider as errored while keeping its previous metrics.

The legacy Claude-only shape is still accepted:

```json
{"s":29,"sr":142,"w":4,"wr":9876,"st":"normal","ok":true}
```

## Push a JSON file

```sh
python3 daemon/push_usage.py --device http://smalltv.local --file usage.json
python3 daemon/push_usage.py --device http://smalltv.local --demo --once
python3 daemon/push_usage.py --device http://smalltv.local --url http://127.0.0.1:8787/usage
```

`--interval` (default 60) repeats the push. The script uses only the Python
standard library.

## Provider collectors

`push_usage.py` is the transport. It deliberately does not contain provider
credentials or scraping. Add a collector that writes the normalized document
(the `--file` or `--url` input) for your Claude, ChatGPT and DeepSeek accounts,
then run the pusher against it. The firmware only needs the contract above.
