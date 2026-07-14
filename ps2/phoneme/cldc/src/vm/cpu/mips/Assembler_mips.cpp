/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_Assembler_mips.cpp.incl"

#if !defined(PRODUCT) || ENABLE_TTY_TRACE

// r5900 GPR mnemonics, indexed by register number (0..31). The fake FP
// registers fp0..fp7 (32..39) round out the table for out-of-range safety.
static const char* const mips_gpr_names[] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8",   "t9", "k0", "k1", "gp", "sp", "s8", "ra",
  "fp0",  "fp1","fp2","fp3","fp4","fp5","fp6","fp7"
};

const char* Assembler::name_for_long_register(const Register reg) {
  if (reg < first_register || reg > last_register) return "noreg";
  return mips_gpr_names[(int)reg];
}

const char* Assembler::name_for_work_register(const Register reg) {
  return name_for_long_register(reg);
}

const char* Assembler::name_for_byte_register(const Register reg) {
  return name_for_long_register(reg);
}

#endif // !PRODUCT || ENABLE_TTY_TRACE

#endif // ENABLE_COMPILER
