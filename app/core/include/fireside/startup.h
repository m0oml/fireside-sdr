// Start-up / shut-down write sequences, ported from tools/firesidectl/startup.py (PowerSDR 2.7.2 console.cs InitRadio / chkPower).
// REVIEWED 2026-11-14 (Opus, analysis/startup-review.md). The three review conditions are enforced here and tested:
//   A. exemptions from the dangerous-write gate are VALUE-specific: INITIALIZE(0,0), SET_MOX(0,0), SET_PA_BIAS(0,0) only, enforced inside Safety (F1/F2)
//   B. writes go only over the FireWire mailbox (PalMailbox); never MIDI SysEx. Refused before any traffic
//   C. shutdown also switches PA bias off, as PowerSDR does when it closes
// Deliberately NOT sent: PA bias ON, EEPROM writes, calibration, transmit, ATU tune commands.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "fireside/client.h"
#include "fireside/identify.h"

namespace fireside {
inline constexpr bool kStartupWritesPorted = true;
inline constexpr double kSettleBeforeAudioS = 0.100;      // PowerSDR Thread.Sleep(100) before Audio.Start(); the caller waits

struct Step { std::string op; double d1, d2; std::string why; };
using LogFn = std::function<void(const std::string&)>;

// rx_ant: NC, ANT1, ANT2, ANT3, RX1IN. Throws std::invalid_argument for a bad antenna or a frequency outside 0..65 MHz.
std::vector<Step> startup_steps(double freq_mhz, const std::string& rx_ant = "ANT1", double clock_mhz = 500.0);
std::vector<Step> shutdown_steps();                       // SET_MOX 0, SET_STANDBY 1, SET_PA_BIAS 0
bool is_reviewed_exemption(const std::string& op);        // INITIALIZE, SET_MOX, SET_PA_BIAS
bool exemption_value_ok(const std::string& op, double d1, double d2);

void require_firewire(Flex& fx);                          // throws StartupError unless the link is a PalMailbox whose byte order verify() has proven (F4)
// Drains the ATU result buffer (read-only); returns the number of reads (at most max_reads).
int flush_atu(Flex& fx, int max_reads = 64);
// Throws StartupError (not over FireWire) or SafetyError (unreviewed value / writes disabled) BEFORE sending the offending step.
// Value check for ALL steps runs first, so a bad step later in the list sends nothing at all.
void run_steps(Flex& fx, const std::vector<Step>& steps, const LogFn& log = {});
// After the last step, waits (read-only) until the mailbox has consumed it (F3); a firmware that never does raises TimeoutError.
// identify -> validate -> (ATU flush) -> init + power on. Caller waits kSettleBeforeAudioS then starts audio.
Identity run_startup(Flex& fx, double freq_mhz, const std::string& rx_ant = "ANT1", const LogFn& log = {}, const IdentifyOptions& ident = {});
}  // namespace fireside
