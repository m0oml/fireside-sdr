# firesidectl: FLEX-5000 control tool (prototype, Python, no dependencies)

Talks the PowerSDR opcode protocol over either the radio's MIDI SysEx port (default) or the
FireWire PAL mailbox (`--fw /dev/fwN`; see ../protocol/02-control-transports.md).
Verified on a real FLEX-5000A over the FireWire mailbox (`--fw`); the MIDI route is only used for read-only identification. The simulator (`--fake`) uses assumed values.

    ./firesidectl-cli list                         # ALSA rawmidi devices
    ./firesidectl-cli probe                        # read-only identification sweep
    ./firesidectl-cli get GET_RX2_OK
    ./firesidectl-cli ops 'ATU|RX2'                # browse the 263 known opcodes
    ./firesidectl-cli identify                     # read-only: model, serial, firmware check, boards fitted
    ./firesidectl-cli eeprom-dump                  # read-only calibration backup (DO THIS FIRST on the real radio)
    ./firesidectl-cli startup --freq 7.1 --dry-run # show PowerSDR's init + power-on writes; add --allow-write to run
    ./firesidectl-cli shutdown                     # MOX off + standby (needs --allow-write)
    ./hardware-day [--events 20] [--rehearse]   # strictly read-only first-contact report (see report.md)
    ./firesidectl-cli monitor                      # print paddle / PTT / mic-button events
    ./firesidectl-cli --fake probe                 # against the built-in simulated radio
    ./firesidectl-cli -v ...                       # hex-dump all traffic

Safety (donated radio: factory calibration lives in EEPROM):
- reads: always allowed
- writes: need `--allow-write`
- EEPROM/register/I2C/bias/reset/power writes, anything that can key the transmitter
  (MOX, ATU tune, T/R), and opcodes PowerSDR never uses: also need `--allow-dangerous`

Tests: `for t in firesidectl pal startup eeprom docs hardware_day cpp_assets; do python3 -I tests/test_$t.py; done`  (82 tests: MIDI over a pty
against the simulator; PAL mailbox against a fake DICE register file; cdev ioctl packing with ioctl mocked;
start-up order/safety; EEPROM decode, checksums, read-only guarantee)
Regenerate opcodes: `python3 -I gen_opcodes.py ../upstream/powersdr-2.7.2/Source firesidectl/opcodes.py`

Layout: protocol.py (framing), transport.py (ALSA rawmidi + SysEx parser), client.py (safety + probe + MidiLink),
firewire.py (/dev/fw* via firewire-cdev), pal.py (mailbox link), startup.py (init/power sequence), eeprom.py (backup + decode),
fake.py (simulator), cli.py, opcodes.py (generated).

Opcode page: `python3 -I gen_protocol_opcodes.py` writes ../protocol/04-opcodes.md (hand notes in opcode_notes.py; a test fails if the page is stale).
