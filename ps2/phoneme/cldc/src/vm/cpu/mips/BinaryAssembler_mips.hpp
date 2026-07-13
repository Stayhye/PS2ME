/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#if ENABLE_COMPILER

// The binary (in-memory) assembler for the r5900. Emits little-endian 32-bit
// instruction words into the code buffer. No literal pool (USE_LITERAL_POOL==0);
// 32-bit immediates are materialized with lui/ori. Every branch/jump has one
// delay slot -- filling it is the job of the emit layer (Fase 1+).

class BinaryAssembler: public BinaryAssemblerCommon {
 public:
  // A label: sign of _encoding encodes bound/unbound, magnitude the position.
  // (Identical scheme to the i386 backend.)
  class InternalLabel {
   public:
    InternalLabel() { unuse(); }
    bool is_bound()   const { return _encoding <  0; }
    bool is_unbound() const { return _encoding >  0; }
    bool is_unused()  const { return _encoding == 0; }
    int  position()   const {
      if (_encoding < 0) return - _encoding - 1;
      if (_encoding > 0) return   _encoding - 1;
      return 0;
    }
    void bind_to(int position) { _encoding = - position - 1; }
    void link_to(int position) { _encoding =   position + 1; }
    void unuse()               { _encoding = 0; }

    int _encoding;
   private:
    friend class Compiler;
    friend class Entry;
  };

  class Label     : public InternalLabel { };
  class NearLabel : public InternalLabel { };

  // A MIPS memory operand: base register + signed 16-bit displacement.
  class Address {
   public:
    Address(int disp) : _base(no_reg), _disp(disp) { }
    Address(Register base, int disp = 0) : _base(base), _disp(disp) { }
    Register base() const { return _base; }
    int      disp() const { return _disp; }
   private:
    Register _base;
    int      _disp;
    friend class BinaryAssembler;
    friend class CodeGenerator;
  };

  // for platform-independent code
  void mov(Register dst, Register src);

  // x86-style alias used by shared Value::force_to_byte_register (a no-op path
  // on the r5900, since is_valid_byte_register is always true).
  void movl(Register dst, Register src) { mov(dst, src); }

  // Unconditional jump family (shared CompilationQueue/BytecodeCompileClosure
  // emit these). Real delay-slot-aware encodings arrive in Fase 1.
  void jmp(Register reg);
  void jmp(address entry);
  void jmp(Label& L);
  void jmp(NearLabel& L);
  void jmp(CompilationQueueElement* cqe);

  // Label binding.
  void bind      (Label& L, int alignment = 0);
  void bind_to   (Label& L, jint code_offset);
  void bind      (NearLabel& L) { bind_to(L, _code_offset); }
  void bind_to   (NearLabel& L, int code_offset);

  void get_thread(Register dst);

  static void instruction_emitted() {}

  void emit(int instr) { emit_code_int(instr); }
  void emit_byte(const jint value) { emit_code_byte (value); }
  void emit_word(const jint value) { emit_code_short(value); }
  void emit_long(const jint value) { emit_code_int  (value); }

  void emit_data(int data,
                 Relocation::Kind reloc = Relocation::no_relocation);

  void generate_sentinel() {
    // A trap on the r5900 ("break 0") is a hard, obvious sentinel.
    emit_code_int(0x0000000D /* break */);
    emit_sentinel();
  }

 private:
  jint long_at    (const int position) const;
  void long_at_put(const int position, const jint value) const;
};

#endif // ENABLE_COMPILER
