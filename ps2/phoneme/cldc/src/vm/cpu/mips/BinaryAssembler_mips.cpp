/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_BinaryAssembler_mips.cpp.incl"

// Fase 1/2: real r5900 emission. `mov` and the label-binding below feed the
// encoder into the VM code buffer. The forward-branch chain machinery (jmp to an
// unbound Label) stays a bail-out stub until Fase 3 -- it is co-designed with
// r5900 branch emission (delay slots + imm16 offset patching). All dormant
// (UseCompiler=false) until a method is actually compiled.

// register move: `or dst, src, zero` (canonical MIPS reg-reg copy).
void BinaryAssembler::mov(Register dst, Register src) {
  emit(Assembler::encode_or(dst, src, Assembler::zero));
}

// Buffer accessors used by label back-patching (delegate to BinaryAssemblerCommon,
// which reads/writes the CompiledMethod's code field at a code offset).
jint BinaryAssembler::long_at(const int position) const {
  return int_at(position);
}
void BinaryAssembler::long_at_put(const int position, const jint value) const {
  int_at_put(position, value);
}

void BinaryAssembler::jmp(Label& L) {
  (void)L;
  SHOULD_NOT_REACH_HERE();
}

void BinaryAssembler::jmp(CompilationQueueElement* cqe) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

// Bind label L to the current code offset. In the Fase 2 trivial-method path the
// only label bound is the unused method-entry label (no forward references), so
// there is no link chain to patch. The r5900 forward-branch chain patching
// (walk the chain, write each branch's imm16 = (target-(pos+4))>>2) is
// co-designed with real branch emission and arrives in Fase 3; the GUARANTEE
// trips if an unbound label ever reaches here before then.
void BinaryAssembler::bind_to(Label& L, jint code_offset) {
  GUARANTEE(!L.is_unbound(),
            "r5900 forward-branch chain patching arrives in Fase 3");
  L.bind_to(code_offset);
}

void BinaryAssembler::bind_to(NearLabel& L, int code_offset) {
  GUARANTEE(!L.is_unbound(),
            "r5900 forward-branch chain patching arrives in Fase 3");
  L.bind_to(code_offset);
}

void BinaryAssembler::bind(Label& L, int alignment) {
  if (alignment > 0) {
    while ((code_size() % alignment) != 0) {
      emit(Assembler::encode_nop());
    }
  }
  bind_to(L, _code_offset);
}

#if defined(PS2ME_JIT_SELFTEST)
// ---------------------------------------------------------------------------
// Fase 1 milestone harness ("returns 42"). Opt-in via -DPS2ME_JIT_SELFTEST
// (ps2_mips.cfg, alongside PS2ME_JIT). Called once from JVM::initialize (patch
// #36). It emits a real r5900 function AT RUNTIME using the pure encoders,
// performs the self-modifying-code cache maintenance the EE requires, calls the
// emitted code, and verifies the result -- validating the whole emission base
// (encoders + I-cache flush) before the 6k-line CodeGenerator depends on it.
//
// This deliberately does NOT go through the VM code buffer / CompiledMethod /
// relocation machinery (that is Fase 2). It writes a private scratch buffer, so
// the base is provable in isolation.
//
// Delay slot: `jr ra` executes its following instruction (the slot) before the
// jump takes effect. We fill it with an explicit nop -- the correct, minimal
// discipline. Automatic slot filling (moving a useful instruction into the slot)
// is a Fase 2 optimization on the branch emitter, not needed for correctness.

// share/runtime/OsMisc.hpp -- C++ linkage; forward-declared to avoid pulling the
// header through the includeDB (the symbol resolves at link on the EE target,
// where it routes to javacall_os_flush_icache -> FlushCache WRITEBACK/INVALIDATE).
void OsMisc_flush_icache(address start, int size);

// Scratch code buffer. EE main RAM (cached KSEG0) is executable -- no W^X on the
// r5900 -- so we can jump straight into a data array once the caches agree.
static int ps2me_jit_selftest_code[8] __attribute__((aligned(16)));

void ps2me_jit_selftest(void) {
  int* c = ps2me_jit_selftest_code;
  int  n = 0;
  c[n++] = (int) Assembler::encode_ori(Assembler::v0, Assembler::zero, 42); // v0 = 42
  c[n++] = (int) Assembler::encode_jr (Assembler::ra);                      // return
  c[n++] = (int) Assembler::encode_nop();                                   //   delay slot

  OsMisc_flush_icache((address) c, n * BytesPerInt);

  int (*emitted)(void) = (int (*)(void)) c;
  const int result = emitted();

  if (result == 42) {
    tty->print_cr("[PS2ME-JIT] Fase 1 selftest: emitted r5900 returned %d (OK)", result);
  } else {
    tty->print_cr("[PS2ME-JIT] Fase 1 selftest: FAIL -- expected 42, got %d", result);
  }
}
#endif // PS2ME_JIT_SELFTEST

#endif // ENABLE_COMPILER
