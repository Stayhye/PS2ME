/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#if ENABLE_COMPILER

// Helpers to inspect/patch already-emitted r5900 instructions in the code
// buffer (used when back-patching branches and immediates). Generic accessors
// now; MIPS-specific field decoders arrive with the encoder in Fase 1.

class Instruction: public StackObj {
 private:
  address _addr;
 public:
  Instruction(address addr) { _addr = addr; }
  address addr() const { return _addr; }

  jubyte  byte_at  (int offset) const { return *(jbyte*)  (_addr + offset); }
  jushort short_at (int offset) const { return *(jshort*) (_addr + offset); }
  juint   int_at   (int offset) const { return *(jint*)   (_addr + offset); }
  jubyte  ubyte_at (int offset) const { return *(jubyte*) (_addr + offset); }
  jushort ushort_at(int offset) const { return *(jushort*)(_addr + offset); }
  juint   uint_at  (int offset) const { return *(juint*)  (_addr + offset); }

  void byte_at_put (int offset, jbyte  value) { *(jbyte*) (_addr + offset) = value; }
  void short_at_put(int offset, jshort value) { *(jshort*)(_addr + offset) = value; }
  void int_at_put  (int offset, jint   value) { *(jint*)  (_addr + offset) = value; }
};

#endif // ENABLE_COMPILER
