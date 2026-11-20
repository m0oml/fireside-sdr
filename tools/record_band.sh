#!/usr/bin/env bash
# Unattended band recording for survey / decoder work: radio start-up -> 192 kHz direct stream aligned to a 15 s UTC boundary -> radio shutdown (always).
# usage: tools/record_band.sh DIAL_MHZ SECONDS LABEL   e.g. tools/record_band.sh 7.100 180 CW40   (the 192 kHz window shows +-96 kHz around the dial: a whole band segment)
DIAL=${1:?usage: rec.sh DIAL_MHZ SECONDS LABEL}; LABEL=${3:-BAND}
cd ~/fireside-sdr || exit 1
OUT=~/fireside-sdr/recordings/live; LOG=$OUT/record_band.log; : > "$LOG"
say() { echo "$(date -u +%H:%M:%S.%N | cut -c1-12) $*" | tee -a "$LOG"; }
build/firesidecli --fw /dev/fw1 --allow-write serve --freq "$DIAL" --port 4532 >>"$LOG" 2>&1 & P=$!
cleanup() { say "shutdown requested"; kill -INT $P 2>/dev/null; wait $P 2>/dev/null; say "serve exited rc=$?"; tail -4 "$LOG"; }
trap cleanup EXIT
for i in $(seq 1 90); do grep -q "CAT (Kenwood" "$LOG" && break; kill -0 $P 2>/dev/null || { say "serve died early"; exit 2; }; sleep 1; done
grep -q "CAT (Kenwood" "$LOG" || { say "radio did not come up"; exit 3; }
say "radio is up"
t0=$(date -u +%Y%m%d_%H%M%S); F=$OUT/${t0}_${DIAL/./}_${LABEL}_ant1_192k_iq_direct.wav
say "capture (direct FireWire, 192 kHz, aligned to the next 15 s boundary), file $F"
build/firesidecapture direct --rate 192000 --seconds "${2:-180}" --stream "$F" --align-ft8 2>&1 | tee -a "$LOG"; rc=${PIPESTATUS[0]}; say "capture rc=$rc"
b=$(grep -o "slot boundary [0-9]*" "$LOG" | tail -1 | awk '{print $3}')
echo "start_epoch_boundary=$b dial_mhz=$DIAL mode=$LABEL antenna=ANT1 rate=192000 source=direct-firewire channels=I,Q mirror_needed=yes if_offset_hz=9000 trim=0" > "${F%.wav}.txt"
ls -l "$F" | tee -a "$LOG"
