// PS2 JavaCall port — KNI bridge for com.j2meps2.loader.GameLoader.
//
// Exposes the game-storage HAL to the bootstrap MIDlet as native methods. The MIDP
// NativesTableGen scans the romized classes.zip, finds GameLoader's `native`
// methods, and emits nativeFunctionTable.cpp entries referencing these C symbols by
// their JNI-mangled name (Java_<pkg>_<class>_<method>); the linker resolves them
// from this object -- no manual registration. Because our romize-midlet.sh injects
// GameLoader into classes.zip before that table is regenerated, the entries appear
// automatically.
//
// Marshalling is int + byte[] only, copied with the KNI region helpers, so only
// <kni.h> is needed from the VM side (no SNI raw pointers / ROMStructs).
#include <kni.h>

#include "../javacall/hal/GameStorage.hpp"

extern "C" {

// int listGames();
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_j2meps2_loader_GameLoader_listGames) {
    KNI_ReturnInt(ps2::hal::GameStorage::instance().list());
}

// int gameName(int index, byte[] buf) -> length written into buf
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_j2meps2_loader_GameLoader_gameName) {
    const int index = KNI_GetParameterAsInt(1);
    const char* name = ps2::hal::GameStorage::instance().nameAt(index);
    int len = 0;

    KNI_StartHandles(1);
    KNI_DeclareHandle(buf);
    KNI_GetParameterAsObject(2, buf);
    if (name != 0) {
        while (name[len] != '\0') {
            len++;
        }
        const int cap = KNI_GetArrayLength(buf);
        if (len > cap) {
            len = cap;
        }
        KNI_SetRawArrayRegion(buf, 0, len, (const jbyte*)name);
    }
    KNI_EndHandles();
    KNI_ReturnInt(len);
}

// int openGame(int index) -> size in bytes, or -1
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_j2meps2_loader_GameLoader_openGame) {
    const int index = KNI_GetParameterAsInt(1);
    KNI_ReturnInt(ps2::hal::GameStorage::instance().openAt(index));
}

// int readChunk(byte[] buf, int max) -> bytes read (0 = EOF), or -1
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_j2meps2_loader_GameLoader_readChunk) {
    int max = KNI_GetParameterAsInt(2);
    int got = 0;

    KNI_StartHandles(1);
    KNI_DeclareHandle(buf);
    KNI_GetParameterAsObject(1, buf);
    const int cap = KNI_GetArrayLength(buf);
    if (max > cap) {
        max = cap;
    }
    if (max > 0) {
        // Read into a bounded C scratch, then copy into the Java array (single OS
        // thread of green threads -> not reentrant, static scratch is safe).
        static char scratch[8192];
        if (max > (int)sizeof(scratch)) {
            max = (int)sizeof(scratch);
        }
        got = ps2::hal::GameStorage::instance().read(scratch, max);
        if (got > 0) {
            KNI_SetRawArrayRegion(buf, 0, got, (const jbyte*)scratch);
        }
    }
    KNI_EndHandles();
    KNI_ReturnInt(got);
}

// void closeGame()
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_j2meps2_loader_GameLoader_closeGame) {
    ps2::hal::GameStorage::instance().close();
    KNI_ReturnVoid();
}

} // extern "C"
