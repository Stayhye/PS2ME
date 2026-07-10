// InterpBench -- FASE 1 host interpreter throughput benchmark (PS2ME perf front).
//
// CLDC 1.1-safe (no MIDP, no I/O, no floats-by-default): pure integer/array/alloc/
// virtual-dispatch work so the run cost is ~100% interpreter dispatch + a little GC,
// mirroring what the memory pinned as the game loading/in-game bottleneck (bytecode).
//
// Determinism: every kernel is seeded/fixed, so the executed-bytecode COUNT is identical
// across interpreter variants; only wall/CPU time and the native bytecode counter change.
// Each kernel returns a checksum that is XOR-accumulated and printed, so nothing is dead.
//
// Usage: cldc_vm ... InterpBench [scale]   (scale multiplies iteration counts; default 1)
public class InterpBench {

    // ---- dispatch-bound kernels (tight arithmetic / branch / array loops) ----

    static long sieve(int limit) {
        boolean[] composite = new boolean[limit];
        long count = 0;
        for (int i = 2; i < limit; i++) {
            if (!composite[i]) {
                count++;
                for (int j = i + i; j < limit; j += i) composite[j] = true;
            }
        }
        return count;
    }

    static long fib(int n) {
        long a = 0, b = 1;
        for (int i = 0; i < n; i++) { long t = a + b; a = b; b = t; }
        return a;
    }

    // xorshift PRNG -- shifts, xors, masks: dense arithmetic bytecode.
    static long xorshift(int iters) {
        long x = 0x9E3779B97F4A7C15L;
        long acc = 0;
        for (int i = 0; i < iters; i++) {
            x ^= x << 13; x ^= x >>> 7; x ^= x << 17;
            acc += x & 0xFFFF;
        }
        return acc;
    }

    // insertion sort over an int[] -- array loads/stores + comparisons + branches.
    static long sortWork(int size, int rounds) {
        int[] a = new int[size];
        long checksum = 0;
        int seed = 12345;
        for (int r = 0; r < rounds; r++) {
            for (int i = 0; i < size; i++) { seed = seed * 1103515245 + 12345; a[i] = seed >>> 8; }
            for (int i = 1; i < size; i++) {
                int key = a[i], j = i - 1;
                while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
                a[j + 1] = key;
            }
            checksum += a[0] ^ a[size >> 1] ^ a[size - 1];
        }
        return checksum;
    }

    // ---- alloc/GC-bound kernel (lots of short-lived small objects) ----

    static final class Node { int v; Node next; Node(int v, Node n) { this.v = v; this.next = n; } }

    static long allocWork(int nodes, int rounds) {
        long checksum = 0;
        for (int r = 0; r < rounds; r++) {
            Node head = null;
            for (int i = 0; i < nodes; i++) head = new Node(i ^ r, head);
            long s = 0;
            for (Node p = head; p != null; p = p.next) s += p.v;
            checksum += s;               // head goes dead -> next GC reclaims it
        }
        return checksum;
    }

    // ---- invoke-bound kernel (polymorphic virtual dispatch) ----

    static abstract class Shape { abstract int area(int x); }
    static final class Sq  extends Shape { int area(int x) { return x * x; } }
    static final class Tri extends Shape { int area(int x) { return (x * x) >> 1; } }
    static final class Lin extends Shape { int area(int x) { return x + x; } }

    static long invokeWork(int iters) {
        Shape[] shapes = new Shape[] { new Sq(), new Tri(), new Lin() };
        long acc = 0;
        for (int i = 0; i < iters; i++) acc += shapes[i % 3].area(i & 0xFFF);   // megamorphic-ish call site
        return acc;
    }

    static void report(String name, long start, long checksum) {
        long ms = System.currentTimeMillis() - start;
        System.out.println("[BENCH] " + name + " ms=" + ms + " chk=" + checksum);
    }

    public static void main(String[] args) {
        int scale = 1;
        if (args != null && args.length > 0) {
            try { scale = Integer.parseInt(args[0]); } catch (Exception e) {}
        }
        if (scale < 1) scale = 1;
        System.out.println("[BENCH] InterpBench start scale=" + scale);

        // Kernels are calibrated to ~1s each at scale=1 on the host so every one
        // carries measurable weight. Accumulators use += (not ^=) so a fixed-value
        // kernel repeated an even number of times does not XOR-cancel to 0.
        long chk = 0, t;

        t = System.currentTimeMillis();
        long s1 = 0; for (int r = 0; r < 110 * scale; r++) s1 += sieve(20000);
        report("sieve",   t, s1); chk += s1;

        t = System.currentTimeMillis();
        long s2 = 0; for (int r = 0; r < 4000 * scale; r++) s2 += fib(1000);
        report("fib",     t, s2); chk += s2;

        t = System.currentTimeMillis();
        long s3 = xorshift(2200000 * scale);
        report("xorshift",t, s3); chk += s3;

        t = System.currentTimeMillis();
        long s4 = sortWork(512, 130 * scale);
        report("sort",    t, s4); chk += s4;

        t = System.currentTimeMillis();
        long s5 = allocWork(4000, 660 * scale);
        report("alloc",   t, s5); chk += s5;

        t = System.currentTimeMillis();
        long s6 = invokeWork(3500000 * scale);
        report("invoke",  t, s6); chk += s6;

        System.out.println("[BENCH] InterpBench done total_chk=" + chk);
    }
}
