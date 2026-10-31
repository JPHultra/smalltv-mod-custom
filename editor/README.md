# SmallTV Dashboard Editor

A dependency-free static editor for the firmware's 240x240 draw list.

## Run it

Open `index.html` directly, or serve the folder over HTTP:

```sh
cd editor
python3 -m http.server 8000
```

Then open `http://localhost:8000/`.

The same folder is published to GitHub Pages with `tools/deploy_pages.sh`. A
page served over HTTPS cannot fetch an HTTP device (mixed-content blocking), so
direct **Load / Save / Test overlay** only works from an HTTP page. On the GitHub
Pages copy, use the device's **View** tab textarea for the final push, or export
the JSON with **Download JSON** and paste it there.

## Device connection

1. On the device, open **View**, set an **Editor token**, and save.
2. In the editor, enter the device address (`http://smalltv.local` or its IP)
   and the same token.
3. Use **Load** to pull the installed dashboard and its live usage, **Save
   dashboard** to replace it, **Test overlay** to show it transiently, and
   **Fetch usage** to refresh the usage preview. Until usage is fetched, the
   usage cards use sample values and the canvas says so below the preview.

The token is a write credential for `/api/dashboard`, `/api/view` and
`/api/usage`; it is not the web UI password and is not returned by the API.

## Editing

- Click a component to add it, click a layer to select it, drag on the canvas to
  move it, and edit its properties on the right.
- The JSON pane is the source of truth: **Apply** loads pasted JSON, **Copy**
  copies the current view, **Download JSON** saves it as `view.json`.
- **Load example** starts from the sample dashboard; **Clear** starts empty.
- Text supports `{HH}`, `{hh}`, `{MM}`, `{SS}`, `{DD}`, `{MON}`, `{MONTH}`,
  `{WD}`, `{WEEKDAY}` and `{YYYY}`.
- Icons are drawn as placeholder boxes in the editor; the device renders the
  real vector icon.
- Usage components read the latest provider snapshot pushed by the daemon. A
  `metric` of `-1` (or no metric) stacks every metric the provider sends; `0`-`2`
  picks one. A metric can carry a `value` string (balance, token count) instead
  of a percentage. The editor uses sample values until **Fetch usage** succeeds.

## JSON shape

```json
{
  "bg": "#0b1420",
  "ttl": 15,
  "draw": [
    {"t":"text","x":120,"y":30,"s":6,"c":"#ffffff","a":"c","v":"{HH}:{MM}"},
    {"t":"rect","x":20,"y":100,"w":200,"h":40,"c":"#1f2937"},
    {"t":"rrect","x":20,"y":150,"w":200,"h":40,"r":12,"c":"#1f2937"},
    {"t":"circle","x":120,"y":210,"r":12,"c":"#3fb950"},
    {"t":"line","x":20,"y":226,"x2":220,"y2":226,"c":"#8b96a5"},
    {"t":"icon","x":40,"y":100,"s":2,"c":"#ffff00","a":"l","v":"sun"},
    {"t":"bitmap","x":60,"y":100,"w":8,"h":8,"s":2,"c":"#3fb950","a":"l","d":"AA55AA55AA55AA55"},
    {"t":"usage","x":8,"y":120,"w":224,"h":82,"c":"#d97757","provider":"claude","metric":-1}
  ]
}
```

`ttl` applies to the transient overlay; the stored dashboard ignores it.
