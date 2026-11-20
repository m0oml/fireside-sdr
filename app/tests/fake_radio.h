// A simulated FLEX-5000 on the far side of a pty (mirror of tools/firesidectl/fake.py). Test use only.
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <poll.h>
#include <pty.h>
#include <set>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include "fireside/protocol.h"
#include "fireside/opcodes.h"

class FakeRadio {
public:
    std::vector<uint8_t> eeprom_trx = std::vector<uint8_t>(0x1000, 0xFF);
    std::set<uint16_t> drop_ids;
    bool noise = false;
    std::function<void(size_t)> on_request;                      // called with the running request count (before answering)

    FakeRadio() {
        if (openpty(&master_, &slave_, nullptr, nullptr, nullptr) != 0) throw std::runtime_error("openpty failed");
        for (int fd : {master_, slave_}) { termios t; tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
        path_ = ttyname(slave_);
        answers_ = {{"RDAL_OP_GET_MODEL", 0}, {"RDAL_OP_GET_SERIAL_NUM", 12345}, {"RDAL_OP_GET_FIRMWARE_REV", 0x02010408}, {"RDAL_OP_GET_TRX_OK", 1},
                    {"RDAL_OP_GET_PA_OK", 1}, {"RDAL_OP_GET_RFIO_OK", 1}, {"RDAL_OP_GET_ATU_OK", 1}, {"RDAL_OP_GET_RX2_OK", 0}, {"RDAL_OP_GET_VU_OK", 0}};
        th_ = std::thread([this] { run(); });
    }
    ~FakeRadio() { stop_ = true; th_.join(); ::close(master_); ::close(slave_); }      // join BEFORE closing: fd numbers get reused
    const std::string& path() const { return path_; }
    void set_answer(const std::string& full_name, uint32_t v) { std::lock_guard<std::mutex> l(m_); answers_[full_name] = v; }
    struct Write { std::string name; uint32_t d1, d2; };
    std::vector<Write> writes() { std::lock_guard<std::mutex> l(m_); return writes_; }
    size_t gets() { std::lock_guard<std::mutex> l(m_); return gets_; }
    void note(uint8_t s, uint8_t a, uint8_t b) { uint8_t m[3] = {s, a, b}; (void)!::write(master_, m, 3); }

private:
    int master_ = -1, slave_ = -1; std::string path_; std::thread th_; std::atomic<bool> stop_{false};
    std::mutex m_; std::map<std::string, uint32_t> answers_; std::vector<Write> writes_; size_t gets_ = 0, count_ = 0;

    void run() {
        fireside::Frame buf;
        while (!stop_) {
            pollfd p{master_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            uint8_t tmp[4096]; ssize_t n = ::read(master_, tmp, sizeof tmp);
            if (n <= 0) continue;
            buf.insert(buf.end(), tmp, tmp + n);
            for (;;) {
                auto it = std::find(buf.begin(), buf.end(), uint8_t(0xF7));
                if (it == buf.end()) break;
                fireside::Frame f(buf.begin(), it + 1); buf.erase(buf.begin(), it + 1);
                handle(f);
            }
        }
    }
    void handle(const fireside::Frame& f) {
        auto r = fireside::decode_request(f); if (!r) return;
        size_t count; { std::lock_guard<std::mutex> l(m_); count = count_++; }
        if (on_request) on_request(count);
        const fireside::OpInfo* op = fireside::find_opcode(r->opcode);
        std::lock_guard<std::mutex> l(m_);
        if (!op || !fireside::is_read(*op)) { writes_.push_back({op ? op->name : "#" + std::to_string(r->opcode), r->d1, r->d2}); return; }
        ++gets_;
        if (drop_ids.count(r->id)) return;
        if (noise) {
            const uint8_t notes[6] = {0x90, 61, 127, 0x80, 61, 0}; (void)!::write(master_, notes, 6);
            auto stale = fireside::encode_reply(uint16_t(r->id - 1), 0xDEAD); (void)!::write(master_, stale.data(), stale.size());
        }
        uint32_t v = 0; const std::string n = op->name;
        if (n == "RDAL_OP_READ_TRX_EEPROM_UINT32") {
            v = r->d1 + 4 <= eeprom_trx.size() ? uint32_t(eeprom_trx[r->d1]) | uint32_t(eeprom_trx[r->d1 + 1]) << 8 | uint32_t(eeprom_trx[r->d1 + 2]) << 16 | uint32_t(eeprom_trx[r->d1 + 3]) << 24 : 0xFFFFFFFFu;
        } else if (auto a = answers_.find(n); a != answers_.end()) v = a->second;
        auto reply = fireside::encode_reply(r->id, v);
        for (size_t i = 0; i < reply.size(); i += 7) (void)!::write(master_, reply.data() + i, std::min<size_t>(7, reply.size() - i));   // dribble, like a slow link
    }
};
