#!/bin/sh
set -eu
systemctl --user disable --now smalltv-usage-worker.service 2>/dev/null || true
rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/smalltv-usage-worker.service"
systemctl --user daemon-reload
printf 'Removed SmallTV usage worker service\n'
