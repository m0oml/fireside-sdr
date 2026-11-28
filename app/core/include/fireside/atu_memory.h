// Per-band, per-antenna ATU memory: the relay settings of the last good tune on each band and transmit antenna (ANT1..ANT3: the ATU sits in front of
// the radio's antenna relays), so changing band or antenna can put them back WITHOUT transmitting (Atu::set_relays). A text format the GUI keeps in
// its settings; one line per band and antenna ("ant=" only for ANT2 / ANT3, so older files read as ANT1):
//   band=20m ind=30 cap=7 hiz=1 swr=1.00 fwd_w=10.60 freq_hz=14335400 utc=2026-11-18T23:15:23Z
//   band=20m ant=2 ind=12 cap=40 hiz=0 ...
#pragma once
#include <map>
#include <optional>
#include <string>

namespace fireside::atu {
struct Setting {
    int ind = 0, cap = 0; bool hiz = false;
    double swr = 0, fwd_w = 0, freq_hz = 0; std::string utc;      // what the tune achieved, where, when
};
class Memory {
public:
    void store(int band, const Setting& s, int ant = 1);          // band: tx_meter.h index (0 = 160 m ... 10 = 6 m), ant 1..3; out of range ignored
    std::optional<Setting> recall(int band, int ant = 1) const;
    void forget(int band, int ant = 1) { m_.erase(key(band, ant)); }
    static int key(int band, int ant) { return band + 100 * (ant - 1); }   // all(): band = key % 100, antenna = key / 100 + 1
    const std::map<int, Setting>& all() const { return m_; }
    std::string to_text() const;
    // Lines it cannot read are skipped and counted in `bad` (a damaged settings file never stops the app).
    static Memory from_text(const std::string& text, int* bad = nullptr);
private:
    std::map<int, Setting> m_;
};
}  // namespace fireside::atu
