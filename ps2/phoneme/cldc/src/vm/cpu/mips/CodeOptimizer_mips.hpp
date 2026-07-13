/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

// Peephole optimizer for generated r5900 code. Gated by ENABLE_CODE_OPTIMIZER,
// which is off in the PS2ME build -- this is an empty Fase-0/optional stub.
#if ENABLE_CODE_OPTIMIZER

class CodeOptimizer: public StackObj {
 public:
  CodeOptimizer(CompiledMethod* cm, int* start, int* end) {
    (void)cm; (void)start; (void)end;
  }
  bool optimize_code(JVM_SINGLE_ARG_TRAPS) { JVM_IGNORE_TRAPS; return false; }
};

#endif // ENABLE_CODE_OPTIMIZER
