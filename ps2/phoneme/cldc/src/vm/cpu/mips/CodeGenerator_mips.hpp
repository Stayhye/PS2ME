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
