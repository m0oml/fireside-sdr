#!/usr/bin/env bash
# Read-only status report for the FLEX-5000 on Linux. Changes nothing.
# Test hooks: SYSFS (default /sys), PROCFS (/proc), DEVDIR (/dev), MODDIR (/lib/modules/$(uname -r)), RULE (installed rule path).
SYSFS="${SYSFS:-/sys}"; PROCFS="${PROCFS:-/proc}"; DEVDIR="${DEVDIR:-/dev}"
MODDIR="${MODDIR:-/lib/modules/$(uname -r)}"; RULE="${RULE:-/etc/udev/rules.d/70-flexradio.rules}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ok=0; bad=0; warn=0
pass() { printf '  [ OK ] %s\n' "$*"; ok=$((ok+1)); }
fail() { printf '  [FAIL] %s\n' "$*"; bad=$((bad+1)); }
note() { printf '  [ -- ] %s\n' "$*"; warn=$((warn+1)); }
attr() { [ -r "$1/$2" ] && tr -d '\n' < "$1/$2" || printf '?'; }

echo "== Host set-up"
id -nG | tr ' ' '\n' | grep -qx audio && pass "user is in group 'audio'" || fail "user not in group 'audio' (sudo usermod -aG audio \$USER, then re-login)"
if [ -f "$RULE" ] && cmp -s "$here/70-flexradio.rules" "$RULE"; then pass "udev rule installed and current ($RULE)"
elif [ -f "$RULE" ]; then note "udev rule differs from system/70-flexradio.rules (re-run system/setup-system.sh)"
else fail "udev rule not installed (run system/setup-system.sh)"; fi
WPRULE="${WPRULE:-${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber/wireplumber.conf.d/52-flexradio-pipewire-ignore.conf}"
if [ -f "$WPRULE" ] && cmp -s "$here/52-flexradio-pipewire-ignore.conf" "$WPRULE"; then pass "PipeWire told to leave the radio's card alone ($WPRULE)"
elif [ -f "$WPRULE" ]; then note "WirePlumber rule differs from system/52-flexradio-pipewire-ignore.conf (re-run system/setup-system.sh)"
else note "WirePlumber rule not installed: PipeWire may grab the radio's card or make it the default output (run system/setup-system.sh)"; fi
for m in firewire-core firewire-ohci snd-firewire-lib snd-dice; do
  f=$(find "$MODDIR" -name "$m.ko*" 2>/dev/null | head -1)
  [ -n "$f" ] && pass "kernel module available: $m" || fail "kernel module missing: $m"
done

echo "== FireWire controller"
ctl=0
for d in "$SYSFS"/bus/pci/devices/*; do
  [ -r "$d/class" ] && [ "$(cat "$d/class")" = "0x0c0010" ] || continue
  ctl=$((ctl+1)); drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
  vid=$(attr "$d" vendor)
  pass "FireWire controller $(basename "$d") vendor $vid device $(attr "$d" device), driver: ${drv:-none}"
  case "$vid" in                                               # chip maker verdict (the FLEX-5000 manual wants a 1394a host adapter; TI is the best supported on Linux)
    0x104c) pass "chip maker: Texas Instruments: the best-supported FireWire chip on Linux" ;;
    0x1106) pass "chip maker: VIA: works, a few old quirks with audio streaming" ;;
    0x11c1) pass "chip maker: LSI/Agere: works" ;;
    0x1180) pass "chip maker: Ricoh: works" ;;
    0x197b) fail "chip maker: JMicron: known to be unreliable for FireWire audio; use a TI or VIA card" ;;
    0x1033) note "chip maker: NEC/Renesas: usually fine for OHCI 1394a, but less tested with DICE audio" ;;
    0x1039) note "chip maker: SiS: rare and little tested on Linux; if streaming misbehaves, try a TI card" ;;
    *) note "chip maker $vid: not in the known-good list; expect to check the kernel log for firewire_ohci errors" ;;
  esac
done
[ "$ctl" = 0 ] && fail "no FireWire (IEEE 1394 OHCI) controller on the PCI bus: fit the radio's PCIe FireWire card (a TI-chip 1394a card is ideal)"

echo "== FireWire nodes"
radio=""
if [ -d "$SYSFS/bus/firewire/devices" ]; then
  for n in "$SYSFS"/bus/firewire/devices/fw[0-9]*; do
    [ -e "$n" ] || continue; b=$(basename "$n")
    case "$b" in fw*.*) continue;; esac                       # units are handled with their device
    vn=$(attr "$n" vendor_name); mn=$(attr "$n" model_name); guid=$(attr "$n" guid)
    printf '  %s  vendor=%s (%s)  model=%s (%s)  guid=%s\n' "$b" "$(attr "$n" vendor)" "$vn" "$(attr "$n" model)" "$mn" "$guid"
    case "$vn$mn" in *FlexRadio*|*FLEX-5000*|*FLEX-3000*) radio="$n";; esac
  done
fi
[ -z "$radio" ] && fail "no FlexRadio node on the FireWire bus (radio off, cable, or the controller is not working)"

if [ -n "$radio" ]; then
  b=$(basename "$radio")
  echo "== Radio ($b)"
  pass "found: $(attr "$radio" vendor_name) / $(attr "$radio" model_name)"
  [ "$(attr "$radio" model_name)" = "FLEX-3000" ] && note "this is a FLEX-3000, not a 5000"
  echo "     numeric vendor id: $(attr "$radio" vendor)  <- add to system/70-flexradio.rules if you want an ID-based match"
  g=$(attr "$radio" guid); cat_byte=$(( (g >> 32) & 0xFF )) 2>/dev/null
  if [ "$cat_byte" = 4 ]; then pass "GUID category byte 0x04 (DICE): snd-dice's generic match will accept it"
  else note "GUID category byte is 0x$(printf %02X "${cat_byte:-0}"), snd-dice needs 0x04 for its generic match (may need a device-table patch)"; fi
  [ -c "$DEVDIR/$b" ] && { [ -r "$DEVDIR/$b" ] && [ -w "$DEVDIR/$b" ] && pass "$DEVDIR/$b is readable and writable by you" \
      || fail "$DEVDIR/$b exists but you cannot read/write it ($(stat -L -c '%U:%G %a' "$DEVDIR/$b")); udev rule not applied? try: sudo udevadm trigger --subsystem-match=firewire"; } \
    || note "$DEVDIR/$b not present"
  for u in "$SYSFS"/bus/firewire/devices/$b.*; do
    [ -e "$u" ] || continue
    drv=$(basename "$(readlink -f "$u/driver" 2>/dev/null)" 2>/dev/null)
    printf '     unit %s: specifier=%s version=%s model=%s driver=%s\n' "$(basename "$u")" "$(attr "$u" specifier_id)" "$(attr "$u" version)" "$(attr "$u" model)" "${drv:-none}"
    [ "$drv" = snd-dice ] && pass "unit $(basename "$u") is bound to snd-dice"
  done
fi

echo "== ALSA"
if [ -r "$PROCFS/asound/cards" ]; then
  grep -iE 'flex|dice' "$PROCFS/asound/cards" && pass "ALSA card for the radio present" || note "no ALSA card mentions FLEX/Dice (expected until the radio is bound)"
fi
ls "$DEVDIR"/snd/midiC*D* >/dev/null 2>&1 && printf '  MIDI nodes: %s\n' "$(ls "$DEVDIR"/snd/midiC*D* | tr '\n' ' ')" || note "no ALSA rawmidi nodes"

echo; echo "== Result: $ok ok, $warn notes, $bad failures"
[ "$bad" = 0 ]
