/*
 *   PS2ME: MIPS r5900 (PlayStation 2 Emotion Engine) JIT backend.
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

// The Assembler class for the r5900. Modelled on the i386 backend (which, like
// the r5900 and unlike ARM, has USE_LITERAL_POOL == 0 and derives from the
// shared AssemblerCommon). The r5900 is a MIPS III core (32-bit ABI here) with a
// branch delay slot on every jump/branch. This header declares only the register
// file and the enums the shared compiler framework references; the instruction
// encoders live in BinaryAssembler_mips.

class Assembler: public AssemblerCommon {
 public:
  // Branch/compare conditions. Arch-private: the shared framework speaks
  // BytecodeClosure::cond_op and CodeGenerator_mips::convert_condition maps
  // those to these. `always` is the pseudo-condition for unconditional flow.
  enum Condition {
    eq,          // ==
    ne,          // !=
    lt,          // <   (signed)
    ge,          // >=  (signed)
    le,          // <=  (signed)
    gt,          // >   (signed)
    below,       // <   (unsigned)
    above_equal, // >=  (unsigned)
    below_equal, // <=  (unsigned)
    above,       // >   (unsigned)

    number_of_conditions,

    // Alternative names used by platform-independent code. (No `zero`/`not_zero`
    // aliases here: `zero` is also the name of GPR $0, and enum values share the
    // class scope -- the collision is a compile error. Shared code speaks
    // BytecodeClosure::cond_op, not Assembler::Condition, so this is harmless.)
    equal        = eq,
    not_equal    = ne,
    less         = lt,
    greater_equal= ge,
    less_equal   = le,
    greater      = gt,

    always = number_of_conditions
  };

  // The 32 general-purpose registers of the r5900, o32-style names.
  enum Register {
    zero =  0,               // hardwired 0
    at   =  1,               // assembler temporary (reserved)
    v0   =  2, v1 =  3,      // return values / scratch
    a0   =  4, a1 =  5, a2 =  6, a3 =  7,   // arguments
    t0   =  8, t1 =  9, t2 = 10, t3 = 11,   // caller-saved temporaries
    t4   = 12, t5 = 13, t6 = 14, t7 = 15,
    s0   = 16, s1 = 17, s2 = 18, s3 = 19,   // callee-saved (s0-s3 pin the
    s4   = 20, s5 = 21, s6 = 22, s7 = 23,   //   interpreter state, see global-reg)
    t8   = 24, t9 = 25,                      // more caller-saved temporaries
    k0   = 26, k1 = 27,                      // kernel-reserved
    gp   = 28,               // global pointer
    sp   = 29,               // stack pointer
    fp   = 30,               // frame pointer (a.k.a. s8)
    ra   = 31,               // return address

    number_of_gp_registers = 32,

    // Fake floating-point registers. The shared compiler models FP allocation
    // through FPURegisterMap (USE_COMPILER_FPU_MAP==1, forced by
    // USE_LITERAL_POOL==0), exactly as the i386 backend does. The r5900 COP1 is
    // a real register file (not a stack), so this is a Fase-0 placeholder that
    // FloatSupport_mips (Fase 4) will replace; FP bytecodes bail out until then.
    fp0 = 32, fp1 = 33, fp2 = 34, fp3 = 35,
    fp4 = 36, fp5 = 37, fp6 = 38, fp7 = 39,

    // Constants required by the platform-independent framework.
    no_reg = -1,
    first_register = zero,
    last_register  = fp7,

    // Allocatable working set starts at the first caller-saved temporary. The
    // ACTUAL allocatable set (which skips s0-s3 = pinned interpreter state, and
    // the ABI-reserved at/k0/k1/gp/sp/fp/ra/zero) is defined by the tables in
    // RegisterAllocator_mips.cpp, not by a contiguous range.
    first_allocatable_register = t0,

    // Platform-independent aliases.
    return_register     = v0,
    stack_lock_register = t0
  };

  enum {
    first_int_register   = zero,
    last_int_register    = ra,
    first_float_register = fp0,
    last_float_register  = fp7,
    number_of_float_registers = last_float_register - first_float_register + 1,
    number_of_registers  = last_register + 1
  };

  enum {
    // Every r5900 instruction is a naturally-aligned 32-bit word.
    instruction_alignment = 4
  };

  static bool is_valid_register(Register reg) {
    return reg >= first_register && reg <= last_register;
  }
  static bool is_valid_int_register(Register reg) {
    return reg >= (Register)first_int_register &&
           reg <= (Register)last_int_register;
  }
  // The r5900 has no separate byte-register file (any GPR does lb/sb), so every
  // integer register is a valid "byte register". Shared Value::force_to_byte_
  // register then becomes a no-op.
  static bool is_valid_byte_register(Register reg) {
    return is_valid_int_register(reg);
  }

  static Register register_from_encoding(int encoding) {
    return (Register) encoding;
  }

  // for platform-independent code
  static Register reg(Register r) { return r; }

#if !defined(PRODUCT) || ENABLE_TTY_TRACE
  // Name accessors used by RegisterAllocator/VirtualStackFrame in debug builds.
  // The r5900 GPRs are 32-bit; "long"/"work" registers map to the same file.
  static const char* name_for_byte_register(const Register reg);
  static const char* name_for_work_register(const Register reg);
  static const char* name_for_long_register(const Register reg);
#endif
};

class Macros: public Assembler {
};
