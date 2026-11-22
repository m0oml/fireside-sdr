// What a decoded station would add to the log: a new DXCC entity, continent, CQ / ITU zone or grid square, overall or on this band. Built from the user's own log
// (Wavelog's QSO list) with the entity worked out from each callsign by the cty.dat table (dxcc.h): Wavelog's QSO records carry no country. Pure: no network, no radio.
#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace fireside::award {
// Tag numbers continue the decoder's worked-before tags (0 NEW call, 1 new band/mode, 2 worked); highest value first in this order.
enum Tag { kNone = -1, kNewDxcc = 3, kNewDxccBand = 4, kNewCont = 5, kNewCq = 6, kNewItu = 7, kNewGrid = 8, kNewGridBand = 9 };
struct Options { bool dxcc = true, dxcc_band = true, cont = true, cq = false, itu = false, grid = true, grid_band = false; };   // zones off: cty.dat gives one zone for all of the US, Canada, Russia, Australia...
const char* tag_text(int tag);                  // "NEW DXCC", "DXCC new band"...; also the three worked-before tags; "" for an unknown tag

std::string grid4(const std::string& grid);     // "io92ar" -> "IO92"; "" unless it starts with a valid 4-character locator (RR73 is not a grid)
std::string norm_band(const std::string& band); // "20M" -> "20m"

class WorkedIndex {
public:
    void clear();
    void add(const std::string& call, const std::string& band, const std::string& grid, bool count = true);   // one logged QSO (grid: theirs, any length, may be empty); count=false: worked, but Wavelog will report it (no double count)
    // The best tag the options allow for working this station on this band now, or kNone. grid: the locator they sent ("" when not known).
    int tag(const std::string& call, const std::string& band, const std::string& grid, const Options& o) const;
    size_t qsos() const { return qsos_; }
    size_t dxcc_count() const { return dxcc_.size(); }
    size_t grid_count() const { return grid_.size(); }
private:
    using Key = std::pair<std::string, std::string>;   // (value, band)
    std::set<std::string> dxcc_, cont_, grid_; std::set<int> cq_, itu_; std::set<Key> dxcc_band_, grid_band_;
    size_t qsos_ = 0;
};
}  // namespace fireside::award
