/*
 * Ps2Main.cpp - PlayStation 2 entrypoint for the phoneME Feature CLDC VM.
 *
 * Milestone A boot glue. On the desktop ports, cldc/src/vm/os/javacall/
 * Main_javacall.cpp provides main() plus the JVMSPI print/exit/event callbacks,
 * driven by a real command line. The PS2 has no command line, so this file
 * replaces Main_javacall.cpp and:
 *   - assembles a fixed argv (VM options / the class to run),
 *   - runs the same JVM_Initialize -> parse-args -> JVM_Start sequence, and
 *   - supplies the JVMSPI callbacks Main_javacall.cpp would, routed through our
 *     javacall port (JVMSPI_PrintRaw -> pcsl_print_chars, which becomes
 *     javacall_print_chars once PCSL's PRINT_MODULE=javacall).
 *
 * Kept in ps2/vm/ (not ps2/javacall/) because this is VM-integration glue, not
 * part of the javacall HAL contract.
 *
 * Compiled with the exact same target flags as the VM's EXE_OBJS (same -I set,
 * -DROMIZING=1, -DUSE_UNICODE_FOR_FILENAMES=1, -DMIPS, -G0, ...): jvm.h derives
 * JvmPathChar from those, so the ABI must match the archives we link against.
 */

#include "jvm.h"        // JVM_Initialize / JVM_ParseOneArg / JVM_Start / JVM_GetConfig
#include "jvmspi.h"     // JVMSPI_* prototypes + JVMSPI_BlockedThreadInfo

extern "C" {
#include <pcsl_memory.h>   // pcsl_mem_initialize / pcsl_mem_finalize
#include <pcsl_print.h>    // pcsl_print_chars (the VM's stdout path under ENABLE_PCSL)
#include <javacall_logging.h> // javacall_print (our own layer; boot sign-of-life)
}

/* -------------------------------------------------------------------------
 * JVMSPI callbacks normally provided by Main_javacall.cpp.
 * ------------------------------------------------------------------------- */

void JVMSPI_PrintRaw(const char* s, int length) {
    /* System.out and VM diagnostics. With ENABLE_PCSL the VM core sends stdout
     * through pcsl_print_chars; keep that path so output flows through PCSL's
     * print module (-> javacall_print_chars -> our StdoutSink). */
    pcsl_print_chars(s, length);
}

void JVMSPI_Exit(int /*code*/) {
    /* Nothing to unwind on the PS2 for Milestone A; JVM_Start returns to main(). */
}

/* NB: JVMSPI_CheckEvents is provided by the VM itself (BSDSocket.o in libcldc_vmx),
 * so we must NOT define it here (it would be a duplicate symbol). */

/* -------------------------------------------------------------------------
 * PS2 entrypoint.
 * ------------------------------------------------------------------------- */

int main(int /*argc*/, char** /*argv*/) {
    int code;

    /* Sign of life straight through our javacall layer, before the VM starts:
     * proves libjavacall + the print path linked and run on real hardware. */
    javacall_print("j2me-ps2: booting phoneME Feature CLDC VM\n");

#if ENABLE_PCSL
    pcsl_mem_initialize(NULL, -1);
#endif

    JVM_Initialize();

    /* No command line on the PS2: assemble a fixed argv. jargv[0] stands in for
     * the program name (skipped, mirroring Main_javacall.cpp's argc--/argv++).
     * Milestone A boots with "-version": the VM prints its banner through the
     * same print path and returns, proving the end-to-end pipeline. Once a
     * HelloWorld is romized into the ROM image, replace "-version" with the
     * class name. */
    char  a_prog[] = "j2me";
    char  a_opt[]  = "-version";
    char* jargv[]  = { a_prog, a_opt };
    int   jargc    = 2;

    /* Skip the program name, then run the VM option parse loop. */
    jargc--;
    char** p = jargv + 1;
    while (true) {
        int n = JVM_ParseOneArg(jargc, p);
        if (n < 0) {
            JVMSPI_DisplayUsage(NULL);
            code = -1;
            goto end;
        } else if (n == 0) {
            break;
        }
        jargc -= n;
        p     += n;
    }

    code = JVM_Start(NULL, NULL, jargc, p);

end:
#if ENABLE_PCSL
    pcsl_mem_finalize();
#endif
    return code;
}
