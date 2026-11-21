// The radio's built-in signal generator, receive only. Mimics PowerSDR 2.7.2 for the FLEX-5000: console.cs CalibrateLevel() and Setup > Tests
// "Enable HW Signal Generator" (setup.cs ckEnableSigGen_CheckedChanged):
//   on:  SET_FULL_DUPLEX 1, SET_TX_FREQ_TW (where the generator sits), SET_GEN 1, SET_TEST 1, SET_SIG 1, about 50 ms apart
//   off: SET_FULL_DUPLEX 0, SET_GEN 0, SET_TEST 0, SET_SIG 0 (PowerSDR's order); every write is attempted even if an earlier one fails
// The receiver's input is then the generator instead of the antenna. PowerSDR's production test takes its level as -24.0 dBm
// (FLEX5000ProdTestForm.cs udLevel) and computes the EEPROM's RX level table from it.
// NEVER sent from here: the key (MOX), the T/R relay, the exciter / detector enables, the PA-driver monitor or any EEPROM write: nothing is keyed
// and the exciter stays off (the guard test lists the forbidden opcodes).
// Callers hold the transmit controller's lock (TxControl::sig_gen_check), so nothing can key while the generator is on.
#pragma once
#include <string>
#include "fireside/client.h"

namespace fireside::siggen {
inline constexpr double kLevelDbm = -24.0;              // PowerSDR's assumed generator level at the receiver input (FLEX-5000)
void on(Flex& fx, double gen_mhz);                       // throws (after switching everything off again) on any failure
std::string off(Flex& fx);                               // "" when every write went through, else the errors
}  // namespace fireside::siggen
