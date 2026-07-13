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

// initialize() is called only from Compiler::begin_compile(), i.e. behind the
// UseCompiler gate. Real GPR-rotation tables for the r5900 arrive in Fase 3.
void RegisterAllocator::initialize() {
  SHOULD_NOT_REACH_HERE();
}

#endif // ENABLE_COMPILER
