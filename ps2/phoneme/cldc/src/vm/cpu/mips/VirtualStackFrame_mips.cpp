/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_VirtualStackFrame_mips.cpp.incl"

// FASE 0 (dormant JIT): the only arch-specific VirtualStackFrame member the
// shared framework references. Compile path only; bail out. Mirrors
// VirtualStackFrame_i386.cpp.
bool VirtualStackFrame::flush_quick() {
  SHOULD_NOT_REACH_HERE();
  return false;
}

#endif // ENABLE_COMPILER
