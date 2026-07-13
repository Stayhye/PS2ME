/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

// The disassembler is a debug-only aid; in a PRODUCT build it compiles away.
// Fase-0 stub -- a real r5900 disassembler is optional (JIT_PLAN 3).
#if !defined(PRODUCT) || USE_COMPILER_DISASSEMBLER

class Disassembler: public StackObj {
 public:
  Disassembler(Stream* stream) : _stream(stream) {}
  Stream* stream() const { return _stream; }

  static const char* register_name(const Assembler::Register reg);

  void disasm(int* start, int instr, int* end) { (void)start; (void)instr; (void)end; }

 private:
  Stream* _stream;
};

#endif
