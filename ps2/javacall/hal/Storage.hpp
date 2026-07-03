// PS2 JavaCall port — HAL layer.
//
// Storage: answers the javacall_dir path queries that tell MIDP where its
// application database (root) and configuration files live. Milestone B1 has no
// real filesystem yet, so these are fixed logical paths; when RMS/JAR loading
// arrives (B4) a real ps2sdk fileio backend fills in the rest of javacall_dir.
#ifndef PS2_JAVACALL_HAL_STORAGE_HPP
#define PS2_JAVACALL_HAL_STORAGE_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class Storage {
public:
    static Storage& instance();

    /// javacall_dir_get_root_path: application storage root.
    javacall_result rootPath(javacall_utf16* out, int* ioLen);

    /// javacall_dir_get_config_path: configuration directory.
    javacall_result configPath(javacall_utf16* out, int* ioLen);

private:
    Storage() {}
    Storage(const Storage&);
    Storage& operator=(const Storage&);

    // Copy an ASCII path into the UTF16 out buffer, honouring *ioLen as the max
    // and updating it to the count written. Returns FAIL if it does not fit.
    static javacall_result fill(const char* ascii, javacall_utf16* out, int* ioLen);
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_STORAGE_HPP
