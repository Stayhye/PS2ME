/*
 * HelloWorld.java - Milestone A smoke-test MIDlet-less app.
 *
 * The smallest CLDC 1.1 program that proves end-to-end bytecode execution on the
 * PS2: a plain class with a static main() exercising System.out.println plus
 * String concatenation and a loop. It is romized into the ROM image (no file I/O)
 * and launched by name from ps2/vm/Ps2Main.cpp. Its output reaches the EE Console
 * through JVMSPI_PrintRaw -> pcsl_print_chars.
 */
public class HelloWorld {
    public static void main(String[] args) {
        System.out.println("Hello from J2ME on the PlayStation 2!");
        for (int i = 1; i <= 3; i++) {
            System.out.println("  tick " + i);
        }
        System.out.println("HelloWorld done.");
    }
}
