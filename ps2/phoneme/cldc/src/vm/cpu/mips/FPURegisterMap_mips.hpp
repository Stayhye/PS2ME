/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#if ENABLE_COMPILER

// Fase-0 placeholder mirroring the i386 FP-stack map so VirtualStackFrame
// (USE_COMPILER_FPU_MAP==1) compiles. The r5900 COP1 is a flat register file,
// not a stack; FloatSupport_mips (Fase 4) will replace this. FP bytecodes bail
// to the interpreter until then, so this stack model is never exercised.

class FPURegisterMap {
 public:
  typedef Assembler::Register Register;

 private:
  enum {
    size = Assembler::number_of_float_registers,
    item_bits = 4,
    empty_stack = 0,
    item_base = Assembler::fp0 - 1,   // 0 is the undef value
    item_mask = (1 << item_bits) - 1,
    last_item_mask = item_mask << (item_bits*(size-1))
  };

  unsigned _stack;

  static unsigned encode(const Register reg) {
    GUARANTEE(Assembler::fp0 <= reg && reg <= Assembler::fp7, "Sanity");
    return unsigned(reg) - item_base;
  }
  static Register decode(const unsigned value) {
    GUARANTEE(value != empty_stack, "FPU Register stack underflow");
    return Register((value & item_mask) + item_base);
  }
  static CodeGenerator* code_generator(void) {
    return (CodeGenerator*) _compiler_state;
  }

 public:
  void reset(void) { _stack = empty_stack; }
  void clear(void);
  bool is_empty(void) const { return _stack == empty_stack; }

  Register top_of_stack_register(void) const { return decode(_stack); }
  bool is_top_of_stack(const Register reg) const {
    return top_of_stack_register() == reg;
  }
  bool is_unused(const Register reg) const { return !is_on_stack(reg); }
  unsigned is_on_stack(const Register reg) const;

  void push(const Register reg) {
    GUARANTEE(!(_stack & last_item_mask), "FPU Register stack overflow");
    _stack = encode(reg) | (_stack << item_bits);
  }
  void pop(void) {
    GUARANTEE(!is_empty(), "FPU Register stack underflow");
    _stack >>= item_bits;
  }
  void pop(const Register reg) {
    GUARANTEE(top_of_stack_register() == reg,
              "Can only pop register at top of stack");
    pop();
  }

  int index_for(const Register reg) const;
  Register register_for(const int index) const {
    GUARANTEE((_stack >> (index*item_bits)) != 0, "Index out of bounds");
    return decode(_stack >> (index*item_bits));
  }

  int swap_with_top(const Register reg) {
    const unsigned encoded_reg = encode(reg);
    int i = 0;
    for (unsigned x = _stack; (x & item_mask) != encoded_reg; x >>= item_bits) {
      GUARANTEE(x != empty_stack, "Sanity");
      i++;
    }
    const int shift = i * item_bits;
    _stack = ((_stack &~item_mask) & ~(item_mask << shift)) | encoded_reg |
             ((_stack & item_mask) << shift);
    return i;
  }

#ifndef PRODUCT
  bool is_clearable(void) const;
  void dump(const bool as_comment) const;
#endif
};

#endif // ENABLE_COMPILER
