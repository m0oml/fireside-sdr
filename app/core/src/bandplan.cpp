#include "fireside/bandplan.h"
#include <algorithm>
#include <cstdio>

namespace fireside::bandplan {
namespace {
using K = Kind;
constexpr double k = 1e3;     // kHz
// Ofcom OFW611 (21 Feb 2024) Table C, Full licence. 1810-1830 and 50-51 MHz: primary, non-interference to services outside the UK;
// 1850-2000, 10.1, 51-52, 70 MHz and 60 m: secondary. 60 m: "100 W (20 dBW) but not exceeding 200 W EIRP", note (g) restrictions.
const Allocation kAlloc[] = {
    {135.7 * k, 137.8 * k, "2200m", false, 1, "1 W ERP"}, {472 * k, 479 * k, "630m", false, 5, "5 W EIRP"},
    {1810 * k, 1830 * k, "160m", true, 1000, "non-interference outside the UK"}, {1830 * k, 1850 * k, "160m", true, 1000, ""}, {1850 * k, 2000 * k, "160m", false, 32, ""},
    {3500 * k, 3800 * k, "80m", true, 1000, "shared with other services"},
    {5258.5 * k, 5264 * k, "60m", false, 100, "max 200 W EIRP"}, {5276 * k, 5284 * k, "60m", false, 100, "max 200 W EIRP"}, {5288.5 * k, 5292 * k, "60m", false, 100, "max 200 W EIRP"},
    {5298 * k, 5307 * k, "60m", false, 100, "max 200 W EIRP"}, {5313 * k, 5323 * k, "60m", false, 100, "max 200 W EIRP"}, {5333 * k, 5338 * k, "60m", false, 100, "max 200 W EIRP"},
    {5354 * k, 5358 * k, "60m", false, 100, "max 200 W EIRP; no Tx above 5358"}, {5362 * k, 5374.5 * k, "60m", false, 100, "max 200 W EIRP"}, {5378 * k, 5382 * k, "60m", false, 100, "max 200 W EIRP"},
    {5395 * k, 5401.5 * k, "60m", false, 100, "max 200 W EIRP"}, {5403.5 * k, 5406.5 * k, "60m", false, 100, "max 200 W EIRP"},
    {7000 * k, 7200 * k, "40m", true, 1000, ""}, {10100 * k, 10150 * k, "30m", false, 400, ""},
    {14000 * k, 14350 * k, "20m", true, 1000, ""}, {18068 * k, 18168 * k, "17m", true, 1000, ""}, {21000 * k, 21450 * k, "15m", true, 1000, ""},
    {24890 * k, 24990 * k, "12m", true, 1000, ""}, {28000 * k, 29700 * k, "10m", true, 1000, ""},
    {50000 * k, 51000 * k, "6m", true, 1000, "non-interference outside the UK"}, {51000 * k, 52000 * k, "6m", false, 100, ""},
    {70000 * k, 70500 * k, "4m", false, 160, ""},
};
// RSGB 2026 band plan (edition 260125). Where the printed rows overlap (80 m 3600-3620/3600-3650, 40 m 7130-7200/7175-7200) the later row
// starts at the inner edge. 60 m: UK usage plan, all modes unless stated.
const Segment kSegments[] = {
    {1810 * k, 1838 * k, K::Cw, "Telegraphy; 1836 QRP"}, {1838 * k, 1840 * k, K::Narrow, "Narrow band modes"}, {1840 * k, 1843 * k, K::AllModes, "All modes"},
    {1843 * k, 2000 * k, K::Phone, "Telephony, telegraphy (lowest LSB dial 1843)"},
    {3500 * k, 3510 * k, K::Cw, "Telegraphy - inter-continental priority"}, {3510 * k, 3560 * k, K::Cw, "Telegraphy - contest preferred; 3555 QRS"},
    {3560 * k, 3570 * k, K::Cw, "Telegraphy; 3560 QRP"}, {3570 * k, 3590 * k, K::Narrow, "Narrow band modes"}, {3590 * k, 3600 * k, K::Narrow, "Narrow band modes - unattended data"},
    {3600 * k, 3620 * k, K::AllModes, "All modes - unattended data (lowest LSB dial 3603)"}, {3620 * k, 3650 * k, K::AllModes, "All modes - phone contest preferred; 3630 DV"},
    {3650 * k, 3700 * k, K::AllModes, "All modes; 3663 UK emergency, 3690 SSB QRP"}, {3700 * k, 3775 * k, K::AllModes, "All modes - phone contest preferred; 3760 R1 emergency"},
    {3775 * k, 3800 * k, K::AllModes, "All modes - inter-continental SSB priority"},
    {5258.5 * k, 5264 * k, K::AllModes, "5262 CW QRP centre"}, {5276 * k, 5284 * k, K::AllModes, "5278.5 UK emergency comms"}, {5288.5 * k, 5292 * k, K::AllModes, "Beacons on 5290"},
    {5298 * k, 5307 * k, K::AllModes, "All modes"}, {5313 * k, 5323 * k, K::AllModes, "5317 AM (6 kHz max)"}, {5333 * k, 5338 * k, K::AllModes, "All modes"},
    {5354 * k, 5358 * k, K::AllModes, "WRC-15 band: NO Tx above 5358 kHz (FT8 on 5357: keep the signal below 5358)"},
    {5362 * k, 5374.5 * k, K::AllModes, "Partly WRC-15 band; WSPR"}, {5378 * k, 5382 * k, K::AllModes, "All modes"}, {5395 * k, 5401.5 * k, K::AllModes, "All modes"},
    {5403.5 * k, 5406.5 * k, K::AllModes, "All modes"},
    {7000 * k, 7040 * k, K::Cw, "Telegraphy; 7030 QRP"}, {7040 * k, 7047 * k, K::Narrow, "Narrow band modes (PSK31 from 7040)"},
    {7047 * k, 7050 * k, K::Narrow, "Narrow band modes - unattended data"}, {7050 * k, 7053 * k, K::AllModes, "All modes - unattended data"},
    {7053 * k, 7060 * k, K::AllModes, "All modes, digimodes (lowest LSB dial 7053)"}, {7060 * k, 7100 * k, K::AllModes, "All modes - SSB contest preferred; 7070 DV, 7090 SSB QRP"},
    {7100 * k, 7130 * k, K::AllModes, "All modes; 7110 R1 emergency"}, {7130 * k, 7175 * k, K::AllModes, "All modes - SSB contest preferred; 7165 image"},
    {7175 * k, 7200 * k, K::AllModes, "All modes - inter-continental priority"},
    {10100 * k, 10130 * k, K::Cw, "Telegraphy; 10116 QRP"}, {10130 * k, 10150 * k, K::Narrow, "Narrow band modes"},
    {14000 * k, 14060 * k, K::Cw, "Telegraphy - contest preferred; 14055 QRS"}, {14060 * k, 14070 * k, K::Cw, "Telegraphy; 14060 QRP"},
    {14070 * k, 14089 * k, K::Narrow, "Narrow band modes"}, {14089 * k, 14099 * k, K::Narrow, "Narrow band modes - unattended data"},
    {14099 * k, 14101 * k, K::Beacon, "IBP beacons only"}, {14101 * k, 14112 * k, K::AllModes, "All modes - unattended data"},
    {14112 * k, 14125 * k, K::AllModes, "All modes (no digimodes)"}, {14125 * k, 14300 * k, K::AllModes, "All modes - SSB contest preferred; 14230 image, 14285 QRP"},
    {14300 * k, 14350 * k, K::AllModes, "All modes; 14300 global emergency"},
    {18068 * k, 18095 * k, K::Cw, "Telegraphy; 18086 QRP"}, {18095 * k, 18105 * k, K::Narrow, "Narrow band modes"}, {18105 * k, 18109 * k, K::Narrow, "Narrow band modes - unattended data"},
    {18109 * k, 18111 * k, K::Beacon, "IBP beacons only"}, {18111 * k, 18120 * k, K::AllModes, "All modes - unattended data"},
    {18120 * k, 18168 * k, K::AllModes, "All modes; 18130 SSB QRP, 18160 global emergency"},
    {21000 * k, 21070 * k, K::Cw, "Telegraphy; 21055 QRS, 21060 QRP"}, {21070 * k, 21090 * k, K::Narrow, "Narrow band modes"},
    {21090 * k, 21110 * k, K::Narrow, "Narrow band modes - unattended data"}, {21110 * k, 21120 * k, K::AllModes, "All modes (no SSB) - unattended data"},
    {21120 * k, 21149 * k, K::Narrow, "Narrow band modes"}, {21149 * k, 21151 * k, K::Beacon, "IBP beacons only"},
    {21151 * k, 21450 * k, K::AllModes, "All modes; 21285 QRP, 21340 image, 21360 global emergency"},
    {24890 * k, 24915 * k, K::Cw, "Telegraphy; 24906 QRP"}, {24915 * k, 24925 * k, K::Narrow, "Narrow band modes"}, {24925 * k, 24929 * k, K::Narrow, "Narrow band modes - unattended data"},
    {24929 * k, 24931 * k, K::Beacon, "IBP beacons only"}, {24931 * k, 24940 * k, K::AllModes, "All modes - unattended data"}, {24940 * k, 24990 * k, K::AllModes, "All modes; 24950 SSB QRP"},
    {28000 * k, 28070 * k, K::Cw, "Telegraphy; 28055 QRS, 28060 QRP"}, {28070 * k, 28120 * k, K::Narrow, "Narrow band modes"},
    {28120 * k, 28150 * k, K::Narrow, "Narrow band modes - unattended data"}, {28150 * k, 28190 * k, K::Narrow, "Narrow band modes"},
    {28190 * k, 28225 * k, K::Beacon, "IBP and continuous-duty beacons"}, {28225 * k, 28300 * k, K::AllModes, "All modes - beacons"},
    {28300 * k, 28320 * k, K::AllModes, "All modes - unattended data"}, {28320 * k, 29000 * k, K::AllModes, "All modes; 28330 DV, 28360 QRP, 28680 image"},
    {29000 * k, 29100 * k, K::AllModes, "All modes"}, {29100 * k, 29200 * k, K::Fm, "FM simplex, 10 kHz channels"},
    {29200 * k, 29300 * k, K::AllModes, "All modes - unattended data; UK internet gateways"}, {29300 * k, 29510 * k, K::Satellite, "Satellite links"},
    {29510 * k, 29520 * k, K::Other, "Guard channel"}, {29520 * k, 29590 * k, K::Fm, "FM repeater inputs RH1-RH8"},
    {29590 * k, 29620 * k, K::Fm, "29600 FM calling, 29610 parrot repeater"}, {29620 * k, 29700 * k, K::Fm, "FM repeater outputs RH1-RH8"},
    {50000 * k, 50100 * k, K::Cw, "Telegraphy only (beacon project 50.000-50.030)"}, {50100 * k, 50200 * k, K::Phone, "SSB/telegraphy - international preferred; 50.150"},
    {50200 * k, 50300 * k, K::Phone, "SSB/telegraphy - general; 50.285 crossband"}, {50300 * k, 50400 * k, K::Narrow, "MGM/narrowband/telegraphy; 50.305 PSK"},
    {50400 * k, 50500 * k, K::Beacon, "Propagation beacons only"}, {50500 * k, 50700 * k, K::AllModes, "All modes; 50.630 DV calling"},
    {50700 * k, 50900 * k, K::Fm, "FM/DV repeater outputs"}, {50900 * k, 51200 * k, K::AllModes, "All modes"}, {51200 * k, 51400 * k, K::Fm, "FM/DV repeater inputs"},
    {51400 * k, 52000 * k, K::AllModes, "All modes; 51.510 FM calling, 51.530 GB2RS"},
    {70000 * k, 70090 * k, K::Beacon, "Propagation beacons only"}, {70090 * k, 70100 * k, K::Beacon, "Personal beacons"},
    {70100 * k, 70250 * k, K::Narrow, "Narrow band modes; 70.200 CW/SSB centre"}, {70250 * k, 70294 * k, K::AllModes, "All modes; 70.260 AM/FM calling"},
    {70294 * k, 70500 * k, K::Fm, "All modes, 12.5 kHz channels"},
};
// FT8 and FT4 dial frequencies (WSJT-X defaults; 60 m uses the UK 5357 kHz channel).
const Mark kMarks[] = {
    {1840 * k, "FT8"}, {3573 * k, "FT8"}, {3575 * k, "FT4"}, {5357 * k, "FT8"}, {7047.5 * k, "FT4"}, {7074 * k, "FT8"}, {10136 * k, "FT8"}, {10140 * k, "FT4"},
    {14074 * k, "FT8"}, {14080 * k, "FT4"}, {18100 * k, "FT8"}, {18104 * k, "FT4"}, {21074 * k, "FT8"}, {21140 * k, "FT4"}, {24915 * k, "FT8"}, {24919 * k, "FT4"},
    {28074 * k, "FT8"}, {28180 * k, "FT4"}, {50313 * k, "FT8"}, {50318 * k, "FT4"},
};
template <class T> std::vector<T> overlapping(const T* b, const T* e, double lo, double hi) {
    std::vector<T> v; for (auto* p = b; p != e; ++p) if (p->hi_hz > lo && p->lo_hz < hi) v.push_back(*p);
    std::sort(v.begin(), v.end(), [](const T& a, const T& c) { return a.lo_hz < c.lo_hz; }); return v;
}
}  // namespace

std::vector<Segment> segments_between(double lo, double hi) { return overlapping(std::begin(kSegments), std::end(kSegments), lo, hi); }
std::vector<Allocation> allocations_between(double lo, double hi) { return overlapping(std::begin(kAlloc), std::end(kAlloc), lo, hi); }
std::vector<Gap> gaps_between(double lo, double hi) {
    std::vector<Gap> g;
    for (size_t i = 0; i + 1 < std::size(kAlloc); ++i) {
        const auto& a = kAlloc[i]; const auto& b = kAlloc[i + 1];
        if (std::string(a.band) == b.band && b.lo_hz > a.hi_hz && b.lo_hz > lo && a.hi_hz < hi) g.push_back({a.hi_hz, b.lo_hz, a.band});
    }
    return g;
}
std::vector<Mark> marks_between(double lo, double hi) { std::vector<Mark> v; for (const auto& m : kMarks) if (m.hz >= lo && m.hz <= hi) v.push_back(m); return v; }
const Allocation* allocation_at(double hz) { for (const auto& a : kAlloc) if (hz >= a.lo_hz && hz < a.hi_hz) return &a; return nullptr; }
const Segment* segment_at(double hz) { for (const auto& s : kSegments) if (hz >= s.lo_hz && hz < s.hi_hz) return &s; return nullptr; }
const char* kind_name(Kind kd) {
    switch (kd) {
        case Kind::Cw: return "CW"; case Kind::Narrow: return "Narrow"; case Kind::AllModes: return "All modes"; case Kind::Phone: return "SSB/CW";
        case Kind::Beacon: return "Beacons"; case Kind::Fm: return "FM"; case Kind::Satellite: return "Satellite"; default: return "Other";
    }
}
std::string describe(double hz) {
    if (const Allocation* a = allocation_at(hz)) {
        const Segment* s = segment_at(hz);
        char p[96]; std::snprintf(p, sizeof p, "UK Full: %s, %g W%s%s", a->primary ? "primary" : "secondary", a->max_pep_w, *a->note ? ", " : "", a->note);
        return std::string(a->band) + " " + (s ? s->usage : "") + (s ? " | " : "") + p;
    }
    for (const auto& g : gaps_between(hz, hz)) if (hz >= g.lo_hz && hz < g.hi_hz) return std::string(g.band) + " NOT a UK allocation - do not transmit";
    return "";
}
}  // namespace fireside::bandplan
