// PSK Reporter: the MQTT 3.1.1 codec (bytes per the 3.1.1 spec), the topic for my call, report parsing (the feed page's example) and the heard-by summary.
#include "fireside/pskr.h"
#include "fireside/qrz.h"
#include "test_util.h"
using namespace fireside;

TEST(pskr_topic_for_my_call) {
    CHECK_EQ(pskr::sender_topic("m0oml"), std::string("pskr/filter/v2/+/+/M0OML/#"));
    CHECK(pskr::sender_topic("M0OML/P").empty()); CHECK(pskr::sender_topic("").empty()); CHECK(pskr::sender_topic("A+B").empty()); CHECK(pskr::sender_topic("#").empty());
}
TEST(pskr_mqtt_packets_are_3_1_1) {
    const auto c = pskr::mqtt::connect("fireside-1", 60);
    const std::vector<uint8_t> want{0x10, 22, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 60, 0, 10, 'f', 'i', 'r', 'e', 's', 'i', 'd', 'e', '-', '1'};
    CHECK(c == want);
    const auto s = pskr::mqtt::subscribe(1, "a/b");
    CHECK(s == (std::vector<uint8_t>{0x82, 8, 0, 1, 0, 3, 'a', '/', 'b', 0}));
    CHECK(pskr::mqtt::pingreq() == (std::vector<uint8_t>{0xC0, 0})); CHECK(pskr::mqtt::disconnect() == (std::vector<uint8_t>{0xE0, 0}));
    const std::string topic(200, 't'); const auto big = pskr::mqtt::subscribe(2, topic);      // remaining length 205 -> two length bytes
    CHECK_EQ(big[1], uint8_t(0x80 | (205 % 128))); CHECK_EQ(big[2], uint8_t(1)); CHECK_EQ(big.size(), size_t(3 + 205));
}
TEST(pskr_mqtt_decoder_splits_and_joins_packets) {
    pskr::mqtt::Decoder d;
    const uint8_t connack[] = {0x20, 2, 0, 0}, suback[] = {0x90, 3, 0, 1, 0}, ping[] = {0xD0, 0};
    std::vector<uint8_t> pub{0x30}; const std::string t = "pskr/x", pl = R"({"sc":"M0OML"})"; pub.push_back(uint8_t(2 + t.size() + pl.size()));
    pub.push_back(0); pub.push_back(uint8_t(t.size())); pub.insert(pub.end(), t.begin(), t.end()); pub.insert(pub.end(), pl.begin(), pl.end());
    std::vector<uint8_t> all; for (auto* p : {connack, suback}) all.insert(all.end(), p, p + (p == connack ? 4 : 5));
    all.insert(all.end(), pub.begin(), pub.end()); all.insert(all.end(), ping, ping + 2);
    for (size_t i = 0; i < all.size(); ++i) d.feed(&all[i], 1);                                    // one byte at a time
    auto a = d.next(); CHECK(a && a->type == pskr::mqtt::Type::Connack && a->connack_code == 0);
    auto b = d.next(); CHECK(b && b->type == pskr::mqtt::Type::Suback && b->suback_code == 0);
    auto c = d.next(); CHECK(c && c->type == pskr::mqtt::Type::Publish && c->topic == t && c->payload == pl);
    auto e = d.next(); CHECK(e && e->type == pskr::mqtt::Type::Pingresp); CHECK(!d.next()); CHECK(!d.broken());
    pskr::mqtt::Decoder bad; const uint8_t junk[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}; bad.feed(junk, sizeof junk); CHECK(!bad.next()); CHECK(bad.broken());
}
TEST(pskr_parses_the_feed_example) {
    // the feed page's example payload (its // comments removed)
    const auto s = pskr::parse_spot(R"({ "sq": 30142870791, "f": 21074653, "md": "FT8", "rp": -5, "t": 1662407712, "t_tx": 1662407700, "sc": "SP2EWQ", "sl": "JO93fn42",
                                         "rc": "CU3AT", "rl": "HM68jp36", "sa": 269, "ra": 149, "b": "15m" })");
    CHECK(s.has_value()); CHECK_EQ(s->seq, uint64_t(30142870791ull)); CHECK_EQ(s->freq_hz, 21074653.0); CHECK_EQ(s->mode, std::string("FT8"));
    CHECK(s->has_snr); CHECK_EQ(s->snr, -5); CHECK_EQ(s->t, int64_t(1662407712)); CHECK_EQ(s->sender, std::string("SP2EWQ")); CHECK_EQ(s->sender_grid, std::string("JO93fn42"));
    CHECK_EQ(s->receiver, std::string("CU3AT")); CHECK_EQ(s->receiver_grid, std::string("HM68jp36")); CHECK_EQ(s->sender_dxcc, 269); CHECK_EQ(s->receiver_dxcc, 149); CHECK_EQ(s->band, std::string("15m"));
    CHECK(!pskr::parse_spot("not json")); CHECK(!pskr::parse_spot(R"({"sc":"M0OML"})")); CHECK(!pskr::parse_spot(R"({"sc":"M0OML","rc":)"));
    const auto n = pskr::parse_spot(R"({"sc":"m0oml","rc":"dl1abc","x":{"a":[1,2]},"t":5})"); CHECK(n && n->sender == "M0OML" && !n->has_snr && n->t == 5);
}
TEST(pskr_heard_by_summary) {
    pskr::Heard h("M0OML", "IO92ar");
    auto spot = [](uint64_t sq, const char* rx, const char* grid, const char* band, int snr, int64_t t) {
        pskr::Spot s; s.seq = sq; s.sender = "M0OML"; s.receiver = rx; s.receiver_grid = grid; s.band = band; s.mode = "FT8"; s.snr = snr; s.has_snr = true; s.t = t; return s; };
    CHECK(h.add(spot(1, "DL1ABC", "JO62", "40m", -12, 1000)));
    CHECK(!h.add(spot(1, "DL1ABC", "JO62", "40m", -12, 1000)));                                       // the same report twice: once
    CHECK(h.add(spot(2, "DL1ABC", "JO62", "40m", -8, 1100)));
    CHECK(h.add(spot(3, "VK2XYZ", "QF56", "20m", -20, 1200)));
    CHECK(h.add(spot(4, "G4AAA", "IO91", "40m", 5, 900)));
    pskr::Spot other = spot(5, "W1AW", "FN31", "20m", 0, 1300); other.sender = "G0XXX"; CHECK(!h.add(other));   // someone else's signal
    const auto all = h.since(0); CHECK_EQ(all.size(), size_t(3)); CHECK_EQ(all[0].receiver, std::string("VK2XYZ"));   // newest first
    const auto dl = h.since(0, "40m"); CHECK_EQ(dl.size(), size_t(2)); CHECK_EQ(dl[0].receiver, std::string("DL1ABC")); CHECK_EQ(dl[0].count, 2); CHECK_EQ(dl[0].best_snr, -8); CHECK_EQ(dl[0].last_snr, -8);
    CHECK(dl[0].km > 900 && dl[0].km < 1300); CHECK(dl[0].bearing > 60 && dl[0].bearing < 110);      // home to Berlin: ~1100 km, east
    const auto sm = h.summary(0); CHECK_EQ(sm.stations, 3); CHECK_EQ(sm.spots, 4); CHECK_EQ(sm.furthest, std::string("VK2XYZ")); CHECK(sm.max_km > 16000);
    CHECK_EQ(sm.best, std::string("G4AAA")); CHECK_EQ(sm.by_band.at("40m"), 2);
    CHECK_EQ(dl[0].country, std::string("Fed. Rep. of Germany")); CHECK_EQ(all[0].country, std::string("Australia")); CHECK_EQ(sm.countries, 3);   // DL, VK, G (England)
    CHECK_EQ(h.summary(1050).stations, 2);                                                            // the window drops G4AAA (900) but keeps DL1ABC (last 1100)
    h.prune(1150); CHECK_EQ(h.size(), size_t(1)); CHECK(h.add(spot(2, "DL1ABC", "JO62", "40m", -8, 1300)));   // pruned sequence numbers are forgotten too
    pskr::Heard none; CHECK(!none.add(spot(9, "DL1ABC", "JO62", "40m", 0, 1)));                       // no call set: nothing is mine
}
TEST(pskr_csv_line) {
    pskr::Spot s; s.t = 1700000000; s.band = "40m"; s.mode = "FT8"; s.freq_hz = 7075512; s.snr = -12; s.has_snr = true; s.receiver = "DL1ABC"; s.receiver_grid = "JO62";
    CHECK_EQ(pskr::csv_line(s, 1104.4, 87.6, "end-fed, 10 m up"), std::string("1700000000,40m,FT8,7075512,-12,DL1ABC,JO62,1104,88,end-fed 10 m up"));
    s.has_snr = false; CHECK_EQ(pskr::csv_line(s, -1, -1, ""), std::string("1700000000,40m,FT8,7075512,,DL1ABC,JO62,,,"));
    CHECK_EQ(pskr::csv_header(), std::string("utc_epoch,band,mode,freq_hz,snr,receiver,receiver_grid,km,bearing,antenna"));
}
namespace {
uint16_t g16(const std::vector<uint8_t>& v, size_t i) { return uint16_t(v[i] << 8 | v[i + 1]); }
uint32_t g32(const std::vector<uint8_t>& v, size_t i) { return uint32_t(v[i]) << 24 | uint32_t(v[i + 1]) << 16 | uint32_t(v[i + 2]) << 8 | v[i + 3]; }
std::string gstr(const std::vector<uint8_t>& v, size_t& i) { const size_t n = v[i++]; std::string s(v.begin() + long(i), v.begin() + long(i + n)); i += n; return s; }
}  // namespace
TEST(pskr_report_from_decoder_lines) {
    auto r = pskr::report_from_line("203822 -12  0.1 1200 ~  CQ DL1ABC JO62", 7.074e6, "FT8", 1700000000, "M0OML");
    CHECK(r.has_value()); CHECK_EQ(r->call, std::string("DL1ABC")); CHECK_EQ(r->grid, std::string("JO62")); CHECK_EQ(r->snr, -12); CHECK_EQ(r->freq_hz, uint32_t(7075200)); CHECK_EQ(r->mode, std::string("FT8"));
    r = pskr::report_from_line("203822  -3  0.2  850 ~  G4AAA EA8XYZ IL18", 14.074e6, "FT8", 1, "M0OML"); CHECK(r && r->call == "EA8XYZ" && r->grid == "IL18" && r->freq_hz == 14074850u);
    r = pskr::report_from_line("203822  -3  0.2  850 ~  G4AAA EA8XYZ R-09", 14.074e6, "FT4", 1, "M0OML"); CHECK(r && r->call == "EA8XYZ" && r->grid.empty() && r->mode == "FT4");
    CHECK(!pskr::report_from_line("203822  -3  0.2  850 ~  DL1ABC M0OML IO92", 7.074e6, "FT8", 1, "M0OML"));     // my own transmission
    CHECK(!pskr::report_from_line("203822  -3  0.2  850 ~  DL1ABC <...> RR73", 7.074e6, "FT8", 1, "M0OML"));      // hashed call
    CHECK(!pskr::report_from_line("Waiting for a signal", 7.074e6, "FT8", 1, "M0OML"));
}
TEST(pskr_upload_datagram_layout) {
    pskr::Uploader u("m0oml", "IO92ar", "fireside 1.0", 0x12345678u, 1000, 7); u.set_antenna("end-fed");
    pskr::Report a; a.call = "DL1ABC"; a.grid = "JO62"; a.mode = "FT8"; a.freq_hz = 7075200; a.snr = -12; a.t = 1100;
    pskr::Report b = a; b.call = "EA8XYZ"; b.grid = ""; b.snr = 5; b.t = 1110;
    CHECK(u.add(a)); CHECK(u.add(b)); CHECK(!u.add(a));                                                 // DL1ABC on 40 m: once an hour
    pskr::Report a20 = a; a20.freq_hz = 14075000; CHECK(u.add(a20));                                       // another band counts as a change
    CHECK(!u.due(1100)); CHECK(u.next_send() >= 1300 && u.next_send() <= 1360);                            // 5 min + 0..60 s after start
    const auto v = u.build(1400); CHECK(!v.empty()); CHECK_EQ(u.pending(), size_t(0)); CHECK_EQ(u.sent_reports(), 3u);
    CHECK_EQ(g16(v, 0), uint16_t(10)); CHECK_EQ(size_t(g16(v, 2)), v.size()); CHECK_EQ(g32(v, 4), 1400u); CHECK_EQ(g32(v, 8), 0u); CHECK_EQ(g32(v, 12), 0x12345678u);
    // the receiver options template, byte for byte as on pskdev.html (receiverCallsign, receiverLocator, decodingSoftware, antennaInformation), our id
    const std::vector<uint8_t> rt{0x00, 0x03, 0x00, 0x2C, 0x6A, 0x42, 0x00, 0x04, 0x00, 0x01, 0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F, 0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
                                  0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F, 0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F, 0x00, 0x00};
    CHECK(std::equal(rt.begin(), rt.end(), v.begin() + 16));
    size_t i = 16; int sets = 0; std::vector<std::string> calls; std::string rx_call, rx_grid, sw, ant;
    while (i < v.size()) {                                                                                  // walk the sets: lengths add up, each a multiple of 4
        const uint16_t id = g16(v, i), len = g16(v, i + 2); CHECK(len % 4 == 0); CHECK(i + len <= v.size()); ++sets;
        if (id == 2) { CHECK_EQ(g16(v, i + 4), pskr::kSenderTemplate); CHECK_EQ(g16(v, i + 6), uint16_t(7)); CHECK_EQ(g16(v, i + 56), uint16_t(150)); }   // flowStartSeconds last
        if (id == pskr::kReceiverTemplate) { size_t j = i + 4; rx_call = gstr(v, j); rx_grid = gstr(v, j); sw = gstr(v, j); ant = gstr(v, j); }
        if (id == pskr::kSenderTemplate) {
            size_t j = i + 4;
            while (j + 4 < i + len && v[j] != 0) {
                calls.push_back(gstr(v, j)); const uint32_t f = g32(v, j); j += 4; const int8_t snr = int8_t(v[j++]); const std::string md = gstr(v, j), gr = gstr(v, j); const uint8_t src = v[j++]; const uint32_t t = g32(v, j); j += 4;
                CHECK(f == 7075200u || f == 14075000u); CHECK_EQ(md, std::string("FT8")); CHECK_EQ(src, uint8_t(1)); CHECK(t == 1100u || t == 1110u);
                if (calls.back() == "EA8XYZ") { CHECK_EQ(snr, int8_t(5)); CHECK(gr.empty()); } else { CHECK_EQ(snr, int8_t(-12)); CHECK_EQ(gr, std::string("JO62")); }
            }
        }
        i += len;
    }
    CHECK_EQ(i, v.size()); CHECK_EQ(sets, 4); CHECK_EQ(calls.size(), size_t(3));
    CHECK_EQ(rx_call, std::string("M0OML")); CHECK_EQ(rx_grid, std::string("IO92ar")); CHECK_EQ(sw, std::string("fireside 1.0")); CHECK_EQ(ant, std::string("end-fed"));
    CHECK(u.next_send() >= 1700 && u.next_send() <= 1760);                                                 // the next one 5 min later, not on a clock boundary
    // templates in the first three datagrams, then only hourly; the sequence number counts reports sent before
    for (int k = 0; k < 2; ++k) { pskr::Report c = a; c.call = "G" + std::to_string(k) + "AAA"; c.t = 2000 + k; u.add(c); const auto w = u.build(2000 + 400 * k); CHECK_EQ(g16(w, 16), uint16_t(3)); CHECK_EQ(g32(w, 8), uint32_t(3 + k)); }
    { pskr::Report c = a; c.call = "G9ZZZ"; c.t = 3000; u.add(c); const auto w = u.build(3000); CHECK_EQ(g16(w, 16), pskr::kReceiverTemplate); }   // 4th: no templates
    { pskr::Report c = a; c.call = "G8ZZZ"; c.t = 6100; u.add(c); const auto w = u.build(6100); CHECK_EQ(g16(w, 16), uint16_t(3)); }               // an hour on: again
    pskr::Uploader none("", "", "x", 1, 0, 1); CHECK(none.build(1000).empty()); CHECK(!none.due(99999));       // no station: nothing is sent
}
TEST(pskr_upload_splits_a_full_datagram) {
    pskr::Uploader u("M0OML", "IO92ar", "fireside 1.0", 1, 0, 3);
    for (int k = 0; k < 120; ++k) { pskr::Report r; r.call = "DL" + std::to_string(k) + "ABC"; r.mode = "FT8"; r.freq_hz = 7075000; r.t = 10; u.add(r); }
    CHECK(u.due(10));                                                                                      // full: sooner than 5 minutes
    const auto v = u.build(10); CHECK(v.size() <= 1400); CHECK(u.pending() > 0); CHECK(u.sent_reports() > 40);
    const auto w = u.build(11); CHECK(w.size() <= 1400); CHECK_EQ(u.sent_reports() + u.pending(), 120u);
}
TEST(pskr_distances_follow_my_locator_and_take_8_character_squares) {
    pskr::Heard h("M0OML", "");
    pskr::Spot s; s.seq = 1; s.sender = "M0OML"; s.receiver = "HA7MAC"; s.receiver_grid = "JN97ri23"; s.band = "40m"; s.t = 10; CHECK(h.add(s));
    CHECK(h.since(0)[0].km < 0);                                                                        // my locator not known yet
    h.set_station("M0OML", "IO92ar"); const auto r = h.since(0)[0]; CHECK(r.km > 1500 && r.km < 1700); CHECK(r.bearing > 90 && r.bearing < 115);   // home -> Budapest area
    double la, lo; CHECK(qrz::grid_latlon("JN97RI23", la, lo)); CHECK(!qrz::grid_latlon("JN97RIXX", la, lo));
}
