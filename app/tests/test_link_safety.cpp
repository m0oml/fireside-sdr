#include "test_util.h"
#include "fake_radio.h"
#include "fireside/client.h"
#include "fireside/identify.h"
using namespace fireside;

TEST(safety_gates) {
    Safety s;
    const OpInfo& rx = *find_opcode("SET_RX1_FREQ"); const OpInfo& eew = *find_opcode("WRITE_TRX_EEPROM_UINT8"); const OpInfo& mox = *find_opcode("SET_MOX");
    CHECK_THROWS(s.check(rx, false), SafetyError);                                    // writes blocked by default
    s.allow_write = true;
    s.check(rx, false);                                                               // plain write ok
    for (const char* n : {"WRITE_TRX_EEPROM_UINT8", "SET_EEPROM_WC", "REG_SET", "I2C_WRITE_VALUE", "SET_PA_BIAS", "SET_POWER_OFF", "NOOP", "SET_MOX", "ATU_SEND_CMD", "SET_TR", "INITIALIZE"})
        CHECK_THROWS(s.check(*find_opcode(n), false), SafetyError);
    s.permit("RDAL_OP_INITIALIZE"); s.check_write(*find_opcode("INITIALIZE"), 0, 0);  // reviewed exemption, reviewed value, still needs allow_write
    CHECK_THROWS(s.check_write(*find_opcode("INITIALIZE"), 1, 0), SafetyError);      // F1: the permit covers only the reviewed value
    CHECK_THROWS(s.check_write(*find_opcode("INITIALIZE"), 0, 7), SafetyError);
    CHECK_THROWS(s.check(*find_opcode("INITIALIZE"), false), SafetyError);            // value unknown: never covered by a permit
    s.unpermit("RDAL_OP_INITIALIZE"); CHECK_THROWS(s.check_write(*find_opcode("INITIALIZE"), 0, 0), SafetyError);
    s.allow_write = false; s.permit("RDAL_OP_INITIALIZE"); CHECK_THROWS(s.check_write(*find_opcode("INITIALIZE"), 0, 0), SafetyError);
    Safety r;
    CHECK_THROWS(r.check(eew, true), SafetyError); CHECK_THROWS(r.check(mox, true), SafetyError);           // reading a write opcode is refused
    r.check(*find_opcode("READ_TRX_EEPROM_UINT32"), true); r.check(*find_opcode("GET_MODEL"), true);
    CHECK(Safety::matches_dangerous("RDAL_OP_SET_PA_BIAS")); CHECK(!Safety::matches_dangerous("RDAL_OP_SET_RX1_FREQ"));
}
TEST(get_over_pty_and_stale_replies) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    CHECK_EQ(fx.get("GET_MODEL"), 0u); CHECK_EQ(fx.get("GET_SERIAL_NUM"), 12345u);
    radio.noise = true;                                                               // stale reply + notes injected before each answer
    CHECK_EQ(fx.get("GET_SERIAL_NUM"), 12345u); CHECK_EQ(fx.get("GET_SERIAL_NUM"), 12345u);
    bool saw_note = false; for (auto& e : t.events()) if (e.status == 0x90 && e.d1 == 61) saw_note = true;
    CHECK(saw_note);                                                                  // paddle events not lost
}
TEST(timeout_and_id_wrap) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    radio.drop_ids = {0}; CHECK_THROWS(fx.get("GET_MODEL", 0, 0, 0.2), TimeoutError);
    radio.drop_ids.clear();
}
TEST(message_id_wraps_at_16_bits) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    link.set_next_id(0xFFFE);                                                         // ids 0xFFFE, 0xFFFF, then 0, 1 ...
    for (int i = 0; i < 4; ++i) CHECK_EQ(fx.get("GET_SERIAL_NUM"), 12345u);
    radio.drop_ids = {0xFFFF}; link.set_next_id(0xFFFF);
    CHECK_THROWS(fx.get("GET_SERIAL_NUM", 0, 0, 0.2), TimeoutError);                  // the dropped id really was 0xFFFF...
    CHECK_EQ(fx.get("GET_SERIAL_NUM"), 12345u);                                       // ...and the next one is id 0 after the wrap
}
TEST(set_sends_correctly_packed_float_and_obeys_gate) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t);
    Flex ro(link); CHECK_THROWS(ro.set("SET_RX1_FREQ", 14.2345), SafetyError);
    Safety s; s.allow_write = true; Flex fx(link, s);
    fx.set("SET_RX1_FREQ", 14.2345); CHECK_THROWS(fx.set("SET_MOX", 1), SafetyError); CHECK_THROWS(fx.set("NO_SUCH_OP", 1), std::invalid_argument);
    fx.get("GET_MODEL");                                                              // round trip so the set has been processed
    auto w = radio.writes(); CHECK_EQ(w.size(), size_t(1));
    CHECK_EQ(w[0].name, std::string("RDAL_OP_SET_RX1_FREQ")); CHECK_NEAR(as_f32(w[0].d1), 14.2345, 1e-4); CHECK_EQ(w[0].d2, 0u);
}
TEST(identify_over_pty) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    auto id = identify(fx);
    CHECK_EQ(id.model_name(), std::string("FLEX-5000A")); CHECK_EQ(serial_str(id.serial), std::string("5748-0000")); CHECK_EQ(ver_str(id.firmware), std::string("2.1.4.8"));
    CHECK(id.has_atu()); CHECK(!id.has_rx2()); CHECK(id.describe().find("FLEX-5000A") != std::string::npos);
    fx.get("GET_MODEL"); CHECK_EQ(radio.writes().size(), size_t(0));                  // identification is read-only
}
TEST(identify_rejections_and_boot_wait) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link);
    for (uint32_t fw : {0x01030008u, 0x02010407u, 0x02020000u}) {
        radio.set_answer("RDAL_OP_GET_FIRMWARE_REV", fw);
        try { identify(fx); CHECK(false); } catch (const StartupError& e) { CHECK(std::string(e.what()).find("outside PowerSDR's supported range") != std::string::npos); }
    }
    radio.set_answer("RDAL_OP_GET_FIRMWARE_REV", 0x02010408); radio.set_answer("RDAL_OP_GET_MODEL", 3);
    try { identify(fx); CHECK(false); } catch (const StartupError& e) { CHECK(std::string(e.what()).find("FLEX-3000") != std::string::npos); }
    radio.set_answer("RDAL_OP_GET_MODEL", 0); radio.set_answer("RDAL_OP_GET_FIRMWARE_REV", 0);       // never answers: give up on a fake clock
    double now = 0; IdentifyOptions o; o.boot_wait_s = 5; o.poll_s = 1; o.sleep = [&](double s) { now += s; }; o.clock = [&] { return now; };
    try { identify(fx, o); CHECK(false); } catch (const StartupError& e) { CHECK(std::string(e.what()).find("too soon after power-on") != std::string::npos); }
    CHECK_EQ(radio.writes().size(), size_t(0));
}
TEST(safety_codec_write_only_the_mic_gain_register_with_its_reviewed_values) {
    const OpInfo* op = find_opcode(std::string("RDAL_OP_WRITE_CODEC_REG")); CHECK(op != nullptr);
    Safety s; s.allow_write = true;
    CHECK_THROWS(s.check_write(*op, 0x16, 0x00), SafetyError);                     // not without a permit
    s.permit("RDAL_OP_WRITE_CODEC_REG");
    s.check_write(*op, 0x16, 0x00); s.check_write(*op, 0x16, 0x80); s.check_write(*op, 0x16, 0xE0); s.check_write(*op, 0x16, 0xFF);   // 0 dB, mute, -32 dB, -1 dB
    CHECK_THROWS(s.check_write(*op, 0x16, 0x10), SafetyError);                     // positive gain: not reviewed
    CHECK_THROWS(s.check_write(*op, 0x07, 0x00), SafetyError);                     // output mutes: not this permit
    CHECK_THROWS(s.check_write(*op, 0x14, 0x00), SafetyError);                     // line-in: not reviewed
    CHECK_THROWS(s.check(*op, false), SafetyError);                                // without values: never
}
