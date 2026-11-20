#include <cstdlib>
#include <cstring>
#include "test_util.h"
// FIRESIDE_TEST_FILTER=a,b runs only tests whose name contains "a" or "b" (for sanitizer builds and quick loops); unset runs everything.
static bool wanted(const char* name) {
    const char* f = std::getenv("FIRESIDE_TEST_FILTER"); if (!f || !*f) return true;
    std::string s(f); size_t p = 0;
    while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); const std::string part = s.substr(p, q - p); if (!part.empty() && std::strstr(name, part.c_str())) return true; p = q + 1; }
    return false;
}
int main() {
    int failed = 0, ran = 0;
    for (auto& t : registry()) {
        if (!wanted(t.name)) continue;
        ++ran;
        try { t.fn(); std::cout << "[ OK ] " << t.name << "\n"; }
        catch (const std::exception& e) { ++failed; std::cout << "[FAIL] " << t.name << ": " << e.what() << "\n"; }
    }
    std::cout << ran - failed << "/" << ran << " passed\n";
    return failed || ran == 0 ? 1 : 0;
}
