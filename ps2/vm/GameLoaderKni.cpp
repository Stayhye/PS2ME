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

#include <string.h>   // memcpy (game-save restore)
#include <stdio.h>    // printf (game-save restore log)

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

// --- PS2 game-save restore ---------------------------------------------------------
// Before the chosen game runs, repopulate its RecordStores in the volatile rmfs from the
// blob the previous run wrote to the memory card (ps2GameSaveSnapshot in javaTask.c does
// the capture; same "RMS1" format). Called from GameLoader.run() after the suite is
// installed (so the rmfs is set up) and before MIDletSuiteUtils.execute() hands off -- the
// game runs in the next SVM cycle on the same rmfs, so files written here are visible to
// it. Verbatim filenames: the suite id is stable across launches (its counter lives in the
// rmfs, which resets each cycle), so the packed "/j2me/appdb//<id>..." names match what the
// game looks up. Raw rmfs writes (libjvm) + the memory-card bridge (Ps2GameSaveBridge.cpp).
int  ps2mc_available(void);
int  ps2gs_read_save(int gameIndex, void* buf, int cap);
int  rmfsOpen(const char* filename, int flags, int creationMode);
int  rmfsClose(int fd);
int  rmfsWrite(int fd, void* buffer, int size);

static unsigned int ps2gsGetU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void ps2GameSaveRestore(void) {
    static unsigned char blob[256 * 1024];
    unsigned int magic, fileCount, i;
    int total, pos, restored = 0;
    const int wmode = 0x01 | 0x40 | 0x200;   // PCSL_FILE_O_WRONLY | O_CREAT | O_TRUNC

    if (!ps2mc_available()) {
        return;
    }
    total = ps2gs_read_save(ps2_chosen_game, blob, (int)sizeof(blob));
    if (total < 8) {
        printf("[gamesave] restore: no save for game %d (%d)\n", ps2_chosen_game, total);
        return;
    }
    magic = ps2gsGetU32(blob);
    fileCount = ps2gsGetU32(blob + 4);
    if (magic != 0x31534d52u) {              // 'RMS1'
        printf("[gamesave] restore: bad magic for game %d\n", ps2_chosen_game);
        return;
    }
    pos = 8;
    for (i = 0; i < fileCount; i++) {
        char name[256];
        unsigned int nlen, dlen;
        int fd, wrote = 0;
        if (pos + 4 > total) break;
        nlen = ps2gsGetU32(blob + pos); pos += 4;
        if (nlen == 0 || nlen > 255 || pos + (int)nlen > total) break;
        memcpy(name, blob + pos, nlen); name[nlen] = '\0'; pos += (int)nlen;
        if (pos + 4 > total) break;
        dlen = ps2gsGetU32(blob + pos); pos += 4;
        if (pos + (int)dlen > total) break;

        fd = rmfsOpen(name, wmode, 0);
        if (fd >= 0) {
            while (wrote < (int)dlen) {
                int w = rmfsWrite(fd, blob + pos + wrote, (int)dlen - wrote);
                if (w <= 0) break;
                wrote += w;
            }
            rmfsClose(fd);
            restored++;
            printf("[gamesave] restore: %s (%u bytes)\n", name, dlen);
        } else {
            printf("[gamesave] restore: open FAILED %s\n", name);
        }
        pos += (int)dlen;
    }
    printf("[gamesave] restored %d/%u file(s) for game %d\n",
           restored, fileCount, ps2_chosen_game);
}


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

// void restoreGameSave() -> repopulate the chosen game's RecordStores in the rmfs from the
// memory card before it runs. No-op when there is no card or no save. See ps2GameSaveRestore.
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_j2meps2_loader_GameLoader_restoreGameSave) {
    ps2GameSaveRestore();
    KNI_ReturnVoid();
}

} // extern "C"
