/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_CompilationQueue_mips.cpp.incl"

// FASE 0 (dormant JIT): arch-specific OSR (on-stack replacement) entry emission.
// On the compile path only; bail out. Mirrors CompilationQueue_i386.cpp.
void OSRStub::emit_osr_entry_and_callinfo(CodeGenerator* gen JVM_TRAPS) {
  (void)gen;
  SHOULD_NOT_REACH_HERE();
}

#endif // ENABLE_COMPILER
