// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_random_get_seed: thin shim over hal::RandomSource.
#include "javacall_random.h"   // phoneME contract header (provided via -I at build)

#include "../hal/RandomSource.hpp"

extern "C" {

javacall_result javacall_random_get_seed(unsigned char* outbuf, int bufsize) {
    return ps2::hal::RandomSource::instance().getSeed(outbuf, bufsize);
}

} // extern "C"
