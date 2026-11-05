#!/bin/sh
# Install the native usage worker as a per-user systemd service.
set -eu
APP=${1:-"$(pwd)/emulator/build/smalltv-emulator"}
DEVICE=${2:-http://smalltv.local}
APP=$(CDPATH= cd -- "$(dirname -- "$APP")" && pwd)/$(basename -- "$APP")
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
mkdir -p "$UNIT_DIR"
cat > "$UNIT_DIR/smalltv-usage-worker.service" <<UNIT
[Unit]
Description=SmallTV native usage collector
After=graphical-session.target

[Service]
Type=simple
ExecStart="$APP" --usage-worker --usage-device "$DEVICE"
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now smalltv-usage-worker.service
printf 'Installed SmallTV usage worker: %s\n' "$UNIT_DIR/smalltv-usage-worker.service"
