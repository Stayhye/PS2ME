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

// The game index the standalone native front-end (Ps2Frontend, driven from main()
// before the VM starts) chose. main() writes it; GameLoader.chosenGame() reads it.
// The menu is drawn entirely in C -- the VM only ever receives the final choice.
int ps2_chosen_game = -1;

// Defined in libjvm (midp/.../suitestore/internal_api/.../suitestore_locks.c). The
// native suite lock list is a C static that outlives a VM restart (it lives in the
// pcsl_mem pool alongside the rmfs, and midpFinalize is not called between the SVM
// loop's VM cycles). A stored suite is locked while it runs; our MIDletSuiteLoader
// .closeSuite() deliberately skips suite.close(), so unlocking is left to a finalizer
// -- which the CLDC VM may not run on teardown. The stale lock then makes the next
// menu cycle's MIDletSuiteStorage.remove() fail with SUITE_LOCKED (midp_remove_suite
// returns SUITE_LOCKED for any non-update lock), so the previous game is never freed
// and the tiny rmfs fills up ("storage full"). remove_storage_lock() drops the node
// outright; it is a no-op if the id is not locked, and safe at menu time because no
// stored suite is running then.
void remove_storage_lock(int suiteId);


// int chosenGame() -> the game index the native front-end already picked (or -1).
// No UI here: the menu ran in C before the VM started; this just returns the result.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_j2meps2_loader_GameLoader_chosenGame) {
    KNI_ReturnInt(ps2_chosen_game);
}

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

// void clearSuiteLock(int id) -> drop a stale native suite storage lock so the suite
// can be removed. See the remove_storage_lock note above.
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_j2meps2_loader_GameLoader_clearSuiteLock) {
    remove_storage_lock(KNI_GetParameterAsInt(1));
    KNI_ReturnVoid();
}

} // extern "C"
