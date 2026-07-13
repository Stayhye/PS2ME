/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER || USE_COMPILER_STRUCTURES
#include "incls/_CompiledMethodDesc_mips.cpp.incl"

// -----------------------------------------------------------------------------
// FASE 0 (dormant JIT): globals + arch members that the assembly interpreter
// skeleton (InterpreterSkeleton/ROMSkeleton) would normally provide. Those
// skeletons are not compiled in the C-interpreter config, so with the JIT
// enabled these become undefined. Define them here.
// -----------------------------------------------------------------------------

// The method-execution sensor: a byte per cache slot, decremented as compiled
// methods run (hotness). 512 slots (method_execution_sensor_size). Provided by
// InterpreterSkeleton on asm-interpreter targets; we supply it for r5900.
unsigned char _method_execution_sensor[method_execution_sensor_size];

#if ENABLE_COMPILER
// ROM has no ahead-of-time compiled methods in our build (no AOT). Normally
// emitted by ROMSkeleton/the ROM writer; supply the empty table so
// ROM::compiled_method_from_address links.
const unsigned int _rom_compiled_methods[]   = { 0 };
const unsigned int _rom_compiled_methods_count = 0;

// GC relocation of generated code. Compiler-area is empty while dormant, so this
// is never reached. Real r5900 relocation arrives in Fase 3.
void CompiledMethodDesc::update_relative_offsets(int delta) {
  (void)delta;
  SHOULD_NOT_REACH_HERE();
}
#endif // ENABLE_COMPILER

#if USE_COMPILER_STRUCTURES
// The i386 versions decode the x86 method_execution_sensor update opcode baked
// into the compiled prologue. The r5900 encoding differs and arrives in Fase 3;
// dormant, no compiled method exists so these are never called.
int CompiledMethodDesc::get_cache_index(void) const {
  SHOULD_NOT_REACH_HERE();
  return 0;
}

void CompiledMethodDesc::set_cache_index(const int i) {
  (void)i;
  SHOULD_NOT_REACH_HERE();
}
#endif // USE_COMPILER_STRUCTURES

#endif // ENABLE_COMPILER || USE_COMPILER_STRUCTURES
