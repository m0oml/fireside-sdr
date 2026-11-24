// Callsign prefix -> country centre, for one sanity check: a weak decode whose 4-character locator is impossible for its callsign's country (a typical false decode).
// Data: cty.dat by Jim Reisert AD1C (www.country-files.com), reduced to prefix, country centre and a plausible radius by tools/gen_dxcc.py (see NOTICE).
#pragma once
#include <string>

namespace fireside::decode {
// name: cty.dat's entity name ("England", "Fed. Rep. of Germany", "Sicily"); dxcc: the DXCC entity it counts for ("Sicily" -> "Italy"); cont: "EU", "NA"...; lon east-positive
struct DxccEntry { const char* prefix; float lat, lon; int radius_km; const char* name; const char* cont; int cq, itu; const char* dxcc; };
const DxccEntry* dxcc_lookup(const std::string& callsign);                       // longest matching prefix; nullptr for an unknown prefix (the decoder's locator check uses only this)

// The DXCC entity, continent and zones a callsign counts for, as a logger would: cty.dat's exact-call entries first, then the prefix. Portable calls: "EA8/G4ABC" and
// "G4ABC/EA8" count for EA8; /P, /M, /QRP, /A, /B, /LH and a call-area digit ("W1AW/4") are ignored; /MM and /AM (maritime and aeronautical mobile) count for no entity.
struct Entity { bool ok = false; std::string dxcc, cont; int cq = 0, itu = 0; };
Entity dxcc_entity(const std::string& callsign);
// false when a callsign-shaped part of `callsign` ("0J0BQR", the "0J0BQR" of "0J0BQR/P") has a prefix no country uses: no ITU series starts with 0, for example. A false
// FT8 decode (from noise, often an a-priori one) has a random callsign, so this catches many of them. Hashed calls ("<...>") and words ("CQ", "RR73", reports) pass.
// Checked on 2521 callsigns in real WSJT-X / jt9 decodes: none refused.
bool callsign_prefix_known(const std::string& callsign);
bool grid4_center(const std::string& grid, double& lat, double& lon);            // "JN58" -> centre of the square; false if not a 4-character locator
double great_circle_km(double lat1, double lon1, double lat2, double lon2);
// false only when the callsign's country is known and the locator's square centre is farther than that country's radius from the country centre. Portable calls
// ("/"), unknown prefixes, hashed calls and non-locators always pass: a country can be operated from anywhere, so this is only used on weak decodes.
bool callsign_grid_plausible(const std::string& callsign, const std::string& grid);
}  // namespace fireside::decode
