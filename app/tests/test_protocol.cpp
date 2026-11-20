#include "test_util.h"
#include <cstring>
#include "fireside/opcodes.h"
#include "fireside/protocol.h"
#include "fireside/rawmidi.h"
#include "fireside/tuning.h"
using namespace fireside;

static Frame frame_of(const std::string& hex) { return from_hex(hex); }

TEST(frames_match_python_vectors) {
    auto rows = read_table("frames.txt"); CHECK(rows.size() >= 8);
    for (auto& r : rows) {
        const uint16_t id = uint16_t(std::stoul(r[0])); const uint32_t op = uint32_t(std::stoul(r[1])), d1 = uint32_t(std::stoull(r[2])), d2 = uint32_t(std::stoull(r[3]));
        Frame want = frame_of(r[4]);
        CHECK_EQ(want.size(), size_t(37));
        CHECK(encode_request(id, op, d1, d2) == want);
        auto q = decode_request(want); CHECK(q.has_value());
        CHECK_EQ(q->id, id); CHECK_EQ(q->opcode, op); CHECK_EQ(q->d1, d1); CHECK_EQ(q->d2, d2); CHECK_EQ(q->proto, 0);
    }
}
TEST(replies_match_python_vectors) {
    for (auto& r : read_table("replies.txt")) {
        const uint16_t id = uint16_t(std::stoul(r[0])); const uint32_t v = uint32_t(std::stoull(r[1]));
        CHECK(encode_reply(id, v) == frame_of(r[2]));
        auto d = decode_reply(frame_of(r[2])); CHECK(d.has_value()); CHECK_EQ(d->id, id); CHECK_EQ(d->value, v);
    }
}
TEST(malformed_frames_are_rejected) {
    CHECK(!decode_request({}));
    CHECK(!decode_request(frame_of("f00000415a5af7")));                        // non-hex payload
    Frame wrong_mfr = encode_request(1, 2, 3, 4); wrong_mfr[3] = 0x42; CHECK(!decode_request(wrong_mfr));
    Frame lower = encode_request(1, 2, 3, 4); for (auto& c : lower) if (c >= 'A' && c <= 'F') c = uint8_t(c + 32); CHECK(!decode_request(lower));   // lowercase hex rejected like Python
    Frame odd = encode_request(1, 2, 3, 4); odd.erase(odd.end() - 2); CHECK(!decode_request(odd));
    CHECK(decode_reply(encode_request(1, 2, 3, 4)).has_value());               // a 16-byte request parses as a reply (extra bytes ignored), as in Python
    Frame shortf = {0xF0, 0, 0, 0x41, 'A', 'B', 0xF7}; CHECK(!decode_reply(shortf));
}
TEST(argument_packing) {
    CHECK_EQ(pack_i32(-1), 0xFFFFFFFFu); CHECK_EQ(as_i32(0xFFFFFFFEu), -2);
    CHECK_THROWS(pack_u32(1LL << 32), std::out_of_range); CHECK_THROWS(pack_u32(-1), std::out_of_range);
    CHECK_THROWS(pack_i32(1LL << 31), std::out_of_range);
    CHECK_NEAR(as_f32(pack_f32(14.2)), 14.2, 1e-5);
    CHECK_EQ(pack_arg(7.1, ArgType::F32), pack_f32(7.1));
}
TEST(opcode_table) {
    CHECK_EQ(kOpCount, size_t(263));
    for (size_t i = 1; i < kOpCount; ++i) CHECK(kOps[i - 1].value < kOps[i].value);            // sorted, unique
    CHECK_EQ(find_opcode("GET_MODEL")->value, 1221u); CHECK_EQ(find_opcode("get_model")->value, 1221u);
    CHECK_EQ(find_opcode("RDAL_OP_NOOP")->value, 1018u); CHECK_EQ(find_opcode("SET_RX1_FREQ")->value, 1244u);
    CHECK_EQ(find_opcode(1347u)->arg2, ArgType::F32); CHECK_EQ(find_opcode("INITIALIZE")->arg1, ArgType::I32);
    CHECK(find_opcode("NOT_AN_OPCODE") == nullptr); CHECK(find_opcode(5u) == nullptr);
    CHECK_EQ(short_name(*find_opcode("RDAL_OP_GET_MODEL")), std::string("GET_MODEL"));
    CHECK(is_read(*find_opcode("READ_TRX_EEPROM_UINT32")));                                    // typed u32 (the float-misread bug found in Python)
    CHECK_EQ(find_opcode("READ_TRX_EEPROM_UINT32")->result, ArgType::U32);
}
TEST(tuning_words_match_python) {
    for (auto& r : read_table("tw.txt")) CHECK_EQ(freq_to_tw(std::stod(r[0])), uint32_t(std::stoul(r[1])));
    CHECK_EQ(freq_to_tw(7.1), 0x03A29C77u);
    for (double bad : {0.0, -1.0, 65.01, 500.0}) CHECK_THROWS(freq_to_tw(bad), std::invalid_argument);
}
TEST(parser_fragmentation_and_mixed_traffic) {
    Frame f = encode_reply(3, 77);
    Frame both = f; both.insert(both.end(), f.begin(), f.end());
    for (size_t step : {1u, 2u, 5u, 100u}) { MidiParser p; for (size_t i = 0; i < both.size(); i += step) p.feed(&both[i], std::min(step, both.size() - i)); CHECK_EQ(p.frames.size(), size_t(2)); CHECK(p.frames[0] == f); }
    MidiParser p;                                                                 // notes + real-time byte inside a frame
    Frame mix = {0x90, 61, 127}; mix.insert(mix.end(), f.begin(), f.begin() + 7); mix.push_back(0xF8); mix.insert(mix.end(), f.begin() + 7, f.end()); mix.insert(mix.end(), {0x80, 61, 0});
    p.feed(mix.data(), mix.size());
    CHECK_EQ(p.frames.size(), size_t(1)); CHECK(p.frames[0] == f); CHECK_EQ(p.events.size(), size_t(2));
    CHECK_EQ(p.events[0].status, 0x90); CHECK_EQ(p.events[0].d1, 61); CHECK_EQ(p.events[1].status, 0x80);
    MidiParser q; Frame t(f.begin(), f.begin() + 10); t.insert(t.end(), {0x90, 63, 100}); t.insert(t.end(), f.begin(), f.end());   // truncated SysEx recovers
    q.feed(t.data(), t.size()); CHECK_EQ(q.frames.size(), size_t(1)); CHECK_EQ(q.events.size(), size_t(1)); CHECK_EQ(q.events[0].d1, 63);
}
