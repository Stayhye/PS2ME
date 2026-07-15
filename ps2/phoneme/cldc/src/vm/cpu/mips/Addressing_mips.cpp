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
  // Marco 3.4b: the register-index IndexedAddress path (compute_address_for)
  // materializes the effective address into a freshly allocated register; free
  // it here. Field / immediate-index accesses never allocate one, so this is a
  // no-op for them. Mirrors HeapAddress::~HeapAddress in Addressing_arm.cpp.
  if (has_address_register()) {
    RegisterAllocator::dereference(address_register());
  }
}

void HeapAddress::write_barrier_prolog() {
  SHOULD_NOT_REACH_HERE();
}

void HeapAddress::write_barrier_epilog() {
  SHOULD_NOT_REACH_HERE();
}

// Fase 3 (Marco 3.4a): return the base+disp16 operand for a heap access. If a
// prior compute_address_for materialized the effective address into a register
// (the IndexedAddress register-index case, Marco 3.4b), reuse it; otherwise
// compute it now. Mirrors HeapAddress::address_for in Addressing_i386.cpp.
BinaryAssembler::Address HeapAddress::address_for(jint address_offset) {
  if (has_address_register()) {
    GUARANTEE(address_offset == lo_offset(),
              "the address register holds the address for the low offset");
    return BinaryAssembler::Address(address_register());
  }
  return compute_address_for(address_offset);
}

// Fase 3 (Marco 3.4a): a field is [object + (field_offset + word_offset)]. The
// object is already register-resident (the shared caller flushes it). Used for
// Array::length_offset() (arraylength / bounds check) and getfield/putfield.
// Same shape as Addressing_i386.cpp (MIPS also has base+disp).
BinaryAssembler::Address FieldAddress::compute_address_for(jint address_offset) {
  return BinaryAssembler::Address(object()->lo_register(), address_offset + offset());
}

// Fase 3 (Marco 3.4b): address of array element [index]. The r5900 has no
// base+index+scale operand, so only the immediate-index case is a pure disp16;
// a register index must be folded into a register. Element offset within the
// array data = index << index_shift (log2 of the element byte size); the array
// header (Array::base_offset()) plus the caller's word offset become the disp.
BinaryAssembler::Address IndexedAddress::compute_address_for(jint address_offset) {
  const jint disp_base = address_offset + Array::base_offset();
  if (index()->is_immediate()) {
    // [ array + (base_offset + address_offset + index*scale) ] -- disp only.
    return BinaryAssembler::Address(array()->lo_register(),
        disp_base + (index()->as_int() << index_shift()));
  }
  // Register index: addr = array + (index << shift); the (base_offset +
  // address_offset) part stays in the disp16. Allocate addr (freed by
  // ~HeapAddress) and use $at as the shift scratch (outside the register pool).
  set_address_register(RegisterAllocator::allocate());
  CodeGenerator* gen = code_generator();
  const BinaryAssembler::Register addr = address_register();
  const BinaryAssembler::Register arr  = array()->lo_register();
  const BinaryAssembler::Register idx  = index()->lo_register();
  const jint shift = index_shift();
  if (shift != 0) {
    gen->emit(Assembler::encode_sll (Assembler::at, idx, shift));
    gen->emit(Assembler::encode_addu(addr, arr, Assembler::at));
  } else {
    gen->emit(Assembler::encode_addu(addr, arr, idx));
  }
  return BinaryAssembler::Address(addr, disp_base);
}

// Fase 3: base register + signed displacement. base() is the interpreter's live
// pointer (g_jlocals for locals, g_jsp for stack slots); compute_base_offset()
// supplies the byte displacement. MIPS disp16 is signed, so (unlike i386) the
// offset is allowed to be negative -- locals sit BELOW g_jlocals.
BinaryAssembler::Address StackAddress::address_for(jint address_offset) {
  return BinaryAssembler::Address(base(), address_offset + compute_base_offset());
}

jint LocationAddress::compute_base_offset() {
  if (is_local()) {
    // Mirror Interpreter_c.cpp's GET_LOCAL(n) == *((jint*)g_jlocals - n): local n
    // lives at g_jlocals - n*BytesPerWord. base() == locals == s3 == g_jlocals.
    return - index() * BytesPerWord;
  }
  // Fase 3 (Marco 3.6a): expression-stack slot, addressed relative to the physical
  // Java stack pointer (base() == jsp == s1 == g_jsp). Same arithmetic as the i386
  // backend: the element at absolute stack index `index` sits at
  // jsp + arg_offset_from_sp(real_stack_pointer - index). ensure_sufficient_stack_for
  // grows the physical stack (moving s1 via increment_stack_pointer_by) if this slot
  // lies above the current real stack pointer, exactly as on i386. Reached when the
  // register allocator spills, or when the invoke path materializes arguments.
  code_generator()->ensure_sufficient_stack_for(index(), type());
  return JavaFrame::arg_offset_from_sp(frame()->stack_pointer() - index());
}

bool LocationAddress::is_local_index(jint index) {
  return code_generator()->root_method()->is_local(index);
}

#endif // ENABLE_COMPILER
