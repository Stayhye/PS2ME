// PS2 JavaCall port — HAL layer. GameStorage singleton.
#include "GameStorage.hpp"

namespace ps2 {
namespace hal {

GameStorage& GameStorage::instance() {
    static GameStorage inst;
    return inst;
}

} // namespace hal
} // namespace ps2
