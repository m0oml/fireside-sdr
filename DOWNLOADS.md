# Downloaded FlexRadio material

Fetched 2026-11-14. Every file's size, SHA-256 and source URL are in `MANIFEST.tsv`.
Official files come from flexradio.com (edge.flexradio.com); `ke9ns/` files come from ke9ns.com
(KE9NS, the FlexRadio-authorised legacy maintainer of PowerSDR for the 1500/3000/5000).
This is copyrighted FlexRadio material kept for personal reference. Don't redistribute it.

## docs/flexradio/ (official)
- FLEX-5000 Owner's Manual (281 pp), FLEX-5000A and 5000C quick start guides, datasheet, FDX block diagram
- FLEX-5000 RX2, ATU and VU5K install guides, ATU-100 service manual, EU DoC
- PowerSDR release notes v2.4.4 / 2.5.3 / 2.6.4 / 2.7.2, TX audio chain diagram, TNF white paper, AGC-T and digital mode notes, FlexControl guide
- `FLEX-5000-Service-Manual.zip` and `FLEX-5000-VU-Module-Schematics.zip`, both unpacked in `unpacked/`:
  **full schematics, parts lists and board placement** for TRX (HTRX), PA (HPA100), RFIO, RX2, MIC, VU;
  block diagrams; BITE alignment and diagnostics procedure

## docs/ke9ns/
- FLEX-5000 Owner's Manual v2.8 (KE9NS edition, 218 pp), FLEX-3000 and FLEX-1500 owner's manuals, FLEX-1500 service manual
- PowerSDR CAT command reference (100 pp), keyboard shortcuts
- 5000A specs, Class A / bias adjustment, BITE alignment (preliminary), VU5K addendum, RFIO install guide, repair story
- EEPROM / FireWire diagnostic tool instructions (.htm)
- Chip datasheets: **TCD2210** (the DICE chip in the radio) and **TI TSB41AB2** (its 1394 PHY)

## software/flexradio/
- `PowerSDR_v2.7.2_Installer.exe`: last official FlexRadio release; bundles the FLEX FireWire driver and firmware

## software/ke9ns/
- `PowerSDR_v3.0.0_Installer.exe`, `PowerSDR_KE9NS_V3.0.0.13_Incremental_Installer.msi`: current KE9NS builds
- `F5K_firmware_old.zip`: `F5K_firmware_1.3.0.8.exe` and **`FLEX-Firewire_Driver_v3.5.0.7171.exe`**
- `EEPROM.exe`: KE9NS FireWire/EEPROM diagnostic tool
- `1394_OHCI_LegacyDriver_X86.msi`: Microsoft legacy 1394 OHCI driver (needed on Win8+)
- `PowerSDR.zip` (database XML), `powerSDR_Skins.zip`

## Not obtained
- FLEX-3000 schematics (NPA100 / NTRX PDFs): dead links on ke9ns.com, not on the Wayback Machine
- Standalone `F5K_firmware_1.3.0.8.exe` link: dead, but the same file is inside `F5K_firmware_old.zip`
- PowerSDR installers older than v2.7.2: FlexRadio no longer hosts them
