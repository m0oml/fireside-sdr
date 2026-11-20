#include "fireside/nr2.h"
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef FIRESIDE_HAVE_NR2
extern "C" {   // the three entry points of third_party/wdsp-nr2/src/emnr.c that we use (its header needs WDSP's own types)
struct _emnr;
struct _emnr* create_emnr(int run, int position, int size, double* in, double* out, int fsize, int ovrlp, int rate, int wintype, double gain, int gain_method, int npe_method, int ae_run);
void xemnr(struct _emnr* a, int pos);
void destroy_emnr(struct _emnr* a);
void fireside_emnr_post2(struct _emnr* a, int run, double factor, double nlevel);   // third_party/wdsp-nr2/src/fireside_emnr.c
}
#endif

namespace fireside::dsp {
#ifdef FIRESIDE_HAVE_NR2
namespace {
constexpr int kBlock = 128;                              // samples per call into EMNR (2.7 ms at 48 kHz); WDSP's own defaults for the rest
constexpr int kFft = 4096, kOverlap = 4;
std::mutex g_planner;                                    // FFTW's planner (plan creation / destruction) is not thread-safe
}
struct SpectralNr::Impl {
    struct _emnr* e = nullptr; std::vector<double> in, out; int pos = 0; double rate;
    explicit Impl(double r) : in(2 * kBlock, 0.0), out(2 * kBlock, 0.0), rate(r) {
        std::lock_guard<std::mutex> l(g_planner);
        e = create_emnr(1, 0, kBlock, in.data(), out.data(), kFft, kOverlap, int(r + 0.5), 0 /*Hann*/, 1.0, 2 /*trained gain*/, 0 /*OSMS noise estimate*/, 1 /*artifact elimination*/);
        if (!e) throw std::runtime_error("NR2: out of memory");
        fireside_emnr_post2(e, 1, 0.15, 0.15);                 // post-processing on, WDSP's own factor / level: +3..4 dB more output SNR than without (HANDOFF 4g)
    }
    ~Impl() { std::lock_guard<std::mutex> l(g_planner); if (e) destroy_emnr(e); }
};
bool spectral_nr_available() { return true; }
void enable_spectral_nr() { set_spectral_nr_factory([](double rate) -> std::unique_ptr<AudioStage> { return std::make_unique<SpectralNr>(rate); }); }
SpectralNr::SpectralNr(double rate) : p_(new Impl(rate)) {}
SpectralNr::~SpectralNr() = default;
float SpectralNr::process(float x) {
    Impl& s = *p_; const float y = float(s.out[size_t(2 * s.pos)]);   // the previous block's result, one block late
    s.in[size_t(2 * s.pos)] = double(x); s.in[size_t(2 * s.pos + 1)] = 0.0;
    if (++s.pos == kBlock) { xemnr(s.e, 0); s.pos = 0; }
    return y;
}
double SpectralNr::delay_s() const { return (double(kBlock) + 0.75 * kFft) / p_->rate; }   // measured: ~64 ms of its own plus the block
#else
struct SpectralNr::Impl {};
bool spectral_nr_available() { return false; }
void enable_spectral_nr() {}
SpectralNr::SpectralNr(double) { throw std::runtime_error("NR2 is not built in (libfftw3 was not found)"); }
SpectralNr::~SpectralNr() = default;
float SpectralNr::process(float x) { return x; }
double SpectralNr::delay_s() const { return 0; }
#endif
}  // namespace fireside::dsp
