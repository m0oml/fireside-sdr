// A simulated FLEX-5000 beyond the mailbox (sim_bus.h): its calibration EEPROM, its ATU (behaving like the one on the radio:
// result word [queue][b3][b2][code], 0x00FFFFFF = nothing pending, a pass followed by the relay / power / SWR reports, a fail with its reason; SET_IND carries
// Hi-Z in bit 7, as measured on the radio) and its PA detector (fixed readings, or forward power = k x drive^2 from the transmit samples' level).
// Used by the tests and by `fireside --simulate` (practice the transmit controls with nothing on the air). Installs itself on a DiceBus.
#pragma once
#include <atomic>
#include <deque>
#include <vector>
#include "fireside/sim_bus.h"
#include "fireside/tx_meter.h"

namespace fireside::sim {
struct RadioModel {
    DiceBus& bus;
    // ATU
    std::deque<uint32_t> q; bool keyed = false; int ind = 0, cap = 0, hiz = 0, thr = 6, tune_polls = -1; bool full = false;
    double best_swr = 1.0;                         // what this antenna can be tuned to
    int tune_time_polls = 60;                      // result polls until the tune finishes
    bool silent = false;                           // never answers a tune
    int swr_b3 = 0;                                // the tuner's SWR report on request (b3: 0 = 1.00, 28 = ~2.0, 64 = 3.0)
    int fail_searches = 0;
    int pass_ind = 30, pass_cap = 7;               // the relays a passing search ends on (a 50 ohm load needs only a few steps)                         // this many full searches fail ("could not bring the SWR below the threshold") before one passes
    bool pass_regardless = false; int pass_swr_b3 = 0;   // a tuner that reports PASS even when it found no match, with this SWR byte (255 = SWR ~1022, seen on 12 m 2026-11-21)
    int pass_fwd_cw = 0x0424;                      // the tuner's forward power report after a tune, in 0.01 W (10.60 W; 0 = read without power, seen on 60 m 2026-11-25)
    std::vector<uint8_t> requests;
    // PA detector
    int adc_fwd = 966, adc_rev = 238;              // fixed readings while keyed (the last readings of a passing tune on the radio) ...
    bool pa_from_level = false;                    // ... or from the transmit level: forward W = k * level^2, reflected = rev_frac of it
    std::atomic<double> level{0}; double k = 11150, rev_frac = 0.02; bool dead_pa = false; int band = 5; txm::PaCal meter;
    double meter_lag_s = 0;                        // the detector answers with the level of this long ago (the real PA meter lags ~one 0.2 s step)
    mutable std::deque<std::pair<double, double>> lag_hist; double lagged_level() const;
    // The KEY jack and PTT contacts (EVENT register and READ_PTT): bit 0 dot, 1 dash, 2 rear RCA PTT, 3 mic PTT
    std::atomic<uint32_t> contacts{0};
    // EEPROM (READ_TRX_EEPROM_UINT32 answers from it when not empty)
    std::vector<uint8_t> eeprom;

    explicit RadioModel(DiceBus& b);
    void on_send(uint32_t d1);
    void report(bool after_tune);
    uint32_t next();
    uint32_t pa_adc(uint32_t ch) const;
};
}  // namespace fireside::sim
