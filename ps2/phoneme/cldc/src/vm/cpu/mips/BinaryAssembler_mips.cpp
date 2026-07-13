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

// FASE 0 (dormant JIT): bail-out bodies for the arch entry points the shared
// compiler framework references. Real delay-slot-aware r5900 emission arrives in
// Fase 1 ("emit a function that returns 42"). Signatures mirror BinaryAssembler_mips.hpp.

void BinaryAssembler::mov(Register dst, Register src) {
  (void)dst; (void)src;
  SHOULD_NOT_REACH_HERE();
}

void BinaryAssembler::jmp(Label& L) {
  (void)L;
  SHOULD_NOT_REACH_HERE();
}

void BinaryAssembler::jmp(CompilationQueueElement* cqe) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

void BinaryAssembler::bind(Label& L, int alignment) {
  (void)L; (void)alignment;
  SHOULD_NOT_REACH_HERE();
}

void BinaryAssembler::bind_to(Label& L, jint code_offset) {
  (void)L; (void)code_offset;
  SHOULD_NOT_REACH_HERE();
}

#endif // ENABLE_COMPILER
