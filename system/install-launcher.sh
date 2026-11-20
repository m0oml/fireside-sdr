#!/usr/bin/env bash
# Installs (or removes) a desktop launcher for fireside in the CURRENT USER's applications folder. No sudo. Idempotent.
#   system/install-launcher.sh [--dry-run | --remove] [--simulate]
# The launcher runs build/gui/fireside from this checkout (build it first: cmake -S app -B build -G Ninja && cmake --build build).
# --simulate makes the launcher start the simulated radio (no hardware) instead of the real one.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; root="$(cd "$here/.." && pwd)"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"; icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
mode=install; extra=""
for a in "$@"; do case "$a" in --dry-run) mode=dry;; --remove) mode=remove;; --simulate) extra=" --simulate --demo";; *) echo "unknown option $a" >&2; exit 2;; esac; done
desktop="$apps/fireside.desktop"; icon="$icons/fireside.svg"
content="[Desktop Entry]
Type=Application
Name=Fireside SDR
GenericName=SDR transceiver
Comment=Linux transceiver application for the FlexRadio FLEX-5000A
Exec=$root/build/gui/fireside$extra
Icon=fireside
Terminal=false
Categories=HamRadio;Audio;AudioVideo;
Keywords=sdr;ham;radio;transceiver;ft8;flex;
Actions=Simulate;

[Desktop Action Simulate]
Name=Simulated radio (no hardware)
Exec=$root/build/gui/fireside --simulate --demo"
case "$mode" in
  dry) echo "would write $desktop:"; echo "$content"; echo "would copy $here/fireside.svg to $icon"; [ -x "$root/build/gui/fireside" ] || echo "NOTE: $root/build/gui/fireside does not exist yet (build first)";;
  remove) rm -f "$desktop" "$icon"; echo "removed $desktop and $icon";;
  install)
    [ -x "$root/build/gui/fireside" ] || echo "NOTE: $root/build/gui/fireside does not exist yet (build first); installing the launcher anyway" >&2
    mkdir -p "$apps" "$icons"; printf '%s\n' "$content" > "$desktop"; cp "$here/fireside.svg" "$icon"; chmod 644 "$desktop"
    command -v update-desktop-database >/dev/null && update-desktop-database "$apps" 2>/dev/null || true
    echo "installed $desktop (icon $icon)";;
esac
