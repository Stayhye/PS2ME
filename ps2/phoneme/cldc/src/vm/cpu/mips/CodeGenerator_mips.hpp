/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

// This file is #include'd by src/vm/share/compiler/CodeGenerator.hpp INSIDE the
// declaration of the CodeGenerator class (via _CodeGenerator_pd.hpp.incl). It is
// therefore a class-body fragment, NOT a standalone class. It declares only the
// r5900-specific members; the cross-CPU members are declared in the shared
// CodeGenerator.hpp and defined in CodeGenerator_mips.cpp.

private:
  void call_from_compiled_code(Register reg, int offset,
                               int parameters_size JVM_TRAPS);
  void call_from_compiled_code(address entry, int parameters_size JVM_TRAPS);

  // JVM_TRAPS sometimes requires an optional argument in the middle.
  void call_from_compiled_code(Register reg, int offset JVM_TRAPS) {
    call_from_compiled_code(reg, offset, 0 JVM_NO_CHECK_AT_BOTTOM);
  }
  void call_from_compiled_code(address entry JVM_TRAPS) {
    call_from_compiled_code(entry, 0 JVM_NO_CHECK_AT_BOTTOM);
  }

  void ishift_helper(Value& result, Value& op1, Value& op2);
  void idiv_helper(Value& result, Value& op1, Value& op2 JVM_TRAPS);

  // Grupo 4: long mul/shift via a C helper (no 64-bit-native path in this build).
  // Flushes the frame, then (n64 ABI: the ps2dev EE toolchain, so each jlong word is
  // passed as its own jint arg register, NOT an o32 pair) copies op1's two words to
  // a0/a1 and op2 either as a long's two words (a2/a3) or an int shift count (a2),
  // calls `helper`, and splits the v0-packed jlong result into result's lo/hi via
  // sll/dsra32. Mirrors the i386 runtime_long_op.
  void long_call_c_helper(Value& result, Value& op1, Value& op2,
                          address helper, bool op2_is_int JVM_TRAPS);

  void cmp_values(Value& op1, Value& op2);
  void verify_no_redo() PRODUCT_RETURN;

public:
  // Called by VirtualStackFrame when USE_COMPILER_FPU_MAP && ENABLE_FLOAT to
  // drop the FP register-map state. Real body arrives with FloatSupport (Fase 4).
  void fpu_clear(bool flush = false);

  void write_call_info(int parameters_size JVM_TRAPS);

  // increment the CPU stack pointer
  void increment_stack_pointer_by(int increment);

  void cmp_values(Value& op1, Value& op2,
                  BytecodeClosure::cond_op condition) {
    (void)condition;
    cmp_values(op1, op2);
  }
