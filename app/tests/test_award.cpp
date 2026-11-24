// The DXCC entity of a callsign (exact calls, portable calls, WAE-only entities) and the "new DXCC / continent / zone / grid" tags built from the user's log.
#include "fireside/award.h"
#include "fireside/dxcc.h"
#include "test_util.h"
using namespace fireside;

TEST(dxcc_entity_counts_calls_as_a_logger_does) {
    auto ent = [](const char* c) { return decode::dxcc_entity(c); };
    CHECK(ent("M0OML").ok && ent("M0OML").dxcc == "England" && ent("M0OML").cont == "EU" && ent("M0OML").cq == 14 && ent("M0OML").itu == 27);
    CHECK_EQ(ent("gm4abc").dxcc, std::string("Scotland"));                                     // lower case is fine
    CHECK_EQ(ent("IT9ABC").dxcc, std::string("Italy"));                                        // Sicily is a WAE entity only: it counts for Italy
    CHECK_EQ(ent("TA1ABC").dxcc, std::string("Asiatic Turkey")); CHECK_EQ(ent("TA1ABC").cont, std::string("EU"));   // ...but keeps its own continent
    CHECK_EQ(ent("EA8/G4ABC").dxcc, std::string("Canary Islands")); CHECK_EQ(ent("G4ABC/EA8").dxcc, std::string("Canary Islands")); CHECK_EQ(ent("EA8/G4ABC").cont, std::string("AF"));
    CHECK_EQ(ent("G4ABC/P").dxcc, std::string("England")); CHECK_EQ(ent("W1AW/4").dxcc, std::string("United States")); CHECK_EQ(ent("G4ABC/QRP").dxcc, std::string("England"));
    CHECK_EQ(ent("KH6ABC").dxcc, std::string("Hawaii"));
    CHECK_EQ(ent("3D2CCC").dxcc, std::string("Conway Reef")); CHECK_EQ(ent("3D2ABC").dxcc, std::string("Fiji"));   // an exact-call entry beats its prefix
    CHECK_EQ(ent("4U0ITU").dxcc, std::string("ITU HQ"));
    CHECK(!ent("G4ABC/MM").ok); CHECK(!ent("G4ABC/AM").ok); CHECK(!ent("<...>").ok); CHECK(!ent("").ok); CHECK(!ent("A/B/C/D").ok);
    CHECK(decode::dxcc_lookup("IT9ABC") && std::string(decode::dxcc_lookup("IT9ABC")->name) == "Sicily");         // the PSK Reporter column keeps cty.dat's own name
}
TEST(callsigns_with_a_prefix_no_country_uses_are_spotted) {
    for (const char* bad : {"0J0BQR", "0J0BQR/P", "EA8/0J0BQR", "1Z1ABC"}) CHECK(!decode::callsign_prefix_known(bad));
    for (const char* good : {"M0OML", "3X4U", "1A0KM", "4U1ITU", "3D2CCC", "G4ABC/P", "EA8/G4ABC", "KH6ABC/MM", "<...>", "<PJ4/K1ABC>", "CQ", "RR73", "R-09", "+05", "JA35", "73", "CALL1X", "TNX", ""})
        CHECK(decode::callsign_prefix_known(good));
}
TEST(grid4_and_band_normalising) {
    CHECK_EQ(award::grid4("io92ar"), std::string("IO92")); CHECK_EQ(award::grid4("JN58"), std::string("JN58"));
    for (const char* bad : {"", "IO9", "RR73", "SS12", "IOAB", "73"}) CHECK_EQ(award::grid4(bad), std::string(""));
    CHECK_EQ(award::norm_band(" 20M "), std::string("20m"));
}
TEST(worked_index_tags_the_most_valuable_new_thing) {
    award::WorkedIndex w; award::Options o;
    w.add("DL2AL", "20m", "JN58"); w.add("G4ABC", "40M", "IO91ab"); w.add("F5XYZ", "40m", ""); w.add("PA4PS", "20m", "JO22");
    CHECK_EQ(w.qsos(), size_t(4)); CHECK_EQ(w.dxcc_count(), size_t(4)); CHECK_EQ(w.grid_count(), size_t(3));
    CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewDxcc));                         // Japan: never worked
    CHECK_EQ(w.tag("DL9XX", "40m", "JN58", o), int(award::kNewDxccBand));                     // Germany on 20 m only
    CHECK_EQ(w.tag("DL9XX", "20m", "JO40", o), int(award::kNewGrid));                         // Germany on 20 m done; JO40 is new
    CHECK_EQ(w.tag("DL9XX", "20m", "JN58", o), int(award::kNone));
    CHECK_EQ(w.tag("DL9XX", "20m", "", o), int(award::kNone));                                // no grid sent: nothing to say about grids
    CHECK_EQ(w.tag("DL9XX", "20m", "RR73", o), int(award::kNone));
    o.grid_band = true; CHECK_EQ(w.tag("G4ZZZ", "40m", "JN58", o), int(award::kNewGridBand)); o.grid_band = false;
    o.dxcc = false; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewDxccBand));   // each tag can be turned off
    o.dxcc_band = false; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewCont));   // Asia: a new continent
    o.cont = false; o.cq = true; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewCq));
    o.cq = false; o.itu = true; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewItu));
    o.itu = false; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNewGrid));
    o.grid = false; CHECK_EQ(w.tag("JA1ABC", "20m", "PM95", o), int(award::kNone));
    CHECK_EQ(w.tag("G4ABC/MM", "20m", "", award::Options()), int(award::kNone));             // no entity, no grid: no tag
    w.add("JA1ABC", "20m", "PM95"); CHECK_EQ(w.tag("JA2XYZ", "20m", "PM95", award::Options()), int(award::kNone));   // logging updates the index at once
    w.clear(); CHECK_EQ(w.qsos(), size_t(0));
    CHECK_EQ(std::string(award::tag_text(award::kNewDxcc)), std::string("NEW DXCC")); CHECK_EQ(std::string(award::tag_text(99)), std::string(""));
    for (int t = 0; t <= 9; ++t) CHECK(std::string(award::tag_text(t)).size() <= 15);          // fits the decoder's tag column
}
