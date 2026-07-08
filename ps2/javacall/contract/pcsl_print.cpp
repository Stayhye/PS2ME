// PS2 JavaCall port — contract layer. PCSL print module (javacall-routed).
//
// The VM's stdout -- System.out plus the VM's own diagnostics -- reaches native code
// through JVMSPI_PrintRaw -> pcsl_print_chars. The prebuilt libpcsl_print.a in this
// build was compiled with PCSL's "stdout" print module, which fwrite()s straight to the
// EE tty and so bypasses our hal::Logger/StdoutSink tee -- meaning that output shows on
// the SIF console but never reaches the on-screen debug log (Ps2Frontend::logWrite).
//
// Define pcsl_print/pcsl_print_chars ourselves so the VM's stdout flows through
// javacall_print* -> Logger -> StdoutSink, which both mirrors it to the native GS debug
// overlay AND writes it to the tty (under the shared SifLock). Because these are the
// only two symbols libpcsl_print.a's stdout member provides and we define both here (a
// regular object, always linked), that archive member is never pulled in, so this wins
// with no multiple-definition conflict -- effectively swapping in the "javacall" module.
extern "C" {
#include <javacall_logging.h>

void pcsl_print(const char* s) {
    javacall_print(s);
}

void pcsl_print_chars(const char* s, int length) {
    javacall_print_chars(s, length);
}
}
