#!/usr/bin/env bash
# Install the usage daemon as a user service and open the credential setup page.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=${CONFIG:-"$ROOT/daemon/usage-daemon.json"}
EXAMPLE="$ROOT/daemon/usage-daemon.example.json"
PYTHON=${PYTHON:-/usr/bin/python3}
DEVICE=""
NO_BROWSER=0
LINGER=0

while [ $# -gt 0 ]; do
  case "$1" in
    --device)
      [ $# -ge 2 ] || { echo "--device needs a URL" >&2; exit 2; }
      DEVICE=$2
      shift 2
      ;;
    --no-browser)
      NO_BROWSER=1
      shift
      ;;
    --linger)
      LINGER=1
      shift
      ;;
    *)
      echo "usage: $0 [--device URL] [--no-browser] [--linger]" >&2
      exit 2
      ;;
  esac
done

[ -x "$PYTHON" ] || PYTHON=$(command -v python3 || true)
[ -n "$PYTHON" ] || { echo "python3 was not found" >&2; exit 1; }

if [ ! -f "$CONFIG" ]; then
  cp "$EXAMPLE" "$CONFIG"
  echo "Created $CONFIG from the example."
fi

if [ -n "$DEVICE" ]; then
  "$PYTHON" - "$CONFIG" "$DEVICE" <<'PY'
import json, sys
path, device = sys.argv[1:3]
with open(path, encoding="utf-8") as handle:
    config = json.load(handle)
config["device"] = device.strip()
with open(path, "w", encoding="utf-8") as handle:
    json.dump(config, handle, indent=2)
    handle.write("\n")
PY
  echo "Device URL set to $DEVICE."
fi

SERVICE_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SERVICE="$SERVICE_DIR/smalltv-usage.service"
mkdir -p "$SERVICE_DIR"
cat > "$SERVICE" <<SERVICE_EOF
[Unit]
Description=SmallTV usage daemon
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$ROOT
ExecStart="$PYTHON" "$ROOT/daemon/usage_daemon.py" --config "$CONFIG"
Environment=PYTHONUNBUFFERED=1
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
SERVICE_EOF

if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
  systemctl --user daemon-reload
  systemctl --user enable --now smalltv-usage.service
  echo "Enabled smalltv-usage.service."
  if command -v loginctl >/dev/null 2>&1; then
    if [ "$LINGER" -eq 1 ]; then
      if loginctl enable-linger "$USER" 2>/dev/null; then
        echo "Enabled lingering for $USER; the service starts at boot."
      else
        echo "Could not enable linger. Run: sudo loginctl enable-linger $USER" >&2
      fi
    else
      linger=$(loginctl show-user "$USER" -p Linger --value 2>/dev/null || true)
      if [ "$linger" != "yes" ]; then
        echo "To start before login, run: sudo loginctl enable-linger $USER (or use --linger)."
      fi
    fi
  fi
else
  echo "systemd user services are unavailable; start the daemon manually:" >&2
  echo "  $PYTHON $ROOT/daemon/usage_daemon.py --config $CONFIG" >&2
fi

if [ "$NO_BROWSER" -eq 1 ]; then
  exec "$PYTHON" "$ROOT/daemon/setup_ui.py" --config "$CONFIG" --no-open
fi
exec "$PYTHON" "$ROOT/daemon/setup_ui.py" --config "$CONFIG"
