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

// The quick (post/pre-indexed) flush is an ARM-only optimization; every other
// backend flushes location-by-location. Return false so the shared flush() takes
// the generic path. Mirrors VirtualStackFrame_i386.cpp (which also returns false).
bool VirtualStackFrame::flush_quick() {
  return false;
}

#endif // ENABLE_COMPILER
