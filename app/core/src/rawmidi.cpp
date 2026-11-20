#include "fireside/rawmidi.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <poll.h>
#include <regex>
#include <unistd.h>

namespace fireside {
using Clock = std::chrono::steady_clock;

void MidiParser::feed(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (b >= 0xF8) continue;                                // real-time bytes may interleave anywhere
        if (b == 0xF7 && !in_sysex_) continue;                  // stray end-of-sysex
        if (b == 0xF0) { buf_.assign(1, b); in_sysex_ = true; continue; }
        if (b == 0xF7) { buf_.push_back(b); frames.push_back(buf_); buf_.clear(); in_sysex_ = false; continue; }
        if (in_sysex_) {
            if (b & 0x80) { in_sysex_ = false; buf_.clear(); chan(b); }   // status byte inside SysEx aborts the frame
            else buf_.push_back(b);
            continue;
        }
        chan(b);
    }
}
void MidiParser::chan(uint8_t b) {
    if (b & 0x80) { pend_[0] = b; npend_ = 1; return; }
    if (npend_ == 0) return;
    pend_[npend_++] = b;
    const uint8_t st = pend_[0] & 0xF0;
    const int need = (st == 0x80 || st == 0x90 || st == 0xA0 || st == 0xB0 || st == 0xE0) ? 3 : 2;
    if (npend_ == need) {
        events.push_back({pend_[0], pend_[1], need == 3 ? pend_[2] : uint8_t(0)});
        npend_ = 1;                                             // running status
    }
}

RawMidi::RawMidi(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) throw IoError("cannot open " + path + ": " + std::strerror(errno));
}
RawMidi::~RawMidi() { if (fd_ >= 0) ::close(fd_); }

void RawMidi::send(const Frame& data) {
    size_t off = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (off < data.size()) {
        ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
        if (n > 0) { off += size_t(n); continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) throw IoError(std::string("MIDI write: ") + std::strerror(errno));
        if (Clock::now() > deadline) throw TimeoutError("MIDI write blocked");
        pollfd p{fd_, POLLOUT, 0}; ::poll(&p, 1, 50);
    }
}
void RawMidi::pump(double timeout_s) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, int(timeout_s * 1000)) <= 0) return;
    uint8_t buf[4096];
    ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n > 0) parser_.feed(buf, size_t(n));
}
std::optional<Frame> RawMidi::next_frame(double timeout_s) {
    const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_s));
    for (;;) {
        if (!parser_.frames.empty()) { Frame f = std::move(parser_.frames.front()); parser_.frames.pop_front(); return f; }
        const double left = std::chrono::duration<double>(end - Clock::now()).count();
        if (left <= 0) return std::nullopt;
        pump(std::min(left, 0.05));
    }
}

std::vector<MidiDevice> list_midi_devices(const std::string& dev_dir, const std::string& proc_dir) {
    std::vector<std::pair<int, std::string>> cards;
    std::ifstream in(proc_dir + "/asound/cards");
    std::string line; const std::regex re(R"(^\s*(\d+)\s+\[(\S+)\s*\]:\s*(.*))");
    while (std::getline(in, line)) { std::smatch m; if (std::regex_match(line, m, re)) cards.emplace_back(std::stoi(m[1]), m[2].str() + " " + m[3].str()); }
    std::vector<MidiDevice> out;
    glob_t g{}; const std::string pat = dev_dir + "/snd/midiC*D*";
    if (::glob(pat.c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string p = g.gl_pathv[i]; std::smatch m;
            std::string name = "?";
            if (std::regex_search(p, m, std::regex(R"(midiC(\d+)D)"))) for (auto& c : cards) if (c.first == std::stoi(m[1])) name = c.second;
            out.push_back({p, name});
        }
    }
    globfree(&g);
    auto flexy = [](const MidiDevice& d) { std::string s = d.card; std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s.find("flex") != std::string::npos; };
    std::stable_sort(out.begin(), out.end(), [&](const MidiDevice& a, const MidiDevice& b) { return flexy(a) && !flexy(b); });
    return out;
}
}  // namespace fireside
