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
    s8   = 30,               // callee-saved ($30, a.k.a. ABI frame pointer)
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
    stack_lock_register = t0,

    // Java-frame registers the shared compiler names (Assembler::fp / ::jsp) and
    // that CodeGenerator_mips uses to manipulate the interpreted Java frame.
    // These MUST equal the interpreter's global-reg pins (ps2me-global-reg.patch:
    // s0=g_jfp, s1=g_jsp, s2=g_jpc, s3=g_jlocals) so compiled code and the C
    // interpreter share one coherent Java frame across the call_from_interpreter
    // boundary. (The ARM backend does the same in its C-interpreter config:
    // fp/jsp/locals are dedicated GPRs, not the ABI frame pointer.)
    fp     = s0,   // Java frame pointer   (g_jfp)
    jsp    = s1,   // Java stack pointer   (g_jsp)
    jpc    = s2,   // Java program counter (g_jpc)
    locals = s3    // current Java locals  (g_jlocals)
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

  // --------------------------------------------------------------------------
  // r5900 instruction encoders (Fase 1). Pure and stateless: each returns the
  // 32-bit machine word (host-endian int; the LE build writes LE bytes, which is
  // what the LE r5900 fetches). No buffer, no relocation -- that machinery lives
  // in BinaryAssembler (Fase 2). This layer is testable in isolation (the Fase 1
  // "returns 42" self-test drives it directly).
  //
  // MIPS32 field layout:
  //   R-type: [31:26]op=0 [25:21]rs [20:16]rt [15:11]rd [10:6]sa [5:0]funct
  //   I-type: [31:26]op    [25:21]rs [20:16]rt [15:0]imm16
  //   J-type: [31:26]op    [25:0]target
  typedef unsigned int Instr;

  static Instr r_type(int op, int rs, int rt, int rd, int sa, int funct) {
    return ((unsigned)(op    & 0x3f) << 26) | ((unsigned)(rs & 0x1f) << 21) |
           ((unsigned)(rt    & 0x1f) << 16) | ((unsigned)(rd & 0x1f) << 11) |
           ((unsigned)(sa    & 0x1f) <<  6) |  (unsigned)(funct & 0x3f);
  }
  static Instr i_type(int op, int rs, int rt, int imm16) {
    return ((unsigned)(op & 0x3f) << 26) | ((unsigned)(rs & 0x1f) << 21) |
           ((unsigned)(rt & 0x1f) << 16) |  (unsigned)(imm16 & 0xffff);
  }
  static Instr j_type(int op, unsigned target26) {
    return ((unsigned)(op & 0x3f) << 26) | (target26 & 0x03ffffff);
  }

  // SPECIAL (op=0) three-register ALU.
  static Instr encode_addu(Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x21); }
  static Instr encode_subu(Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x23); }
  static Instr encode_and (Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x24); }
  static Instr encode_or  (Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x25); }
  static Instr encode_xor (Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x26); }
  static Instr encode_nor (Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x27); }
  static Instr encode_slt (Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x2a); }
  static Instr encode_sltu(Register rd, Register rs, Register rt) { return r_type(0, rs, rt, rd, 0, 0x2b); }

  // Variable shifts (shift amount in rs).
  static Instr encode_sllv(Register rd, Register rt, Register rs) { return r_type(0, rs, rt, rd, 0, 0x04); }
  static Instr encode_srlv(Register rd, Register rt, Register rs) { return r_type(0, rs, rt, rd, 0, 0x06); }
  static Instr encode_srav(Register rd, Register rt, Register rs) { return r_type(0, rs, rt, rd, 0, 0x07); }

  // Immediate shifts (shift amount in sa; rs unused).
  static Instr encode_sll (Register rd, Register rt, int sa) { return r_type(0, 0, rt, rd, sa, 0x00); }
  static Instr encode_srl (Register rd, Register rt, int sa) { return r_type(0, 0, rt, rd, sa, 0x02); }
  static Instr encode_sra (Register rd, Register rt, int sa) { return r_type(0, 0, rt, rd, sa, 0x03); }
  static Instr encode_nop () { return 0u; }  // sll zero,zero,0

  // Multiply/divide + hi/lo moves.
  static Instr encode_mult (Register rs, Register rt) { return r_type(0, rs, rt, 0, 0, 0x18); }
  static Instr encode_multu(Register rs, Register rt) { return r_type(0, rs, rt, 0, 0, 0x19); }
  static Instr encode_div  (Register rs, Register rt) { return r_type(0, rs, rt, 0, 0, 0x1a); }
  static Instr encode_divu (Register rs, Register rt) { return r_type(0, rs, rt, 0, 0, 0x1b); }
  static Instr encode_mfhi (Register rd) { return r_type(0, 0, 0, rd, 0, 0x10); }
  static Instr encode_mflo (Register rd) { return r_type(0, 0, 0, rd, 0, 0x12); }

  // Register-indirect jumps. Both carry one delay slot (the caller emits it).
  static Instr encode_jr  (Register rs)              { return r_type(0, rs, 0, 0,  0, 0x08); }
  static Instr encode_jalr(Register rd, Register rs) { return r_type(0, rs, 0, rd, 0, 0x09); }

  // I-type ALU-immediate. (addiu/slti/sltiu sign-extend imm; andi/ori/xori zero-extend.)
  static Instr encode_addiu(Register rt, Register rs, int imm) { return i_type(0x09, rs, rt, imm); }
  static Instr encode_slti (Register rt, Register rs, int imm) { return i_type(0x0a, rs, rt, imm); }
  static Instr encode_sltiu(Register rt, Register rs, int imm) { return i_type(0x0b, rs, rt, imm); }
  static Instr encode_andi (Register rt, Register rs, int imm) { return i_type(0x0c, rs, rt, imm); }
  static Instr encode_ori  (Register rt, Register rs, int imm) { return i_type(0x0d, rs, rt, imm); }
  static Instr encode_xori (Register rt, Register rs, int imm) { return i_type(0x0e, rs, rt, imm); }
  static Instr encode_lui  (Register rt, int imm)              { return i_type(0x0f,  0, rt, imm); }

  // Loads / stores: base + signed 16-bit offset.
  static Instr encode_lb (Register rt, Register base, int off) { return i_type(0x20, base, rt, off); }
  static Instr encode_lh (Register rt, Register base, int off) { return i_type(0x21, base, rt, off); }
  static Instr encode_lw (Register rt, Register base, int off) { return i_type(0x23, base, rt, off); }
  static Instr encode_lbu(Register rt, Register base, int off) { return i_type(0x24, base, rt, off); }
  static Instr encode_lhu(Register rt, Register base, int off) { return i_type(0x25, base, rt, off); }
  static Instr encode_sb (Register rt, Register base, int off) { return i_type(0x28, base, rt, off); }
  static Instr encode_sh (Register rt, Register base, int off) { return i_type(0x29, base, rt, off); }
  static Instr encode_sw (Register rt, Register base, int off) { return i_type(0x2b, base, rt, off); }

  // Branches: PC-relative, offset counted in instruction words, one delay slot.
  static Instr encode_beq (Register rs, Register rt, int off) { return i_type(0x04, rs, rt, off); }
  static Instr encode_bne (Register rs, Register rt, int off) { return i_type(0x05, rs, rt, off); }
  static Instr encode_blez(Register rs, int off)              { return i_type(0x06, rs, 0, off); }
  static Instr encode_bgtz(Register rs, int off)              { return i_type(0x07, rs, 0, off); }
  // REGIMM (op=1): the sub-op lives in the rt field (bltz=0, bgez=1).
  static Instr encode_bltz(Register rs, int off)              { return i_type(0x01, rs, 0x00, off); }
  static Instr encode_bgez(Register rs, int off)              { return i_type(0x01, rs, 0x01, off); }

  // J-type absolute (target = (word_address >> 2) & 0x03ffffff).
  static Instr encode_j  (unsigned target26) { return j_type(0x02, target26); }
  static Instr encode_jal(unsigned target26) { return j_type(0x03, target26); }
};

class Macros: public Assembler {
};
