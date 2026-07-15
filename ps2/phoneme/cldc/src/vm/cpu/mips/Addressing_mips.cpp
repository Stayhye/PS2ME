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

// Fase 3 (Marco 3.7a): GC write barrier for object/reference field stores
// (fast_aputfield; object-array stores later reuse it). A store of a heap pointer
// must set the slot's bit in the object-heap marking bit vector so the GC's
// generational/incremental scan visits it. Mirrors the i386 backend (Addressing_
// i386.cpp: leal &slot; shrl >> LogBytesPerWord; bts [_bitvector_base]) with the
// r5900's plain arithmetic. write_barrier_prolog/epilog are virtual no-ops on the
// base Address, overridden only here on HeapAddress, so a reference store to a
// LOCAL (LocationAddress, not a HeapAddress) emits no barrier -- correct, locals
// are not in the heap.
void HeapAddress::write_barrier_prolog() {
  GUARANTEE(stack_type() == T_OBJECT,
            "write barrier is only for object/array reference stores");
  // Leave the slot's effective address in address_register (mirrors i386's leal).
  // compute_address_for yields base + disp16. Two cases:
  //  - FieldAddress / immediate-index IndexedAddress: no register is allocated yet;
  //    allocate one and materialize base + disp (the MIPS "load effective address").
  //  - register-index IndexedAddress (Marco 3.4b): compute_address_for ALREADY
  //    allocated the address register (array + index<<shift) and returns it with the
  //    header displacement. Do NOT allocate a second one (that would leak a register
  //    and misplace the barrier) -- fold the displacement into the existing register.
  // store_to_address then finds the address register via HeapAddress::address_for
  // and stores through it (disp 0).
  const BinaryAssembler::Address a = compute_address_for(lo_offset());
  if (has_address_register()) {
    GUARANTEE(a.base() == address_register(),
              "register-index IndexedAddress returns its own address register");
    if (a.disp() != 0) {
      code_generator()->emit(
          Assembler::encode_addiu(address_register(), a.base(), a.disp()));
    }
  } else {
    set_address_register(RegisterAllocator::allocate());
    code_generator()->emit(
        Assembler::encode_addiu(address_register(), a.base(), a.disp()));
  }
}

void HeapAddress::write_barrier_epilog() {
  GUARANTEE(stack_type() == T_OBJECT,
            "write barrier is only for object/array reference stores");
  GUARANTEE(has_address_register(),
            "write_barrier_prolog must have allocated the address register");
  CodeGenerator* const gen = code_generator();
  const BinaryAssembler::Register addr = address_register();   // &slot
  const BinaryAssembler::Register at   = Assembler::at;        // scratch (out of pool)

  // i = oop_index(slot) = ((unsigned)&slot) >> LogBytesPerWord. This mirrors
  // ObjectHeap::set_bit_for exactly (share/ObjectHeap.hpp), so the bit the GC
  // reads is the bit compiled code sets.
  gen->emit(Assembler::encode_srl(addr, addr, LogBytesPerWord));

  // Load the CURRENT bit-vector base from memory (jvm_fast_globals.bitvector_base)
  // rather than embedding its value: a heap expansion relocates the bit vector, so
  // a live load keeps a long-lived compiled method's barrier correct.
  const BinaryAssembler::Register bv = RegisterAllocator::allocate();
  const juint bva = (juint)(unsigned long)(address)&_bitvector_base;
  gen->emit(Assembler::encode_lui(bv, (bva >> 16) & 0xffff));
  gen->emit(Assembler::encode_ori(bv, bv, bva & 0xffff));
  gen->emit(Assembler::encode_lw (bv, bv, 0));                 // bv = _bitvector_base

  // &bitvector_word = bv + (i >> LogBitsPerWord) * BytesPerWord
  gen->emit(Assembler::encode_srl (at, addr, LogBitsPerWord)); // at = i >> 5
  gen->emit(Assembler::encode_sll (at, at,   LogBytesPerWord));// at = (i>>5) << 2
  gen->emit(Assembler::encode_addu(bv, bv,   at));             // bv = &word

  // mask = 1 << (i & (BitsPerWord-1))
  gen->emit(Assembler::encode_andi (at,   addr, BitsPerWord - 1)); // at = i & 31
  const BinaryAssembler::Register mask = RegisterAllocator::allocate();
  gen->emit(Assembler::encode_addiu(mask, Assembler::zero, 1));    // mask = 1
  gen->emit(Assembler::encode_sllv (mask, mask, at));             // mask = 1 << (i&31)

  // *word |= mask  (reuse addr as the loaded word value -- it is dead otherwise)
  gen->emit(Assembler::encode_lw(addr, bv, 0));
  gen->emit(Assembler::encode_or(addr, addr, mask));
  gen->emit(Assembler::encode_sw(addr, bv, 0));

  RegisterAllocator::dereference(mask);
  RegisterAllocator::dereference(bv);
  RegisterAllocator::dereference(address_register());
  clear_address_register();
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
