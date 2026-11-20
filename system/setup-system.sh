#!/usr/bin/env bash
# Install the udev rule for the FLEX-5000, and the WirePlumber rule that keeps PipeWire off the radio's card. Idempotent.
# Needs sudo only for the two udev steps; the WirePlumber rule goes in your own ~/.config.
#   ./setup-system.sh            install (or confirm already installed)
#   ./setup-system.sh --dry-run  show what would happen
#   ./setup-system.sh --remove   remove the rule again
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/70-flexradio.rules"; dst=/etc/udev/rules.d/70-flexradio.rules
mode="${1:-install}"
say() { printf '%s\n' "$*"; }

wp_src="$here/52-flexradio-pipewire-ignore.conf"; wp_dst="${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber/wireplumber.conf.d/52-flexradio-pipewire-ignore.conf"
wp_restart() { systemctl --user try-restart wireplumber.service 2>/dev/null && say "WirePlumber restarted" || say "restart WirePlumber (or log out and in) to apply"; }
[ "$mode" = "--remove" ] && { sudo rm -f "$dst" && sudo udevadm control --reload && say "removed $dst"; rm -f "$wp_dst" && say "removed $wp_dst"; wp_restart; exit 0; }

if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
  say "udev rule already installed and identical: $dst"; changed=0
else
  changed=1
  if [ "$mode" = "--dry-run" ]; then
    say "would install $src -> $dst"; [ -f "$dst" ] && diff -u "$dst" "$src" || true
  else
    sudo install -m 0644 "$src" "$dst"; say "installed $dst"
  fi
fi
if [ "$mode" != "--dry-run" ]; then
  [ "$changed" = 1 ] && { sudo udevadm control --reload; say "udev rules reloaded"; }
  udevadm verify "$dst" && say "rule syntax OK"
fi
if [ -f "$wp_dst" ] && cmp -s "$wp_src" "$wp_dst"; then say "WirePlumber rule already installed and identical: $wp_dst"
elif [ "$mode" = "--dry-run" ]; then say "would install $wp_src -> $wp_dst (PipeWire then leaves the radio's card alone)"
else install -D -m 0644 "$wp_src" "$wp_dst"; say "installed $wp_dst"; wp_restart; fi
id -nG | tr ' ' '\n' | grep -qx audio && say "you are in group 'audio': OK" \
  || say "NOTE: you are not in group 'audio'. Run: sudo usermod -aG audio $USER   (then log out and in)"
say "No modules are loaded or hardware touched: firewire-ohci and snd-dice autoload when the card/radio appear."
