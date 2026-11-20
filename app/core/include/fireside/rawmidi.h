// Raw MIDI over ALSA rawmidi device nodes (/dev/snd/midiCxDy) using plain POSIX I/O. Port of tools/firesidectl/transport.py.
// Splits the byte stream into SysEx frames and channel messages; real-time bytes (>= 0xF8) are ignored anywhere.
#pragma once
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "fireside/protocol.h"

namespace fireside {
struct IoError : std::runtime_error { using std::runtime_error::runtime_error; };
struct TimeoutError : std::runtime_error { using std::runtime_error::runtime_error; };

struct MidiEvent { uint8_t status, d1, d2; };
struct MidiDevice { std::string path, card; };
std::vector<MidiDevice> list_midi_devices(const std::string& dev_dir = "/dev", const std::string& proc_dir = "/proc");  // FLEX-looking cards first

class MidiParser {                                  // pure, so it can be tested without a device
public:
    void feed(const uint8_t* data, size_t n);
    std::deque<Frame> frames;
    std::deque<MidiEvent> events;
private:
    Frame buf_; bool in_sysex_ = false;
    uint8_t pend_[3] = {0, 0, 0}; int npend_ = 0;
    void chan(uint8_t b);
};

class RawMidi {
public:
    explicit RawMidi(const std::string& path);      // throws IoError
    ~RawMidi();
    RawMidi(const RawMidi&) = delete; RawMidi& operator=(const RawMidi&) = delete;
    void send(const Frame& data);                   // throws IoError / TimeoutError
    void pump(double timeout_s);                    // read what is available (waiting up to timeout)
    std::optional<Frame> next_frame(double timeout_s);
    MidiParser& parser() { return parser_; }
    std::deque<MidiEvent>& events() { return parser_.events; }
private:
    int fd_ = -1; MidiParser parser_;
};
}  // namespace fireside
