// QRZ.com XML callbook (subscription data): request queries, reply parsing, a local cache format, and the bits the decoder shows (name, town,
// country, distance). No network here: the GUI's QrzClient does the HTTP. Spec: https://www.qrz.com/docs/xml/current_spec.html
//   login:  ?username=U;password=P;agent=A   -> <Session><Key>..</Key><Count>..</Count><SubExp>..</SubExp></Session>
//   lookup: ?s=KEY;callsign=C                -> <Callsign>..</Callsign> or <Session><Error>Not found: C</Error>
// E-mail and street address are in the reply but never parsed, stored or shown.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace fireside::qrz {
constexpr const char* kBaseUrl = "https://xmldata.qrz.com/xml/current/";
constexpr const char* kAgent = "fireside-1.0";

struct Info {
    std::string call, first_name, name, town, state, country, grid, qsl_manager;
    std::optional<double> lat, lon; int dxcc = 0, cq_zone = 0, itu_zone = 0;
    bool lotw = false, eqsl = false, paper_qsl = false;
    int64_t fetched_utc = 0;        // when QRZ answered (cache age)
    bool not_found = false;         // QRZ has no record (cached too, for less time)
};
struct Reply {
    std::string key, error, message, sub_exp;   // sub_exp: "non-subscriber" or an expiry date
    int count = -1;                             // lookups by this account in the last 24 h (QRZ's own count)
    std::optional<Info> info;
    bool session_expired() const;               // "Session Timeout" / "Invalid session key": log in again and retry
    bool not_found() const;                     // "Not found: CALL"
};
Reply parse_reply(const std::string& xml);

// Query strings (to append to kBaseUrl), values percent-encoded, ';'-separated as in the spec.
std::string login_query(const std::string& user, const std::string& password, const std::string& agent = kAgent);
std::string lookup_query(const std::string& key, const std::string& callsign);

// The callsign to look up for a decoded one: "EA8/M0OML" -> "M0OML", "M0OML/P" -> "M0OML", "<M0OML>" -> "M0OML"; "" if nothing usable.
std::string base_call(const std::string& decoded);

// Cache: one line per call, tab separated; parse skips bad lines. fresh(): found records 30 days, not-found 3 days.
std::string cache_line(const Info& i);
std::optional<Info> parse_cache_line(const std::string& line);
bool fresh(const Info& i, int64_t now_utc);

// Maidenhead 2/4/6 characters -> centre of the square; false if not a locator.
bool grid_latlon(const std::string& grid, double& lat, double& lon);
// Great-circle km and initial bearing (degrees from north) from (lat1, lon1) to (lat2, lon2).
void distance_bearing(double lat1, double lon1, double lat2, double lon2, double& km, double& deg);
// What the decoder line shows: "Klaus, Hamburg, Germany, 612 km". The distance comes from `heard_grid` (the locator the station itself sent: where it is now,
// portable / mobile / someone else's shack) when given; otherwise from QRZ's record (their home address, maybe stale), marked "(QRZ)". Not found on QRZ:
// just the distance from the heard grid, or "".
std::string short_text(const Info& i, const std::string& my_grid, const std::string& heard_grid = "");

// ~/.config/fireside/qrz.env: QRZ_USER=... / QRZ_PASSWORD=...
std::map<std::string, std::string> parse_env(const std::string& text);
}  // namespace fireside::qrz
