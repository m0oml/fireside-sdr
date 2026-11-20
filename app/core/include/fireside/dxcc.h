// Callsign prefix -> country centre, for one sanity check: a weak decode whose 4-character locator is impossible for its callsign's country (a typical false decode).
// Data: cty.dat by Jim Reisert AD1C (www.country-files.com), reduced to prefix, country centre and a plausible radius by tools/gen_dxcc.py (see NOTICE).
#pragma once
#include <string>

namespace fireside::decode {
struct DxccEntry { const char* prefix; float lat, lon; int radius_km; const char* name; };   // name: cty.dat's entity name ("England", "Fed. Rep. of Germany")         // lon east-positive
const DxccEntry* dxcc_lookup(const std::string& callsign);                       // longest matching prefix; nullptr for an unknown prefix
bool grid4_center(const std::string& grid, double& lat, double& lon);            // "JN58" -> centre of the square; false if not a 4-character locator
double great_circle_km(double lat1, double lon1, double lat2, double lon2);
// false only when the callsign's country is known and the locator's square centre is farther than that country's radius from the country centre. Portable calls
// ("/"), unknown prefixes, hashed calls and non-locators always pass: a country can be operated from anywhere, so this is only used on weak decodes.
bool callsign_grid_plausible(const std::string& callsign, const std::string& grid);
}  // namespace fireside::decode
