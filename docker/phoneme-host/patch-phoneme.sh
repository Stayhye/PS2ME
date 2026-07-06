#!/bin/sh
# Idempotent patches to the phoneME source tree so it builds with a modern
# toolchain (GCC 12+). The tree (references/phoneme) is git-ignored, so THIS
# script is the versioned record of every change we make to it. Re-runnable.
#
# Usage: docker/phoneme-host/patch-phoneme.sh [path-to-phoneme]   (default: references/phoneme)
set -e
PHONEME="${1:-references/phoneme}"
test -d "$PHONEME/cldc" || { echo "phoneME tree not found at $PHONEME"; exit 1; }

# 1) CLDC VM: modern GCC is much stricter; the VM's global -Werror turns spurious
#    warnings (maybe-uninitialized, etc.) into hard errors. Disable -Werror.
sed -i 's/^CPP_FLAGS[[:space:]]*+=[[:space:]]*-Werror/# CPP_FLAGS += -Werror  (disabled for modern gcc)/' \
    "$PHONEME/cldc/build/share/jvm.make"

# 2) CLDC VM: the build forces -fstrict-aliasing. Modern GCC's aliasing optimizer is
#    far more aggressive than 2009's and miscompiles the VM's heavy type-punning,
#    crashing the romizer (romgen SIGSEGV at addr 0x50 while generating ROMImage.cpp).
#    Force -fno-strict-aliasing.
sed -i 's/CPP_DEF_FLAGS[[:space:]]*+=[[:space:]]*-fstrict-aliasing/CPP_DEF_FLAGS += -fno-strict-aliasing/' \
    "$PHONEME/cldc/build/share/jvm.make"

# 3) THE romizer fix: romgen SIGSEGVs deterministically at addr 0x50 during class
#    romization when built at -O2 with modern gcc (the 2009 VM has undefined
#    behaviour that gcc 12's optimizer exploits). -O0 makes romgen run correctly
#    (confirmed: "Loading classes...Done!"). Build the VM at -O0.
#    NOTE: -O0 also slows the final cldc_vm; narrow this to the minimal -fno-* set
#    (or build only romgen at -O0) once the exact miscompiled pass is identified.
sed -i 's/-O2 /-O0 /g' "$PHONEME/cldc/build/share/jvm.make"

# 4) Large-file support for the 32-bit host tools (romgen/loopgen).
#    romgen is built -m32. On a Docker Desktop (Windows) bind mount the shared
#    files get 64-bit inode numbers, so a 32-bit stat() over the mount fails with
#    EOVERFLOW ("Value too large for defined data type"). romgen resolves the
#    `Include cldcx_rom.cfg` directive via OsFile_exists() -> stat()+S_ISREG(),
#    so it reported "Cannot find included ROM configuration file" even though the
#    file opened fine with fopen(). -D_FILE_OFFSET_BITS=64 makes stat() use the
#    64-bit stat64 path and resolves the include. (fopen already worked, which is
#    why the main -romconfig loaded but the Include failed.)
# NOTE: jvm.make has CRLF line endings, so do NOT anchor on '$' (the line really
# ends in "-DGCC\r"). Insert the flag before -pipe so the trailing \r stays where
# it already is.
grep -q '_FILE_OFFSET_BITS' "$PHONEME/cldc/build/share/jvm.make" || \
  sed -i 's/+= -pipe -DGCC/+= -D_FILE_OFFSET_BITS=64 -pipe -DGCC/' \
      "$PHONEME/cldc/build/share/jvm.make"

# 5) The generated ROMImage.cpp initializes int[] arrays with word values > INT_MAX
#    (e.g. 0xFFFFFFFF = 4294967295). In C++11+ brace-init, that is a narrowing
#    conversion, which modern gcc reports as a hard ERROR. The 2009 code is C++03,
#    where this is fine. -Wno-narrowing restores the old (accepting) behaviour.
#    (Same CRLF caveat: insert before -pipe, not at end of line.)
grep -q 'Wno-narrowing' "$PHONEME/cldc/build/share/jvm.make" || \
  sed -i 's/+= -D_FILE_OFFSET_BITS=64 -pipe -DGCC/+= -D_FILE_OFFSET_BITS=64 -Wno-narrowing -pipe -DGCC/' \
      "$PHONEME/cldc/build/share/jvm.make"

# 6) Modern binutils (2.4x) no longer enable the x87 FPU implicitly under
#    `.arch i486`, so the i386 float-support stubs (AsmStubs_i386.s: fld1/fmul/…,
#    used by the HOST romizer) fail to assemble: "`fld1' is not supported on
#    `i486'". Explicitly enable the 387 coprocessor extension right after the
#    .arch directive. Idempotent. (File is CRLF; the inserted line is LF, which
#    the assembler accepts.)
grep -q '\.arch \.387' "$PHONEME/cldc/src/vm/cpu/c/AsmStubs_i386.s" || \
  sed -i 's/^\.arch i486/.arch i486\n.arch .387/' \
      "$PHONEME/cldc/src/vm/cpu/c/AsmStubs_i386.s"

# 7) PCSL's stub network module declares a `javacall_result` local where it only
#    means an int — it calls no javacall function, just assigns a PCSL return
#    value — so it fails to compile unless the javacall headers happen to be on
#    the include path. Make the stub self-contained by using int. Idempotent
#    (re-running finds nothing to change). Only NETWORK_MODULE=stubs (the PS2
#    build) hits this; the host build uses bsd/generic.
sed -i 's/javacall_result res;/int res;/' \
    "$PHONEME/pcsl/network/stubs/pcsl_network.c"

# 8) MIDP security: `javacall_policy_load.c : policy_files` (midp_permissions/
#    lib.gmk) makes the make treat the checked-in source javacall_policy_load.c as
#    a build-dir target to (re)generate, which defeats the `vpath ... reference/
#    native` lookup -> "javacall_policy_load.c: No such file or directory" at
#    compile. policy_files only copies the runtime _policy/_function_groups files;
#    it does not affect the .c. Make it an order-only prerequisite (| policy_files)
#    so it still runs but the .c is resolved from the source tree. Idempotent.
grep -q 'javacall_policy_load.c : | policy_files' \
    "$PHONEME/midp/src/security/midp_permissions/lib.gmk" || \
  sed -i 's/^javacall_policy_load.c : policy_files/javacall_policy_load.c : | policy_files/' \
      "$PHONEME/midp/src/security/midp_permissions/lib.gmk"

# 9) Chameleon skin: romize it into the image (lfj_image_rom.c) unconditionally.
#    Stock lib.gmk only passes SkinRomizationTool `-romizeall` when
#    USE_FILE_SYSTEM=false; with USE_FILE_SYSTEM=true (our build, for rmfs-backed
#    RMS) the skin description is written only to skin.bin, so the generated
#    lfj_get_skin_description() returns NULL and chameleon falls back to reading
#    skin.bin from storage -- which does not exist in our fresh RAM filesystem, so
#    LCDUI init dies with "IOException while loading skin". Add -romizeall right
#    after the tool invocation is defined so the skin (and images) are embedded in
#    ROM and lfj_get_skin_description() returns real data. Idempotent.
SKIN_GMK="$PHONEME/midp/src/highlevelui/lcdlf/lfjava/lib.gmk"
grep -q 'ps2-always-romize-skin' "$SKIN_GMK" || \
  sed -i "/'com.sun.midp.skinromization.SkinRomizationTool'/a INT_ROMIZE_SKIN += -romizeall  # ps2-always-romize-skin" \
      "$SKIN_GMK"

# (patches #10 and #11 were temporary AMS-boot diagnostics -- Class.java getName()
#  in the "Static initializer" message, and -RenameNonPublicROMClasses to keep real
#  ROM class names -- used to identify MIDletCustomItem as the culprit. Removed once
#  the culprit was fixed by #12/#13. See git history / midp-port-map memory.)

# 12) AMS icons fix, part A (romize the AppManager icons even with a file system).
#     MIDletCustomItem.<clinit> does `ICON_BG = getImageFromInternalStorage(
#     "_ch_hilight_bg")` then `bgIconW = ICON_BG.getWidth()`. With USE_FILE_SYSTEM=
#     true the phoneME design expects those PNGs to be *installed on disk*: the
#     appmanager_ui_resources/lib.gmk only romizes them (ams_resources_rom.c, via
#     com.sun.midp.romization.Romizer) in its `else` (FS=off) branch; the FS=on
#     branch merely copies the PNGs into $(STORAGEDIR). Our rmfs is empty at boot, so
#     ICON_BG comes back null -> NPE in the static initializer. Force the romizing
#     branch to run regardless of USE_FILE_SYSTEM by neutralizing the `ifeq` guard,
#     so ams_resources_rom.o (the icon table) is compiled into libmidp. RMS still
#     lives in the rmfs; only the read-only AMS icons become ROM-embedded. Idempotent.
RES_GMK="$PHONEME/midp/src/ams/appmanager_ui_resources/lib.gmk"
grep -q 'ps2-force-romize-ams' "$RES_GMK" || \
  sed -i 's/^ifeq ($(USE_FILE_SYSTEM), true)/ifeq (true,false) # ps2-force-romize-ams (was USE_FILE_SYSTEM: romize AMS icons even with FS on)/' \
      "$RES_GMK"

# 13) AMS icons fix, part B (let the native resource loader serve the romized icons
#     even with a file system). resource_handler_kni.c hard-codes
#     loadRomizedResource0() to return NULL whenever ENABLE_FILE_SYSTEM is on
#     (assuming icons come from files), and only #includes resources_rom.h / calls
#     ams_get_resource() when FS is off. Drop ENABLE_FILE_SYSTEM from both guards so
#     the romized-lookup path is compiled whenever the native AMS is off (our case:
#     USE_NATIVE_APP_MANAGER=false). ResourceHandler.getResourceImpl already falls
#     back to the file system if the native returns null, so nothing regresses.
#     Idempotent (patterns vanish after the first pass).
KNI_C="$PHONEME/midp/src/core/resource_handler/file_based/native/resource_handler_kni.c"
grep -q 'ps2-romized-icons-with-fs' "$KNI_C" || { \
  sed -i 's@^#if !ENABLE_FILE_SYSTEM && !ENABLE_NATIVE_APP_MANAGER@#if !ENABLE_NATIVE_APP_MANAGER /* ps2-romized-icons-with-fs (was: \&\& !ENABLE_FILE_SYSTEM) */@' \
      "$KNI_C"; \
  sed -i 's@^#if ENABLE_FILE_SYSTEM || ENABLE_NATIVE_APP_MANAGER@#if ENABLE_NATIVE_APP_MANAGER /* ps2-romized-icons-with-fs (was: ENABLE_FILE_SYSTEM or NATIVE_APP_MANAGER) */@' \
      "$KNI_C"; \
}

# 14) j2me-ps2: show our romized demo Canvas MIDlet in the AppManager list.
#     AppManagerPeer.updateContent() lists getListOfSuites() (empty rmfs -> none)
#     plus the hardcoded internal MIDlets (installer/CA/component/ODT). Add
#     HelloCanvas as one more internal MIDlet (INTERNAL_SUITE_ID) so it appears in
#     the list and launches through the UI (launchMidlet -> Manager.launchSuite ->
#     MIDletSuiteUtils.execute(INTERNAL_SUITE_ID, class)). Inserted right before the
#     caManagerIncluded block, mirroring the DISCOVERY_APP pattern. Java is
#     whitespace-insensitive so the inserted lines need no indentation. Milestone
#     B3.2. Idempotent (guarded by the marker comment).
APM="$PHONEME/midp/src/ams/appmanager_base/reference/classes/com/sun/midp/appmanager/AppManagerPeer.java"
grep -q 'ps2-demo-canvas-in-list' "$APM" || \
  sed -i '/if (caManagerIncluded) {/i\
// j2me-ps2 (ps2-demo-canvas-in-list): our romized demo Canvas MIDlet, shown as an\
// internal suite so it appears in and launches from the AppManager list (B3.2).\
if (null == findInternalMidletRmsi("com.j2meps2.demo.HelloCanvas")) {\
msi = new RunningMIDletSuiteInfo(MIDletSuite.INTERNAL_SUITE_ID,\
"com.j2meps2.demo.HelloCanvas", "Hello Canvas (PS2)", true);\
append(msi);\
}' "$APM"

# 15) j2me-ps2: launch generic internal MIDlets from the AppManager (B3.2).
#     AppManagerUIImpl.setupDefaultCommand() only knows the 4 system internal
#     MIDlets (installer/CA/component/ODT); any other internal MIDlet falls into
#     the "This should never happen" else and gets infoCmd as its default select
#     command. Selecting our HelloCanvas then opened AppInfo, whose <init> calls
#     MIDletSuiteStorage.getMIDletSuite(INTERNAL_SUITE_ID) -> reads a .ss settings
#     file for suite FFFFFFFF that does not exist in the rmfs -> IOException. Give
#     that fallback openCmd instead, so select -> enterSuite -> launchMidlet ->
#     MIDletSuiteUtils.execute(INTERNAL_SUITE_ID, class) (InternalMIDletSuiteImpl,
#     no storage read). Anchored on the comment, then advance one line to the
#     setDefaultCommand. Idempotent (guarded by the inline marker).
APMUI="$PHONEME/midp/src/ams/appmanager_ui/reference/classes/com/sun/midp/appmanager/AppManagerUIImpl.java"
grep -q 'ps2-demo-launch-cmd' "$APMUI" || \
  sed -i '/internal applications must be listed above/{n;s@mci.setDefaultCommand(infoCmd);@mci.setDefaultCommand(openCmd); // ps2-demo-launch-cmd (launch generic internal MIDlets like HelloCanvas)@;}' \
      "$APMUI"

# 16) j2me-ps2: mark our demo suite as single-MIDlet so enterSuite() launches it.
#     The RunningMIDletSuiteInfo(int,String,String,boolean) constructor leaves
#     numberOfMidlets=0, so hasSingleMidlet() is false and enterSuite() would call
#     showMidletSelector() (which also reads getMIDletSuite() -> same IOException).
#     Set numberOfMidlets=1 on our msi so enterSuite() -> launchMidlet() directly.
#     Separate marker from #14 so it also applies to an already-#14-patched tree.
grep -q 'ps2-demo-single-midlet' "$APM" || \
  sed -i '/"Hello Canvas (PS2)", true);/a\
msi.numberOfMidlets = 1; // ps2-demo-single-midlet (hasSingleMidlet -> launchMidlet, not AppInfo)' \
      "$APM"

# 17) j2me-ps2: size the RMFS (in-memory storage) to 4M. EE RAM is 32M; with
#     MEMORY_MODULE=malloc the Java heap pool, all MIDP structures AND the rmfs share
#     the single newlib heap (~28M usable above the ELF, below the stack). The rmfs
#     comes straight out of that, so it trades directly against the Java pool -- 4M
#     rmfs + 12M pool leaves a wide margin, whereas an 8M rmfs pushed the heap into
#     the stack and corrupted it (crash in _free_r). Normalized (regex matches any
#     prior value) so re-runs converge whatever the tree currently holds.
RMFS_C="$PHONEME/pcsl/file/ram/pcsl_rmfs.c"
sed -i -E 's/DEFAULT_RAMFS_SIZE  = [0-9*]+;.*/DEFAULT_RAMFS_SIZE  = 4*1024*1024; \/* ps2: 4M (fits the 32M EE RAM budget) *\//' \
    "$RMFS_C"

# 18) j2me-ps2: raise the RMFS max block count from 40 to 256. rmfsDataBlockHdrArray
#     is a fixed 40-entry table; each stored file takes >=1 block and the best-fit
#     allocator does not coalesce freed blocks, so a handful of installed suites
#     (jar+ap+ss+ii+rms each) exhausts the 40 slots and rmfsAllocFileMemory returns
#     -1 ("storage full") well before the 8M is used. 256 entries (~6KB of .bss)
#     removes that ceiling.
RMFS_ALLOC_H="$PHONEME/pcsl/file/ram/rmfsAlloc.h"
grep -q 'MAX_DATABLOCK_NUMBER   256' "$RMFS_ALLOC_H" || \
  sed -i 's/#define   MAX_DATABLOCK_NUMBER   40/#define   MAX_DATABLOCK_NUMBER   256/' \
      "$RMFS_ALLOC_H"

# 19) j2me-ps2: set system.jam_space (JAM storage quota) to 3M in the
#     configuration_xml properties. This is the value the Configurator actually
#     merges into jwc_properties.ini -- a per-platform properties.xml is merged
#     AFTER $(JAVACALL_OUTPUT_DIR)/properties.xml, so overriding jam_space in our
#     javacall properties.xml is silently lost (last one wins). The quota gates the
#     installer (Installer.java: suiteSize > getBytesAvailableForFiles -> "storage
#     full"); the stock 1M was exhausted after one game. 3M sits just under the 4M
#     physical rmfs (patch #17) so the clean quota gate fires before a physical rmfs
#     write fails. Normalized (regex) so re-runs converge from any prior value.
for f in "$PHONEME"/midp/src/configuration/configuration_xml/*/properties.xml; do
  [ -f "$f" ] || continue
  sed -i -E 's/Value="(1000000|7000000)"/Value="3000000"/' "$f"
done

# 20) rmfs reclaim fix (ps2-rmfs-iter-reset). midp_remove_suite() frees a suite by
#     iterating its storage-root files (storage_get_next_file_in_iterator) and deleting
#     each one *during* the walk. Our rmfs prefix iterator (searchNameTabStartWith) is
#     STATEFUL: it remembers the last returned name in a file-static `prevFilename` and
#     returns "the next match after it". Deleting that file (delNameTabByID tombstones
#     its name-table entry / moves RmfsNameTableEnd) leaves prevFilename pointing at a
#     stale entry, so the next search can't re-find it to advance and reports "no more"
#     after the FIRST file. Net: remove() drops the suite from _suites.dat (~2 KB) but
#     orphans the ~1 MB jar+metadata in the rmfs, which then fills up after a couple of
#     installs ("storage full"). Reset the iterator on every name-entry delete so the
#     next search restarts from the first remaining match; the delete loop then frees
#     ALL of the suite's files. Plain listing (never deletes mid-walk) is unaffected.
#     Inserted after delNameTabByID's locals (C89-safe). Idempotent.
RMFS_ALLOC_C="$PHONEME/pcsl/file/ram/rmfsAlloc.c"
grep -q 'ps2-rmfs-iter-reset' "$RMFS_ALLOC_C" || \
  sed -i '/entityPos = searchNameTabByID(&filename, identifier);/i\
  prevFilename = NULL; /* ps2-rmfs-iter-reset: a delete invalidates the stateful startWith iterator */' \
      "$RMFS_ALLOC_C"

# 21) rmfs backing-store use-after-free (ps2-ramfs-finalize-null). The RAM filesystem's
#     backing buffer (RamfsMemory, ~4 MB) is malloc'd once by pcsl_file_init() guarded by
#     "if (RamfsMemory == NULL)". pcsl_file_finalize() frees it with pcsl_mem_free() but
#     LEAVES RamfsMemory pointing at the freed block. On the PS2 launcher we run one game
#     per VM cycle and midpFinalize()->storageFinalize()->pcsl_file_finalize() runs at the
#     end of every cycle, so on the SECOND game pcsl_file_init() sees RamfsMemory != NULL,
#     skips the malloc, and hands the DANGLING freed pointer to rmfsInitialize(). The rmfs
#     header re-inits (free space logically resets to full), but the physical buffer is
#     memory newlib has already handed back out (e.g. gsEventQueues lands inside it), so
#     seeding the next JAR (~680 KB of rmfs writes) stomps live heap -> gsEventQueues gets
#     garbage and the newlib free list corrupts (crash in _malloc_r). Null the pointer on
#     finalize so the next init re-allocates a fresh, exclusive buffer. Idempotent (marker).
RMFS_PCSL_C="$PHONEME/pcsl/file/ram/pcsl_rmfs.c"
grep -q 'ps2-ramfs-finalize-null' "$RMFS_PCSL_C" || \
  sed -i '/pcsl_mem_free(RamfsMemory);/a\
        RamfsMemory = NULL; /* ps2-ramfs-finalize-null: else next pcsl_file_init reuses a freed buffer */' \
      "$RMFS_PCSL_C"

echo "phoneME patches applied to: $PHONEME"
