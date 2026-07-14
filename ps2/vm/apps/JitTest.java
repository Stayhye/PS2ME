/*
 * JitTest.java - PS2ME r5900 JIT Fase 3 (Marco 3.1) test harness.
 *
 * A deterministic, game-independent smoke test for the incremental CodeGenerator.
 * It is romized into the CLDC ROM image and launched by name from Ps2Main.cpp
 * when the ELF is built with PS2ME_JIT_FASE3=true (see references/JIT_PLAN.md).
 *
 * Each leaf method is a straight-line integer expression whose bytecodes are all
 * covered by Marco 3.1 (local loads + int add/sub/mul/and/or/xor, register and
 * immediate forms). The strict Fase-3 compile trigger (jit_fase3_arm in
 * Interpreter_c.cpp) arms exactly these leaves; main() invokes each several times
 * so the arm -> compile -> run-compiled sequence completes, then verifies the
 * results the COMPILED code produced and prints a PASS/FAIL table (reaching the
 * EE Console through System.out -> JVMSPI_PrintRaw). main() itself is NOT armed
 * (it contains invokes/branches, outside the whitelist), so it stays interpreted
 * and drives the compiled leaves through the real interp->compiled path.
 */
public class JitTest {
    // Register-operand forms: op2 arrives in a register (both args are locals).
    static int add(int a, int b) { return a + b; }   // addu
    static int sub(int a, int b) { return a - b; }   // subu
    static int mul(int a, int b) { return a * b; }   // mult + mflo
    static int and(int a, int b) { return a & b; }   // and
    static int or (int a, int b) { return a | b; }   // or
    static int xor(int a, int b) { return a ^ b; }   // xor

    // Immediate-operand forms: op2 is a compile-time constant.
    static int addImm(int a) { return a + 5; }       // addiu
    static int subImm(int a) { return a - 3; }       // addiu (negated)
    static int andImm(int a) { return a & 255; }     // andi

    // Marco 3.2a: forward branches (if / if_icmp / goto) + a local store forced
    // to memory at a branch merge. Each leaf's every bytecode is on the Fase-3
    // whitelist and has only forward branch offsets, so the strict trigger arms
    // them and the incremental CodeGenerator compiles them fully (no bail-out).
    static int max2(int a, int b) { return a >= b ? a : b; }      // if_icmp + goto
    static int min2(int a, int b) { return a <= b ? a : b; }      // if_icmp + goto
    static int eqSel(int a, int b) { return a == b ? 111 : 222; } // if_icmpeq (no slt)
    static int neSel(int a, int b) { return a != b ? 111 : 222; } // if_icmpne (no slt)
    static int isNeg(int a) { return a < 0 ? 1 : 0; }             // iflt zero-form
    static int clampLow(int a) { if (a < 0) a = 0; return a; }    // ifge zero + store@merge
    static int clampHi(int a)  { if (a > 100) a = 100; return a; }// if_icmp + store@merge

    static int fails = 0;

    static void check(String name, int got, int want) {
        if (got == want) {
            System.out.println("[JIT-TEST] " + name + " = " + got + " OK");
        } else {
            fails++;
            System.out.println("[JIT-TEST] " + name + " = " + got
                               + " FAIL (want " + want + ")");
        }
    }

    public static void main(String[] args) {
        System.out.println("[JIT-TEST] Fase 3 Marco 3.2a harness start");

        // Warm every leaf: the first invocation arms it, the second compiles it
        // (still runs interpreted), the third+ run the r5900-compiled code. Loop
        // a few extra times for margin; the captured results are from the
        // compiled path.
        int ra = 0, rs = 0, rm = 0, rn = 0, ro = 0, rx = 0, ri = 0, rj = 0, rk = 0;
        // Marco 3.2a branch leaves.
        int mx = 0, mn = 0, eq = 0, ne = 0, ng = 0, cl = 0, ch = 0;
        for (int i = 0; i < 8; i++) {
            ra = add(7, 5);
            rs = sub(7, 5);
            rm = mul(7, 5);
            rn = and(12, 10);
            ro = or(12, 10);
            rx = xor(12, 10);
            ri = addImm(37);
            rj = subImm(40);
            rk = andImm(0x1FF);
            mx = max2(7, 5);
            mn = min2(7, 5);
            eq = eqSel(4, 4);
            ne = neSel(4, 5);
            ng = isNeg(-3);
            cl = clampLow(-8);
            ch = clampHi(150);
        }

        check("add(7,5)",      ra, 12);
        check("sub(7,5)",      rs, 2);
        check("mul(7,5)",      rm, 35);
        check("and(12,10)",    rn, 8);
        check("or(12,10)",     ro, 14);
        check("xor(12,10)",    rx, 6);
        check("addImm(37)",    ri, 42);
        check("subImm(40)",    rj, 37);
        check("andImm(0x1FF)", rk, 255);
        check("max2(7,5)",     mx, 7);
        check("min2(7,5)",     mn, 5);
        check("eqSel(4,4)",    eq, 111);
        check("neSel(4,5)",    ne, 111);
        check("isNeg(-3)",     ng, 1);
        check("clampLow(-8)",  cl, 0);
        check("clampHi(150)",  ch, 100);

        // Exercise the OTHER branch of each leaf (already compiled above), so
        // both the taken and fall-through paths of the emitted branch run.
        check("max2(5,7)",     max2(5, 7),    7);
        check("min2(5,7)",     min2(5, 7),    5);
        check("eqSel(4,5)",    eqSel(4, 5),   222);
        check("neSel(4,4)",    neSel(4, 4),   222);
        check("isNeg(3)",      isNeg(3),      0);
        check("clampLow(9)",   clampLow(9),   9);
        check("clampHi(50)",   clampHi(50),   50);

        if (fails == 0) {
            System.out.println("[JIT-TEST] ALL PASS");
        } else {
            System.out.println("[JIT-TEST] " + fails + " TEST(S) FAILED");
        }
    }
}
