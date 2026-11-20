#include "fireside/identify.h"
#include <cstdio>
#include <thread>

namespace fireside {
std::string ver_str(uint32_t v) {
    return std::to_string((v >> 24) & 0xFF) + "." + std::to_string((v >> 16) & 0xFF) + "." + std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}
std::string serial_str(uint32_t sn) {
    char b[32]; std::snprintf(b, sizeof b, "%02u%02u-%04u", sn & 0xFF, (sn >> 8) & 0xFF, (sn >> 16) & 0xFFFF);
    return b;
}
std::string Identity::model_name() const {
    switch (model) { case 0: return "FLEX-5000A"; case 1: return "FLEX-5000C"; case 2: return "FLEX-5000 (variant 2)"; case 3: return "FLEX-3000"; }
    return "unknown model " + std::to_string(model);
}
bool Identity::has(const std::string& board) const {
    for (auto& b : boards) if (b.first == board) return b.second.ok;
    return false;
}
std::string Identity::describe() const {
    std::string s = model_name() + "  serial " + serial_str(serial) + "  firmware v" + ver_str(firmware) + "  region " + std::to_string(region) +
                    "  extended=" + (extended ? "True" : "False");
    char b[96];
    for (auto& [n, bd] : boards) {
        if (bd.ok && bd.rev) { std::snprintf(b, sizeof b, "\n  %-5s present  rev 0x%08X  sn 0x%08X", n.c_str(), *bd.rev, bd.sn.value_or(0)); s += b; }
        else if (bd.ok) s += "\n  " + n + " present";
        else { std::snprintf(b, sizeof b, "\n  %-5s not fitted", n.c_str()); s += b; }
    }
    for (auto& w : warnings) s += "\n  WARNING: " + w;
    return s;
}

Identity identify(Flex& fx, const IdentifyOptions& opt) {
    auto sleep = opt.sleep ? opt.sleep : std::function<void(double)>([](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); });
    auto clock = opt.clock ? opt.clock : std::function<double()>([] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); });
    const double end = clock() + opt.boot_wait_s;
    uint32_t fw = 0;
    for (;;) {
        try { fw = fx.get("GET_FIRMWARE_REV"); if (fw) break; } catch (const TimeoutError&) {}
        if (clock() >= end)
            throw StartupError("no usable reply from the radio (firmware revision 0 / timeout). PowerSDR notes that starting too soon after power-on causes "
                               "this; wait ~15 s after switching the radio on and try again");
        sleep(opt.poll_s);
    }
    Identity id; id.firmware = fw;
    id.model = fx.get("GET_MODEL"); sleep(0.01);
    id.serial = fx.get("GET_SERIAL_NUM"); sleep(0.01);
    auto board = [&](const char* name, const char* ok_op, const char* rev_op, const char* sn_op) {
        Board b; b.ok = fx.get(ok_op) != 0; sleep(0.01);
        if (b.ok) { b.rev = fx.get(rev_op); sleep(0.01); b.sn = fx.get(sn_op); sleep(0.01); }
        id.boards.emplace_back(name, b);
    };
    board("TRX", "GET_TRX_OK", "GET_TRX_REV", "GET_TRX_SN");
    board("PA", "GET_PA_OK", "GET_PA_REV", "GET_PA_SN");
    if (id.model <= 2) {                                          // 5000 family only, as PowerSDR
        board("RFIO", "GET_RFIO_OK", "GET_RFIO_REV", "GET_RFIO_SN");
        board("RX2", "GET_RX2_OK", "GET_RX2_REV", "GET_RX2_SN");
        board("ATU", "GET_ATU_OK", "GET_ATU_REV", "GET_ATU_SN");
        board("VU", "GET_VU_OK", "GET_VU_REV", "GET_VU_SN");
    }
    id.region = fx.get("GET_REGION");
    id.extended = fx.get("GET_STATUS") != 0;

    if (id.model == 3) throw StartupError("this is a FLEX-3000, not a 5000 (model code 3); not supported by this tool");
    if (id.model > 3) id.warnings.push_back("unknown model code " + std::to_string(id.model) + "; treating as a 5000 like PowerSDR does");
    if (fw < kFwMin || fw > kFwMax)
        throw StartupError("firmware v" + ver_str(fw) + " is outside PowerSDR's supported range v" + ver_str(kFwMin) + "..v" + ver_str(kFwMax) +
                           "; update it with the Windows tools (software/ke9ns/F5K_firmware_old.zip, via the Windows VM) before using firesidectl");
    if (!id.has("TRX")) id.warnings.push_back("TRX board reports not OK");
    if (!id.has("PA")) id.warnings.push_back("PA board reports not OK");
    return id;
}
}  // namespace fireside
