// Callsign prefix -> country centre (cty.dat, reduced by tools/gen_dxcc.py) and the location sanity rule used on weak FT8 decodes.
#include <cmath>
#include <random>
#include "fireside/dxcc.h"
#include "fireside/decode.h"
#include "test_util.h"
using namespace fireside::decode;

TEST(dxcc_longest_prefix_wins_and_unknown_prefixes_are_unknown) {
    CHECK(dxcc_lookup("W1AW") != nullptr); CHECK(dxcc_lookup("W1AW")->radius_km >= 3000);                    // the contiguous United States is big
    const auto* kal = dxcc_lookup("UA2ABC"); const auto* rus = dxcc_lookup("UA3ABC"); CHECK(kal && rus);
    CHECK(kal->radius_km < 500 && rus->radius_km > 2500);                                                       // UA2 = Kaliningrad, UA3 = European Russia (cty.dat splits them)
    CHECK(std::fabs(kal->lat - 54.7) < 1.0 && std::fabs(kal->lon - 20.5) < 1.0);                                // and longitudes are flipped to east-positive
    CHECK(dxcc_lookup("4J0QJH") && std::fabs(dxcc_lookup("4J0QJH")->lon - 47.4) < 1.0);                         // Azerbaijan: a digit-first prefix
    CHECK(dxcc_lookup("W1AW")->lon < 0);                                                                         // the US is west
    CHECK(dxcc_lookup("") == nullptr); CHECK(dxcc_lookup("#") == nullptr);
}
TEST(dxcc_grid_centre_and_distance) {
    double la, lo; CHECK(grid4_center("JN58", la, lo)); CHECK_NEAR(la, 48.5, 1e-9); CHECK_NEAR(lo, 11.0, 1e-9);                      // Munich
    CHECK(grid4_center("AA00", la, lo)); CHECK_NEAR(la, -89.5, 1e-9); CHECK_NEAR(lo, -179.0, 1e-9);
    CHECK(!grid4_center("RR73", la, lo) == false);                                                              // syntactically a locator: the decoder excludes the word separately
    CHECK(!grid4_center("S123", la, lo)); CHECK(!grid4_center("JN5", la, lo)); CHECK(!grid4_center("JN58A", la, lo)); CHECK(!grid4_center("jn58", la, lo));
    CHECK_NEAR(great_circle_km(51.5, -0.1, 40.7, -74.0), 5570.0, 40.0);                                         // London - New York
    CHECK_NEAR(great_circle_km(10, 20, 10, 20), 0.0, 1e-6);
}
TEST(dxcc_plausibility_rejects_only_impossible_locators) {
    CHECK(!callsign_grid_plausible("LY4PRC", "JL76"));                                                          // Lithuania in the Sahara: the false decode that started this
    CHECK(!callsign_grid_plausible("4J0QJH", "JQ29"));                                                          // Azerbaijan in Arctic Norway
    CHECK(callsign_grid_plausible("LY4PRC", "KO24")); CHECK(callsign_grid_plausible("LY4PRC", "KO15"));
    CHECK(callsign_grid_plausible("W1AW", "FN31")); CHECK(callsign_grid_plausible("W7XYZ", "CN87")); CHECK(callsign_grid_plausible("K6ABC", "DM13"));   // across the US
    CHECK(!callsign_grid_plausible("W1AW", "BL10"));                                                            // Hawaii is its own entity
    CHECK(callsign_grid_plausible("VK2ABC", "QF56")); CHECK(callsign_grid_plausible("VK6ABC", "OF78")); CHECK(callsign_grid_plausible("JA1ABC", "PM95"));
    CHECK(callsign_grid_plausible("G4ABC", "IO91")); CHECK(!callsign_grid_plausible("G4ABC", "FN31"));
    CHECK(callsign_grid_plausible("W1AW/P", "JN58"));                                                           // portable: anywhere
    CHECK(callsign_grid_plausible("<...>", "JN58")); CHECK(callsign_grid_plausible("QZ9ZZZ", "JN58")); CHECK(callsign_grid_plausible("LY4PRC", "RR73-")); CHECK(callsign_grid_plausible("", "JN58"));
}
TEST(ft8_location_rule_applies_to_weak_decodes_only) {
    // A clean message with an impossible locator is still printed (a ham can be anywhere, and the stage that decodes it is not guessing); the same message at -20 dB
    // (decoded by the deep stage, which can invent a locator from noise) is refused, while a plausible weak message is kept.
    auto strong = synth_ft8("CQ LY4PRC JL76", 1200, 12000, 0.3, 0.5); const auto r = Ft8Decoder::decode_slot(strong.data(), strong.size(), "");
    CHECK_EQ(r.size(), size_t(1)); CHECK_EQ(r[0].text, std::string("CQ LY4PRC JL76"));
    int odd = 0, fine = 0; const int n = 8; const double s = std::sqrt(0.045 / (std::pow(10.0, -20.0 / 10.0) * 2500.0 / 6000.0));
    for (int seed = 0; seed < n; ++seed) {
        for (const char* msg : {"CQ LY4PRC JL76", "CQ LY4PRC KO25"}) {
            auto x = synth_ft8(msg, 1200, 12000, 0.3, 0.5); std::mt19937 rng(unsigned(seed) * 7 + 1); std::normal_distribution<float> g(0.0f, float(s)); for (auto& v : x) v += g(rng);
            for (auto& m : Ft8Decoder::decode_slot(x.data(), x.size(), "")) { if (m.text == "CQ LY4PRC JL76") ++odd; if (m.text == "CQ LY4PRC KO25") ++fine; }
        }
    }
    CHECK(fine >= n - 1); CHECK(odd <= 2);                                                                      // a few are strong enough for the plain stage; most are refused
}
