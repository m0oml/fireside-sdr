// Minimal Linux firewire-cdev client (/dev/fwN): async quadlet/block reads and writes. Port of tools/firesidectl/firewire.py.
// Uses <linux/firewire-cdev.h> directly (the Python version hand-packed the same layouts). Syscalls go through FwSys so tests can fake them.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "fireside/rawmidi.h"          // IoError, TimeoutError

namespace fireside {
struct FwError : IoError { using IoError::IoError; };

// Anything that can do 1394 async reads/writes: the real device, or a fake register file in tests. Data is raw bus bytes (big-endian).
class IBus {
public:
    virtual ~IBus() = default;
    virtual std::vector<uint8_t> read(uint64_t offset, size_t length) = 0;
    virtual void write(uint64_t offset, const std::vector<uint8_t>& data) = 0;
};

// Syscall seam. The default implementation is the real ioctl/read/poll.
class FwSys {
public:
    virtual ~FwSys() = default;
    virtual int ioctl(int fd, unsigned long req, void* arg) = 0;       // returns 0 or -1 with errno
    virtual ssize_t read(int fd, void* buf, size_t n) = 0;
    virtual bool wait_readable(int fd, double timeout_s) = 0;
    static FwSys& real();
};

constexpr uint32_t kTcodeWriteQuadlet = 0, kTcodeWriteBlock = 1, kTcodeReadQuadlet = 4, kTcodeReadBlock = 5;
std::string rcode_name(uint32_t rcode);

class FwDevice : public IBus {
public:
    explicit FwDevice(const std::string& path);                          // opens O_RDWR and queries info; throws FwError
    FwDevice(FwSys& sys, int fd, const std::string& path, bool query);   // test seam: adopt an fd; query=false skips GET_INFO
    ~FwDevice();
    FwDevice(const FwDevice&) = delete; FwDevice& operator=(const FwDevice&) = delete;

    std::vector<uint8_t> read(uint64_t offset, size_t length) override;
    void write(uint64_t offset, const std::vector<uint8_t>& data) override;

    const std::string& path() const { return path_; }
    bool is_local() const { return node_id_ == local_node_id_; }
    uint32_t local_node_id() const { return local_node_id_; }   // our controller's node id (the SID in packets we send)
    std::vector<uint32_t> rom_quadlets() const;                          // config ROM quadlets (the kernel supplies them in host byte order)
    std::optional<uint64_t> guid() const;
    uint32_t generation() const { return generation_; }
    uint32_t node_id() const { return node_id_; }
    void set_state_for_test(uint32_t generation) { generation_ = generation; }

    double timeout_s = 1.0;

private:
    FwSys& sys_; int fd_; bool owns_fd_; std::string path_;
    uint64_t closure_ = 0; uint32_t node_id_ = 0, local_node_id_ = 0, generation_ = 0;
    std::vector<uint8_t> rom_;
    void get_info();
    std::vector<uint8_t> request(uint32_t tcode, uint64_t offset, size_t length, std::vector<uint8_t> payload);
    // Returns nullopt on a bus reset (state updated), else {rcode, data}.
    std::optional<std::pair<uint32_t, std::vector<uint8_t>>> wait_response(uint64_t closure);
};

struct FwNode { std::string path; std::optional<uint64_t> guid; bool is_local = false; std::string error; };
std::vector<FwNode> list_fw_devices(const std::string& dev_dir = "/dev");   // every readable /dev/fw* node, numerically sorted
}  // namespace fireside
