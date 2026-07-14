/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_Addressing_mips.cpp.incl"

// FASE 0 (dormant JIT): out-of-line definitions of the Address hierarchy's
// virtual members. Defining them here is what makes the compiler emit the
// vtables for FieldAddress/IndexedAddress/LocationAddress (and resolve
// HeapAddress::~HeapAddress + LocationAddress::is_local_index). Bodies bail out;
// real base+disp16 addressing arrives in Fase 3. Mirrors Addressing_i386.cpp.

HeapAddress::~HeapAddress() {
  GUARANTEE(!has_address_register(),
            "address register must be cleared and deallocated");
}

void HeapAddress::write_barrier_prolog() {
  SHOULD_NOT_REACH_HERE();
}

void HeapAddress::write_barrier_epilog() {
  SHOULD_NOT_REACH_HERE();
}

BinaryAssembler::Address HeapAddress::address_for(jint address_offset) {
  (void)address_offset;
  SHOULD_NOT_REACH_HERE();
  return BinaryAssembler::Address(0);
}

BinaryAssembler::Address FieldAddress::compute_address_for(jint address_offset) {
  (void)address_offset;
  SHOULD_NOT_REACH_HERE();
  return BinaryAssembler::Address(0);
}

BinaryAssembler::Address IndexedAddress::compute_address_for(jint address_offset) {
  (void)address_offset;
  SHOULD_NOT_REACH_HERE();
  return BinaryAssembler::Address(0);
}

// Fase 3: base register + signed displacement. base() is the interpreter's live
// pointer (g_jlocals for locals, g_jsp for stack slots); compute_base_offset()
// supplies the byte displacement. MIPS disp16 is signed, so (unlike i386) the
// offset is allowed to be negative -- locals sit BELOW g_jlocals.
BinaryAssembler::Address StackAddress::address_for(jint address_offset) {
  return BinaryAssembler::Address(base(), address_offset + compute_base_offset());
}

jint LocationAddress::compute_base_offset() {
  // Fase 3 (Marco 3.1): only local addressing is implemented. Whitelisted
  // straight-line methods keep all expression values in registers, so the
  // expression-stack (jsp-relative) path is never reached here.
  GUARANTEE(is_local(), "Fase 3: expression-stack slots not addressable yet");
  // Mirror Interpreter_c.cpp's GET_LOCAL(n) == *((jint*)g_jlocals - n): local n
  // lives at g_jlocals - n*BytesPerWord. base() == locals == s3 == g_jlocals.
  return - index() * BytesPerWord;
}

bool LocationAddress::is_local_index(jint index) {
  return code_generator()->root_method()->is_local(index);
}

#endif // ENABLE_COMPILER
