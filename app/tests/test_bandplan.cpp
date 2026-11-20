// UK band plan for the spectrum overlay: Ofcom OFW611 (Feb 2024) Full-licence allocations + RSGB 2026 usage segments.
#include <string>
#include "fireside/bandplan.h"
#include "test_util.h"
using namespace fireside::bandplan;

TEST(bandplan_rsgb_segments_classify_the_usual_frequencies) {
    CHECK(segment_at(7030e3)->kind == Kind::Cw); CHECK(segment_at(7045e3)->kind == Kind::Narrow);
    CHECK(segment_at(7074e3)->kind == Kind::AllModes);                          // RSGB: 7060-7100 all modes (FT8 lives there)
    CHECK(segment_at(14074e3)->kind == Kind::Narrow); CHECK(segment_at(14100e3)->kind == Kind::Beacon); CHECK(segment_at(14200e3)->kind == Kind::AllModes);
    CHECK(segment_at(1900e3)->kind == Kind::Phone); CHECK(segment_at(29600e3)->kind == Kind::Fm); CHECK(segment_at(50150e3)->kind == Kind::Phone);
    CHECK(segment_at(9500e3) == nullptr);
}
TEST(bandplan_ofcom_full_licence_edges_status_and_power) {
    CHECK(allocation_at(7199.9e3) && !allocation_at(7200e3));                    // the UK 40 m band ends at 7200 (7200-7300 is Region 2)
    CHECK(allocation_at(3799e3) && !allocation_at(3800e3) && !allocation_at(3900e3));
    CHECK(allocation_at(1900e3)->max_pep_w == 32 && !allocation_at(1900e3)->primary); CHECK(allocation_at(1820e3)->max_pep_w == 1000 && allocation_at(1820e3)->primary);
    CHECK(allocation_at(10120e3)->max_pep_w == 400 && !allocation_at(10120e3)->primary);
    CHECK(allocation_at(51500e3)->max_pep_w == 100 && !allocation_at(51500e3)->primary); CHECK(allocation_at(50500e3)->primary);
    CHECK(allocation_at(70200e3) && allocation_at(70200e3)->max_pep_w == 160 && !allocation_at(70600e3));
}
TEST(bandplan_60m_is_the_eleven_uk_segments_not_the_wrc15_band) {
    CHECK_EQ(allocations_between(5250e3, 5410e3).size(), size_t(11));
    CHECK(allocation_at(5262e3) && allocation_at(5357e3) && allocation_at(5405e3));
    CHECK(!allocation_at(5360e3));                                               // inside WRC-15 5351.5-5366.5 but NOT a UK allocation
    CHECK(!allocation_at(5270e3)); CHECK(!allocation_at(5358e3));                // a gap; and 5358 is the top edge
    CHECK_EQ(gaps_between(5250e3, 5410e3).size(), size_t(10));
    CHECK(describe(5360e3).find("NOT a UK allocation") != std::string::npos);
    CHECK(describe(5357e3).find("above 5358") != std::string::npos);
    CHECK(gaps_between(7000e3, 7200e3).empty());                                 // 160 m pieces touch: no gaps there either
    CHECK(gaps_between(1810e3, 2000e3).empty());
}
TEST(bandplan_segments_sit_inside_the_allocations_without_overlap) {
    const auto s = segments_between(0, 100e6);
    for (size_t i = 0; i < s.size(); ++i) {
        CHECK(s[i].lo_hz < s[i].hi_hz);
        if (i) CHECK(s[i].lo_hz >= s[i - 1].hi_hz);
        CHECK(allocation_at(s[i].lo_hz) != nullptr); CHECK(allocation_at((s[i].lo_hz + s[i].hi_hz) / 2) != nullptr);
    }
    for (const auto& a : allocations_between(1.8e6, 100e6)) { CHECK(segment_at(a.lo_hz) != nullptr); }   // every HF/VHF allocation has RSGB usage from its bottom edge
}
TEST(bandplan_describe_and_marks) {
    const std::string d = describe(7074e3);
    CHECK(d.find("40m") == 0); CHECK(d.find("SSB contest preferred") != std::string::npos); CHECK(d.find("UK Full: primary, 1000 W") != std::string::npos);
    CHECK_EQ(describe(9500e3), std::string("")); CHECK_EQ(describe(0), std::string(""));
    const auto m = marks_between(14070e3, 14085e3); CHECK_EQ(m.size(), size_t(2)); CHECK_EQ(std::string(m[0].label), std::string("FT8")); CHECK_EQ(std::string(m[1].label), std::string("FT4"));
    CHECK(marks_between(9000e3, 9100e3).empty());
    CHECK_EQ(std::string(kind_name(Kind::AllModes)), std::string("All modes"));
}
