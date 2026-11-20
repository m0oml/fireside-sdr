// ADIF records, band and mode mapping, call validation, the Wavelog JSON bodies, and the local ADIF safety copy.
#include <cstdio>
#include <stdexcept>
#include <unistd.h>
#include "fireside/logbook.h"
#include "test_util.h"
using namespace fireside::logbook;

namespace {
Qso sample() { Qso q; q.call = "dl2al"; q.freq_hz = 14074000; q.mode = "FT8"; q.time_on_utc = 1763164800 + 3 * 3600 + 15 * 60 + 30; q.rst_sent = "-07"; q.rst_rcvd = "-12"; q.grid = "jo62"; q.station_callsign = "m0oml"; return q; }   // 2025-11-15 03:15:30 UTC
bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
}  // namespace

TEST(bands_follow_the_amateur_allocations) {
    CHECK_EQ(band_for_hz(14074000), std::string("20m")); CHECK_EQ(band_for_hz(1840000), std::string("160m")); CHECK_EQ(band_for_hz(7074000), std::string("40m"));
    CHECK_EQ(band_for_hz(28074000), std::string("10m")); CHECK_EQ(band_for_hz(50313000), std::string("6m")); CHECK_EQ(band_for_hz(5357000), std::string("60m"));
    CHECK_EQ(band_for_hz(9500000), std::string("")); CHECK_EQ(band_for_hz(0), std::string("")); CHECK_EQ(band_for_hz(100e6), std::string(""));
}
TEST(modes_map_to_adif_modes) {
    CHECK_EQ(adif_mode("FT4").mode, std::string("MFSK")); CHECK_EQ(adif_mode("FT4").submode, std::string("FT4"));   // ADIF; Wavelog matches worked-before so
    CHECK(wavelog_lookup_path("HG7WDL", "40m", "FT4", false).find("&mode=MFSK") != std::string::npos);
    CHECK_EQ(adif_mode("USB").mode, std::string("SSB")); CHECK_EQ(adif_mode("LSB").mode, std::string("SSB")); CHECK_EQ(adif_mode("FT8").mode, std::string("FT8"));
    CHECK_EQ(adif_mode("PSK31").mode, std::string("PSK")); CHECK_EQ(adif_mode("PSK31").submode, std::string("PSK31")); CHECK_EQ(adif_mode("cw").mode, std::string("CW")); CHECK_EQ(adif_mode("RTTY").mode, std::string("RTTY"));
}
TEST(callsigns_are_validated) {
    for (const char* ok : {"M0OML", "DL2AL", "K1ABC/P", "9A1A", "PA4PS", "VK6WR"}) CHECK(valid_call(ok));
    for (const char* bad : {"", "AB", "ABCDEF", "123456", "M0 OML", "M0OML;", "<script>", "THISISWAYTOOLONGCALL1"}) CHECK(!valid_call(bad));
}
TEST(adif_record_has_correct_lengths_and_fields) {
    const std::string r = adif_record(sample());
    CHECK(has(r, "<CALL:5>DL2AL")); CHECK(has(r, "<BAND:3>20m")); CHECK(has(r, "<FREQ:9>14.074000")); CHECK(has(r, "<MODE:3>FT8")); CHECK(has(r, "<QSO_DATE:8>20251115")); CHECK(has(r, "<TIME_ON:6>031530"));
    CHECK(has(r, "<RST_SENT:3>-07")); CHECK(has(r, "<GRIDSQUARE:4>JO62")); CHECK(has(r, "<STATION_CALLSIGN:5>M0OML")); CHECK(r.size() > 5 && r.substr(r.size() - 5) == "<EOR>");
    CHECK(!has(r, "SUBMODE")); CHECK(!has(r, "<NAME")); Qso p = sample(); p.mode = "PSK31"; CHECK(has(adif_record(p), "<MODE:3>PSK<SUBMODE:5>PSK31"));
    Qso u = sample(); u.comment = "caf\xC3\xA9"; CHECK(has(adif_record(u), "<COMMENT:5>caf\xC3\xA9"));                         // ADIF lengths count bytes
}
TEST(bad_qsos_are_refused) {
    Qso q = sample(); q.call = ""; bool t = false; try { adif_record(q); } catch (const std::invalid_argument&) { t = true; } CHECK(t);
    q = sample(); q.freq_hz = 9.5e6; t = false; try { adif_record(q); } catch (const std::invalid_argument&) { t = true; } CHECK(t);
    q = sample(); q.time_on_utc = 0; t = false; try { adif_record(q); } catch (const std::invalid_argument&) { t = true; } CHECK(t);
    q = sample(); q.mode = ""; t = false; try { adif_record(q); } catch (const std::invalid_argument&) { t = true; } CHECK(t);
}
TEST(wavelog_v2_bodies_and_paths) {
    const std::string b = wavelog_adif_import_body(1, adif_record(sample()), true);
    CHECK(b.rfind("{\"import_type\":\"adif\",\"station_profile_id\":1,\"adif\":\"<CALL:5>DL2AL", 0) == 0); CHECK(b.substr(b.size() - 21) == "<EOR>\",\"dryrun\":true}");
    CHECK(!has(wavelog_adif_import_body(1, "x", false), "dryrun"));
    const std::string r = wavelog_radio_body("FLEX-5000A", 14074000, "USB");
    CHECK_EQ(r, std::string("{\"radio\":\"FLEX-5000A\",\"frequency\":14074000,\"mode\":\"SSB\"}")); CHECK(!has(r, "power")); CHECK(!has(r, "key"));
    CHECK_EQ(wavelog_lookup_path("dl2al", "20m", "FT8"), std::string("/index.php/api/v2/lookup?callsign=DL2AL&detail=full&band=20m&mode=FT8"));
    CHECK_EQ(wavelog_lookup_path("dl2al", "20m", "FT8", true), std::string("/index.php/api/v2/lookup?callsign=DL2AL&detail=full&band=20m&mode=FT8&callbook=true")); CHECK_EQ(wavelog_history_path("k1abc/p"), std::string("/index.php/api/v2/qso?callsign=K1ABC%2FP&limit=1"));
    CHECK_EQ(wavelog_lookup_path("k1abc/p", "", ""), std::string("/index.php/api/v2/lookup?callsign=K1ABC%2FP&detail=full"));
    CHECK_EQ(json_escape("a\nb\\c\t\x01"), std::string("a\\nb\\\\c\\t\\u0001")); CHECK_EQ(url_encode("a b&c"), std::string("a%20b%26c"));
}
TEST(base_url_is_normalised) {
    CHECK_EQ(wavelog_normalise_base_url("  https://log.example.com/ "), std::string("https://log.example.com")); CHECK_EQ(wavelog_normalise_base_url("http://192.168.1.5:8086//"), std::string("http://192.168.1.5:8086"));
    for (const char* bad : {"", "   ", "log.example.com", "ftp://x.y", "https://", "https://a b.com", "javascript:alert(1)"}) CHECK_EQ(wavelog_normalise_base_url(bad), std::string(""));
}
TEST(local_adif_copy_is_append_only_with_one_header) {
    const std::string path = "/tmp/firesidelog_test_" + std::to_string(::getpid()) + ".adi"; std::remove(path.c_str());
    append_adif_file(path, sample(), "fireside", "1"); Qso q2 = sample(); q2.call = "PA4PS"; append_adif_file(path, q2, "fireside", "1");
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); const std::string t = ss.str(); std::remove(path.c_str());
    size_t eoh = 0, eor = 0; for (size_t p = 0; (p = t.find("<EOH>", p)) != std::string::npos; ++p) ++eoh; for (size_t p = 0; (p = t.find("<EOR>", p)) != std::string::npos; ++p) ++eor;
    CHECK_EQ(eoh, size_t(1)); CHECK_EQ(eor, size_t(2)); CHECK(has(t, "<CALL:5>DL2AL")); CHECK(has(t, "<CALL:5>PA4PS"));
    bool threw = false; try { Qso bad = sample(); bad.call = "x"; append_adif_file(path, bad, "fireside", "1"); } catch (const std::invalid_argument&) { threw = true; } CHECK(threw);
    CHECK(access(path.c_str(), F_OK) != 0);                                                                                     // a refused QSO leaves no file behind
}

TEST(ftx_messages_are_parsed) {
    auto a = parse_ftx_message("CQ TA6CQ KN70"); CHECK(a.ok && a.cq && a.from == "TA6CQ" && a.grid == "KN70" && a.to.empty());
    a = parse_ftx_message("CQ DX K1ABC FN42"); CHECK(a.ok && a.from == "K1ABC" && a.grid == "FN42"); a = parse_ftx_message("CQ RU N9OY EN43"); CHECK(a.ok && a.from == "N9OY"); a = parse_ftx_message("CQ POTA W1AW"); CHECK(a.ok && a.from == "W1AW" && a.grid.empty());
    a = parse_ftx_message("M0OML DL2AL -07"); CHECK(a.ok && !a.cq && a.to == "M0OML" && a.from == "DL2AL" && a.report == "-07");
    a = parse_ftx_message("DL2AL M0OML R-12"); CHECK(a.ok && a.from == "M0OML" && a.to == "DL2AL" && a.report == "-12");
    a = parse_ftx_message("W9JA PY2APK RRR"); CHECK(a.ok && a.from == "PY2APK" && a.report.empty() && a.grid.empty()); a = parse_ftx_message("N1TRK KB7RUQ RR73"); CHECK(a.ok && a.from == "KB7RUQ" && a.grid.empty());
    a = parse_ftx_message("M0OML DL2AL JO62"); CHECK(a.ok && a.grid == "JO62"); a = parse_ftx_message("<G3XYZ> DL2AL +05"); CHECK(a.ok && a.to == "G3XYZ" && a.report == "+05");
    a = parse_ftx_message("<...> DL2AL -05"); CHECK(a.ok && a.to.empty() && a.from == "DL2AL"); a = parse_ftx_message("CQ"); CHECK(!a.ok); a = parse_ftx_message("TNX BOB 73"); CHECK(!a.ok); a = parse_ftx_message(""); CHECK(!a.ok);
}
TEST(decoder_lines_are_split) {
    int snr = 99; CHECK_EQ(ftx_line_text("110130  -6  0.7  683 ~  CQ TA6CQ KN70\n", &snr), std::string("CQ TA6CQ KN70")); CHECK_EQ(snr, -6);
    CHECK_EQ(ftx_line_text("120000 -22  0.1 1200 ~  M0OML G4ABC -12 a2\n"), std::string("M0OML G4ABC -12"));      // a-priori marker stripped (WSJT-X a1..a7)
    CHECK_EQ(ftx_line_text("120000 -22  0.1 1200 ~  CQ S57ACD JN75 ?"), std::string("CQ S57ACD JN75"));
    CHECK_EQ(ftx_line_text("T  +0  0.2 2729 ~  K4SQC VE3RX RR73", &snr), std::string("K4SQC VE3RX RR73")); CHECK_EQ(snr, 0); CHECK_EQ(ftx_line_text("hello"), std::string("")); CHECK_EQ(ftx_line_text(""), std::string(""));
}
TEST(recent_qso_path_is_clamped) { CHECK_EQ(wavelog_recent_path(12), std::string("/index.php/api/v2/qso?limit=12")); CHECK_EQ(wavelog_recent_path(0), std::string("/index.php/api/v2/qso?limit=1")); CHECK_EQ(wavelog_recent_path(500), std::string("/index.php/api/v2/qso?limit=50")); }

TEST(adif_carries_tx_power_when_known) {
    Qso q; q.call = "DK0ULR"; q.freq_hz = 14.074e6; q.mode = "FT8"; q.time_on_utc = 1'800'000'000;
    CHECK(adif_record(q).find("TX_PWR") == std::string::npos);                 // unknown: left out
    q.tx_pwr_w = 4.96; CHECK(adif_record(q).find("<TX_PWR:3>5.0") != std::string::npos);
    q.tx_pwr_w = 15.2; CHECK(adif_record(q).find("<TX_PWR:2>15") != std::string::npos);
}
