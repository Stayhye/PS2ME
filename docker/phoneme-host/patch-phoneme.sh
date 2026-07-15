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

# 3) THE romizer fix (ROOT CAUSE, pinned with UBSan): the 2009 VM relies on signed-integer
#    overflow wrapping around. During a GC, ObjectHeap::compute_new_object_locations negates
#    INT_MIN and forms 1<<31 (ObjectHeap.cpp:2737) -- signed-overflow UB. Modern gcc's -O2
#    optimizer assumes signed overflow never happens and miscompiles the surrounding code, so
#    romgen SIGSEGVs (at 0x50) mid-romization. The OLD fix was -O0 everywhere, which also
#    crippled the target interpreter (~5x slower -> games ran at ~6 FPS vs ~30 elsewhere).
#    Real fix: keep -O2 and add -fwrapv, which DEFINES signed overflow as two's-complement
#    wraparound -- exactly the semantics the code assumes. UBSan-verified: romgen romizes
#    cleanly at -O2 -fwrapv (was: deterministic SIGSEGV). This restores full -O2 speed on the
#    target VM. (-fno-strict-aliasing from #2 covers the other UB class; both are needed.)
#    NOTE: the phoneME tree is git-ignored and persistent, so a legacy -O0 (from the old blunt
#    patch) may already be baked in -- FORCE the gcc opt flags back to -O2 (idempotent: a no-op
#    on a pristine -O2 tree), then add -fwrapv.
sed -i 's/-O0 $(GCC_WUNINITIALIZED)/-O2 $(GCC_WUNINITIALIZED)/g' "$PHONEME/cldc/build/share/jvm.make"
# -fwrapv: fully idempotent -- strip every existing occurrence first (older patch runs
# whose grep-guard misfired left it duplicated, e.g. "-fwrapv -fwrapv -fwrapv"), then add
# exactly one after -fno-strict-aliasing. Re-running collapses back to a single flag.
sed -i 's/ -fwrapv//g; s/-fno-strict-aliasing/-fno-strict-aliasing -fwrapv/' \
  "$PHONEME/cldc/build/share/jvm.make"

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

# 22) EE unaligned 64-bit field access (ps2-oop-long-align). The R5900's `sd`/`ld`
#     (store/load doubleword) TRAP on any address that is not 8-byte aligned -- unlike
#     x86/ARM there is no hardware fixup. phoneME's C++ field accessors in Oop.hpp
#     (long_field/_put, double_field/_put, ulong_field/_put) have TWO code paths: a
#     safe two-word (2x 32-bit) split guarded by `#if !HOST_LITTLE_ENDIAN` (added for
#     SPARC, another RISC that faults on misaligned 64-bit), and a fast single 64-bit
#     `*addr = value` on little-endian hosts. Java `long`/`double` fields are only
#     4-byte aligned in the object layout, so on the EE the fast path emits a bare `sd`
#     to a 4-aligned address -> "Address store exception" (BadVAddr not 8-aligned, EPC
#     in JVMBasicOop::long_field_put). PCSX2 silently fixes up the misaligned access, so
#     it only bit us on real hardware. Force the safe split path on MIPS by OR-ing
#     `defined(__mips__)` into the guard (defined by the EE cross-gcc, NOT by the i386
#     host romgen, so host tools keep the fast path). The C interpreter is already safe
#     (Interpreter_c.cpp long_from_addr/long_to_addr split by hand). Idempotent (marker).
OOP_HPP="$PHONEME/cldc/src/vm/share/handles/Oop.hpp"
grep -q 'defined(__mips__)' "$OOP_HPP" || \
  sed -i 's/#if !HOST_LITTLE_ENDIAN/#if !HOST_LITTLE_ENDIAN || defined(__mips__)/g' \
      "$OOP_HPP"

# 23) clean-build skin.bin optional (ps2-skin-optional). We pass SkinRomizationTool
#     `-romizeall` (patch above, ps2-always-romize-skin) so the whole skin is baked into
#     the C image ROM (lfj_image_rom.c) and NO skin.bin is emitted -- exactly so LCDUI
#     init never tries to load it from the (fresh, empty) RAM filesystem. But with
#     USE_FILE_SYSTEM=true the copy_themes recipe still unconditionally `cp -f`s
#     $(GENERATED_DIR)/lib/skin.bin into LIBDIR. On an INCREMENTAL build a stale skin.bin
#     from a pre-`-romizeall` run lingered in the volume so the cp found it; on a
#     from-scratch build (after wiping MIDP_OUTPUT_DIR to force a recompile with new
#     flags) it does not exist and `cp` aborts the build ("cannot stat .../skin.bin").
#     Make that copy non-fatal -- the skin is already in the ROM, so a missing skin.bin
#     is expected and harmless. Idempotent (marker).
LCDLF_GMK="$PHONEME/midp/src/highlevelui/lcdlf/lfjava/lib.gmk"
grep -q 'ps2-skin-optional' "$LCDLF_GMK" || \
  sed -i '/cp -f $(SUBSYSTEM_LCDLF_GENERATED_SKIN_BIN_FILE)/ s@$@ 2>/dev/null || true # ps2-skin-optional@' \
      "$LCDLF_GMK"

# 24) EE unaligned wide framebuffer stores (ps2-safe-fill). gxj_putpixel.c's
#     primDrawHorzLine and primDrawFilledRect each have an optimized fill path (the
#     active `#else` branch) that writes the RGB565 framebuffer 8/16 bytes at a time via
#     `*(jlong*)p = lcol` and `*(registers_4*)p = regs` (a 16-byte struct), aligning the
#     pointer to only 4 bytes (`(uint)pPtr & 0x3`). On x86/ARM a 4-aligned 8-byte store is
#     fine; on the R5900 the resulting `sd` traps ("Address store exception", EPC in
#     primDrawHorzLine) because `sd` needs 8-byte alignment and RGB565 runs on odd x are
#     only 2/4-aligned. Each routine also carries a simple, fully-portable fill in a
#     disabled `#if 0` block that stores one 16-bit pixel at a time (always EE-safe). Flip
#     those `#if 0` guards to `#if defined(__mips__)` so the EE takes the safe path while
#     other targets keep the wide-store fast path. Paired with -fno-store-merging
#     -fno-tree-vectorize in ps2_mips_gcc.gmk, which stop gcc from re-widening the simple
#     loop's 16-bit stores back into `sd`. Only two `#if 0` exist in this file (both are
#     these fill toggles). Idempotent (marker).
GXJ_C="$PHONEME/midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_putpixel.c"
grep -q 'ps2-safe-fill' "$GXJ_C" || \
  sed -i 's|^#if 0.*|#if defined(__mips__) /* ps2-safe-fill: EE traps unaligned sd, use 16-bit store path */|' \
      "$GXJ_C"

# 25) rmfs prefix-iterator rewind (ps2-rmfs-iter-rewind). searchNameTabStartWith remembers
#     the last returned name in the file-static prevFilename and returns "the next match
#     after it", so a FRESH enumeration must begin with prevFilename == NULL. The game-save
#     snapshot (javaTask.c) walks /j2me/appdb/ from the start each time; it used to force
#     that by searching a prefix nothing matches (a no-match search nulls prevFilename as a
#     side effect -- correct but obscure). Expose an explicit, self-documenting reset
#     instead: a public rmfsRewindFileStartWith() sitting next to prevFilename in
#     rmfsAlloc.c (where the static is in scope), declared in rmfsApi.h. Idempotent (marker).
RMFS_ALLOC_C="$PHONEME/pcsl/file/ram/rmfsAlloc.c"
grep -q 'ps2-rmfs-iter-rewind' "$RMFS_ALLOC_C" || \
  sed -i '/^jint searchNameTabStartWith(const char \*filename, uchar \*identifier)/i\
/* ps2-rmfs-iter-rewind: reset the stateful prefix iterator so the next\
   rmfsFileStartWith() restarts from the first match (game-save snapshot). */\
void rmfsRewindFileStartWith(void) {\
  prevFilename = NULL;\
}\
' "$RMFS_ALLOC_C"

RMFS_API_H="$PHONEME/pcsl/file/ram/rmfsApi.h"
grep -q 'ps2-rmfs-iter-rewind' "$RMFS_API_H" || \
  sed -i '/char\* rmfsFileStartWith(const char\* filename);/a\
void rmfsRewindFileStartWith(void); /* ps2-rmfs-iter-rewind */' \
      "$RMFS_API_H"

# 26) ps2me-profile: opt-in bytecode-throughput counter for the interpreter perf
#     front (FASE 1). Adds a per-bytecode tick in the C interpreter's dispatch loop,
#     compiled in ONLY under -DPS2ME_PROFILE (jvm.make hook in 26b), and dumps the total
#     to stderr on exit. Off by default -> production builds (PS2 and host) are byte-
#     identical to before. Used by docker/phoneme-cross/build-host-bench.sh to measure
#     interpreter throughput (bytecodes/s) on the linux_c host twin. The C block is
#     inserted verbatim via sed 'r' (a tmpfile) to avoid escaping the fprintf format.
#     Idempotent (guarded by the ps2me_profile_dump marker).
INTERP="$PHONEME/cldc/src/vm/cpu/c/Interpreter_c.cpp"
if ! grep -q 'ps2me_profile_dump' "$INTERP"; then
  TMP_PROF=$(mktemp)
  cat > "$TMP_PROF" <<'PS2ME_PROF_EOF'

  /* ps2me-profile: opt-in bytecode-throughput counter (PS2ME perf front, FASE 1).
     Compiled in only under -DPS2ME_PROFILE (see jvm.make PS2ME_PROFILE hook), so a
     production build carries ZERO overhead. Counts one tick per dispatched bytecode
     in the interpreter loop; the total is dumped to stderr on process exit. Bytecode
     COUNT is deterministic for a fixed .class, so N is measured once with a profile
     build and time T on a clean build -> bytecodes/s = N/T. */
#ifdef PS2ME_PROFILE
#include <stdio.h>
  jlong ps2me_bc_count = 0;
#define PS2ME_BC_TICK() (ps2me_bc_count++)
  __attribute__((destructor)) static void ps2me_profile_dump(void) {
    fprintf(stderr, "[PS2ME_PROFILE] bytecodes=%lld\n", (long long)ps2me_bc_count);
  }
#else
#define PS2ME_BC_TICK() ((void)0)
#endif
PS2ME_PROF_EOF
  sed -i "/static func_t interpreter_dispatch_table\[256+WIDE_OFFSET\];/r $TMP_PROF" "$INTERP"
  rm -f "$TMP_PROF"
  # Prefix the two dispatch-loop calls with the tick (indent preserved).
  sed -i 's/^\( *\)interpreter_dispatch_table\[\*g_jpc\]();/\1PS2ME_BC_TICK(); interpreter_dispatch_table[*g_jpc]();/' \
      "$INTERP"
fi

# 26b) jvm.make hook: compile -DPS2ME_PROFILE when the make var PS2ME_PROFILE=true
#      (mirrors the stock PROFILING/-pg hook). Inert unless the var is set, so normal
#      builds are unaffected. jvm.make is CRLF; the inserted lines are LF (make accepts
#      them) and the anchor has no $ so CRLF does not matter. Idempotent.
JVMMAKE="$PHONEME/cldc/build/share/jvm.make"
grep -q 'PS2ME_PROFILE' "$JVMMAKE" || \
  sed -i '/# We want 32-bit assembly/i\
# ps2me-profile: opt-in bytecode-throughput counter (PS2ME perf front, FASE 1).\
ifeq ($(PS2ME_PROFILE), true)\
CPP_FLAGS              += -DPS2ME_PROFILE\
endif\
' "$JVMMAKE"
# 27) ps2me-threaded: tail-threaded ("direct-threaded") dispatch for the C interpreter
#     (PS2ME perf front, FASE 3). Each bytecode handler ends by tail-calling the next via
#     __attribute__((musttail)), which gcc 15 turns into a direct jump (host: jmp / r5900:
#     jr t9) -- killing the return-to-loop and giving every bytecode its OWN indirect
#     dispatch site (better branch prediction). Compiled in ONLY under -DPS2ME_THREADED
#     (jvm.make hook in 27b); OFF by default -> production builds are byte-identical. The
#     change is large/structural (macros + ~12 delegating handlers + wide +
#     invokenative_return_point), so it ships as a unified .patch applied with `patch`, not
#     sed. It layers ON TOP of #26: the patch's base already carries the profile hunk, so
#     the two compose (the shared BYTECODE_IMPL_END/dispatch region stays consistent). The
#     patch context is LF while the tree may be CRLF, so normalize line endings to LF first
#     (harmless for gcc; #26 ran earlier and preserved whatever was there). Measured: +28.8%
#     on the linux_c host; on real PS2 hardware the gain was modest (loading a bit faster,
#     FPS unchanged -- the r5900's small 16KB I-cache offsets the win, and PCSX2's dynarec
#     does not model the r5900 BTB so it shows no gain there). Kept OFF-by-default as an
#     opt-in toggle. Idempotent (guarded by the ps2me-threaded marker in the applied comment).
INTERP="$PHONEME/cldc/src/vm/cpu/c/Interpreter_c.cpp"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if ! grep -q 'ps2me-threaded' "$INTERP"; then
  sed -i 's/\r$//' "$INTERP"
  # Feed the patch through sed to strip any CRLF the checkout may have introduced, so
  # its context lines match the LF-normalized source regardless of git's autocrlf.
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-threaded-dispatch.patch" | patch -p1 -d "$PHONEME"
fi

# 27b) jvm.make hook: compile -DPS2ME_THREADED when the make var PS2ME_THREADED=true
#      (mirrors the 26b PS2ME_PROFILE hook). Inert unless the var is set, so normal builds
#      are unaffected. Idempotent.
grep -q 'PS2ME_THREADED' "$JVMMAKE" || \
  sed -i '/# We want 32-bit assembly/i\
# ps2me-threaded: opt-in tail-threaded interpreter dispatch (PS2ME perf front, FASE 3).\
ifeq ($(PS2ME_THREADED), true)\
CPP_FLAGS              += -DPS2ME_THREADED\
endif\
' "$JVMMAKE"

# 28) ps2me-drawimage-selfblit (game compat). Stock phoneME's Graphics.render() -- the
#     drawImage() KNI -- rejects drawing an image onto the Graphics obtained from that
#     SAME image (src==dst) by returning false, which the Java layer turns into an
#     IllegalArgumentException. But the MIDP spec only mandates IAE for a bad anchor on
#     drawImage(); the src==dst restriction belongs to drawRegion() (renderRegion, left
#     untouched here). Real handsets and KEmulator treat a self-blit as a no-op, so games
#     that do backbuffer.getGraphics().drawImage(backbuffer,0,0,0) run there but froze on
#     us -- the IAE escaped the game's paint loop and hung the screen right after loading
#     (found in Zombie Infection / GloftMASS via bytecode disassembly). The patch keeps
#     the anchor check and treats src==dst as a successful no-op instead of an error.
#     Applied with --ignore-whitespace (the fix touched a trailing space on a context
#     line) after LF-normalizing both file and patch (the tree may be CRLF). Idempotent
#     (guarded by the 'src==dst' marker the patch introduces).
GXKNI="$PHONEME/midp/src/lowlevelui/graphics_api/gxapi_native/native/gxapi_graphics_kni.c"
if ! grep -q 'src==dst' "$GXKNI"; then
  sed -i 's/\r$//' "$GXKNI"
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-drawimage-selfblit.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 29) ps2me-frame-profile (PS2ME perf front, PASSO 0b). Times the native gxj draw primitives
#     (gx_fill_rect / gx_draw_line / gx_fill_triangle / gx_copy_area / gx_render_image /
#     gx_render_imageregion) so the in-game frame profiler can split (A) interpret from (B)
#     native draw. Each function gets a scoped __attribute__((cleanup)) timer (robust to early
#     returns) that calls our layer's ps2me_prof_now()/ps2me_prof_draw_add(), resolved at the
#     final --start-group link. Compiled in ONLY under -DPS2ME_PROFILE_FRAME (ps2_mips_gcc.gmk
#     hook, gated by the PS2ME_PROFILE_FRAME make/env var) -> production is byte-identical.
#     Ships as a .patch (multi-hunk, 2 files) applied with --ignore-whitespace after LF-
#     normalizing both file and patch (tree may be CRLF). Idempotent (marker guard).
GXJ_GFX="$PHONEME/midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_graphics.c"
GXJ_IMG="$PHONEME/midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_image.c"
IMGJ_FAC="$PHONEME/midp/src/lowlevelui/image/img_putpixel/native/imgj_imagedatafactory_kni.c"
# These 3 files are touched by NO other patch, so reset them to pristine FIRST (phoneME tree is
# a git checkout). This lets an UPDATED profiler patch re-apply cleanly across iterations -- the
# marker guard alone would skip a changed patch on the persistent tree, leaving stale hooks.
# git-based; harmless (|| true) if git/.git is absent (then the marker guard governs). When the
# flag is off the inserted hooks expand to ((void)0) -> production object code is byte-identical.
git -C "$PHONEME" checkout -- \
  midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_graphics.c \
  midp/src/lowlevelui/graphics/gx_putpixel/native/gxj_image.c \
  midp/src/lowlevelui/image/img_putpixel/native/imgj_imagedatafactory_kni.c 2>/dev/null || true
if ! grep -q 'ps2me-frame-profile' "$GXJ_GFX"; then
  sed -i 's/\r$//' "$GXJ_GFX" "$GXJ_IMG" "$IMGJ_FAC"
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-frame-profile.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 30) ps2me-global-reg (PS2ME perf front, FASE 4). Pins the hot interpreter state (g_jfp/g_jsp/
#     g_jpc/g_jlocals) in fixed callee-saved MIPS GPRs (s0-s3, via `register ... asm("$1x")`)
#     instead of static memory, removing the per-bytecode load/stores that were the structural
#     ceiling (see PERF_PLAN 3 / 5.9). Gated by #if defined(__mips__) INSIDE the patch, so only the
#     ps2_mips target is affected (host/i386 builds keep the static globals). Touches only the 4
#     global pointer declarations -- disjoint from #26 (profile counter) and #27 (threaded dispatch).
#     Measured on PCSX2: comp -34/-39%, Zombie 18->23 and caranguejo 29->36 (crossed 20 FPS).
#     HW validation pending. Idempotent (marker guard on the FASE 4 comment).
if ! grep -q 'PS2ME FASE 4' "$INTERP"; then
  sed -i 's/\r$//' "$INTERP"
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-global-reg.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 31) ps2me-jit-vpath-mips (PS2ME JIT, Fase 0). The r5900 dynamic-compiler backend
#     lives in a brand-new src/vm/cpu/mips/ (delivered by the ps2/phoneme overlay).
#     jvm.make hard-codes the per-CPU include dirs and vpaths (arm/c/i386/sh/thumb/
#     thumb2) and does NOT list mips, so with carch=mips (PS2ME_JIT) the make can't
#     find CodeGenerator_mips.cpp et al. Add cpu/mips to both the -I list (after
#     cpu/thumb2) and the vpath list (after cpu/thumb). Harmless when PS2ME_JIT is
#     off: nothing references the _mips files unless carch=mips. jvm.make is CRLF;
#     the inserted lines are LF (make accepts). Idempotent (grep guard).
JVMMAKE="$PHONEME/cldc/build/share/jvm.make"
grep -q 'src/vm/cpu/mips' "$JVMMAKE" || { \
  sed -i 's@\(-I"$(WorkSpace)/src/vm/cpu/thumb2"        \\\)@\1\n  -I"$(WorkSpace)/src/vm/cpu/mips"          \\@' \
      "$JVMMAKE"; \
  sed -i 's@\(vpath $(VPATH_PATTERNS) $(WorkSpace)/src/vm/cpu/thumb\)$@\1\nvpath $(VPATH_PATTERNS) $(WorkSpace)/src/vm/cpu/mips@' \
      "$JVMMAKE"; \
}

# 32) ps2me-jit-makedeps-mips (PS2ME JIT, Fase 0). MakeDeps' Database.java keeps its
#     OWN hard-coded per-CPU vpath list (jvm.make even warns to keep them in sync).
#     Add cpu/mips there too so the dependency generator resolves the _mips sources
#     referenced via <carch> in includeDB. The buildtool is recompiled from source
#     each build, so editing the .java suffices. Idempotent (grep guard).
DBJAVA="$PHONEME/cldc/src/tools/buildtool/makedep/Database.java"
grep -q 'cpu/mips' "$DBJAVA" || \
  sed -i 's@\(addVpath(workspace + "/src/vm/cpu/i386");\)@\1\n      addVpath(workspace + "/src/vm/cpu/mips");@' \
      "$DBJAVA"

# 33) ps2me-jit-globaldefs-compiler (PS2ME JIT, Fase 0). GlobalDefinitions_c.hpp
#     (pulled via iarch=c) FORCE-undefs ENABLE_COMPILER to 0 unless CROSS_GENERATOR,
#     on the theory that a C-interpreter build never has a compiler. Our hybrid keeps
#     the C interpreter AND adds a runtime MIPS JIT, so on the EE target (where the
#     cfg defines -DMIPS, host romgen does NOT) that undef must not fire. OR
#     !defined(MIPS) into the guard so the target keeps ENABLE_COMPILER as configured
#     while host passes still force it off. Idempotent (marker).
GDEFS_C="$PHONEME/cldc/src/vm/cpu/c/GlobalDefinitions_c.hpp"
grep -q '!defined(MIPS)' "$GDEFS_C" || \
  sed -i 's/^#if !CROSS_GENERATOR/#if !CROSS_GENERATOR \&\& !defined(MIPS) \/* ps2me-jit: keep runtime JIT on the EE target *\//' \
      "$GDEFS_C"

# 34) ps2me-jit-dormant-fase0 (PS2ME JIT, Fase 0). The r5900 dynamic compiler is now
#     compiled in (PS2ME_JIT), but its CodeGenerator/BinaryAssembler backend is still a
#     bail-out skeleton (SHOULD_NOT_REACH_HERE) -- the link milestone, not a working JIT.
#     UseCompiler defaults true, so the interpreter's hotness path (on_timer_tick /
#     shared_invoke_compiler) would eventually hand a method to that skeleton and abort.
#     Force the compiler dormant at VM init: the hybrid then runs pure interpreter,
#     byte-identical to the non-JIT build, while the backend keeps linking. Gated by
#     ENABLE_COMPILER -> the line is absent in production. Remove (or flip to a runtime
#     opt-in) in Fase 2+, once the backend can actually emit r5900 code. Idempotent
#     (marker). Anchored on the unique JVM::initialize() line _startup_phase_count = 0;.
JVMCPP="$PHONEME/cldc/src/vm/share/runtime/JVM.cpp"
grep -q 'ps2me-jit-dormant' "$JVMCPP" || \
  sed -i 's@\(_startup_phase_count = 0;\)@\1\n#if ENABLE_COMPILER\n  UseCompiler = false; \/* ps2me-jit-dormant (Fase 0): JIT backend is a bail-out skeleton; keep the dynamic compiler off *\/\n#endif@' \
      "$JVMCPP"

# 35) ps2me-jit-vtbitmap-gcroots (PS2ME JIT, Fase 0.5). Two host<->target skews that
#     surface only with ENABLE_COMPILER=1 (the JIT build) and crash the VM even while the
#     compiler is dormant:
#       (a) USE_EMBEDDED_VTABLE_BITMAP = (ENABLE_COMPILER && ENABLE_INLINE). Host romgen
#           runs ENABLE_COMPILER=0 (patch #33) => bitmap=0 and sizes romized class objects
#           WITHOUT the trailer; the target with bitmap=1 then has init9->update_vtable_bitmaps()
#           write past each class object -> boot crash (g_jsp garbage, TLB miss). Force the
#           bitmap OFF on the MIPS target so host and target agree; also gate the two callers
#           that need is_method_overridden()/the out-of-line update_vtable_bitmaps on the flag
#           so the (dormant) compiler code still compiles. No-op on non-MIPS builds.
#       (b) ObjectHeap::roots_do_to() calls Compiler::oops_do() on EVERY GC (unconditionally,
#           under ENABLE_COMPILER). Compiler::oops_do dereferenced _compiler_state, which is
#           NULL whenever no compilation is in progress (always, when dormant) -> the GC feeds
#           NULL+field slots to the mark/update callbacks -> TLB miss at 0x10/0x18/0x50 during
#           the game load. Null-guard it (no suspended state => no compiler roots).
#     Four files, all no-ops in production (ENABLE_COMPILER=0). Applied via .patch (marker).
if ! grep -q 'PS2ME JIT (Fase 0.5)' "$PHONEME/cldc/src/vm/share/compiler/Compiler.hpp"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-vtbitmap-gcroots.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 36) ps2me-jit-selftest (PS2ME JIT, Fase 1). Run the "returns 42" milestone at VM
#     init: emit a real r5900 function (pure encoders in Assembler_mips.hpp),
#     flush the I-cache, call it, verify. The body + PS2ME_JIT_SELFTEST toggle live
#     in the overlay (BinaryAssembler_mips.cpp / ps2_mips.cfg); this only injects the
#     ONE call site into JVM::initialize, right after the Fase-0 dormant switch. Nested
#     inside the existing #if ENABLE_COMPILER block AND gated by defined(PS2ME_JIT_SELFTEST),
#     so it is absent unless the self-test build is requested (and always absent in
#     production, ENABLE_COMPILER=0). Idempotent (marker). Anchored on the unique
#     #34 line so it lands inside that block.
JVMCPP="$PHONEME/cldc/src/vm/share/runtime/JVM.cpp"
grep -q 'ps2me-jit-selftest' "$JVMCPP" || \
  sed -i 's@\(  UseCompiler = false; /\* ps2me-jit-dormant.*/\)@\1\n#if defined(PS2ME_JIT_SELFTEST) \/* ps2me-jit-selftest (Fase 1) *\/\n  { extern void ps2me_jit_selftest(void); ps2me_jit_selftest(); }\n#endif@' \
      "$JVMCPP"

# 37) ps2me-jit-fase2-helpers (PS2ME JIT, Fase 2). interp<->compiled glue in Interpreter_c.cpp:
#     jit_frame_enter() builds the callee Java frame exactly like interpreter_method_entry();
#     jit_return_int()/jit_return_void() push the result and run the interpreter's own
#     return_internal() teardown. The emitted compiled method CALLS these instead of open-coding
#     the frame protocol in r5900 -- they reuse the interpreter macros and share g_jfp/g_jsp via
#     global-reg (correct by construction). Gated ENABLE_COMPILER -> absent in production. The
#     hunk sits near line ~2265 (before fast_ldc), disjoint from #30 (global-reg, top of file),
#     so it applies with an offset after the earlier patches. Idempotent (marker).
INTERPC="$PHONEME/cldc/src/vm/cpu/c/Interpreter_c.cpp"
if ! grep -q 'jit_frame_enter' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase2-helpers.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 38) ps2me-jit-fase2-trigger (PS2ME JIT, Fase 2). Two gated (PS2ME_JIT_FASE2) hooks that
#     drive the Fase-2 milestone; both absent unless the Fase-2 build is requested:
#       (a) call site: invoke_java_method arms the target once (jit_fase2_arm, from #37),
#           inserted right after set_callee_method. Block-scope forward decl (plain C++
#           linkage -- same TU as the definition).
#       (b) OmitLeafMethodFrames = false at VM init: the C-interpreter hybrid's compiled
#           methods always build a full Java frame (the ARM omit shortcut needs the compiled
#           calling convention, which the C return path does not use), so omit must be off
#           to keep the VSF consistent. Injected next to the #34 dormant switch.
#     Idempotent (markers).
grep -q 'jit_fase2_arm(method)' "$INTERPC" || \
  sed -i 's@\(    set_callee_method(method);\)@\1\n#if defined(PS2ME_JIT_FASE2)\n    { void jit_fase2_arm(address); jit_fase2_arm(method); }\n#endif@' \
      "$INTERPC"
grep -q 'OmitLeafMethodFrames = false' "$JVMCPP" || \
  sed -i 's@\(  UseCompiler = false; /\* ps2me-jit-dormant.*/\)@\1\n#if defined(PS2ME_JIT_FASE2)\n  OmitLeafMethodFrames = false; \/* ps2me-jit-fase2: compiled methods build a full frame *\/\n#endif@' \
      "$JVMCPP"

# 38b) ps2me-jit-fase3-fwdbranch (PS2ME JIT, Fase 3). OptimizeForwardBranches is
#      a develop flag (const in the PRODUCT build, so it cannot be set at runtime
#      like OmitLeafMethodFrames). It defaults to USE_OPT_FORWARD_BRANCH and, when
#      on, folds a short forward `if` into if_then_else/if_iinc -- both still
#      bail-out in the mips backend. Force its default to 0 at compile time when
#      the Fase-3 build is requested (PS2ME_JIT_FASE3 is defined in every TU via
#      the cfg's CPP_DEF_FLAGS), so branch_if always lowers through
#      cmp_values + conditional_jump_do (Marco 3.2). OptimizeLoops stays default
#      (Marco 3.2a is forward-branch only, so loop peeling never fires). Idempotent.
BUILDFLAGS="$PHONEME/cldc/src/vm/share/utilities/BuildFlags.hpp"
grep -q 'ps2me-jit-fase3: lower via cmp_values' "$BUILDFLAGS" || \
  perl -0777 -i -pe 's/#define USE_OPT_FORWARD_BRANCH 1\n/#if defined(PS2ME_JIT_FASE3)\n#define USE_OPT_FORWARD_BRANCH 0 \/* ps2me-jit-fase3: lower via cmp_values+conditional_jump_do *\/\n#else\n#define USE_OPT_FORWARD_BRANCH 1\n#endif\n/' \
      "$BUILDFLAGS"

# 39) ps2me-jit-fase2-fired-log (PS2ME JIT, Fase 2). One-shot diagnostic in
#     shared_invoke_compiler(): proves an armed method was re-invoked and the
#     compile path fired (companion to the arm/entered logs). Gated PS2ME_JIT_FASE2,
#     absent in production. Idempotent (marker string). Anchored on the unique
#     shared_invoke_compiler() definition line.
grep -q 'shared_invoke_compiler fired' "$INTERPC" || \
  sed -i 's@\(  void shared_invoke_compiler() {\)@\1\n#if defined(PS2ME_JIT_FASE2)\n    { static bool _f2c = false; if (!_f2c) { _f2c = true; tty->print_cr("[PS2ME-JIT] Fase 2: shared_invoke_compiler fired (re-invoked armed method -> compiling)"); } }\n#endif@' \
      "$INTERPC"

# 40) ps2me-jit-fase2-compilerarea (PS2ME JIT, Fase 2). The r5900 JIT keeps
#     UseCompiler dormant (the hotness path would feed still-bailing CodeGenerator
#     handlers arbitrary methods). But ObjectHeap::init zeroes CompilerAreaPercentage
#     whenever UseCompiler is false, so try_to_compile() can never allocate a
#     CompiledMethod -> returns NULL (soft fail, no crash) -> no method ever runs
#     compiled. On the Fase-2 build only, keep the default 20% compiler area so our
#     strict trivial-void trigger can actually emit and enter r5900 code (the hotness
#     path stays off via UseCompiler). Wrap the zeroing block in #if !PS2ME_JIT_FASE2.
#     Gated ENABLE_COMPILER; absent in production. Idempotent (marker).
OHCPP="$PHONEME/cldc/src/vm/share/memory/ObjectHeap.cpp"
if ! grep -q 'ps2me-jit-fase2: the r5900 JIT keeps UseCompiler dormant' "$OHCPP"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase2-compilerarea.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 41) ps2me-jit-fase3-trigger (PS2ME JIT, Fase 3, Marco 3.1). Two gated
#     (PS2ME_JIT_FASE3) additions to Interpreter_c.cpp, applied ON TOP of the Fase 2
#     hunks (#37/#38): (a) jit_fase3_whitelisted()/jit_fase3_arm() -- arms only
#     straight-line integer-arithmetic methods (bytecode whitelist: local loads +
#     iadd/isub/imul/iand/ior/ixor + int consts + ireturn), every one fully covered
#     by the incremental CodeGenerator (never a bail-out); (b) the invoke_java_method
#     call site prefers jit_fase3_arm under FASE3 (#if FASE3 / #elif FASE2). FASE3 is
#     a superset of FASE2 (the cfg defines both), so the compiler-area (#40) and
#     OmitLeafMethodFrames-off (#38) gates apply unchanged. Gated PS2ME_JIT_FASE3 ->
#     absent in production and in the plain PS2ME_JIT build. Interpreter_c.cpp is
#     LF-only here, so a plain patch applies cleanly. Idempotent (marker). See
#     references/JIT_PLAN.md §15.
if ! grep -q 'jit_fase3_arm' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-trigger.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 42) ps2me-jit-fase3-loops (PS2ME JIT, Fase 3, Marco 3.2b). Two gated additions
#     to Interpreter_c.cpp applied ON TOP of #41: (a) the jit_timer_tick() glue
#     helper -- replicates the interpreter's own check_timer_tick() (if
#     _rt_timer_ticks>0 -> interpreter_call_vm_1(&timer_tick)); the r5900
#     CodeGenerator::check_timer_tick emission flushes the frame then calls it on
#     every backward branch, so loops keep the C interpreter's timer semantics
#     without porting call_vm/TimerTickStub/the stub queue; (b) the whitelist now
#     accepts backward branch offsets (loops) and iinc (for-loop counters), both
#     fully covered (iinc = increment_local_int = value_at + int_binary add +
#     value_at_put; the back-edge reuses emit_branch/jmp's bound-target path).
#     Gated PS2ME_JIT_FASE3 -> absent in production and the plain PS2ME_JIT build.
#     Interpreter_c.cpp is LF-only. Idempotent (guard on jit_timer_tick). See
#     references/JIT_PLAN.md §17.
if ! grep -q 'jit_timer_tick' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-loops.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 43) ps2me-jit-fase3-intisa (PS2ME JIT, Fase 3, Marco 3.3). One gated addition to
#     Interpreter_c.cpp applied ON TOP of #42: the compile whitelist now accepts the
#     rest of the integer ISA -- ineg, shifts (ishl/ishr/iushr) and narrowing
#     conversions (i2b/i2c/i2s). All fully covered by the backend (int_unary_do =
#     subu zero,rd; int_binary_do shift cases = sllv/srav/srlv + sll/sra/srl; i2b/i2s
#     = sll/sra sign-extend, i2c = andi zero-extend); no exceptions involved. Gated
#     PS2ME_JIT_FASE3. Interpreter_c.cpp is LF-only. Idempotent (guard on the
#     'Marco 3.3' whitelist marker). See references/JIT_PLAN.md §18.
if ! grep -q 'Marco 3.3: rest of the integer ISA' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-intisa.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 44) ps2me-jit-fase3-exc (PS2ME JIT, Fase 3, Marco 3.4a). One addition to
#     Interpreter_c.cpp applied ON TOP of #43: (a) the jit_throw_null_pointer /
#     jit_throw_array_index glue -- thin wrappers over the interpreter's own
#     interpreter_throw_* that the compiled null_check/array_check call to raise a
#     runtime exception (find_exception_frame repositions g_jfp/g_jsp/g_jpc to the
#     handler; the compiled method returns via its native epilogue, no jit_return);
#     and (b) the compile whitelist now accepts aload_0 + arraylength -- the first
#     bytecode that can throw. This is the exception subsystem isolated from arrays
#     (iaload/iastore arrive in Marco 3.4b); the compiled->interp unwind is proven
#     by a try/catch driver in JitTest. Interpreter_c.cpp is LF-only. Idempotent
#     (guard on the 'Marco 3.4a' whitelist marker). See references/JIT_PLAN.md §7.
if ! grep -q 'Marco 3.4a: object local load' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-exc.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 45) ps2me-jit-fase3-arrays (PS2ME JIT, Fase 3, Marco 3.4b). One-hunk whitelist
#     addition to Interpreter_c.cpp ON TOP of #44: the compile trigger now accepts
#     iaload/iastore (int-array element load/store). Each goes through the backend
#     array_check (null_check + UNSIGNED bounds check -> the Marco-3.4a helper-C
#     throw path) then an IndexedAddress (register index = sll+addu; immediate =
#     pure disp). The exception subsystem is reused from 3.4a; no new interp code.
#     Interpreter_c.cpp is LF-only. Idempotent (guard on the 'Marco 3.4b' marker).
#     See references/JIT_PLAN.md §7.
if ! grep -q 'Marco 3.4b: int-array element' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-arrays.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 46) ps2me-jit-fase3-fields (PS2ME JIT, Fase 3, Marco 3.5). One-hunk whitelist
#     addition to Interpreter_c.cpp ON TOP of #45: the compile trigger now accepts
#     the quickened INT instance-field bytecodes (fast_igetfield / fast_igetfield_1
#     / fast_iputfield and the aload_0-fused fast_igetfield_1/_4/_8). The compiler's
#     fast_get_field/fast_put_field reuse load_from_object/store_to_object
#     (maybe_null_check -> the 3.4a helper-C throw path + FieldAddress + load/store)
#     -- all already implemented; no new backend code. Object fields stay OUT (their
#     store needs a write barrier, still a bail-out). Interpreter_c.cpp is LF-only.
#     Idempotent (guard on the 'Marco 3.5' marker). See references/JIT_PLAN.md §7.
if ! grep -q 'Marco 3.5: instance INT field' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-fields.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 47) ps2me-jit-fase3-invokestatic (PS2ME JIT, Fase 3, Marco 3.6b-inline). Whitelist
#     changes to Interpreter_c.cpp ON TOP of #46: (a) jit_fase3_whitelisted gains an
#     allow_invoke parameter and now accepts fast_invokestatic/fast_init_invokestatic
#     when the resolved callee is a fully-whitelisted LEAF (recurse allow_invoke=false,
#     bounding depth to 1), so the shared front-end can INLINE a small static callee
#     into the compiled caller (internal_compile_inlined reuses our ops -- no frame/
#     call/unwind; a NON-inlined call bails cleanly via CodeGenerator::invoke ->
#     abort_active_compilation instead of crashing); (b) a compiled method may not
#     catch, so an armed method must have no exception table; (c) the arm cap is
#     raised 32 -> 64 to leave room for the new caller leaves. Interpreter_c.cpp is
#     LF-only. Idempotent (guard on the 'bool allow_invoke' signature marker, which
#     #48 preserves -- the old 'Marco 3.6b-inline: resolved static call' comment is
#     rewritten by #48). See references/JIT_PLAN.md §7.
if ! grep -q 'bool allow_invoke' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokestatic.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 48) ps2me-jit-fase3-invokestatic-real (PS2ME JIT, Fase 3, Marco 3.6b-real). Two
#     additions to Interpreter_c.cpp ON TOP of #47: (a) the jit_invoke_static() glue
#     -- the REAL (non-inlined) resolved static call from compiled code. It resolves
#     the callee from the caller frame's cpool by a compile-time index (GC-safe, no
#     oop literal), runs the static class-init barrier (fast_invoke_internal's path),
#     saves the caller frame + invokes, then drives an INTERPRETED callee with a
#     nested dispatch loop bounded by the caller frame (a compiled callee returns via
#     jit_return so the loop does not iterate). Option-B unwind: an exception that
#     repositions g_jfp above the caller frame ends the loop and the compiled
#     method's post-invoke fp-check (CodeGenerator::invoke, git-tracked) ejects. (b)
#     the whitelist now also admits fast_invokestatic whose callee bytecode_inline_
#     prepass NEVER inlines (code_size > 13, or non-leaf): it always becomes a real
#     call, never passes through our backend, so its bytecodes need no coverage.
#     Gated PS2ME_JIT_FASE3 -> absent in production and the plain PS2ME_JIT build.
#     Interpreter_c.cpp is LF-only. Idempotent (guard on jit_invoke_static). See
#     references/JIT_PLAN.md §7.
if ! grep -q 'jit_invoke_static' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokestatic-real.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 49) ps2me-jit-fase3-invokevirtualfinal (PS2ME JIT, Fase 3, Marco 3.6c). One-hunk
#     whitelist addition to Interpreter_c.cpp ON TOP of #48: the compile trigger now
#     also admits _fast_invokevirtual_final (constructors <init> + final methods). The
#     rewriter quickens both invokespecial-to-<init> and a final invokevirtual to it,
#     and the interp resolves it DIRECTLY from the cpool (fast_invoke_internal
#     has_fixed_target -> get_from_cpool), exactly like the static forms -- so it
#     reuses jit_invoke_static + the same inline/never-inline callee rule. The extra
#     receiver null-check lives in the backend (CodeGenerator::invoke, git-tracked).
#     Vtable/itable-resolved forms (_fast_invokespecial / _fast_invokevirtual /
#     interface) stay off (later Marco). Gated PS2ME_JIT_FASE3. Interpreter_c.cpp is
#     LF-only. Idempotent (guard on the 'Marco 3.6c adds' marker -- the bytecode name
#     itself already occurs in the interp). See references/JIT_PLAN.md §7.
if ! grep -q 'Marco 3.6c adds' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokevirtualfinal.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 50) ps2me-jit-fase3-invokespecial (PS2ME JIT, Fase 3, Marco 3.6c-vtable). Two
#     additions to Interpreter_c.cpp ON TOP of #49: (a) the jit_invoke_special() glue
#     -- the REAL super.m()/private call from compiled code. Unlike the static/final
#     forms, invokespecial stores vindex+klazz_id in the cpool (NOT a direct method
#     pointer), so the callee is resolved via the CPOOL class's vtable (first/second_
#     ushort_from_cpool -> get_class_by_id -> class_info -> get_method_from_ci),
#     binding to the class NAMED IN THE CPOOL (e.g. the superclass for super.m()),
#     never the receiver's dynamic type -- that static binding is the whole point of
#     invokespecial. No class-init barrier (a receiver exists -> holder initialized).
#     After resolution it reuses jit_invoke_static's machinery: invoke_java_method +
#     the nested dispatch loop bounded by the caller frame + option-B fp-check
#     (backend). (b) the whitelist now admits _fast_invokespecial: the shared
#     fast_invoke_special closure ALWAYS lowers through __ invoke (a real call, never
#     inlined), so the callee runs the normal VM path and needs no backend coverage.
#     The receiver null-check lives in the backend (CodeGenerator::invoke, git-tracked;
#     _fast_invokespecial routes to jit_invoke_special there). Gated PS2ME_JIT_FASE3 ->
#     absent in production and the plain PS2ME_JIT build. Interpreter_c.cpp is LF-only.
#     Idempotent (guard on jit_invoke_special). See references/JIT_PLAN.md §7.
if ! grep -q 'jit_invoke_special' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokespecial.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 51) ps2me-jit-fase3-invokevirtual (PS2ME JIT, Fase 3, Marco 3.6c-vtable 2/3). TWO
#     files ON TOP of #50: (a) BytecodeCompileClosure.cpp -- gate the type-info
#     devirtualization fast-path in fast_invoke_virtual OFF on MIPS
#     (`#if ENABLE_COMPILER_TYPE_INFO && !defined(MIPS)`). With it ON a known-exact
#     receiver would do_direct_invoke() the concrete callee, which may INLINE an
#     uncovered method (bail-out = crash) or land in CodeGenerator::invoke tagged with
#     _fast_invokevirtual (cpool = vindex+klazz_id, not a method pointer). Gating it off
#     keeps invokevirtual ALWAYS lowering through __ invoke_virtual -> jit_invoke_virtual
#     (dynamic vtable dispatch, always covered); phoneME supports
#     ENABLE_COMPILER_TYPE_INFO=0 and the fall-through is that config's exact path.
#     (b) Interpreter_c.cpp -- the jit_invoke_virtual() glue (mirrors
#     fast_invoke_internal's !has_fixed_target branch: resolve the callee via the
#     RECEIVER's vtable = get_method_from_vtable, so an overriding subclass method is
#     chosen at runtime; receiver null-checked in the backend) + the whitelist now
#     admits _fast_invokevirtual (never inlines -> real call, no callee coverage). The
#     backend CodeGenerator::invoke_virtual emission is git-tracked (overlay mips).
#     Gated PS2ME_JIT_FASE3. Both files LF-only. Idempotent (guard on jit_invoke_virtual;
#     the patch applies both files atomically). See references/JIT_PLAN.md §7.
if ! grep -q 'jit_invoke_virtual' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokevirtual.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 52) ps2me-jit-fase3-invokeinterface (PS2ME JIT, Fase 3, Marco 3.6c-vtable 3/3). One
#     addition to Interpreter_c.cpp ON TOP of #51: the jit_invoke_interface() glue --
#     the REAL interface call from compiled code. Mirrors bc_impl_fast_invokeinterface:
#     read method_index+class_id from the cpool, take the receiver (OBJ_PEEK(num_params-
#     1), null-checked in the backend), two indirections to its class, LINEAR-SEARCH the
#     receiver class's itable for the interface class_id, pick table[method_index], then
#     invoke_java_method + the nested dispatch loop. num_params is passed in (the interp
#     reads it from the bytecode via GET_BYTE(2), but g_jpc does not advance in compiled
#     code); the invoker size is the fixed length 5. A class not implementing the
#     interface raises IncompatibleClassChangeError the interp's own way. The whitelist
#     now admits _fast_invokeinterface (never inlines -> real call, no callee coverage).
#     The backend CodeGenerator::invoke_interface emission is git-tracked (overlay mips).
#     No BytecodeCompileClosure change needed (invoke_interface never devirtualizes/
#     inlines). Gated PS2ME_JIT_FASE3. Interpreter_c.cpp is LF-only. Idempotent (guard
#     on jit_invoke_interface). See references/JIT_PLAN.md §7.
if ! grep -q 'jit_invoke_interface' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-invokeinterface.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

# 53) ps2me-jit-fase3-objfields (PS2ME JIT, Fase 3, Marco 3.7a). One-hunk whitelist
#     addition to Interpreter_c.cpp ON TOP of #52: admit the instance OBJECT/reference
#     field fast forms (fast_agetfield / fast_aputfield / fast_agetfield_1 /
#     aload_0_fast_agetfield_{1,4,8}) plus reference local loads (aload / aload_1..3).
#     fast_agetfield reuses load_from_object (already covered by 3.4a/3.5);
#     fast_aputfield stores a heap pointer and emits the GC WRITE BARRIER -- the one
#     new backend piece -- now in the git-tracked mips overlay (Addressing_mips::
#     write_barrier_prolog/epilog, mirroring i386's shrl+bts: set the slot's word bit
#     in the object-heap marking bit vector, base loaded live to survive heap
#     expansion). The store_to_address change that calls it is likewise git-tracked
#     (overlay mips), NOT here. Gated PS2ME_JIT_FASE3. Interpreter_c.cpp is LF-only.
#     Idempotent (guard on the 3.7a marker). See references/JIT_PLAN.md §7.
if ! grep -q 'Marco 3.7a: instance OBJECT' "$INTERPC"; then
  sed 's/\r$//' "$SCRIPT_DIR/ps2me-jit-fase3-objfields.patch" | patch -p1 --ignore-whitespace -d "$PHONEME"
fi

echo "phoneME patches applied to: $PHONEME"
