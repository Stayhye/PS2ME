// PS2 JavaCall port — HAL layer.
//
// RandomSource: supplies the seed bytes javacall_random_get_seed hands to the
// VM's PRNG. The PS2 has no hardware RNG, so entropy is derived from the monotonic
// clock counter mixed through a linear congruential generator. This is adequate to
// seed java.util.Random for game logic; it is NOT cryptographic strength (which
// the MIDP security stack does not require on this target).
#ifndef PS2_JAVACALL_HAL_RANDOMSOURCE_HPP
#define PS2_JAVACALL_HAL_RANDOMSOURCE_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class RandomSource {
public:
    static RandomSource& instance();

    /// javacall_random_get_seed: fill @p outbuf with @p bufsize pseudo-random
    /// bytes. Returns JAVACALL_OK, or JAVACALL_FAIL on bad arguments.
    javacall_result getSeed(unsigned char* outbuf, int bufsize);

private:
    RandomSource() : state_(0) {}
    RandomSource(const RandomSource&);
    RandomSource& operator=(const RandomSource&);

    javacall_uint64 state_;   // LCG state, lazily seeded from the clock
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_RANDOMSOURCE_HPP
