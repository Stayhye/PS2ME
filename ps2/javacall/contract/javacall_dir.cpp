// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_dir_* path queries used by MIDP to locate its storage.
// Thin shims over hal::Storage. The directory-iteration and free-space parts of
// javacall_dir.h are not referenced by the B1 boot path and are deferred to the
// storage milestone.
#include "javacall_dir.h"   // phoneME contract header (provided via -I at build)

#include "../hal/Storage.hpp"

extern "C" {

javacall_result javacall_dir_get_root_path(javacall_utf16* rootPath, int* rootPathLen) {
    return ps2::hal::Storage::instance().rootPath(rootPath, rootPathLen);
}

javacall_result javacall_dir_get_config_path(javacall_utf16* configPath, int* configPathLen) {
    return ps2::hal::Storage::instance().configPath(configPath, configPathLen);
}

} // extern "C"
