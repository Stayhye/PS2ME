/*
 * Copyright  1990-2008 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

/*
 * PS2 OVERLAY (ps2/phoneme/...): the only change from the stock javaTask.c is that the
 * per-run teardown at the bottom of the outer loop calls midpFinalize() instead of the
 * bare midpFinalizeMemory().
 *
 * Why: the PS2 launcher runs one game per JavaTask() call and loops (native menu ->
 * game -> menu -> game ...), so JavaTask() is re-entered once per game in a single
 * process. Our MIDP is built with the slave-mode event subsystem and runs the VM via
 * midpRunVm()/JVM_Start(), so the stock runMidlet() SKIPS its own midpFinalize(); the
 * per-run teardown here is what closes out each cycle. The bare midpFinalizeMemory()
 * only frees the memory pool, leaving the MIDP init level and subsystem finalizers
 * unrun; midpFinalize() is the complete, level-aware teardown -- it runs the VM-level
 * finalizer (FinalizeEvents() -> gsEventQueues = NULL, storage/config finalize), frees
 * the pool via its own midpFinalizeMemory(), and resets initLevel to NO_INIT, mirroring
 * exactly what the stock NON-slave runMidlet() does per run so the next game re-inits
 * everything cleanly. (The javacall property store self-heals on next access.)
 *
 * NOTE: the multi-game heap corruption originally chased here turned out to be a
 * separate bug -- the rmfs backing-store use-after-free in pcsl_rmfs.c, fixed by the
 * patch-phoneme.sh marker ps2-ramfs-finalize-null. This full teardown is retained as
 * the correct per-cycle teardown regardless.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <midp_logging.h>
#include <midpAMS.h>
#include <suitestore_common.h>
#include <midpMalloc.h>
#include <jvm.h>
#include <jvmspi.h>
#include <findMidlet.h>
#include <midpUtilKni.h>
#include <midp_jc_event_defs.h>
#include <midp_properties_port.h>
#include <javacall_events.h>
#include <javacall_lifecycle.h>
#include <midpStorage.h>
#include <suitestore_task_manager.h>
#include <commandLineUtil.h>
#include <javaTask.h>
#include <exe_entry_point.h>

static javacall_result midpHandleSetVmArgs(int argc, char* argv[]);
static javacall_result midpHandleSetHeapSize(midp_event_heap_size heap_size);
static javacall_result midpHandleListMIDlets(void);
static javacall_result midpHandleListStorageNames(void);
static javacall_result midpHandleRemoveMIDlet(midp_event_remove_midlet removeMidletEvent);

/*
 * PS2 GAME-SAVE SNAPSHOT (phase 2a -- capture the game's RecordStores to the memory card).
 *
 * A game's RecordStores live as files in the volatile rmfs under /j2me/appdb/, which
 * midpFinalize() frees at the end of each JavaTask() cycle. Right after the game exits and
 * before that teardown, this packs the game's RMS files and writes them to the memory card
 * (one blob per game, keyed by the game's stable name) so the next launch can restore them
 * (restore lands in phase 2b -- the GameLoader shim, before it runs the game).
 *
 * The per-suite RMS files are named <root>/<8-hex-suite-id><store-name>.db; the shared
 * "_icons.dat" AMS cache sits in the same directory but is NOT game data, so we keep only
 * the entries whose name (after the root) begins with a hex digit. The suite id is stable
 * (the id counter lives in the rmfs, which resets every cycle, so a fresh install always
 * gets the same id), so the packed filenames are restored verbatim. Uses the raw rmfs API
 * (char* paths) and the memory-card bridge (ps2mc_*, ps2gs_* in the javacall library).
 */
extern char *rmfsFileStartWith(const char *filename);
extern void  rmfsRewindFileStartWith(void);   /* ps2-rmfs-iter-rewind: reset the cursor */
extern int   rmfsOpen(const char *filename, int flags, int creationMode);
extern int   rmfsClose(int fd);
extern int   rmfsRead(int fd, void *buffer, int size);
extern int   rmfsFileSize(int fd);
extern int   rmfsGetUsedSpace(void);
extern int   rmfsGetFreeSpace(void);

/* Memory-card bridge (implemented in the javacall library, Ps2GameSaveBridge.cpp). The
 * blob lands as "/PS2ME_<game>/rms.dat" -- the game's own top-level card save. */
extern int ps2mc_available(void);
extern int ps2gs_write_save(int gameIndex, const void *blob, int len);
/* The game index the native menu picked (GameLoaderKni.cpp). */
extern int ps2_chosen_game;

/* rms.dat layout: [u32 magic 'RMS1'][u32 fileCount] then per file
 * [u32 nameLen][name][u32 dataLen][data]. Same machine writes and reads it, so the u32s
 * are stored in native (little-endian) byte order. */
#define PS2GS_MAGIC 0x31534d52u   /* 'R','M','S','1' little-endian */

static unsigned char ps2gsBlob[256 * 1024];

static void ps2gsPutU32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* A RecordStore backing file, i.e. the only thing that is actual save data. phoneME's RMS
 * names them "<suite-id><store>.db" (and ".idx" for the index module); see rms.c
 * (DB_EXTENSION/IDX_EXTENSION). Games also litter the same /j2me/appdb/ directory with
 * other suite-prefixed files -- decoded-image scratch ("...png.tmp"), the AMS "_icons.dat"
 * cache -- which are NOT progress and would just overflow the blob, so we pack only .db/.idx.
 */
static int ps2gsIsRmsFile(const char *name) {
    int n = (int)strlen(name);
    return (n >= 3 && name[n - 3] == '.' && name[n - 2] == 'd' && name[n - 1] == 'b') ||
           (n >= 4 && name[n - 4] == '.' && name[n - 3] == 'i' &&
            name[n - 2] == 'd' && name[n - 1] == 'x');
}

static void ps2GameSaveSnapshot(void) {
    static char names[64][256];
    const char *prefix = "/j2me/appdb/";
    char *fn;
    int count = 0, k, pos;
    unsigned int fileCount = 0;

    /* Reset the stateful rmfsFileStartWith cursor so this enumeration starts from the first
     * entry (see the ps2-rmfs-iter-rewind patch in patch-phoneme.sh). */
    rmfsRewindFileStartWith();

    /* Pass 1: collect the RMS file names (copy them out -- the returned pointer aims into
     * the rmfs name table, which we must not hold across other rmfs calls). Filter to
     * .db/.idx HERE so the game's image scratch never crowds the 64 slots (a game can leave
     * hundreds of "...png.tmp" files under appdb/, ahead of the real save in enumeration). */
    while ((fn = rmfsFileStartWith(prefix)) != NULL && count < 64) {
        int i;
        if (!ps2gsIsRmsFile(fn)) {
            continue;
        }
        for (i = 0; fn[i] != '\0' && i < 255; i++) {
            names[count][i] = fn[i];
        }
        names[count][i] = '\0';
        count++;
    }

    printf("[gamesave] snapshot: %d RecordStore file(s) under %s, rmfs used=%d free=%d\n",
           count, prefix, rmfsGetUsedSpace(), rmfsGetFreeSpace());

    /* Pass 2: pack the game's RMS files (header first, then each kept entry). */
    pos = 8;   /* leave room for magic + file count */
    for (k = 0; k < count; k++) {
        const char *slash = strrchr(names[k], '/');
        const char *base = slash ? slash + 1 : names[k];
        int fd, sz, got, nlen;
        if (!isxdigit((unsigned char)base[0])) {
            continue;                   /* skip _icons.dat and other non-suite files */
        }
        fd = rmfsOpen(names[k], 0, 0);
        if (fd < 0) {
            continue;
        }
        sz = rmfsFileSize(fd);
        nlen = (int)strlen(names[k]);
        if (sz < 0 || pos + 8 + nlen + sz > (int)sizeof(ps2gsBlob)) {
            printf("[gamesave] blob full / bad size, skip %s (%d bytes)\n", names[k], sz);
            rmfsClose(fd);
            continue;
        }
        ps2gsPutU32(ps2gsBlob + pos, (unsigned int)nlen);   pos += 4;
        memcpy(ps2gsBlob + pos, names[k], (size_t)nlen);    pos += nlen;
        ps2gsPutU32(ps2gsBlob + pos, (unsigned int)sz);     pos += 4;
        got = 0;
        while (got < sz) {
            int r = rmfsRead(fd, ps2gsBlob + pos + got, sz - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        rmfsClose(fd);
        pos += sz;
        fileCount++;
        printf("[gamesave]   packed %s (%d bytes)\n", names[k], sz);
    }

    if (fileCount == 0) {
        printf("[gamesave] game %d wrote no RecordStore -- nothing to save\n", ps2_chosen_game);
        return;
    }
    ps2gsPutU32(ps2gsBlob + 0, PS2GS_MAGIC);
    ps2gsPutU32(ps2gsBlob + 4, fileCount);

    if (!ps2mc_available()) {
        printf("[gamesave] no memory card -- game %d save (%d bytes) NOT written\n",
               ps2_chosen_game, pos);
        return;
    }
    printf("[gamesave] writing game %d save: %u file(s), %d bytes -> %s\n",
           ps2_chosen_game, fileCount, pos,
           ps2gs_write_save(ps2_chosen_game, ps2gsBlob, pos) == 0 ? "OK" : "FAIL");
}

/**
 * An entry point of a thread devoted to run java
 */
void JavaTask(void) {
    midp_jc_event_union event;
    javacall_bool res = JAVACALL_OK;
    javacall_bool JavaTaskIsGoOn = JAVACALL_TRUE;
    long timeTowaitInMillisec = -1;
    int outEventLen;
    int main_memory_chunk_size;

    /* Get java heap memory size */
    main_memory_chunk_size = getInternalPropertyInt("MAIN_MEMORY_CHUNK_SIZE");
    if (main_memory_chunk_size == 0) {
	main_memory_chunk_size = -1;
    }

    /* Outer Event Loop */
    while (JavaTaskIsGoOn) {

        if (midpInitializeMemory(main_memory_chunk_size) != 0) {
            REPORT_CRIT(LC_CORE,"JavaTask() >> midpInitializeMemory()  Not enough memory.\n");
            break;
        }
        REPORT_INFO(LC_CORE,"JavaTask() >> memory initialized.\n");

#if !ENABLE_CDC
        res = javacall_event_receive(timeTowaitInMillisec,
            (unsigned char *)&event, sizeof(midp_jc_event_union), &outEventLen);
#else
        res = javacall_event_receive_cvm(MIDP_EVENT_QUEUE_ID,
            (unsigned char *)&event, sizeof(midp_jc_event_union), &outEventLen);
#endif

        if (!JAVACALL_SUCCEEDED(res)) {
            REPORT_ERROR(LC_CORE,"JavaTask() >> Error javacall_event_receive()\n");
            continue;
        }

        switch (event.eventType) {
        case MIDP_JC_EVENT_START_ARBITRARY_ARG:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_START_ARBITRARY_ARG >>\n");
            javacall_lifecycle_state_changed(JAVACALL_LIFECYCLE_MIDLET_STARTED,
                                             JAVACALL_OK,
                                             NULL);
            JavaTaskImpl(event.data.startMidletArbitraryArgEvent.argc,
                         event.data.startMidletArbitraryArgEvent.argv);

            /* PS2: the game has now run and exited, but its RecordStores are still in the
             * rmfs (midpFinalize() below frees them). Snapshot them to the memory card
             * before that -- see ps2GameSaveSnapshot(). */
            ps2GameSaveSnapshot();

            JavaTaskIsGoOn = JAVACALL_FALSE;
            break;

        case MIDP_JC_EVENT_SET_VM_ARGS:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_SET_VM_ARGS >>\n");
            midpHandleSetVmArgs(event.data.startMidletArbitraryArgEvent.argc,
                                event.data.startMidletArbitraryArgEvent.argv);
            break;

        case MIDP_JC_EVENT_SET_HEAP_SIZE:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_SET_HEAP_SIZE >>\n");
            midpHandleSetHeapSize(event.data.heap_size);
            break;

        case MIDP_JC_EVENT_LIST_MIDLETS:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_LIST_MIDLETS >>\n");
            midpHandleListMIDlets();
            JavaTaskIsGoOn = JAVACALL_FALSE;
            break;

        case MIDP_JC_EVENT_LIST_STORAGE_NAMES:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_LIST_STORAGE_NAMES >>\n");
            midpHandleListStorageNames();
            JavaTaskIsGoOn = JAVACALL_FALSE;
            break;

        case MIDP_JC_EVENT_REMOVE_MIDLET:
            REPORT_INFO(LC_CORE, "JavaTask() MIDP_JC_EVENT_REMOVE_MIDLET >>\n");
            midpHandleRemoveMIDlet(event.data.removeMidletEvent);
            JavaTaskIsGoOn = JAVACALL_FALSE;
            break;


        case MIDP_JC_EVENT_END:
            REPORT_INFO(LC_CORE,"JavaTask() >> MIDP_JC_EVENT_END\n");
            JavaTaskIsGoOn = JAVACALL_FALSE;
            break;

        default:
            REPORT_ERROR(LC_CORE,"Unknown event.\n");
            break;

        } /* end of switch */

        /*
         * PS2 OVERLAY CHANGE: full MIDP teardown between runs (not just the memory
         * pool). See the file header for the rationale -- this resets initLevel and
         * every subsystem static (gsEventQueues, ...) so re-entering JavaTask() for the
         * next game re-initializes cleanly instead of dereferencing a dangling pointer
         * into the freed pcsl_mem pool. midpFinalize() frees the pool itself.
         */
        midpFinalize();

    }   /* end of while 'JavaTaskIsGoOn' */

} /* end of JavaTask */

/**
 *
 */
static javacall_result midpHandleSetVmArgs(int argc, char* argv[]) {
    int used;

    while ((used = JVM_ParseOneArg(argc, argv)) > 0) {
        argc -= used;
        argv += used;
    }
    return JAVACALL_OK;
}

/**
 *
 */
static javacall_result midpHandleSetHeapSize(midp_event_heap_size heap_size) {
    JVM_SetConfig(JVM_CONFIG_HEAP_CAPACITY, heap_size.heap_size);
    JVM_SetConfig(JVM_CONFIG_HEAP_MINIMUM, heap_size.heap_size);
    return JAVACALL_OK;
}

/**
 *
 */
static javacall_result midpHandleListMIDlets() {
    char *argv[3];
    int argc = 0;
    javacall_result res;

    argv[argc++] = "runMidlet";
    argv[argc++] = "internal";
    argv[argc++] = "com.sun.midp.scriptutil.SuiteLister";

    res = JavaTaskImpl(argc, argv);
    return res;
}

/**
 *
 */
static javacall_result midpHandleListStorageNames() {
    char *argv[3];
    int argc = 0;
    javacall_result res;

    argv[argc++] = "runMidlet";
    argv[argc++] = "internal";
    /**
     * IMPL_NOTE: introduce an argument for SuiteLister allowing to print
     *            paths to jars only.
     */
    argv[argc++] = "com.sun.midp.scriptutil.SuiteLister";

    res = JavaTaskImpl(argc, argv);
    return res;
}

/**
 *
 */
static javacall_result
midpHandleRemoveMIDlet(midp_event_remove_midlet removeMidletEvent) {
    char *argv[4];
    int argc = 0;
    javacall_result res;

    argv[argc++] = "runMidlet";
    argv[argc++] = "internal";
    argv[argc++] = "com.sun.midp.scriptutil.SuiteRemover";
    argv[argc++] = removeMidletEvent.suiteID;

    res = JavaTaskImpl(argc, argv);
    return res;
}
