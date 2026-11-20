// QRZ XML callbook: reply parsing (shapes from the current spec), queries, base calls, cache lines, locators, distance and the decoder text.
#include "fireside/qrz.h"
#include "test_util.h"
using namespace fireside;

namespace {
const char* kLogin = R"(<?xml version="1.0" encoding="utf-8" ?>
<QRZDatabase version="1.34" xmlns="http://xmldata.qrz.com">
  <Session>
    <Key>2331uf894c4bd29f3923f3bacf02c532d7bd9</Key>
    <Count>123</Count>
    <SubExp>Wed Jan 1 12:34:03 2027</SubExp>
    <GMTime>Sun Nov 22 21:25:34 2026</GMTime>
  </Session>
</QRZDatabase>)";
const char* kLookup = R"(<?xml version="1.0" encoding="utf-8" ?>
<QRZDatabase version="1.34" xmlns="http://xmldata.qrz.com">
  <Callsign>
    <call>DK0ULR</call><fname>Klaus &amp; Co</fname><name>Club Station</name><addr1>Secret Street 1</addr1><addr2>Ulm</addr2><state></state>
    <country>Germany</country><lat>48.400000</lat><lon>9.983333</lon><grid>JN58ri</grid><dxcc>230</dxcc><cqzone>14</cqzone><ituzone>28</ituzone>
    <email>secret@example.com</email><lotw>1</lotw><eqsl>0</eqsl><mqsl>1</mqsl><qslmgr>via bureau</qslmgr>
  </Callsign>
  <Session><Key>2331uf894c4bd29f3923f3bacf02c532d7bd9</Key><Count>124</Count><SubExp>Wed Jan 1 12:34:03 2027</SubExp></Session>
</QRZDatabase>)";
}  // namespace

TEST(qrz_parses_login_lookup_and_errors) {
    auto l = qrz::parse_reply(kLogin); CHECK_EQ(l.key, std::string("2331uf894c4bd29f3923f3bacf02c532d7bd9")); CHECK_EQ(l.count, 123); CHECK(!l.info); CHECK(l.error.empty());
    auto r = qrz::parse_reply(kLookup); CHECK(r.info); const auto& i = *r.info;
    CHECK_EQ(i.call, std::string("DK0ULR")); CHECK_EQ(i.first_name, std::string("Klaus & Co")); CHECK_EQ(i.town, std::string("Ulm")); CHECK_EQ(i.country, std::string("Germany"));
    CHECK_EQ(i.grid, std::string("JN58ri")); CHECK_NEAR(*i.lat, 48.4, 1e-9); CHECK_NEAR(*i.lon, 9.983333, 1e-9); CHECK_EQ(i.dxcc, 230); CHECK_EQ(i.cq_zone, 14); CHECK_EQ(i.itu_zone, 28);
    CHECK(i.lotw && !i.eqsl && i.paper_qsl); CHECK_EQ(i.qsl_manager, std::string("via bureau")); CHECK_EQ(r.count, 124);
    CHECK(qrz::cache_line(i).find("secret") == std::string::npos);                               // e-mail and street never kept
    auto nf = qrz::parse_reply("<QRZDatabase><Session><Error>Not found: XX9XX</Error><Key>k</Key></Session></QRZDatabase>"); CHECK(nf.not_found()); CHECK(!nf.session_expired()); CHECK(!nf.info);
    auto to = qrz::parse_reply("<QRZDatabase><Session><Error>Session Timeout</Error></Session></QRZDatabase>"); CHECK(to.session_expired()); CHECK(to.key.empty());
    auto bad = qrz::parse_reply("<QRZDatabase><Session><Error>Username/password incorrect</Error></Session></QRZDatabase>"); CHECK(!bad.session_expired() && !bad.not_found() && !bad.error.empty());
    CHECK(!qrz::parse_reply("garbage").info);
}
TEST(qrz_queries_are_encoded) {
    CHECK_EQ(qrz::login_query("m0oml", "p&ss;w=rd"), std::string("?username=m0oml;password=p%26ss%3Bw%3Drd;agent=fireside-1.0"));
    CHECK_EQ(qrz::lookup_query("abc", "dk0ulr"), std::string("?s=abc;callsign=DK0ULR"));
}
TEST(qrz_base_calls_from_decoded_ones) {
    CHECK_EQ(qrz::base_call("M0OML"), std::string("M0OML")); CHECK_EQ(qrz::base_call("m0oml/p"), std::string("M0OML")); CHECK_EQ(qrz::base_call("EA8/M0OML"), std::string("M0OML"));
    CHECK_EQ(qrz::base_call("<DK0ULR>"), std::string("DK0ULR")); CHECK_EQ(qrz::base_call("<...>"), std::string("")); CHECK_EQ(qrz::base_call("CQ"), std::string("")); CHECK_EQ(qrz::base_call("RR73"), std::string(""));
    CHECK_EQ(qrz::base_call("VP2E/W1AW/QRP"), std::string("W1AW"));
}
TEST(qrz_cache_lines_round_trip_and_age) {
    auto i = *qrz::parse_reply(kLookup).info; i.fetched_utc = 1'800'000'000; i.town = "Ulm\tan der\nDonau";
    auto back = qrz::parse_cache_line(qrz::cache_line(i)); CHECK(back);
    CHECK_EQ(back->call, std::string("DK0ULR")); CHECK_EQ(back->town, std::string("Ulm an der Donau")); CHECK_NEAR(*back->lat, 48.4, 1e-6); CHECK_EQ(back->dxcc, 230); CHECK(back->lotw && back->paper_qsl);
    CHECK(qrz::fresh(*back, 1'800'000'000 + 29 * 86400)); CHECK(!qrz::fresh(*back, 1'800'000'000 + 31 * 86400));
    qrz::Info nf; nf.call = "XX9XX"; nf.not_found = true; nf.fetched_utc = 1'800'000'000;
    auto n2 = qrz::parse_cache_line(qrz::cache_line(nf)); CHECK(n2 && n2->not_found && !n2->lat); CHECK(!qrz::fresh(*n2, 1'800'000'000 + 4 * 86400));
    CHECK(!qrz::parse_cache_line("junk")); CHECK(!qrz::parse_cache_line(""));
    qrz::Info bare; bare.call = "SP9ABC"; bare.first_name = "Jan"; bare.fetched_utc = 1'800'000'000;                  // empty last fields: the line ends in tabs
    const std::string bl = qrz::cache_line(bare); CHECK(bl.back() == '\t'); auto bb = qrz::parse_cache_line(bl); CHECK(bb && bb->first_name == "Jan");
    std::string cut = bl; while (!cut.empty() && cut.back() == '\t') cut.pop_back(); CHECK(qrz::parse_cache_line(cut));
}
TEST(qrz_locators_distance_and_decoder_text) {
    double la, lo; CHECK(qrz::grid_latlon("IO91", la, lo)); CHECK_NEAR(la, 51.5, 1e-9); CHECK_NEAR(lo, -1.0, 1e-9);
    CHECK(qrz::grid_latlon("io92ar", la, lo)); CHECK_NEAR(lo, -2 + 1.0 / 24, 1e-9); CHECK_NEAR(la, 52 + 17.5 / 24, 1e-9);
    CHECK(!qrz::grid_latlon("RR73", la, lo)); CHECK(!qrz::grid_latlon("IO9", la, lo)); CHECK(!qrz::grid_latlon("ZZ00", la, lo));
    double km, deg; qrz::distance_bearing(51.5, -0.12, 40.71, -74.0, km, deg); CHECK_NEAR(km, 5570, 15); CHECK_NEAR(deg, 288, 2);   // London -> New York
    auto i = *qrz::parse_reply(kLookup).info;
    const std::string t = qrz::short_text(i, "IO91"); CHECK(t.rfind("Klaus & Co, Ulm, Germany, ", 0) == 0); CHECK(t.find(" km") != std::string::npos);
    i.lat.reset(); i.lon.reset(); CHECK(qrz::short_text(i, "IO91").find(" km") != std::string::npos);   // from their grid instead
    CHECK(qrz::short_text(i, "").find("km") == std::string::npos);                                         // no grid of mine: no distance
    qrz::Info nf; nf.not_found = true; CHECK(qrz::short_text(nf, "IO91").empty());
    // the grid they sent wins over QRZ's record (portable / mobile / another shack); QRZ's is marked
    auto j = *qrz::parse_reply(kLookup).info;                                                             // QRZ: Ulm (48.4 N 10.0 E)
    CHECK(qrz::short_text(j, "IO91").find(" km (QRZ)") != std::string::npos);
    const std::string heard = qrz::short_text(j, "IO91", "IO91"); CHECK(heard.find("Klaus & Co, Ulm, Germany, ") == 0); CHECK(heard.find("(QRZ)") == std::string::npos);
    CHECK(heard.find(", 0 km") != std::string::npos || heard.find(", 1 km") != std::string::npos || heard.find(", 2 km") != std::string::npos);   // they sent IO91: next door
    CHECK(qrz::short_text(j, "IO91", "RR73").find("(QRZ)") != std::string::npos);                       // not a locator: QRZ's
    CHECK(qrz::short_text(nf, "IO91", "JN58").find(" km") != std::string::npos);                        // not on QRZ: still the distance they sent
    CHECK(qrz::short_text(nf, "IO91", "JN58").find("(QRZ)") == std::string::npos);
    auto e = qrz::parse_env("QRZ_USER=m0oml\n# comment\nQRZ_PASSWORD = a=b \n"); CHECK_EQ(e["QRZ_USER"], std::string("m0oml")); CHECK_EQ(e["QRZ_PASSWORD"], std::string("a=b"));
}
