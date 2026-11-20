#pragma once
// UK amateur band plan for the spectrum overlay, in two layers:
//   allocations: the Ofcom Full licence frequency table (OFW611 "Amateur Radio Wireless Telegraphy Licence Conditions Booklet",
//                published 21 February 2024, Table C), with status and maximum PEP;
//   segments:    the RSGB 2026 band plan usage (rsgb.org/bandplans, edition 260125) inside those allocations.
// A guide only: it never limits what the receiver can tune, and the licence document is what counts.
#include <string>
#include <vector>

namespace fireside::bandplan {
enum class Kind { Cw, Narrow, AllModes, Phone, Beacon, Fm, Satellite, Other };
struct Segment { double lo_hz, hi_hz; Kind kind; const char* usage; };      // usage: the RSGB wording, shortened
struct Allocation { double lo_hz, hi_hz; const char* band; bool primary; double max_pep_w; const char* note; };
struct Gap { double lo_hz, hi_hz; const char* band; };                       // inside a band's outer edges but NOT allocated (the 60 m gaps)
struct Mark { double hz; const char* label; };

std::vector<Segment> segments_between(double lo_hz, double hi_hz);          // overlapping [lo, hi], sorted by frequency
std::vector<Allocation> allocations_between(double lo_hz, double hi_hz);
std::vector<Gap> gaps_between(double lo_hz, double hi_hz);
std::vector<Mark> marks_between(double lo_hz, double hi_hz);                 // FT8 / FT4 dial frequencies inside [lo, hi]
const Allocation* allocation_at(double hz);                                  // nullptr outside every UK allocation
const Segment* segment_at(double hz);
const char* kind_name(Kind k);                                               // short strip label: "CW", "Narrow", "All modes", ...
// For the hover readout: "40m All modes - SSB contest preferred | UK Full: primary, 1000 W", "60m NOT a UK allocation - do not transmit",
// or "" well away from any band.
std::string describe(double hz);
}  // namespace fireside::bandplan
