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

    // Marco 3.2b: loops (backward branch + iinc). Each back-edge emits
    // check_timer_tick (frame flush + jit_timer_tick call); the loop body runs
    // r5900-compiled. All bytecodes (iconst/iload_*/istore_*/iadd/imul/iinc/
    // if_icmp*/ifgt/goto/ireturn) are on the Fase-3 whitelist -> no bail-out.
    static int sumTo(int n)   { int s = 0; for (int i = 1; i <= n; i++) s += i; return s; } // for + iadd
    static int factLike(int n){ int p = 1; for (int i = 1; i <= n; i++) p *= i; return p; } // for + imul
    static int powTwo(int k)  { int r = 1; for (int i = 0; i <  k; i++) r += r; return r; } // for + doubling
    static int mulLoop(int a, int b) { int r = 0; while (b > 0) { r += a; b--; } return r; }// while + ifgt + iinc

    // Marco 3.3: rest of the integer ISA -- negate, shifts (register + immediate
    // amount), narrowing conversions. No branches, no exceptions.
    static int neg(int a)            { return -a; }        // ineg
    static int shl(int a, int b)     { return a << b; }    // ishl (register amount)
    static int shr(int a, int b)     { return a >> b; }    // ishr (arithmetic)
    static int ushr(int a, int b)    { return a >>> b; }   // iushr (logical)
    static int shlImm(int a)         { return a << 3; }    // ishl (immediate amount)
    static int i2bTest(int a)        { return (byte)a; }   // i2b (sign-extend)
    static int i2cTest(int a)        { return (char)a; }   // i2c (zero-extend)
    static int i2sTest(int a)        { return (short)a; }  // i2s (sign-extend)

    // Marco 3.4a: the exception subsystem, isolated from arrays. arraylength is
    // the first compiled bytecode that can throw: it emits a null_check (inline
    // reference test + branch to the helper-C throw path) then loads the length.
    // The compiled null_check raising NullPointerException, and the throw
    // unwinding out of the compiled leaf's native frame back to an INTERPRETED
    // try/catch, is what proves the whole compiled->interp unwind before iaload/
    // iastore arrive in 3.4b. Leaf bytecodes = aload_0/arraylength/ireturn.
    static int alen(int[] a) { return a.length; }          // null_check + length

    // Marco 3.4b: int-array element load/store. Each access = array_check
    // (null_check + unsigned bounds check -> throw path) + an IndexedAddress
    // (register-index: sll+addu; disp = header). aget/aset are straight-line;
    // asum combines arraylength + iaload inside a loop (reuses 3.2b back-edge).
    // An OOB / negative index on a compiled aget must throw
    // ArrayIndexOutOfBoundsException and unwind to an interpreted try/catch.
    static int aget(int[] a, int i)        { return a[i]; }         // iaload
    static int aset(int[] a, int i, int v) { a[i] = v; return a[i]; } // iastore + iaload
    static int asum(int[] a) { int s = 0; for (int i = 0; i < a.length; i++) s += a[i]; return s; }

    // Marco 3.5: instance INT field load/store. getfield/putfield of an int
    // field quicken to fast_igetfield*/fast_iputfield (possibly aload_0-fused).
    // The compiled fast_get_field/fast_put_field reuse the 3.4a null_check +
    // throw path (proven by gx(null) -> NPE) then load/store via FieldAddress.
    static final class Pt { int x; int y; }
    static int gx(Pt p)        { return p.x; }            // field load (offset x)
    static int gy(Pt p)        { return p.y; }            // field load (offset y)
    static int sx(Pt p, int v) { p.x = v; return p.x; }  // field store + reload

    // Marco 3.6b-inline: a resolved static call. addHelper is a fully-whitelisted
    // leaf; the shared compiler INLINES it into each caller (internal_compile_inlined
    // reuses our int ops -- no frame, no call, no unwind), so this proves the
    // compiled invokestatic path end to end. caller3 composes the inlined result
    // with an immediate add; caller3b with a multiply. The whitelist arms the caller
    // only once the rewriter has quickened invokestatic -> fast_invokestatic and the
    // callee resolves to a whitelisted leaf (bounded to depth 1).
    static int addHelper(int a, int b) { return a + b; }             // inlined leaf
    static int caller3(int a, int b)   { return addHelper(a, b) + 1; } // invokestatic + iadd
    static int caller3b(int a)         { return addHelper(a, a) * 2; } // invokestatic + imul

    // Marco 3.6b-real: the REAL (non-inlined) static call. A callee that
    // bytecode_inline_prepass NEVER inlines (code_size > 13, or non-leaf) is NOT
    // inlined by the front-end, so CodeGenerator::invoke emits jit_invoke_static +
    // the nested dispatch loop + the option-B fp-check instead.
    //   bigAdd  : a big whitelisted leaf (>13 bytes) -> armed and compiled, so
    //             callBig exercises a compiled->compiled real call (the nested loop
    //             does not iterate: the compiled callee returns via jit_return).
    //   interpAdd: carries an idiv (never whitelisted -> never armed) and is >13
    //             bytes -> ALWAYS interpreted, so callInterp exercises a
    //             compiled->interpreted real call whose body runs in the nested
    //             dispatch loop.
    //   bigLen  : returns a.length, >13 bytes -> armed/compiled; callThrow real-
    //             calls it and bigLen(null) throws NPE that must unwind OUT of
    //             callThrow's compiled frame (option-B fp-check) to a try/catch above.
    static int bigAdd(int a, int b)  { return a+b+a+b+a+b+a+b; }      // 4a+4b, >13 bytes
    static int callBig(int a, int b) { return bigAdd(a, b) + 1; }     // real call, compiled callee
    static int interpAdd(int a, int b) { int d = a / (b + 1); return a+b+a+b+a+b + d - d; } // 3a+3b, idiv
    static int callInterp(int a, int b) { return interpAdd(a, b) + 100; } // real call, interpreted callee
    static int bigLen(int[] a)       { int n = a.length; return n+n+n+n+n+n+n+n; } // 8*len, can NPE
    static int callThrow(int[] a)    { return bigLen(a) + 1; }        // real call; NPE unwinds through it

    // Marco 3.6c: _fast_invokevirtual_final (constructors <init> + FINAL methods).
    // The rewriter quickens a final instance-method invokevirtual to it, and it
    // resolves DIRECTLY from the cpool (like the static forms) -- so it reuses
    // jit_invoke_static (class-init is a no-op: the receiver already exists) plus a
    // RECEIVER null-check in the backend. fadd is small (inlines, exercising the
    // instance-method inline path); fbig is >13 bytes (never inlines -> real call,
    // exercising the receiver null-check + direct resolution). callVBig(null,..)
    // proves the receiver NPE unwinds out of the compiled caller (option-B).
    static final class Adder {
        int base;
        Adder(int b) { base = b; }
        final int fadd(int a) { return base + a; }                    // small final -> inlines
        final int fbig(int a) { return base+a+a+a+a+a+a+a+a; }        // >13 bytes -> real call
    }
    static int callVAdd(Adder x, int a) { return x.fadd(a) + 1; }     // invokevirtual_final (inline)
    static int callVBig(Adder x, int a) { return x.fbig(a) + 1; }     // invokevirtual_final (real call)

    // Marco 3.6c-vtable: _fast_invokespecial (super.m() non-init + private methods).
    // invokespecial binds STATICALLY, via the cpool class's vtable (vindex+klazz_id),
    // NOT the receiver's dynamic type -- the opposite of invokevirtual. Derived
    // overrides who(); super.who() from callSuper MUST call Base.who()=1 even though
    // the receiver is a Derived whose who()=2, proving the static (cpool-class)
    // resolution jit_invoke_special performs. A private method is likewise invoked
    // via invokespecial. Both quicken to _fast_invokespecial (is_init false) and
    // ALWAYS emit a real call (fast_invoke_special -> __ invoke, never inlined),
    // reusing jit_invoke_special + the nested dispatch loop + option-B fp-check +
    // the backend receiver null-check. callSuper/callPriv are INSTANCE methods
    // (this = local 0), armed and compiled like the static leaves.
    static class Base {
        int who() { return 1; }
    }
    static final class Derived extends Base {
        int who() { return 2; }                                       // override
        int callSuper() { return super.who() + 10; }                  // invokespecial Base.who -> 11
        private int priv(int a) { return a * 3; }                     // private -> invokespecial
        int callPriv(int a) { return priv(a) + 1; }                   // invokespecial priv
    }

    // Marco 3.6c-vtable 2/3: _fast_invokevirtual (DYNAMIC dispatch on the receiver's
    // vtable). callSound is ONE compiled leaf; a.sound() re-resolves at runtime via the
    // receiver's vtable, so the SAME compiled code returns 101/102/103 for an Animal/
    // Dog/Cat receiver -- proving true dynamic dispatch (the opposite of invokespecial's
    // static binding). The receiver is a parameter (declared type Animal, not exact),
    // so the type-info devirtualization does not fire even if it were on; on MIPS it is
    // gated off anyway. callSound(null) throws NPE (backend receiver null-check -> 3.4a
    // throw -> option-B unwind out of the compiled frame).
    static class Animal {
        int sound() { return 1; }
    }
    static final class Dog extends Animal { int sound() { return 2; } }   // override
    static final class Cat extends Animal { int sound() { return 3; } }   // override
    static int callSound(Animal a) { return a.sound() + 100; }            // invokevirtual

    // Marco 3.6c-vtable 3/3: _fast_invokeinterface (DYNAMIC dispatch via the receiver's
    // itable). callGreet is ONE compiled leaf; g.greet() linear-searches the receiver
    // class's itable for the Greeter interface at runtime, so the SAME compiled code
    // returns 207/208 for a Hi/Yo receiver. callGreet(null) throws NPE (backend receiver
    // null-check -> option-B unwind).
    interface Greeter { int greet(); }
    static final class Hi implements Greeter { public int greet() { return 7; } }
    static final class Yo implements Greeter { public int greet() { return 8; } }
    static int callGreet(Greeter g) { return g.greet() + 200; }          // invokeinterface

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
        System.out.println("[JIT-TEST] Fase 3 Marco 3.6c-vtable harness start");

        // A valid array to warm/verify the in-bounds arraylength path.
        int[] arr5 = new int[5];
        // Read-only array for aget/asum; a separate mutable array for aset.
        int[] arrG = { 10, 20, 30, 40, 50 };
        int[] arrS = new int[3];
        // Field-access objects: pt read-only (gx/gy), pt2 mutated (sx).
        Pt pt = new Pt(); pt.x = 7; pt.y = 9;
        Pt pt2 = new Pt();

        // Warm every leaf: the first invocation arms it, the second compiles it
        // (still runs interpreted), the third+ run the r5900-compiled code. Loop
        // a few extra times for margin; the captured results are from the
        // compiled path.
        int ra = 0, rs = 0, rm = 0, rn = 0, ro = 0, rx = 0, ri = 0, rj = 0, rk = 0;
        // Marco 3.2a branch leaves.
        int mx = 0, mn = 0, eq = 0, ne = 0, ng = 0, cl = 0, ch = 0;
        // Marco 3.2b loop leaves.
        int st = 0, ft = 0, pw = 0, ml = 0;
        // Marco 3.3 int-ISA leaves.
        int ng2 = 0, sl = 0, sr = 0, us = 0, si = 0, ib = 0, ic = 0, is = 0;
        // Marco 3.4a exception subsystem leaf.
        int al = 0;
        // Marco 3.4b array leaves.
        int ag = 0, sm = 0, av = 0;
        // Marco 3.5 field leaves.
        int gxv = 0, gyv = 0, sxv = 0;
        // Marco 3.6b-inline: static-call (inlined) leaves.
        int c3 = 0, c3b = 0;
        // Marco 3.6b-real: static-call (non-inlined REAL call) leaves.
        int cb = 0, cin = 0, ct = 0;
        // Marco 3.6c: _fast_invokevirtual_final (final instance methods) leaves.
        Adder adr = new Adder(10);
        int va = 0, vb = 0;
        // Marco 3.6c-vtable: _fast_invokespecial (super.m() + private) leaves.
        Derived der = new Derived();
        int cs = 0, cpv = 0;
        // Marco 3.6c-vtable 2/3: _fast_invokevirtual (dynamic dispatch) leaves.
        Animal animal = new Animal(); Dog dog = new Dog(); Cat cat = new Cat();
        int vsnd = 0;
        // Marco 3.6c-vtable 3/3: _fast_invokeinterface (itable dispatch) leaves.
        Greeter hi = new Hi(); Greeter yo = new Yo();
        int vgrt = 0;
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
            st = sumTo(10);
            ft = factLike(5);
            pw = powTwo(5);
            ml = mulLoop(6, 7);
            ng2 = neg(7);
            sl = shl(1, 4);
            sr = shr(-16, 2);
            us = ushr(-1, 28);
            si = shlImm(5);
            ib = i2bTest(200);
            ic = i2cTest(-1);
            is = i2sTest(40000);
            al = alen(arr5);
            ag = aget(arrG, 3);
            sm = asum(arrG);
            av = aset(arrS, 1, 99);
            gxv = gx(pt);
            gyv = gy(pt);
            sxv = sx(pt2, 55);
            c3 = caller3(7, 5);
            c3b = caller3b(10);
            cb = callBig(7, 5);        // real call -> compiled bigAdd
            cin = callInterp(7, 5);    // real call -> interpreted interpAdd
            ct = callThrow(arr5);      // real call -> compiled bigLen (in-bounds)
            va = callVAdd(adr, 5);     // invokevirtual_final -> inlined fadd
            vb = callVBig(adr, 5);     // invokevirtual_final -> real call fbig
            cs = der.callSuper();      // invokespecial super.who() -> Base.who()=1, +10
            cpv = der.callPriv(7);     // invokespecial private priv(7)=21, +1
            vsnd = callSound(animal);  // invokevirtual -> Animal.sound()=1, +100 (warms leaf)
            vgrt = callGreet(hi);      // invokeinterface -> Hi.greet()=7, +200 (warms leaf)
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
        check("sumTo(10)",     st, 55);
        check("factLike(5)",   ft, 120);
        check("powTwo(5)",     pw, 32);
        check("mulLoop(6,7)",  ml, 42);
        check("neg(7)",        ng2, -7);
        check("shl(1,4)",      sl, 16);
        check("shr(-16,2)",    sr, -4);
        check("ushr(-1,28)",   us, 15);
        check("shlImm(5)",     si, 40);
        check("i2bTest(200)",  ib, -56);
        check("i2cTest(-1)",   ic, 65535);
        check("i2sTest(40000)", is, -25536);
        check("alen(arr5)",    al, 5);

        // Marco 3.4a: the compiled null_check must throw NullPointerException and
        // the throw must unwind OUT of the compiled leaf's native frame back to
        // this interpreted try/catch. alen is already compiled (warmed above), so
        // alen(null) runs the r5900 null_check -> jit_throw_null_pointer -> interp
        // unwind -> handler here. A miss (no throw, or a crash) fails/hangs.
        boolean npeCaught = false;
        try {
            alen(null);
            System.out.println("[JIT-TEST] alen(null) did NOT throw");
        } catch (NullPointerException e) {
            npeCaught = true;
        }
        check("alen(null) throws NPE", npeCaught ? 1 : 0, 1);
        // Prove the handler resumes cleanly: normal compiled calls still work
        // after the unwind (globals coherent, no leaked native frame).
        check("alen(arr5) post-throw", alen(arr5), 5);

        // Marco 3.4b: in-bounds int-array element load/store + sum loop.
        check("aget(arrG,3)",   ag, 40);
        check("asum(arrG)",     sm, 150);
        check("aset(arrS,1,99)", av, 99);
        check("aget(arrS,1)",   aget(arrS, 1), 99);   // read back the stored value
        check("aget(arrG,0)",   aget(arrG, 0), 10);   // first element (index 0)

        // Marco 3.4b: the compiled bounds check must throw
        // ArrayIndexOutOfBoundsException for an out-of-range index AND for a
        // negative one (unsigned compare catches both), unwinding to this
        // interpreted try/catch. aget is already compiled (warmed above).
        boolean oobCaught = false;
        try {
            aget(arrG, 10);   // 10 >= length 5
        } catch (ArrayIndexOutOfBoundsException e) {
            oobCaught = true;
        }
        check("aget(arrG,10) throws AIOOBE", oobCaught ? 1 : 0, 1);
        boolean negCaught = false;
        try {
            aget(arrG, -1);   // negative -> huge unsigned -> out of bounds
        } catch (ArrayIndexOutOfBoundsException e) {
            negCaught = true;
        }
        check("aget(arrG,-1) throws AIOOBE", negCaught ? 1 : 0, 1);
        check("aget(arrG,2) post-throw", aget(arrG, 2), 30);  // resumes clean

        // Marco 3.5: instance int field load/store (compiled).
        check("gx(pt)",      gxv, 7);
        check("gy(pt)",      gyv, 9);
        check("sx(pt2,55)",  sxv, 55);
        check("gx(pt2)",     gx(pt2), 55);   // read back the stored field

        // Marco 3.5: the compiled field null_check must throw NPE and unwind to
        // this interpreted try/catch (same helper-C mechanism as 3.4a).
        boolean fldNpe = false;
        try {
            gx(null);
        } catch (NullPointerException e) {
            fldNpe = true;
        }
        check("gx(null) throws NPE", fldNpe ? 1 : 0, 1);
        check("gx(pt) post-throw",   gx(pt), 7);   // resumes clean

        // Marco 3.6b-inline: static call inlined into the compiled caller.
        check("caller3(7,5)",   c3,  13);   // addHelper(7,5)+1
        check("caller3b(10)",   c3b, 40);   // addHelper(10,10)*2
        check("caller3(20,22)", caller3(20, 22), 43);  // other args, compiled path

        // Marco 3.6b-real: the REAL (non-inlined) static call.
        // compiled->compiled: callBig real-calls the compiled big leaf bigAdd.
        check("callBig(7,5)",    cb, 49);                 // bigAdd(7,5)=48, +1
        check("callBig(20,22)",  callBig(20, 22), 169);   // 4*(42)=168, +1
        // compiled->interpreted: callInterp real-calls the never-armed interpAdd
        // (idiv), whose body runs in jit_invoke_static's nested dispatch loop.
        check("callInterp(7,5)", cin, 136);               // interpAdd=3*(12)=36, +100
        check("callInterp(10,4)", callInterp(10, 4), 142); // 3*(14)=42, +100
        // In-bounds real call to the compiled bigLen.
        check("callThrow(arr5)", ct, 41);                 // 8*5=40, +1

        // Option-B unwind: bigLen(null) throws NPE that must unwind OUT of
        // callThrow's compiled native frame (post-invoke fp-check: g_jfp != fp_A ->
        // bare epilogue) and propagate to this interpreted try/catch above. A miss
        // (no throw / crash / leaked frame) fails or hangs.
        boolean realNpe = false;
        try {
            callThrow(null);
        } catch (NullPointerException e) {
            realNpe = true;
        }
        check("callThrow(null) throws NPE", realNpe ? 1 : 0, 1);
        // The compiled caller resumes cleanly after the unwind.
        check("callThrow(arr5) post-throw", callThrow(arr5), 41);

        // Marco 3.6c: _fast_invokevirtual_final (final instance methods).
        check("callVAdd(adr,5)",  va, 16);                // fadd=10+5=15, +1 (inlined)
        check("callVBig(adr,5)",  vb, 51);                // fbig=10+8*5=50, +1 (real call)
        check("callVBig(adr,9)",  callVBig(adr, 9), 83);  // 10+72=82, +1
        // Receiver null-check: callVBig(null,..) must throw NPE and unwind OUT of the
        // compiled caller's frame (backend null_check -> 3.4a throw -> option-B).
        boolean vNpe = false;
        try {
            callVBig(null, 5);
        } catch (NullPointerException e) {
            vNpe = true;
        }
        check("callVBig(null,5) throws NPE", vNpe ? 1 : 0, 1);
        check("callVBig(adr,5) post-throw", callVBig(adr, 5), 51);

        // Marco 3.6c-vtable: _fast_invokespecial. callSuper()=11 PROVES static
        // (cpool-class) binding: super.who() resolves Base.who()=1, NOT the
        // receiver's overriding Derived.who()=2 (which would give 12). callPriv
        // exercises a private-method invokespecial. Both run through the compiled
        // caller -> jit_invoke_special (dynamic-vtable resolution of the cpool class).
        check("der.callSuper()",  cs,  11);               // super.who()=1 (NOT Derived's 2), +10
        check("der.callPriv(7)",  cpv, 22);               // private priv(7)=21, +1
        check("der.callSuper() again", der.callSuper(), 11);  // compiled path, stable
        check("der.callPriv(11)", der.callPriv(11), 34);      // priv(11)=33, +1

        // Marco 3.6c-vtable 2/3: _fast_invokevirtual. The SAME compiled callSound leaf
        // (warmed with an Animal above) dispatches DYNAMICALLY on the receiver's vtable:
        // Animal.sound()=1, Dog.sound()=2, Cat.sound()=3 -> 101/102/103. Different
        // results from one compiled method PROVE runtime dispatch by the receiver.
        check("callSound(animal)", vsnd, 101);            // Animal.sound()=1, +100
        check("callSound(dog)",   callSound(dog), 102);   // Dog.sound()=2 (override), +100
        check("callSound(cat)",   callSound(cat), 103);   // Cat.sound()=3 (override), +100
        check("callSound(animal) again", callSound(animal), 101);  // stable base dispatch
        // Receiver null-check: callSound(null) must throw NPE and unwind OUT of the
        // compiled leaf's frame (backend null_check -> 3.4a throw -> option-B).
        boolean vsndNpe = false;
        try {
            callSound(null);
        } catch (NullPointerException e) {
            vsndNpe = true;
        }
        check("callSound(null) throws NPE", vsndNpe ? 1 : 0, 1);
        check("callSound(dog) post-throw", callSound(dog), 102);   // resumes clean

        // Marco 3.6c-vtable 3/3: _fast_invokeinterface. The SAME compiled callGreet leaf
        // (warmed with a Hi above) dispatches via the receiver's itable: Hi.greet()=7,
        // Yo.greet()=8 -> 207/208. Different results from one compiled method prove
        // runtime interface dispatch (linear itable search).
        check("callGreet(hi)",  vgrt, 207);               // Hi.greet()=7, +200
        check("callGreet(yo)",  callGreet(yo), 208);      // Yo.greet()=8, +200
        check("callGreet(hi) again", callGreet(hi), 207); // stable itable dispatch
        // Receiver null-check: callGreet(null) must throw NPE and unwind OUT of the
        // compiled leaf's frame (backend null_check -> 3.4a throw -> option-B).
        boolean vgrtNpe = false;
        try {
            callGreet(null);
        } catch (NullPointerException e) {
            vgrtNpe = true;
        }
        check("callGreet(null) throws NPE", vgrtNpe ? 1 : 0, 1);
        check("callGreet(yo) post-throw", callGreet(yo), 208);     // resumes clean

        // Exercise the OTHER branch of each leaf (already compiled above), so
        // both the taken and fall-through paths of the emitted branch run.
        check("max2(5,7)",     max2(5, 7),    7);
        check("min2(5,7)",     min2(5, 7),    5);
        check("eqSel(4,5)",    eqSel(4, 5),   222);
        check("neSel(4,4)",    neSel(4, 4),   222);
        check("isNeg(3)",      isNeg(3),      0);
        check("clampLow(9)",   clampLow(9),   9);
        check("clampHi(50)",   clampHi(50),   50);
        // Loops with different iteration counts (incl. zero-iteration: goto->
        // check exits immediately without running the body).
        check("sumTo(100)",    sumTo(100),    5050);
        check("factLike(6)",   factLike(6),   720);
        check("powTwo(10)",    powTwo(10),    1024);
        check("mulLoop(12,0)", mulLoop(12, 0), 0);
        // Marco 3.3 int-ISA with other values.
        check("neg(-9)",       neg(-9),        9);
        check("shr(15,1)",     shr(15, 1),     7);
        check("ushr(-8,1)",    ushr(-8, 1),    0x7FFFFFFC);
        check("i2bTest(-1)",   i2bTest(-1),    -1);
        check("i2sTest(-1)",   i2sTest(-1),    -1);

        if (fails == 0) {
            System.out.println("[JIT-TEST] ALL PASS");
        } else {
            System.out.println("[JIT-TEST] " + fails + " TEST(S) FAILED");
        }
    }
}
