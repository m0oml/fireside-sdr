#include "fireside/firewire.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/firewire-cdev.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fireside {
// The Python reference hard-codes these from the same header; keep them locked together.
static_assert(FW_CDEV_IOC_GET_INFO == 0xC0282300u, "GET_INFO ioctl number");
static_assert(FW_CDEV_IOC_SEND_REQUEST == 0x40282301u, "SEND_REQUEST ioctl number");
static_assert(sizeof(fw_cdev_get_info) == 40 && sizeof(fw_cdev_send_request) == 40 && sizeof(fw_cdev_event_bus_reset) == 40, "cdev layouts");

namespace {
class RealSys : public FwSys {
public:
    int ioctl(int fd, unsigned long req, void* arg) override { return ::ioctl(fd, req, arg); }
    ssize_t read(int fd, void* buf, size_t n) override { return ::read(fd, buf, n); }
    bool wait_readable(int fd, double t) override {
        pollfd p{fd, POLLIN, 0};
        return ::poll(&p, 1, int(t * 1000) + 1) > 0;
    }
};
constexpr uint32_t kRcodeGeneration = 19;
}  // namespace

FwSys& FwSys::real() { static RealSys s; return s; }

std::string rcode_name(uint32_t rc) {
    switch (rc) {
        case 0: return "complete"; case 4: return "conflict"; case 5: return "data error"; case 6: return "type error";
        case 7: return "address error"; case 16: return "send error"; case 17: return "cancelled"; case 18: return "busy";
        case 19: return "generation"; case 20: return "no ack"; default: return "?";
    }
}

FwDevice::FwDevice(const std::string& path) : sys_(FwSys::real()), fd_(::open(path.c_str(), O_RDWR)), owns_fd_(true), path_(path) {
    if (fd_ < 0) throw FwError(path + ": " + std::strerror(errno));
    try { get_info(); } catch (...) { ::close(fd_); throw; }
}
FwDevice::FwDevice(FwSys& sys, int fd, const std::string& path, bool query) : sys_(sys), fd_(fd), owns_fd_(false), path_(path) {
    if (query) get_info();
}
FwDevice::~FwDevice() { if (owns_fd_ && fd_ >= 0) ::close(fd_); }

void FwDevice::get_info() {
    std::vector<uint8_t> rom(1024);
    fw_cdev_event_bus_reset reset{};
    fw_cdev_get_info info{};
    info.version = 4;                                           // ABI 4: classic fw_cdev_event_response (no timestamp)
    info.rom_length = uint32_t(rom.size());
    info.rom = reinterpret_cast<uintptr_t>(rom.data());
    info.bus_reset = reinterpret_cast<uintptr_t>(&reset);
    if (sys_.ioctl(fd_, FW_CDEV_IOC_GET_INFO, &info) != 0) throw FwError(path_ + ": GET_INFO: " + std::strerror(errno));
    rom.resize(std::min<size_t>(info.rom_length, 1024));
    rom_ = std::move(rom);
    node_id_ = reset.node_id; local_node_id_ = reset.local_node_id; generation_ = reset.generation;
}

std::vector<uint32_t> FwDevice::rom_quadlets() const {
    // The kernel copies the config ROM as an array of host-order quadlets (measured: the radio's GUID 001c2d04007fffff read
    // big-endian on x86 came out quadlet-swapped as 042d1c00ffff7f00), so the bytes are taken as they are.
    std::vector<uint32_t> q(rom_.size() / 4);
    if (!q.empty()) std::memcpy(q.data(), rom_.data(), q.size() * 4);
    return q;
}
std::optional<uint64_t> FwDevice::guid() const {
    auto q = rom_quadlets();
    if (q.size() <= 4) return std::nullopt;
    return uint64_t(q[3]) << 32 | q[4];
}

std::optional<std::pair<uint32_t, std::vector<uint8_t>>> FwDevice::wait_response(uint64_t closure) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout_s));
    std::vector<uint8_t> ev(4096);
    for (;;) {
        const double left = std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) throw TimeoutError(path_ + ": no FireWire response");
        if (!sys_.wait_readable(fd_, left)) continue;
        const ssize_t n = sys_.read(fd_, ev.data(), ev.size());
        if (n < 12) { if (n < 0 && errno != EAGAIN && errno != EINTR) throw FwError(path_ + ": read: " + std::strerror(errno)); continue; }
        uint32_t type; std::memcpy(&type, ev.data() + 8, 4);
        if (type == FW_CDEV_EVENT_BUS_RESET && size_t(n) >= sizeof(fw_cdev_event_bus_reset)) {
            fw_cdev_event_bus_reset r; std::memcpy(&r, ev.data(), sizeof r);
            node_id_ = r.node_id; local_node_id_ = r.local_node_id; generation_ = r.generation;
            return std::nullopt;
        }
        uint64_t c; std::memcpy(&c, ev.data(), 8);
        if (type == FW_CDEV_EVENT_RESPONSE && c == closure && n >= 20) {
            uint32_t rcode, len; std::memcpy(&rcode, ev.data() + 12, 4); std::memcpy(&len, ev.data() + 16, 4);
            len = std::min<uint32_t>(len, uint32_t(n - 20));
            return std::make_pair(rcode, std::vector<uint8_t>(ev.begin() + 20, ev.begin() + 20 + len));
        }
    }
}

std::vector<uint8_t> FwDevice::request(uint32_t tcode, uint64_t offset, size_t length, std::vector<uint8_t> payload) {
    const bool is_read = tcode == kTcodeReadQuadlet || tcode == kTcodeReadBlock;
    std::vector<uint8_t> buf = is_read ? std::vector<uint8_t>(length) : std::move(payload);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t closure = ++closure_;
        fw_cdev_send_request rq{};
        rq.tcode = tcode; rq.length = uint32_t(length); rq.offset = offset; rq.closure = closure;
        rq.data = reinterpret_cast<uintptr_t>(buf.data()); rq.generation = generation_;
        if (sys_.ioctl(fd_, FW_CDEV_IOC_SEND_REQUEST, &rq) != 0) throw FwError(path_ + ": SEND_REQUEST: " + std::strerror(errno));
        auto r = wait_response(closure);
        if (!r) continue;                                       // bus reset: retry on the new generation
        if (r->first == kRcodeGeneration) continue;             // stale generation
        if (r->first != 0) {
            char off[32]; std::snprintf(off, sizeof off, "0x%012llX", static_cast<unsigned long long>(offset));
            throw FwError(path_ + " offset " + off + ": rcode " + std::to_string(r->first) + " (" + rcode_name(r->first) + ")");
        }
        return is_read ? std::move(r->second) : std::vector<uint8_t>{};
    }
    throw FwError(path_ + ": request failed after bus resets");
}

std::vector<uint8_t> FwDevice::read(uint64_t offset, size_t length) {
    return request(length == 4 ? kTcodeReadQuadlet : kTcodeReadBlock, offset, length, {});
}
void FwDevice::write(uint64_t offset, const std::vector<uint8_t>& data) {
    request(data.size() == 4 ? kTcodeWriteQuadlet : kTcodeWriteBlock, offset, data.size(), data);
}

std::vector<FwNode> list_fw_devices(const std::string& dev_dir) {
    std::vector<std::pair<int, std::string>> found;
    if (DIR* d = ::opendir(dev_dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 2 && n.compare(0, 2, "fw") == 0 && std::all_of(n.begin() + 2, n.end(), [](char c) { return c >= '0' && c <= '9'; }))
                found.emplace_back(std::stoi(n.substr(2)), dev_dir + "/" + n);
        }
        ::closedir(d);
    }
    std::sort(found.begin(), found.end());
    std::vector<FwNode> out;
    for (auto& f : found) {
        FwNode node; node.path = f.second;
        try { FwDevice dev(f.second); node.guid = dev.guid(); node.is_local = dev.is_local(); }
        catch (const std::exception& e) { node.error = e.what(); }
        out.push_back(node);
    }
    return out;
}
}  // namespace fireside
