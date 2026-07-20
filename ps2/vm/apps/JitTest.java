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

    // Marco 3.7a: instance OBJECT/reference field load/store. getfield/putfield of
    // a reference field quicken to fast_agetfield/fast_aputfield (aload_0-fused
    // forms too). fast_agetfield reuses load_from_object (already covered).
    // fast_aputfield stores a heap pointer and needs the GC WRITE BARRIER
    // (HeapAddress::write_barrier_prolog/epilog, now inline in Addressing_mips,
    // mirroring i386's shrl+bts: set the slot's word bit in the object-heap marking
    // bit vector so the GC visits the stored pointer). linkTag writes the ref field
    // then reads it back THROUGH the stored pointer (n.link.tag), proving the store
    // landed AND the barrier emission left value/registers intact; getLinkTag is a
    // read-only deref (fast_agetfield). getLinkTag(null) throws NPE that unwinds
    // out of the compiled leaf (3.4a throw path). Both are leaves: aload_0/aload_1/
    // fast_aputfield/fast_agetfield/fast_igetfield/ireturn -- all whitelisted.
    static final class Node { int tag; Node link; }
    static int linkTag(Node n, Node v) { n.link = v; return n.link.tag; } // aputfield(barrier)+agetfield
    static int getLinkTag(Node n)      { return n.link.tag; }             // agetfield (read-only)

    // Marco 3.7b: object-array element load/store. aaGet reads a[i] (aaload, a
    // reference-array element load = IndexedAddress T_OBJECT) then its int field.
    // aaStore writes a[i]=v (aastore): the array-store TYPE CHECK (jit_array_store_check
    // -> the interp's array_store_type_check, raising ArrayStoreException on a type
    // mismatch and unwinding via option-B) followed by the write-barrier'd store. The
    // array parameter is declared Object[] so the compiler cannot elide the type check
    // (not an exact/final type), which is what exercises the new backend path. Storing
    // a String into a real Node[] aliased as Object[] must throw ArrayStoreException.
    static int aaGet(Node[] a, int i) { return a[i].tag; }                  // aaload + igetfield
    static int aaStore(Object[] a, int i, Object v) { a[i] = v; return i; } // aastore (typecheck+barrier)

    // Marco 3.8: object allocation (new) + type checks (checkcast / instanceof).
    // new quickens to fast_new (both chain to bc_impl_new); the closure resolved AND
    // initialized Box at compile time, so jit_new is a plain call-vm allocation (may
    // GC -> flush first; may throw OutOfMemoryError -> option-B fp-check). mkBox
    // exercises the full `Foo f = new Foo(); f.field = v; return f.field` shape: new +
    // dup + invokespecial <init> (a real call to Box.<init>, non-leaf -> never inlined,
    // reusing 3.6c) + astore/aload of a reference local + int field put/get. The type
    // checks take an `Object` parameter so the compile-time type is not exact and the
    // type-info fast-path cannot elide the runtime check: isBox lowers to jit_instanceof
    // (0/1, no throw), castBox to jit_checkcast (throws ClassCastException on a bad cast,
    // unwinding option-B to an interpreted try/catch) followed by a field load. isAnimal
    // proves the subtype walk succeeds across a real hierarchy (Dog extends Animal).
    static final class Box { int v; }
    static int mkBox(int v)        { Box b = new Box(); b.v = v; return b.v; } // new+dup+<init>+astore+put/get
    static boolean isBox(Object o)    { return o instanceof Box; }            // instanceof (Object param)
    static boolean isAnimal(Object o) { return o instanceof Animal; }         // instanceof (subtype hierarchy)
    static int castBox(Object o)      { return ((Box)o).v; }                  // checkcast + field load

    // Fase 5 (bail-limpo self-test): each of these leaves contains exactly ONE
    // bytecode the r5900 backend does not yet emit -- newarray (mkArr) and idiv
    // (divBy). The compile trigger's whitelist admits them via dedicated Fase-5
    // test entries, so each IS armed and the backend STARTS compiling it, then
    // reaches new_basic_array (a whole-method bail-out) / int_binary_do's div
    // default (a mid-method switch-default bail-out). Both now call
    // Compiler::abort_active_compilation cleanly instead of SHOULD_NOT_REACH_HERE:
    // the partial compiled code is discarded and the method runs interpreted. A
    // correct result here -- with the rest of the harness reaching ALL PASS rather
    // than hanging/resetting -- proves the bail is clean and does not disturb the
    // covered leaves, which still compile normally.
    static int mkArr(int n) { int[] a = new int[n]; return a.length; }  // newarray -> new_basic_array
    static int divBy(int a) { return a / 3; }                           // idiv -> int_binary_do default

    // FASE 6 de-risk: compile a method that HAS an exception table (try/catch) and
    // catches its OWN exception. The array access throws INSIDE the compiled code, and
    // the compiled method's own handler -- not an interpreted caller as in every prior
    // marco -- must catch it. That requires the hybrid to deopt to the interpreter at the
    // handler bci: find_exception_frame checks each frame's table by bci (is_compiled_
    // frame()=false, so our frame is treated interpreted), and jit_throw_* now parks the
    // throwing-op bci as the safepoint bcp so the RIGHT handler matches. catchOOB/catchNPE
    // use ONLY parameters (the handler reads no MODIFIED local), isolating the deopt-to-
    // handler mechanism. catchMod additionally sets a local BEFORE the throwing op and its
    // handler reads it back -- that needs a pre-throw frame flush (a distinct concern); if
    // catchMod is wrong while catchOOB/catchNPE pass, the flush is the only thing missing.
    static int catchOOB(int[] a, int i) {
        try { return a[i]; }                // AIOOBE (or NPE if a==null) thrown in compiled code
        catch (Exception e) { return -1; }  // caught by THIS method's own handler
    }
    static int catchNPE(int[] a) {
        try { return a[0]; }                // a==null -> NPE from the compiled array_check
        catch (Exception e) { return -2; }
    }
    static int catchMod(int[] a, int i) {
        int r = 99;                         // set before the try (lazy in a register)
        try { r = a[i]; }                   // on throw, the handler must still see r==99
        catch (Exception e) { r = r + 7; }  // reads r -> needs the pre-throw frame flush
        return r;
    }
    // FASE 6: an immediate local set BEFORE an INVOKE, read in the handler when the
    // CALLEE throws. Exercises the immediate-materialization at CodeGenerator::invoke's
    // flush site (mips_prepare_flush_for_handler), a distinct site from the array/null
    // throw. throwIf is non-leaf (RuntimeException.<init>) so it is a REAL call, not
    // inlined -> catchInvoke compiles with a jit_invoke_static and its own exc-table.
    static int throwIf(int a) {
        if (a < 0) { throw new RuntimeException(); }
        return a + a;
    }
    static int catchInvoke(int a) {
        int r = 88;                         // immediate local, live across the invoke
        try { r = throwIf(a); }             // callee throws when a<0 (r must stay 88)
        catch (Exception e) { r = r + 3; }  // reads r -> needs the pre-invoke flush
        return r;
    }
    // FASE 6: an immediate local (k) live across a LOOP (timer-tick flush per back-edge)
    // and a throwing array access, read in the handler. Exercises the materialization on
    // the loop back-edge + array-check throw in a handler-bearing method.
    static int catchLoop(int[] a) {
        int k = 77;                         // immediate, untouched by the loop body
        int s = 0;
        try { for (int i = 0; i <= a.length; i++) s += a[i]; }  // AIOOBE at i==length
        catch (Exception e) { return s + k; }                   // reads k=77 + full sum
        return s;
    }
    // FASE 6 catchLoop ISOLATION (V1/V2): the compiled catchLoop returns garbage and
    // never throws (no [CL-RT] fires) at i==a.length. These two minimal variants split
    // "loop structure" from "index==length boundary value", so the log alone decides
    // whether the fix is a boundary nit or a loop-codegen task. For EACH variant, cross-
    // reference its [JIT-DIAG] (must be COMPILED, not NOCODE) with the returned value and
    // whether [CL-RT] fires (fires only when the compiled self-catch handler actually runs).
    //   V1 catchIdxLen: a VARIABLE index equal to a.length, self-catch, but NO loop.
    //     -99 (+[CL-RT]) => the boundary check works outside a loop -> catchLoop's bug is
    //     loop-specific. Garbage (no [CL-RT]) => the index==length boundary is broken in
    //     general, independent of any loop (note catchOOB(arrG,10) with index>length works,
    //     so a garbage here would pin the fault at index==length exactly).
    static int catchIdxLen(int[] a) {
        int idx = a.length;                 // variable index == length (out of bounds)
        try { return a[idx]; }              // a[length] -> AIOOBE if the compiled check fires
        catch (Exception e) { return -99; } // self-caught
    }
    //   V2 catchLoopMin: the MINIMAL loop (no immediate k, no accumulator) -- just the
    //     induction variable i running 0..length inclusive and a bare a[i] read.
    //     -98 (+[CL-RT]) => the loop bounds check works and catchLoop's bug is the k/
    //     accumulator interaction. Garbage (no [CL-RT]) => the loop itself reproduces the
    //     bug -> a clean minimal repro for the loop-index/length codegen fix.
    static int catchLoopMin(int[] a) {
        int x = 0;
        try { for (int i = 0; i <= a.length; i++) x = a[i]; }  // AIOOBE at i==length
        catch (Exception e) { return -98; }                    // self-caught
        return x;                                              // dead unless the check misses
    }

    // Fase 6 (static INT fields): getstatic/putstatic of a 32-bit static field. The class
    // BASE is materialized GC-safely via the class_list indirection (CodeGenerator::move(
    // Value&,Oop*) == get_class_by_id) -- no moveable oop baked into native code -- then
    // load/store at the static field offset. The compiler resolves the holder at COMPILE
    // time (get_klass_or_null_from_id -> uncommon_trap/bail if not yet initialized), so the
    // class-init barrier is honored without runtime code. Non-final so the load path runs (a
    // final static int folds to a compile-time immediate, exercising nothing new).
    //   COVERAGE (proven on PCSX2, 2026-07-19):
    //   - sPut COMPILED: putstatic + getstatic of THIS class's field (own-class class_list base).
    //   - sGetOther COMPILED: getstatic of ANOTHER class's field -- proves cross-class class_id
    //     resolution (the whole point of the indirection). It quickens to _fast_init_1_getstatic
    //     (the holder is not initialized when the ROM image rewrites it), which is why the
    //     whitelist must accept the _fast_init_* variants (the compiler lowers them identically).
    //   - sGet is NOT a compiled leaf: the ROM inliner (ROMInliner::try_inline_static_getter)
    //     inlines an own-class static getter of code_size 4 straight into its caller, so sGet()
    //     is never invoked and never arms -- its getstatic runs INLINE in the (interpreted) test
    //     method. The compiled own-class getstatic path is already covered by sPut's "return sBox".
    //     Kept as a value sanity check, not a JIT check (an own-class getter is always inlined).
    static int sReadOnly = 100;   // read-only static int (sGet, inlined by romizer); never written
    static int sBox = 0;          // static int written by sPut
    static int sGet()        { return sReadOnly; }        // getstatic -> 100
    static int sPut(int v)   { sBox = v; return sBox; }   // putstatic + getstatic -> v
    static final class SHolder { static int val = 77; }   // a separate holder class
    static int sGetOther()   { return SHolder.val; }      // getstatic of another class -> 77

    // Reference branches (ISA next op -- firstbad #1 on the Asphalt sampler, ~35% of hot interp
    // time): ifnull/ifnonnull compare an oop against null, if_acmpeq/if_acmpne compare two oops
    // for identity. javac lowers the ternaries below to (respectively) ifnonnull / ifnull /
    // if_acmpne / if_acmpeq, so the four cover all four opcodes. They lower through the SAME
    // shared branch_if as the int forms; the backend already emits null/nonnull and reuses eq/ne.
    static int refIsNull(Object o)         { return o == null ? 1 : 0; }    // ifnonnull
    static int refNotNull(Object o)        { return o != null ? 1 : 0; }    // ifnull
    static int refSame(Object a, Object b) { return a == b ? 10 : 20; }     // if_acmpne
    static int refDiff(Object a, Object b) { return a != b ? 10 : 20; }     // if_acmpeq

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

    // Fase 5 PASSO 2B: raw JIT-vs-interp TIMING probe. A pure-integer compute loop
    // with NO invokes and NO allocation -- every bytecode (iconst/iload/istore/iadd/
    // isub/imul/ixor/ishl/iand/iinc/if_icmp/goto) is on the Fase-3 whitelist, so it
    // arms and compiles fully. One method (boundary cost amortized over millions of
    // iterations) + a counted loop isolates the loop back-edge cost: this is where
    // CodeGenerator::check_timer_tick emits a full frame flush + an unconditional C
    // call PER ITERATION. If compiled is not markedly faster than interpreted here,
    // that per-iteration flush+call is the bottleneck (the JIT's best case crippled).
    static int bench(int iters) {
        int acc = 0;
        for (int i = 0; i < iters; i++) {
            acc += i * 3;
            acc ^= (acc << 1);
            acc -= (i & 15);
        }
        return acc;
    }

    // Time bench() interpreted (first call: armed but runs interpreted) vs compiled
    // (after warming: r5900 code). Same big-N call in both states -> the delta is the
    // codegen/back-edge speedup, isolated from game/boundary noise.
    static void runBench() {
        final int BIG = 2000000;
        long t0 = System.currentTimeMillis();
        int b1 = bench(BIG);                     // 1st call: interpreted
        long t1 = System.currentTimeMillis();
        for (int w = 0; w < 20; w++) bench(200); // warm -> arm -> compile -> run compiled
        long t2 = System.currentTimeMillis();
        int b2 = bench(BIG);                     // now r5900-compiled
        long t3 = System.currentTimeMillis();
        long interpMs = t1 - t0, jitMs = t3 - t2;
        System.out.println("[JIT-BENCH] bench(" + BIG + "): interp=" + interpMs
            + "ms jit=" + jitMs + "ms  result=" + b1 + "/" + b2
            + (b1 == b2 ? " OK" : " MISMATCH"));
        if (jitMs > 0) {
            System.out.println("[JIT-BENCH] speedup(x100)=" + (interpMs * 100 / jitMs)
                + "  (>100 = JIT faster, 100 = tie)");
        }
    }

    // Fase 5 PASSO 2B: FRONTIER-cost probe. bench() above amortizes the frame
    // boundary over millions of loop iterations (one entry) and shows the codegen
    // wins ~7x. Games instead spend their time in MANY calls to tiny methods, where
    // the per-call frontier (compiled caller -> jit_invoke_static, callee
    // jit_frame_enter/jit_return -- all helper-C) is paid every time. benchCall and
    // benchInline do the SAME 9-add work per iteration; the only difference is a REAL
    // call vs inline, so (compiled call time - compiled inline time) isolates the
    // per-call frontier. leafBig is >13 bytes so the front-end NEVER inlines it (a
    // true cross-frame call, like the getters/setters that are 63% of a game's armed
    // methods). All bytecodes are whitelisted -> everything arms and compiles.
    static int leafBig(int a) { return a+a+a+a+a+a+a+a+a; }   // 9a; >13 bytes -> real call
    static int benchCall(int iters) {                          // work via a real call/iter
        int acc = 0;
        for (int i = 0; i < iters; i++) acc += leafBig(i);
        return acc;
    }
    static int benchInline(int iters) {                        // SAME work inline, no frontier
        int acc = 0;
        for (int i = 0; i < iters; i++) acc += i+i+i+i+i+i+i+i+i;
        return acc;
    }

    // Measure the per-call frontier. Warm both to the r5900-compiled state (and
    // leafBig with them), then time the same big-N in each. jitInline = loop+adds
    // compiled with NO boundary; jitCall = the same adds reached through a
    // compiled->compiled invoke every iteration. The gap is N * frontier cost. Also
    // report call-vs-interp so the raw JIT-vs-interp number for a call-bound loop (the
    // game shape) sits next to bench()'s 7x loop-bound one. NOTE: the interp baseline
    // is conservative -- during it, leafBig warms mid-call and starts running compiled,
    // which only makes interp look FASTER, so a call-speedup <= ~100 still proves the
    // frontier dominates.
    static void runBenchCall() {
        final int BIG = 2000000;
        long t0 = System.currentTimeMillis();
        int ci = benchCall(BIG);                          // benchCall interpreted (compile switches next entry)
        long t1 = System.currentTimeMillis();
        for (int w = 0; w < 30; w++) { benchInline(300); benchCall(300); } // warm both + leafBig
        long t2 = System.currentTimeMillis();
        int inl = benchInline(BIG);                       // compiled, no frontier
        long t3 = System.currentTimeMillis();
        int cc = benchCall(BIG);                          // compiled, real call/iter
        long t4 = System.currentTimeMillis();
        long callInterpMs = t1 - t0, inlineMs = t3 - t2, callJitMs = t4 - t3;
        boolean ok = (ci == cc && cc == inl);
        System.out.println("[JIT-FRONTIER] benchCall(" + BIG + "): interp=" + callInterpMs
            + "ms jitInline=" + inlineMs + "ms jitCall=" + callJitMs
            + "ms  result=" + ci + "/" + inl + "/" + cc + (ok ? " OK" : " MISMATCH"));
        if (inlineMs > 0) {
            System.out.println("[JIT-FRONTIER] call/inline(x100)=" + (callJitMs * 100 / inlineMs)
                + "  (100 = frontier free; higher = frontier dominates)");
        }
        if (callJitMs > 0) {
            System.out.println("[JIT-FRONTIER] call speedup vs interp(x100)="
                + (callInterpMs * 100 / callJitMs) + "  (>100 = JIT faster)");
        }
        long deltaMs = callJitMs - inlineMs;
        System.out.println("[JIT-FRONTIER] frontier ~= " + (deltaMs * 1000000L / BIG)
            + " ns/call (delta=" + deltaMs + "ms over " + BIG + " calls)");
    }

    public static void main(String[] args) {
        System.out.println("[JIT-TEST] Fase 3 Marco 3.8 + Fase 5 bail-limpo harness start");
        runBench();
        runBenchCall();

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
        // Marco 3.7a: object/reference field leaves. n2 is the container whose
        // reference field 'link' is written (write barrier) and read back.
        Node n1 = new Node(); n1.tag = 71;
        Node n1b = new Node(); n1b.tag = 73;
        Node n2 = new Node();
        int lt = 0, glt = 0;
        // Marco 3.7b: object-array leaves. narr is a real Node[]; oarr is an Object[]
        // (so aaStore's type check is not elided); narrAsObj aliases narr as Object[]
        // to force an ArrayStoreException when a String is stored into it.
        Node[] narr = { new Node(), new Node(), new Node() };
        narr[0].tag = 61; narr[1].tag = 62; narr[2].tag = 63;
        Object[] oarr = new Object[3];
        Object[] narrAsObj = narr;
        Node someNode = new Node(); someNode.tag = 88;
        int aag = 0, aas = 0;
        // Marco 3.8: object allocation + type checks. Box is instantiated here first so
        // it is INITIALIZED before mkBox is armed (the new_object closure needs an
        // initialized class or it takes uncommon_trap). boxObj holds a Box seen as
        // Object (non-exact type -> the type-info check-elision does not fire); strObj
        // is a non-Box reference for the negative / ClassCastException cases.
        Box boxWarm = new Box(); boxWarm.v = 5;
        Object boxObj = boxWarm;
        Object strObj = "hello";
        int mb = 0, cbx = 0;
        boolean ibx = false, ianm = false;
        // Fase 5 bail-limpo self-test leaves (armed but bail-out -> interpreted).
        int mar = 0, dvb = 0;
        // FASE 6 de-risk: exception-table (try/catch) leaves, warmed on the no-throw path.
        int cOOB = 0, cNPE = 0, cMod = 0, cInv = 0, cLp = 0;
        // FASE 6 catchLoop isolation (V1 no-loop idx==len / V2 minimal loop). Both ALWAYS
        // take the throwing path (idx==length / i reaches length), so the warmed value is
        // the compiled self-catch result: -99 / -98 if the check fires, garbage if it misses.
        int cIL = 0, cLm = 0;
        // Fase 6 static-field accumulators. Touch SHolder.val here to run SHolder.<clinit>
        // BEFORE sGetOther is compiled: the holder must be INITIALIZED at compile time or
        // get_klass_or_null_from_id returns null and the compile bails (uncommon_trap). This
        // does NOT change the quickened form -- sGetOther stays _fast_init_1_getstatic (the ROM
        // image rewrote it before any class was initialized); the whitelist accepts that variant.
        int sg = 0, sp = 0, sgo = 0;
        int shInit = SHolder.val;   // ensure SHolder is initialized before sGetOther compiles
        // Reference-branch accumulators (ifnull/ifnonnull/if_acmpeq/if_acmpne).
        int rbn = 0, rbnn = 0, rbs = 0, rbd = 0;
        // Warm the bail-out leaves FIRST, before the covered leaves fill/pressure the
        // compiler area, so they definitely reach the backend and bail. The bail is
        // PERMANENT (is_permanent=true -> impossible_to_compile), so each bails ONCE
        // then runs interpreted -- no per-invocation re-compile that would churn the
        // compiler area and evict the covered leaves' compiled code. A few iterations
        // are enough: arm, compile+bail, confirm interpreted.
        for (int i = 0; i < 4; i++) { mar = mkArr(5); dvb = divBy(30); }
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
            lt = linkTag(n2, n1);      // n2.link = n1 (write barrier); return n1.tag = 71
            glt = getLinkTag(n2);      // n2.link.tag = 71 (read reference field)
            aag = aaGet(narr, 1);      // narr[1].tag = 62 (aaload + field deref)
            aas = aaStore(oarr, 0, someNode); // oarr[0] = someNode (aastore: typecheck+barrier)
            mb = mkBox(42);            // new Box(); b.v=42; return b.v (new + <init> + field)
            ibx = isBox(boxObj);       // Box instanceof Box -> true
            ianm = isAnimal(dog);      // Dog instanceof Animal -> true (subtype)
            cbx = castBox(boxObj);     // (Box)boxObj).v = 5 (checkcast + field)
            cOOB = catchOOB(arrG, 3);  // FASE 6: normal path arrG[3]=40 (arms exc-table method)
            cNPE = catchNPE(arrG);     // FASE 6: normal path arrG[0]=10
            cMod = catchMod(arrG, 3);  // FASE 6: normal path arrG[3]=40
            cInv = catchInvoke(5);     // FASE 6: normal path throwIf(5)=10 (arms exc-table method)
            cLp  = catchLoop(arrG);    // FASE 6: normal path -> AIOOBE at i==len -> 150+77=227
            cIL  = catchIdxLen(arrG);  // FASE 6 V1: a[a.length] -> AIOOBE self-catch -> -99
            cLm  = catchLoopMin(arrG); // FASE 6 V2: minimal loop -> AIOOBE at i==len -> -98
            sg   = sGet();             // Fase 6: getstatic sReadOnly -> 100
            sp   = sPut(55);           // Fase 6: putstatic sBox=55 + getstatic -> 55
            sgo  = sGetOther();        // Fase 6: getstatic SHolder.val -> 77 (cross-class)
            rbn  = refIsNull(boxObj);  // ref-branch ifnonnull: boxObj != null -> 0
            rbnn = refNotNull(boxObj); // ref-branch ifnull:    boxObj != null -> 1
            rbs  = refSame(boxObj, boxObj); // ref-branch if_acmpne: same -> 10
            rbd  = refDiff(boxObj, strObj); // ref-branch if_acmpeq: diff -> 10
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

        // Marco 3.7a: object/reference field load/store (compiled). linkTag stored
        // n1 into n2.link via the write barrier and read it back through the stored
        // pointer; getLinkTag is a read-only reference deref.
        check("linkTag(n2,n1)",  lt,  71);                // n2.link=n1 (barrier), n1.tag=71
        check("getLinkTag(n2)",  glt, 71);                // n2.link.tag=71
        check("getLinkTag(n2) direct", getLinkTag(n2), 71);   // n2.link still n1
        // Relink to a different node -> the write barrier runs again and overwrites.
        check("linkTag(n2,n1b)", linkTag(n2, n1b), 73);   // n2.link=n1b (barrier), n1b.tag=73
        check("getLinkTag(n2) after relink", getLinkTag(n2), 73);  // n2.link=n1b now
        // The compiled reference null_check must throw NPE and unwind to this
        // interpreted try/catch (same helper-C mechanism as 3.4a / 3.5).
        boolean aFldNpe = false;
        try {
            getLinkTag(null);
        } catch (NullPointerException e) {
            aFldNpe = true;
        }
        check("getLinkTag(null) throws NPE", aFldNpe ? 1 : 0, 1);
        check("getLinkTag(n2) post-throw", getLinkTag(n2), 73);   // resumes clean

        // Marco 3.7b: object-array element load/store (compiled).
        check("aaGet(narr,1)", aag, 62);                 // aaload + field deref
        check("aaGet(narr,2)", aaGet(narr, 2), 63);
        check("aaStore(oarr,0,someNode)", aas, 0);       // aastore into Object[] (returned i)
        check("oarr[0] stored", oarr[0] == someNode ? 1 : 0, 1);   // driver verifies the store landed
        check("aaStore(oarr,1,null)", aaStore(oarr, 1, null), 1);  // null value: type-OK, no throw
        // Type-compatible store into a real Node[] (aliased as Object[]) succeeds.
        check("aaStore(narrAsObj,0,someNode)", aaStore(narrAsObj, 0, someNode), 0);
        check("narr[0] retargeted", narr[0] == someNode ? 1 : 0, 1);
        // The compiled array-store type check must throw ArrayStoreException for an
        // incompatible element (a String into a Node[]) and unwind (option-B) to this
        // interpreted try/catch. A miss (no throw / crash) fails or hangs.
        boolean aseCaught = false;
        try {
            aaStore(narrAsObj, 1, "not a node");
        } catch (ArrayStoreException e) {
            aseCaught = true;
        }
        check("aaStore(Node[],String) throws ASE", aseCaught ? 1 : 0, 1);
        check("aaStore post-throw", aaStore(narrAsObj, 2, someNode), 2);  // resumes clean
        // null array reuses the 3.4b array_check null path (before the type check).
        boolean aaNpe = false;
        try { aaStore(null, 0, someNode); } catch (NullPointerException e) { aaNpe = true; }
        check("aaStore(null,..) throws NPE", aaNpe ? 1 : 0, 1);
        boolean aagNpe = false;
        try { aaGet(null, 0); } catch (NullPointerException e) { aagNpe = true; }
        check("aaGet(null,0) throws NPE", aagNpe ? 1 : 0, 1);

        // Marco 3.8: object allocation (new) -- mkBox allocated a Box, ran its <init>,
        // stored a field and read it back, all from compiled code.
        check("mkBox(42)",     mb, 42);
        check("mkBox(7)",      mkBox(7), 7);          // fresh allocation, other value
        // instanceof: 0/1 result, no throw. Object parameter -> the compiler cannot
        // elide the runtime check, so jit_instanceof actually runs.
        check("isBox(box)",    ibx ? 1 : 0, 1);
        check("isBox(str)",    isBox(strObj) ? 1 : 0, 0);      // String is not a Box
        check("isBox(null)",   isBox(null) ? 1 : 0, 0);        // null is never an instance
        check("isAnimal(dog)", ianm ? 1 : 0, 1);               // Dog extends Animal (subtype walk)
        check("isAnimal(str)", isAnimal(strObj) ? 1 : 0, 0);
        // checkcast: a good cast passes through to the field load; a null passes the cast.
        check("castBox(box)",  cbx, 5);
        check("castBox(box) direct", castBox(boxObj), 5);
        // The compiled checkcast must throw ClassCastException for an incompatible type
        // and unwind (option-B) out of the compiled leaf to this interpreted try/catch.
        boolean cceCaught = false;
        try {
            castBox(strObj);   // a String cannot be cast to Box
        } catch (ClassCastException e) {
            cceCaught = true;
        }
        check("castBox(str) throws CCE", cceCaught ? 1 : 0, 1);
        check("castBox(box) post-throw", castBox(boxObj), 5);  // resumes clean

        // Fase 5 (bail-limpo): a method containing an unsupported bytecode is armed,
        // the backend bails the compilation cleanly, and it runs interpreted with the
        // right answer -- no crash, no corruption. Whole-method bail (newarray ->
        // new_basic_array) and mid-method switch-default bail (idiv -> int_binary_do).
        // The re-runs confirm the method stays runnable after repeated bail-outs.
        check("mkArr(5) bails clean",  mar, 5);
        check("divBy(30) bails clean", dvb, 10);
        check("mkArr(9) bails clean",  mkArr(9), 9);
        check("divBy(99) bails clean", divBy(99), 33);

        // FASE 6 de-risk: a COMPILED method with an exception table catches its OWN
        // exception. catchOOB/catchNPE (params only) isolate the deopt-to-handler path:
        // an AIOOBE/NPE thrown inside the compiled code must resume the interpreter at
        // this method's handler bci and return the handler's value. If these PASS, the
        // hybrid exception-table mechanism works (the #1 gate for the Asphalt hot path).
        check("catchOOB(arrG,3) normal",      cOOB, 40);              // compiled, no throw
        check("catchOOB(arrG,10) self-catch", catchOOB(arrG, 10), -1); // AIOOBE caught in-method
        check("catchOOB(null,0) self-catch",  catchOOB(null, 0), -1);  // NPE caught in-method
        check("catchOOB(arrG,2) post-catch",  catchOOB(arrG, 2), 30);  // resumes clean after catch
        check("catchNPE(arrG) normal",        cNPE, 10);
        check("catchNPE(null) self-catch",    catchNPE(null), -2);     // NPE caught in-method
        // catchMod reads a local set BEFORE the throw -> also needs the pre-throw frame
        // flush. If this FAILS while the catchOOB/catchNPE cases PASS, the deopt-to-handler
        // core works and only the local flush is missing (the next, separate step).
        check("catchMod(arrG,3) normal",      cMod, 40);              // no throw
        check("catchMod(arrG,10) needs-flush", catchMod(arrG, 10), 106); // 99 (pre-throw) + 7
        // FASE 6: try-around-INVOKE. catchInvoke(-1) -> throwIf(-1) throws -> handler reads
        // the pre-invoke immediate r==88 -> 91. Proves the invoke-flush materialization.
        check("catchInvoke(5) normal",        cInv, 10);              // throwIf(5)=10, no throw
        check("catchInvoke(-1) callee-throws", catchInvoke(-1), 91);  // 88 (pre-invoke) + 3
        // FASE 6: immediate (k=77) live across a loop + throwing array access.
        check("catchLoop(arrG) normal",       cLp, 227);              // 150 (sum) + 77 (k)
        check("catchLoop(arrG) again",        catchLoop(arrG), 227);
        // FASE 6 catchLoop ISOLATION (V1/V2): a FAIL here is EXPECTED data. The returned
        // value + whether [CL-RT] fired (and [JIT-DIAG] COMPILED) localizes the bug:
        //   V1 == -99  -> boundary index==length throws OUTSIDE a loop  => loop-specific.
        //   V2 == -98  -> the minimal loop throws                        => k/accumulator.
        //   garbage    -> the compiled bounds check missed on that shape (see method docs).
        check("catchIdxLen(arrG) V1 no-loop idx==len", cIL, -99);
        check("catchLoopMin(arrG) V2 minimal-loop",    cLm, -98);
        check("catchIdxLen(arrG) V1 again",            catchIdxLen(arrG), -99);
        check("catchLoopMin(arrG) V2 again",           catchLoopMin(arrG), -98);

        // Fase 6 (static INT fields): getstatic/putstatic compiled with a GC-safe class
        // base (class_list indirection). sPut(v)=v / sGetOther=77 reading/writing the correct
        // slot PROVES the class base resolved right (a stale/embedded oop base would read or
        // clobber garbage). Cross-check [JIT-DIAG]: sPut and sGetOther must be COMPILED (not
        // NOCODE) -- those are the JIT. sGet is romizer-INLINED (own-class getter, see above),
        // so its getstatic runs interpreted inline in this method; sGet=100 is only a value
        // sanity check. The compiled own-class getstatic path is proven by sPut's "return sBox".
        check("sGet() static read",      sg, 100);
        check("sPut(55) static write",   sp, 55);
        check("sBox after sPut",         sBox, 55);
        check("sGetOther() cross-class", sgo, 77);
        check("sGet() again",            sGet(), 100);
        check("sPut(99) fresh",          sPut(99), 99);
        check("shInit sanity",           shInit, 77);

        // Reference branches (ifnull/ifnonnull/if_acmpeq/if_acmpne) compiled with the r5900
        // backend's null/nonnull + eq/ne cond_ops. The warmed values took the non-null / distinct
        // path; the direct calls below take the OPPOSITE path so both polarities of each emitted
        // beq/bne run. Cross-check [JIT-DIAG] shows refIsNull/refNotNull/refSame/refDiff COMPILED.
        check("refIsNull(box) warm",     rbn, 0);      // ifnonnull, non-null -> 0
        check("refNotNull(box) warm",    rbnn, 1);     // ifnull, non-null -> 1
        check("refSame(box,box) warm",   rbs, 10);     // if_acmpne, same -> 10
        check("refDiff(box,str) warm",   rbd, 10);     // if_acmpeq, distinct -> 10
        check("refIsNull(null)",         refIsNull(null), 1);         // null -> 1
        check("refNotNull(null)",        refNotNull(null), 0);        // null -> 0
        check("refSame(box,str)",        refSame(boxObj, strObj), 20);// distinct -> 20
        check("refDiff(box,box)",        refDiff(boxObj, boxObj), 20);// same -> 20

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
