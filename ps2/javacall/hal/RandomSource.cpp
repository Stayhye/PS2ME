// PS2 JavaCall port — HAL layer. RandomSource implementation.
#include "RandomSource.hpp"
#include "SystemClock.hpp"

namespace ps2 {
namespace hal {

namespace {
// Numerical Recipes' 64-bit LCG constants — a fast, decent-quality generator.
const javacall_uint64 LCG_MULT = 6364136223846793005ULL;
const javacall_uint64 LCG_INC  = 1442695040888963407ULL;
} // namespace

RandomSource& RandomSource::instance() {
    static RandomSource inst;
    return inst;
}

javacall_result RandomSource::getSeed(unsigned char* outbuf, int bufsize) {
    if (outbuf == 0 || bufsize < 0) {
        return JAVACALL_FAIL;
    }
    if (state_ == 0) {
        // Seed once from the monotonic counter; fold in a constant so a zero
        // counter still yields a non-degenerate state.
        state_ = static_cast<javacall_uint64>(
                     SystemClock::instance().monotonicCounter())
                 ^ 0x9E3779B97F4A7C15ULL;
    }
    for (int i = 0; i < bufsize; ++i) {
        state_ = state_ * LCG_MULT + LCG_INC;
        // Take a high byte, where the LCG's bits are best mixed.
        outbuf[i] = static_cast<unsigned char>(state_ >> 56);
    }
    return JAVACALL_OK;
}

} // namespace hal
} // namespace ps2
