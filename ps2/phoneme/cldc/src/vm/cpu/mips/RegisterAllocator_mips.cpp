/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_RegisterAllocator_mips.cpp.incl"

// NOTE: the _register_references[] storage is defined by the shared
// RegisterAllocator.cpp (HotRoutines), not per-arch -- defining it here too
// would be a multiple-definition link error.

// Round-robin allocation chain (Fase 3). Modelled on the i386 table: each entry
// maps a register to the NEXT register the allocator will try; `no_reg` marks a
// register that is never handed out. allocate() does `next = table[next]` before
// testing, so the chain must be a single cycle over the allocatable set.
//
// Allocatable integer set = the caller-saved temporaries t0..t8 (9 regs). We
// deliberately exclude:
//   - zero/at/k0/k1/gp/sp/ra  : ABI / assembler reserved.
//   - v0/v1/a0..a3            : return + C-argument regs, used by return_result
//                               (a0) and mips_call_c; kept free of Java values.
//   - t9                      : the PIC call target used by mips_call_c.
//   - s0/s1/s2/s3             : PINNED interpreter state (g_jfp/g_jsp/g_jpc/
//                               g_jlocals) via the global-reg patch -- must NOT
//                               be allocated or the interpreter frame corrupts.
//   - s4..s7/s8               : callee-saved; using them would require prologue
//                               save/restore we don't emit yet (later Marco).
// t0..t8 are caller-saved, and no Java value is live across our C-helper calls
// (jit_frame_enter runs before any allocation; the result is passed in a0 to
// jit_return_int at the very end), so no save/restore is needed for them.
static const Assembler::Register
next_register_table[Assembler::number_of_registers] = {
  Assembler::no_reg, // zero
  Assembler::no_reg, // at
  Assembler::no_reg, // v0
  Assembler::no_reg, // v1
  Assembler::no_reg, // a0
  Assembler::no_reg, // a1
  Assembler::no_reg, // a2
  Assembler::no_reg, // a3
  Assembler::t1,     // t0 -> t1
  Assembler::t2,     // t1 -> t2
  Assembler::t3,     // t2 -> t3
  Assembler::t4,     // t3 -> t4
  Assembler::t5,     // t4 -> t5
  Assembler::t6,     // t5 -> t6
  Assembler::t7,     // t6 -> t7
  Assembler::t8,     // t7 -> t8   (t8 == reg 24, skips over s0..s7)
  Assembler::no_reg, // s0  (g_jfp, pinned)
  Assembler::no_reg, // s1  (g_jsp, pinned)
  Assembler::no_reg, // s2  (g_jpc, pinned)
  Assembler::no_reg, // s3  (g_jlocals, pinned)
  Assembler::no_reg, // s4
  Assembler::no_reg, // s5
  Assembler::no_reg, // s6
  Assembler::no_reg, // s7
  Assembler::t0,     // t8 -> t0   (closes the cycle)
  Assembler::no_reg, // t9  (PIC call target)
  Assembler::no_reg, // k0
  Assembler::no_reg, // k1
  Assembler::no_reg, // gp
  Assembler::no_reg, // sp
  Assembler::no_reg, // s8
  Assembler::no_reg, // ra
  // Fake FP registers (fp0..fp7): a closed cycle so allocate_float_register()
  // is well-formed, though no float bytecode compiles until Fase 4.
  Assembler::fp1,    // fp0 -> fp1
  Assembler::fp2,    // fp1 -> fp2
  Assembler::fp3,    // fp2 -> fp3
  Assembler::fp4,    // fp3 -> fp4
  Assembler::fp5,    // fp4 -> fp5
  Assembler::fp6,    // fp5 -> fp6
  Assembler::fp7,    // fp6 -> fp7
  Assembler::fp0     // fp7 -> fp0
};

// Any r5900 GPR does byte lb/sb (is_valid_byte_register is always true), so the
// byte-register rotation is the same t0..t8 cycle.
static const Assembler::Register* const next_byte_register_table =
  next_register_table;

// initialize() is called only from Compiler::begin_compile(), i.e. behind the
// UseCompiler / PS2ME_JIT gate.
void RegisterAllocator::initialize() {
  _next_register_table      = next_register_table;
  _next_byte_register_table = next_byte_register_table;

  // `allocate` advances (next = table[next]) before returning, so seeding with
  // t8 makes the first allocation t0.
  _next_allocate       = Assembler::t8;
  _next_byte_allocate  = Assembler::t8;
  _next_float_allocate = Assembler::fp0;

  _next_spill          = Assembler::t8;
  _next_byte_spill     = Assembler::t8;
  _next_float_spill    = Assembler::fp0;

  initialize_register_references();
}

#endif // ENABLE_COMPILER
