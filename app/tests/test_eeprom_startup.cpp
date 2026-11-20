#include "test_util.h"
#include <filesystem>
#include <sys/stat.h>
#include "fake_radio.h"
#include "fireside/eeprom.h"
#include "fireside/startup.h"
using namespace fireside;

static std::map<std::string, std::string> kv_first(const std::string& text, std::vector<std::string>& tables) {
    std::map<std::string, std::string> m; std::istringstream in(text); std::string l;
    while (std::getline(in, l)) { auto p = l.find('='); if (p == std::string::npos) continue; std::string k = l.substr(0, p), v = l.substr(p + 1); if (k == "table") tables.push_back(v); else m[k] = v; }
    return m;
}
static void check_decode_matches(const std::string& bin, const std::string& expect) {
    Decoded d = decode_eeprom(read_bin(bin)); std::vector<std::string> tables; auto m = kv_first(read_text(expect), tables);
    CHECK_EQ(d.header.serial, m["serial"]); CHECK_EQ(d.header.last_cal.value_or("None"), m["last_cal"]);
    CHECK_EQ(int(d.header.checksums_present), std::stoi(m["checksums_present"])); CHECK_EQ(int(d.header.image_version), std::stoi(m["image_version"]));
    CHECK_EQ(d.tables.size(), tables.size());
    for (size_t i = 0; i < tables.size(); ++i) {
        std::vector<std::string> f; std::istringstream s(tables[i]); std::string c; while (std::getline(s, c, '|')) f.push_back(c);
        const TableResult& t = d.tables[i];
        CHECK_EQ(t.name, f[0]); CHECK_EQ(int(t.pointer_ok), std::stoi(f[1])); CHECK_EQ(int(t.checksum_ok), std::stoi(f[2]));
        CHECK_EQ(int(t.checksum_stored), std::stoi(f[3])); CHECK_EQ(int(t.checksum_calc), std::stoi(f[4])); CHECK_EQ(int(t.has_nan), std::stoi(f[5]));
        CHECK_EQ(f[6] == "-" ? -1 : int(*t.in_range), f[6] == "-" ? -1 : std::stoi(f[6]));
    }
    CHECK_EQ(d.unverified, size_t(std::stoul(m["unverified"]))); CHECK_EQ(d.tx_carrier->size(), size_t(std::stoul(m["tx_carrier"])));
    CHECK_EQ(int(d.tx_terminated), std::stoi(m["tx_terminated"])); CHECK_EQ(d.problems.size(), size_t(std::stoul(m["problems"])));
    CHECK_NEAR(d.tables[0].values[0][0], std::stod(m["first_value"]), 1e-9);
}
TEST(decode_matches_python_good_image) { check_decode_matches("trx.bin", "trx.expect"); }
TEST(decode_matches_python_corrupt_image) {
    check_decode_matches("trx_bad.bin", "trx_bad.expect");
    Decoded d = decode_eeprom(read_bin("trx_bad.bin")); bool found = false;
    for (auto& p : d.problems) if (p.find("PA bridge: checksum") != std::string::npos) found = true;
    CHECK(found);
}
TEST(decode_edge_cases) {
    CHECK(decode_eeprom(std::vector<uint8_t>(64, 0)).problems[0].find("too short") != std::string::npos);
    CHECK(decode_eeprom(std::vector<uint8_t>(kTrxLen, 0xFF)).problems[0].find("blank") != std::string::npos);
    auto img = read_bin("trx.bin"); img[0x3F] = 0; bool never = false; for (auto& p : decode_eeprom(img).problems) if (p.find("never calibrated") != std::string::npos) never = true; CHECK(never);
    uint32_t s = 12345; for (int n = 0; n < 50; ++n) { std::vector<uint8_t> g(kTrxLen); for (auto& b : g) { s = s * 1664525u + 1013904223u; b = uint8_t(s >> 24); } decode_eeprom(g); }   // garbage never throws
}
TEST(csv_matches_python_byte_for_byte) {
    auto img = read_bin("trx.bin"); CHECK_EQ(csv_text(img), read_text("trx.csv")); CHECK(parse_csv(csv_text(img)) == img);
}
TEST(read_image_and_verification_over_pty) {
    FakeRadio radio; auto img = read_bin("trx.bin"); std::copy(img.begin(), img.end(), radio.eeprom_trx.begin());
    RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    CHECK(read_image(fx) == img);
    size_t before = radio.gets(); CHECK(read_verified(fx, "TRX", 0, 2) == img); CHECK_EQ(radio.gets() - before, 2 * kTrxLen / 4);
    CHECK_EQ(radio.writes().size(), size_t(0));                                       // a backup never writes
    size_t base = radio.gets();                                                       // flaky link: flip a bit inside pass 2 of the next verified read
    radio.on_request = [&, base](size_t n) { if (n == base + kTrxLen / 4 + 10) radio.eeprom_trx[0x100] ^= 1; };
    CHECK_THROWS(read_verified(fx, "TRX", 0, 2), EepromReadError);
}
TEST(save_backup_is_read_only_and_never_overwrites) {
    std::string dir = std::filesystem::temp_directory_path().string() + "/firesidecore_bk_" + std::to_string(::getpid()); std::filesystem::remove_all(dir);
    auto img = read_bin("trx.bin");
    auto paths = save_backup(dir, {{"TRX", img}});
    CHECK_EQ(paths.size(), size_t(2));
    for (auto& p : paths) { struct stat st; CHECK_EQ(::stat(p.c_str(), &st), 0); CHECK_EQ(int(st.st_mode & 0777), 0444); }
    CHECK(paths[0].find("F5K_0301-1234_") != std::string::npos);
    CHECK_THROWS(save_backup(dir, {{"TRX", img}}), IoError);                          // same-second rerun: refuses to overwrite
    std::string csv = read_text("trx.csv"); std::ifstream in(dir + "/F5K_0301-1234 backup.csv"); std::stringstream ss; ss << in.rdbuf(); CHECK_EQ(ss.str(), csv);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all); std::filesystem::remove_all(dir);
}
