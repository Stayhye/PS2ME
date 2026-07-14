/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#include "incls/_precompiled.incl"

#if ENABLE_COMPILER
#include "incls/_CodeGenerator_mips.cpp.incl"

// -----------------------------------------------------------------------------
// FASE 0 (dormant JIT): every arch-specific CodeGenerator entry point is defined
// here with a bail-out body. With UseCompiler=false the compiler never runs, so
// none of these can be reached; they exist only to let the ELF link. Real r5900
// emission arrives in Fase 3 (see references/JIT_PLAN.md). Signatures mirror the
// shared CodeGenerator.hpp declarations (and the i386 arch fragment for the few
// platform-private ones), so the mangled names match what the shared compiler
// framework references.
// -----------------------------------------------------------------------------

// ---- Fase 2 emission helpers -------------------------------------------------
// The compiled method interoperates with the C interpreter through two C glue
// helpers (Interpreter_c.cpp, patch #37): jit_frame_enter builds the callee Java
// frame; jit_return_int/void push the result and run the interpreter's return
// teardown. The emitted code just calls them via the EE C ABI. The intricate
// frame protocol therefore lives in reviewable C, not open-coded r5900.
extern "C" void jit_frame_enter();
extern "C" void jit_return_int(jint result);
extern "C" void jit_return_void();
extern "C" void jit_timer_tick();       // Marco 3.2b: backward-branch timer check
// Marco 3.4a: runtime-exception throw helpers. Thin wrappers over the C
// interpreter's own interpreter_throw_* (Interpreter_c.cpp, patch #44): allocate
// the exception, run find_exception_frame over the g_jfp chain jit_frame_enter
// built, and reposition g_jfp/g_jsp/g_jpc/g_jlocals to the handler frame. See the
// throw-path emission in mips_emit_runtime_throw and JIT_PLAN.md 7.
extern "C" void jit_throw_null_pointer();
extern "C" void jit_throw_array_index();

// Materialize a 32-bit constant into dst (no r5900 PC-relative load). One
// instruction for values that fit a signed/zero-extended 16-bit field, else
// lui/ori.
static void mips_li(BinaryAssembler* a, Assembler::Register dst, juint imm) {
  const jint s = (jint)imm;
  if (s >= -32768 && s <= 32767) {
    a->emit(Assembler::encode_addiu(dst, Assembler::zero, s));
  } else if ((imm & 0xffff0000u) == 0) {
    a->emit(Assembler::encode_ori(dst, Assembler::zero, imm & 0xffff));
  } else {
    a->emit(Assembler::encode_lui(dst, (imm >> 16) & 0xffff));
    if ((imm & 0xffff) != 0) {
      a->emit(Assembler::encode_ori(dst, dst, imm & 0xffff));
    }
  }
}

// --- Fase 3 (Marco 3.2): deferred compare state ------------------------------
// The r5900 has no condition-code register, but the shared compiler emits a
// compare (cmp_values) immediately followed by a branch (conditional_jump_do)
// with nothing in between (share/CodeGenerator.cpp branch_if/branch_if_do). We
// park the compared operands here in cmp_values and synthesize the actual
// slt/beq/bne inside conditional_jump_do. A single slot suffices: the compiler
// is single-threaded and the two calls are always adjacent. The inline
// if_then_else/if_iinc paths (which would interleave differently) stay in
// bail-out and are barred by the compile trigger's whitelist.
static bool                s_cmp_pending    = false;
static Assembler::Register s_cmp_op1_reg    = Assembler::no_reg;
static bool                s_cmp_op2_is_imm = false;
static Assembler::Register s_cmp_op2_reg    = Assembler::no_reg;
static jint                s_cmp_op2_imm    = 0;

// Emit an EE C-ABI call to an absolute code address: materialize it in t9 (the
// MIPS PIC call register), jalr, and fill the branch delay slot with a nop.
// (ra is clobbered by jalr; the caller must have saved the real return address.)
static void mips_call_c(BinaryAssembler* a, address target) {
  mips_li(a, Assembler::t9, (juint)(unsigned long)target);
  a->emit(Assembler::encode_jalr(Assembler::ra, Assembler::t9));
  a->emit(Assembler::encode_nop());
}

// --- Fase 3 (Marco 3.4a): runtime-exception throw path -----------------------
// Emit the throw of a runtime exception (null-pointer / array-index) from
// compiled code. The r5900 hybrid does NOT port ThrowExceptionStub / call_vm /
// return_error; instead it mirrors the interpreted throw exactly (JIT_PLAN.md
// 3, 7): call a C helper that raises the exception the interpreter's own way
// (find_exception_frame walks the g_jfp chain jit_frame_enter built and
// repositions g_jfp/g_jsp/g_jpc to the handler frame), then run the compiled
// method's NATIVE-stack epilogue (restore $ra/$sp, jr ra) -- WITHOUT jit_return
// (the Java frame was already unwound by find_exception_frame). Natural C returns
// then carry control call_from_interpreter -> invoke_java_method -> the flat
// interpreter dispatch loop, which resumes at the handler bcp. No longjmp needed
// for the handler-found case; the longjmps resume_thread itself may fire (no
// handler -> thread exit) also discard this nested native frame correctly.
//
// GC-safety: the compiled leaf frame is discarded, so its register-resident
// locals/stack (t0-t8) need not be flushed before the call -- they are never read
// again; the handler's ancestor frame state lives in interpreter memory.
static void mips_emit_runtime_throw(BinaryAssembler* a, int rte) {
  address helper;
  switch (rte) {
    case ThrowExceptionStub::rte_null_pointer:
      helper = (address)jit_throw_null_pointer; break;
    case ThrowExceptionStub::rte_array_index_out_of_bounds:
      helper = (address)jit_throw_array_index; break;
    default:
      SHOULD_NOT_REACH_HERE(); return;
  }
  mips_call_c(a, helper);
  // native-stack epilogue (mirrors return_void's, minus the jit_return helper).
  a->emit(Assembler::encode_lw(Assembler::ra, Assembler::sp, 0));
  a->emit(Assembler::encode_addiu(Assembler::sp, Assembler::sp, 16));
  a->emit(Assembler::encode_jr(Assembler::ra));
  a->emit(Assembler::encode_nop());
}

// ---- method prologue / stack -------------------------------------------------

void CodeGenerator::overflow(const Assembler::Register& stack_pointer,
                             const Assembler::Register& method) {
  (void)stack_pointer; (void)method;
  SHOULD_NOT_REACH_HERE();
}

// Fase 2: the compiled method always builds a full Java frame (via jit_frame_enter),
// matching the C interpreter's frame protocol so the return teardown works. The
// ARM omit_stack_frame shortcut does NOT apply to the C hybrid (the return path
// reads the frame descriptor); OmitLeafMethodFrames must be off for JIT methods
// (enforced by the Fase 2 trigger). We also open a small native-stack (EE $sp)
// frame to preserve the return address across our C-helper calls; the matching
// teardown is emitted by return_result/return_void.
void CodeGenerator::method_entry(Method* method JVM_TRAPS) {
  (void)method;
  GUARANTEE(!omit_stack_frame(),
            "Fase 2 JIT methods build a full frame (OmitLeafMethodFrames off)");
  // native-stack prologue: reserve a 16-byte aligned frame and save ra.
  emit(Assembler::encode_addiu(Assembler::sp, Assembler::sp, -16));
  emit(Assembler::encode_sw(Assembler::ra, Assembler::sp, 0));
  // build the Java frame in C (shares g_jfp/g_jsp/g_jlocals via global-reg).
  mips_call_c(this, (address)jit_frame_enter);
}

// In the C-interpreter hybrid the Java expression stack is g_jsp (s1), owned by
// the interpreter and set up by jit_frame_enter; the compiled body keeps
// expression values in registers and (for Marco 3.1 whitelisted methods) never
// spills them to the Java stack. So these CPU-stack adjustments are no-ops here
// (real g_jsp maintenance for expression-stack flushing arrives with a later
// Marco). They are called during normal compilation, so they must NOT bail out.
void CodeGenerator::increment_stack_pointer_by(int adjustment) {
  (void)adjustment;
}

void CodeGenerator::clear_stack() {
}

// Both are literal-pool maintenance on backends that load 32-bit immediates from
// an in-code pool (ARM/thumb). The r5900 materializes immediates with lui/ori
// (USE_LITERAL_POOL==0), so there is never a pool to flush -> no-ops. Called
// unconditionally in the compile loop (per bytecode / at frame flush), so they
// must NOT bail out.
void CodeGenerator::bytecode_prolog() {
}

void CodeGenerator::flush_epilogue(JVM_SINGLE_ARG_TRAPS) {
}

// ---- loads / stores / moves --------------------------------------------------

// Fase 3 (Marco 3.1): load a single-word value from memory (base+disp16). Used
// for iload (LocationAddress over g_jlocals) and later array/field loads. The
// shared read_value() calls this eagerly for a flushed local, so after iload the
// value is register-resident -- which is what int_binary_do requires.
void CodeGenerator::load_from_address(Value& result, BasicType type,
                                      MemoryAddress& address, Condition cond) {
  GUARANTEE(cond == Assembler::always, "Fase 3: unconditional loads only");
  (void)cond;
  if (type == T_ILLEGAL) {
    return;  // illegal types require no load
  }
  GUARANTEE(stack_type_for(type) == result.stack_type(),
            "types must match (taking stack types into account)");
  result.try_to_assign_register();
  const Assembler::Register lo = result.lo_register();
  const BinaryAssembler::Address addr = address.lo_address();
  const Assembler::Register base = addr.base();
  const int off = addr.disp();

  switch (type) {
    case T_BOOLEAN:                              // fall through (signed byte)
    case T_BYTE:   emit(Assembler::encode_lb (lo, base, off)); break;
    case T_CHAR:   emit(Assembler::encode_lhu(lo, base, off)); break;
    case T_SHORT:  emit(Assembler::encode_lh (lo, base, off)); break;
    case T_INT:                                  // fall through
    case T_FLOAT:                                // fall through
    case T_ARRAY:                                // fall through
    case T_OBJECT: emit(Assembler::encode_lw (lo, base, off)); break;
    default:
      // T_LONG / T_DOUBLE are two-word; those arrive with a later Marco.
      SHOULD_NOT_REACH_HERE();
      break;
  }
}

// Fase 3 (Marco 3.2): store a single-word value to memory (base+disp16). Dual
// of load_from_address; used when the VSF flushes a "changed" local to memory
// (RawLocation::write_changes -> store_to_location -> LocationAddress over
// g_jlocals), which happens at branch merges (conform_to) and register spills.
// Immediates are materialized into $at (or $zero when 0); a register value is
// stored directly. Two-word (long/double) and heap write barriers arrive with
// later Marcos. Locals/expression-stack are plain MemoryAddress (no barrier).
void CodeGenerator::store_to_address(Value& value, BasicType type,
                                     MemoryAddress& address) {
  if (!value.is_present()) return;   // nothing to store
  GUARANTEE(stack_type_for(type) == value.stack_type(),
            "types must match (taking stack types into account)");

  const BinaryAssembler::Address addr = address.lo_address();
  const Assembler::Register base = addr.base();
  const int off = addr.disp();

  Assembler::Register src;
  if (value.is_immediate()) {
    if (value.as_int() == 0) {
      src = Assembler::zero;                 // store $zero directly
    } else {
      mips_li(this, Assembler::at, (juint)value.as_int());
      src = Assembler::at;
    }
  } else {
    GUARANTEE(value.in_register(), "only case left");
    src = value.lo_register();
  }

  switch (type) {
    case T_BOOLEAN:                              // fall through
    case T_BYTE:   emit(Assembler::encode_sb(src, base, off)); break;
    case T_CHAR:                                 // fall through
    case T_SHORT:  emit(Assembler::encode_sh(src, base, off)); break;
    case T_INT:                                  // fall through
    case T_FLOAT:                                // fall through
    case T_ARRAY:                                // fall through
    case T_OBJECT: emit(Assembler::encode_sw(src, base, off)); break;
    default:
      // T_LONG / T_DOUBLE are two-word; those arrive with a later Marco.
      SHOULD_NOT_REACH_HERE();
      break;
  }
}

// move(Value,Value): the shared Value::materialize()/writable_copy() call this to
// (a) materialize an immediate into an assigned register, or (b) copy a register
// value. Single-word only for Marco 3.1.
void CodeGenerator::move(const Value& dst, const Value& src, const Condition cond) {
  GUARANTEE(cond == Assembler::always, "Fase 3: unconditional moves only");
  (void)cond;
  GUARANTEE(dst.in_register(), "move destination must have a register");
  GUARANTEE(dst.is_one_word() && src.is_one_word(),
            "Fase 3: single-word moves only");
  if (src.is_immediate()) {
    mips_li(this, dst.lo_register(), (juint)src.as_int());
  } else {
    GUARANTEE(src.in_register(), "source must be immediate or in a register");
    if (dst.lo_register() != src.lo_register()) {
      mov(dst.lo_register(), src.lo_register());  // BinaryAssembler::mov -> `or`
    }
  }
}

void CodeGenerator::move(Value& dst, Oop* obj, Condition cond) {
  (void)dst; (void)obj; (void)cond;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::move(Assembler::Register dst, Assembler::Register src,
                         Condition cond) {
  GUARANTEE(cond == Assembler::always, "Fase 3: unconditional moves only");
  (void)cond;
  if (dst != src) {
    mov(dst, src);  // BinaryAssembler::mov -> `or dst,src,zero`
  }
}

// ---- comparisons / conditional control flow ----------------------------------

// Fase 3 (Marco 3.2): no machine compare here -- the r5900 has no flags. Park
// the operands; conditional_jump_do consumes them. The shared caller guarantees
// op1 is in a register and op2 is a register or an immediate (op1-immediate is
// folded away earlier in branch_if). Emits nothing.
void CodeGenerator::cmp_values(Value& op1, Value& op2) {
  GUARANTEE(op1.in_register(), "op1 must be in a register");
  GUARANTEE(op2.is_immediate() || op2.in_register(),
            "op2 must be in a register or an immediate");
  s_cmp_op1_reg = op1.lo_register();
  if (op2.is_immediate()) {
    s_cmp_op2_is_imm = true;
    s_cmp_op2_imm    = op2.as_int();
    s_cmp_op2_reg    = Assembler::no_reg;
  } else {
    s_cmp_op2_is_imm = false;
    s_cmp_op2_reg    = op2.lo_register();
  }
  s_cmp_pending = true;
}

// Fase 3 (Marco 3.2): synthesize the condition parked by cmp_values and branch
// to `destination` when it holds. MIPS has no flags, so we materialize the
// truth of the comparison with slt/sltu into $at (the assembler-temporary,
// which is outside the allocatable pool and thus safe to clobber -- the VSF is
// conformed only later, at the merge) and branch on it. The shared framework
// already negates the condition when it wants the fall-through polarity, so we
// only implement the eight direct cond_ops. Comparing against the immediate 0
// uses the r5900's dedicated compare-with-zero branches, skipping the slt.
// if_icmp compares are signed (slt); eq/ne need no slt at all.
void CodeGenerator::conditional_jump_do(BytecodeClosure::cond_op condition,
                                        Label& destination) {
  GUARANTEE(s_cmp_pending, "conditional_jump_do must follow a cmp_values");
  s_cmp_pending = false;

  const Assembler::Register rs = s_cmp_op1_reg;
  const Assembler::Register at = Assembler::at;

  if (s_cmp_op2_is_imm && s_cmp_op2_imm == 0) {
    switch (condition) {
      case BytecodeClosure::null:                    // ptr == null -> reg == 0
      case BytecodeClosure::eq:
        emit_branch(Assembler::encode_beq (rs, Assembler::zero, 0), destination); return;
      case BytecodeClosure::nonnull:                 // ptr != null -> reg != 0
      case BytecodeClosure::ne:
        emit_branch(Assembler::encode_bne (rs, Assembler::zero, 0), destination); return;
      case BytecodeClosure::lt:
        emit_branch(Assembler::encode_bltz(rs, 0), destination); return;
      case BytecodeClosure::ge:
        emit_branch(Assembler::encode_bgez(rs, 0), destination); return;
      case BytecodeClosure::gt:
        emit_branch(Assembler::encode_bgtz(rs, 0), destination); return;
      case BytecodeClosure::le:
        emit_branch(Assembler::encode_blez(rs, 0), destination); return;
      default:
        SHOULD_NOT_REACH_HERE(); return;
    }
  }

  // General case: get op2 into a register (immediates go to $at).
  Assembler::Register rt;
  if (s_cmp_op2_is_imm) {
    mips_li(this, at, (juint)s_cmp_op2_imm);
    rt = at;
  } else {
    rt = s_cmp_op2_reg;
  }

  switch (condition) {
    case BytecodeClosure::null:
    case BytecodeClosure::eq:
      emit_branch(Assembler::encode_beq(rs, rt, 0), destination); return;
    case BytecodeClosure::nonnull:
    case BytecodeClosure::ne:
      emit_branch(Assembler::encode_bne(rs, rt, 0), destination); return;
    case BytecodeClosure::lt:                        // rs <  rt
      emit(Assembler::encode_slt(at, rs, rt));
      emit_branch(Assembler::encode_bne(at, Assembler::zero, 0), destination); return;
    case BytecodeClosure::ge:                        // rs >= rt == !(rs < rt)
      emit(Assembler::encode_slt(at, rs, rt));
      emit_branch(Assembler::encode_beq(at, Assembler::zero, 0), destination); return;
    case BytecodeClosure::gt:                        // rs >  rt == rt < rs
      emit(Assembler::encode_slt(at, rt, rs));
      emit_branch(Assembler::encode_bne(at, Assembler::zero, 0), destination); return;
    case BytecodeClosure::le:                        // rs <= rt == !(rt < rs)
      emit(Assembler::encode_slt(at, rt, rs));
      emit_branch(Assembler::encode_beq(at, Assembler::zero, 0), destination); return;
    default:
      SHOULD_NOT_REACH_HERE(); return;
  }
}

void CodeGenerator::if_then_else(Value& result, BytecodeClosure::cond_op condition,
                                 Value& op1, Value& op2,
                                 ExtendedValue& result_true,
                                 ExtendedValue& result_false JVM_TRAPS) {
  (void)result; (void)condition; (void)op1; (void)op2;
  (void)result_true; (void)result_false;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::if_iinc(Value& result, BytecodeClosure::cond_op condition,
                            Value& op1, Value& op2,
                            Value& arg, int increment JVM_TRAPS) {
  (void)result; (void)condition; (void)op1; (void)op2; (void)arg; (void)increment;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::table_switch(Value& index, jint table_index, jint default_dest,
                                 jint low, jint high JVM_TRAPS) {
  (void)index; (void)table_index; (void)default_dest; (void)low; (void)high;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::lookup_switch(Value& index, jint table_index, jint default_dest,
                                  jint num_of_pairs JVM_TRAPS) {
  (void)index; (void)table_index; (void)default_dest; (void)num_of_pairs;
  SHOULD_NOT_REACH_HERE();
}

// ---- allocation / type checks ------------------------------------------------

void CodeGenerator::new_object(Value& result, JavaClass* klass JVM_TRAPS) {
  (void)result; (void)klass;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::new_object_array(Value& result, JavaClass* element_class,
                                     Value& length JVM_TRAPS) {
  (void)result; (void)element_class; (void)length;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::new_basic_array(Value& result, BasicType type,
                                    Value& length JVM_TRAPS) {
  (void)result; (void)type; (void)length;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::new_multi_array(Value& result JVM_TRAPS) {
  (void)result;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::init_static_array(Value& array JVM_TRAPS) {
  (void)array;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::check_cast(Value& object, Value& klass, int class_id JVM_TRAPS) {
  (void)object; (void)klass; (void)class_id;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::instance_of(Value& result, Value& object, Value& klass,
                                int class_id JVM_TRAPS) {
  (void)result; (void)object; (void)klass; (void)class_id;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::array_check(Value& array, Value& index JVM_TRAPS) {
  (void)array; (void)index;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::type_check(Value& object, Value& array, Value& index JVM_TRAPS) {
  (void)object; (void)array; (void)index;
  SHOULD_NOT_REACH_HERE();
}

// Fase 3 (Marco 3.4a): inline null check. The r5900 has no flags; test the
// reference directly with a branch-if-nonzero over the throw. When it is null,
// fall through to the runtime-throw path (NPE). i386 branches to a NullCheckStub
// instead; the hybrid emits the throw inline (no stub machinery -- see
// mips_emit_runtime_throw). The object must be register-resident (the shared
// maybe_null_check flushes it).
void CodeGenerator::null_check(const Value& object JVM_TRAPS) {
  GUARANTEE(object.in_register(), "null_check needs the object in a register");
  Label ok;
  // if object != null, skip the throw (emit_branch fills the delay slot with nop)
  emit_branch(Assembler::encode_bne(object.lo_register(), Assembler::zero, 0), ok);
  mips_emit_runtime_throw(this, ThrowExceptionStub::rte_null_pointer);
  bind(ok);
}

// ---- monitors / returns ------------------------------------------------------

void CodeGenerator::monitor_enter(Value& object JVM_TRAPS) {
  (void)object;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::monitor_exit(Value& object JVM_TRAPS) {
  (void)object;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::check_monitors(JVM_SINGLE_ARG_TRAPS) {
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::unlock_activation(JVM_SINGLE_ARG_TRAPS) {
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::return_result(Value& value JVM_TRAPS) {
  // Fase 2: single-word (int/object/float) returns via the C teardown helper.
  // long/double (two-word) returns arrive with FloatSupport/Fase 3.
  GUARANTEE(value.is_one_word(), "Fase 2: single-word returns only");
  // materialize the result into a0 (the jit_return_int argument register).
  if (value.is_immediate()) {
    mips_li(this, Assembler::a0, (juint)value.as_int());
  } else {
    GUARANTEE(value.in_register(), "result must be immediate or in a register");
    if (value.lo_register() != Assembler::a0) {
      mov(Assembler::a0, value.lo_register());
    }
  }
  mips_call_c(this, (address)jit_return_int);
  // native-stack epilogue: restore ra and return to call_from_interpreter.
  emit(Assembler::encode_lw(Assembler::ra, Assembler::sp, 0));
  emit(Assembler::encode_addiu(Assembler::sp, Assembler::sp, 16));
  emit(Assembler::encode_jr(Assembler::ra));
  emit(Assembler::encode_nop());
}

void CodeGenerator::return_error(Value& value JVM_TRAPS) {
  (void)value;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::return_void(JVM_SINGLE_ARG_TRAPS) {
  mips_call_c(this, (address)jit_return_void);
  emit(Assembler::encode_lw(Assembler::ra, Assembler::sp, 0));
  emit(Assembler::encode_addiu(Assembler::sp, Assembler::sp, 16));
  emit(Assembler::encode_jr(Assembler::ra));
  emit(Assembler::encode_nop());
}

// ---- integer / long arithmetic ----------------------------------------------

// Fase 3 (Marco 3.1): integer add/sub/and/or/xor/mul. The shared int_binary()
// guarantees op1 is in a register and op2 is a register or an immediate (and
// folds the all-immediate case away). We take a writable copy of op1 into the
// result register (reusing op1's register when it is dead) and emit the r5900
// three-operand form. Shifts/div/rem and reverse-subtract stay in bail-out until
// their Marcos (they must not be admitted by the compile trigger's whitelist).
void CodeGenerator::int_binary_do(Value& result, Value& op1, Value& op2,
                                  BytecodeClosure::binary_op op JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  GUARANTEE(!result.is_present(), "result must not be present");
  GUARANTEE(op1.in_register(), "op1 must be in a register");
  GUARANTEE(op2.is_immediate() || op2.in_register(),
            "op2 must be in a register or an immediate");

  op1.writable_copy(result);
  const Assembler::Register rd = result.lo_register();

  if (op2.in_register()) {
    const Assembler::Register rt = op2.lo_register();
    switch (op) {
      case BytecodeClosure::bin_add:
        emit(Assembler::encode_addu(rd, rd, rt)); return;
      case BytecodeClosure::bin_sub:
        emit(Assembler::encode_subu(rd, rd, rt)); return;
      case BytecodeClosure::bin_and:
        emit(Assembler::encode_and (rd, rd, rt)); return;
      case BytecodeClosure::bin_or:
        emit(Assembler::encode_or  (rd, rd, rt)); return;
      case BytecodeClosure::bin_xor:
        emit(Assembler::encode_xor (rd, rd, rt)); return;
      case BytecodeClosure::bin_mul:
        emit(Assembler::encode_mult(rd, rt));
        emit(Assembler::encode_mflo(rd)); return;
      // Marco 3.3: shifts. The r5900 variable-shift takes the amount from the
      // low 5 bits of rs (Java's `x << (n & 31)` semantics come for free).
      case BytecodeClosure::bin_shl:
        emit(Assembler::encode_sllv(rd, rd, rt)); return;   // rd = rd << rt
      case BytecodeClosure::bin_shr:
        emit(Assembler::encode_srav(rd, rd, rt)); return;   // arithmetic (signed)
      case BytecodeClosure::bin_ushr:
        emit(Assembler::encode_srlv(rd, rd, rt)); return;   // logical (unsigned)
      default:
        SHOULD_NOT_REACH_HERE(); return;
    }
  }

  // op2 is an immediate. Use an inline I-type form when the constant fits its
  // field (addiu sign-extends; andi/ori/xori zero-extend), else materialize it
  // into a scratch register and use the R-type form.
  const jint imm = op2.as_int();
  switch (op) {
    case BytecodeClosure::bin_add:
      if (imm >= -32768 && imm <= 32767) {
        emit(Assembler::encode_addiu(rd, rd, imm)); return;
      }
      break;
    case BytecodeClosure::bin_sub:
      if (-imm >= -32768 && -imm <= 32767) {
        emit(Assembler::encode_addiu(rd, rd, -imm)); return;  // rd += (-imm)
      }
      break;
    case BytecodeClosure::bin_and:
      if (imm >= 0 && imm <= 0xffff) {
        emit(Assembler::encode_andi(rd, rd, imm)); return;
      }
      break;
    case BytecodeClosure::bin_or:
      if (imm >= 0 && imm <= 0xffff) {
        emit(Assembler::encode_ori(rd, rd, imm)); return;
      }
      break;
    case BytecodeClosure::bin_xor:
      if (imm >= 0 && imm <= 0xffff) {
        emit(Assembler::encode_xori(rd, rd, imm)); return;
      }
      break;
    case BytecodeClosure::bin_mul:
      break;  // no mul-immediate form; always materialize
    // Marco 3.3: constant shift amount -> fixed-shift form (amount masked to the
    // 5-bit field, matching Java's `x << (n & 31)`). Always fits: never falls to
    // the materialize path below.
    case BytecodeClosure::bin_shl:
      emit(Assembler::encode_sll(rd, rd, imm & 31)); return;
    case BytecodeClosure::bin_shr:
      emit(Assembler::encode_sra(rd, rd, imm & 31)); return;
    case BytecodeClosure::bin_ushr:
      emit(Assembler::encode_srl(rd, rd, imm & 31)); return;
    default:
      SHOULD_NOT_REACH_HERE(); return;
  }

  const Assembler::Register rt = RegisterAllocator::allocate();
  mips_li(this, rt, (juint)imm);
  switch (op) {
    case BytecodeClosure::bin_add:
      emit(Assembler::encode_addu(rd, rd, rt)); break;
    case BytecodeClosure::bin_sub:
      emit(Assembler::encode_subu(rd, rd, rt)); break;
    case BytecodeClosure::bin_and:
      emit(Assembler::encode_and (rd, rd, rt)); break;
    case BytecodeClosure::bin_or:
      emit(Assembler::encode_or  (rd, rd, rt)); break;
    case BytecodeClosure::bin_xor:
      emit(Assembler::encode_xor (rd, rd, rt)); break;
    case BytecodeClosure::bin_mul:
      emit(Assembler::encode_mult(rd, rt));
      emit(Assembler::encode_mflo(rd)); break;
    default:
      SHOULD_NOT_REACH_HERE(); break;
  }
  RegisterAllocator::dereference(rt);
}

// Marco 3.3: integer negate (ineg). una_abs is only produced by Math.abs
// inlining (an invoke) -- outside the whitelist -- so it stays a bail-out.
void CodeGenerator::int_unary_do(Value& result, Value& op1,
                                 BytecodeClosure::unary_op op JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  op1.writable_copy(result);
  const Assembler::Register rd = result.lo_register();
  switch (op) {
    case BytecodeClosure::una_neg:
      emit(Assembler::encode_subu(rd, Assembler::zero, rd)); return;  // rd = 0 - rd
    default:
      SHOULD_NOT_REACH_HERE(); return;
  }
}

void CodeGenerator::long_binary_do(Value& result, Value& op1, Value& op2,
                                   BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::long_unary_do(Value& result, Value& op1,
                                  BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::long_cmp(Value& result, Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)op1; (void)op2;
  SHOULD_NOT_REACH_HERE();
}

// ---- float / double arithmetic (bodies emulated in Fase 4) -------------------

void CodeGenerator::float_binary_do(Value& result, Value& op1, Value& op2,
                                    BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::float_unary_do(Value& result, Value& op1,
                                   BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::double_binary_do(Value& result, Value& op1, Value& op2,
                                     BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::double_unary_do(Value& result, Value& op1,
                                    BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::float_cmp(Value& result, BytecodeClosure::cond_op cond,
                              Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)cond; (void)op1; (void)op2;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::double_cmp(Value& result, BytecodeClosure::cond_op cond,
                               Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)cond; (void)op1; (void)op2;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::fpu_clear(bool flush) {
  (void)flush;
  SHOULD_NOT_REACH_HERE();
}

// ---- conversions -------------------------------------------------------------

// Marco 3.3: narrowing int conversions. The shared layer handles the immediate
// case, so value is always register-resident. i2b/i2s sign-extend the low
// byte/halfword (shift up then arithmetic shift down); i2c zero-extends to a
// 16-bit char (andi). result may reuse value's register (in-place is fine).
void CodeGenerator::i2b(Value& result, Value& value JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  GUARANTEE(value.in_register(), "immediate case handled by the shared layer");
  result.assign_register();
  const Assembler::Register rd = result.lo_register();
  emit(Assembler::encode_sll(rd, value.lo_register(), 24));
  emit(Assembler::encode_sra(rd, rd, 24));
}
void CodeGenerator::i2c(Value& result, Value& value JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  GUARANTEE(value.in_register(), "immediate case handled by the shared layer");
  result.assign_register();
  emit(Assembler::encode_andi(result.lo_register(), value.lo_register(), 0xffff));
}
void CodeGenerator::i2s(Value& result, Value& value JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  GUARANTEE(value.in_register(), "immediate case handled by the shared layer");
  result.assign_register();
  const Assembler::Register rd = result.lo_register();
  emit(Assembler::encode_sll(rd, value.lo_register(), 16));
  emit(Assembler::encode_sra(rd, rd, 16));
}
void CodeGenerator::i2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::i2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::i2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::l2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::l2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::f2i(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::f2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::f2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::d2i(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::d2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::d2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}

// ---- invokes -----------------------------------------------------------------

void CodeGenerator::invoke(const Method* method, bool must_do_null_check JVM_TRAPS) {
  (void)method; (void)must_do_null_check;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::invoke_virtual(Method* method, int vtable_index,
                                   BasicType return_type JVM_TRAPS) {
  (void)method; (void)vtable_index; (void)return_type;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::invoke_interface(JavaClass* klass, int itable_index,
                                     int parameters_size,
                                     BasicType return_type JVM_TRAPS) {
  (void)klass; (void)itable_index; (void)parameters_size; (void)return_type;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::invoke_native(BasicType return_kind, address entry JVM_TRAPS) {
  (void)return_kind; (void)entry;
  SHOULD_NOT_REACH_HERE();
}

// ---- exceptions / vm calls ---------------------------------------------------

bool CodeGenerator::quick_catch_exception(const Value& value, JavaClass* catch_type,
                                          int handler_bci JVM_TRAPS) {
  (void)value; (void)catch_type; (void)handler_bci;
  SHOULD_NOT_REACH_HERE();
  return false;
}

// Fase 3 (Marco 3.4a): unconditional throw of a simple runtime exception, used
// by the shared compiler for a statically-known-null access (array.must_be_null).
// i386 does frame()->clear() + call_vm(exception_allocator) + return_error; the
// hybrid clears the model and emits the same helper-C throw path as null_check /
// array_check. The code after this point in the basic block is dead (the throw
// never returns to compiled code).
void CodeGenerator::throw_simple_exception(int rte JVM_TRAPS) {
  frame()->clear();
  mips_emit_runtime_throw(this, rte);
}

void CodeGenerator::call_vm_extra_arg(const Register extra_arg) {
  (void)extra_arg;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::call_vm(address entry, BasicType return_value_type JVM_TRAPS) {
  (void)entry; (void)return_value_type;
  SHOULD_NOT_REACH_HERE();
}

// Fase 3 (Marco 3.2b): emitted on every backward branch (loop back-edge) by the
// shared CodeGenerator::branch when destination <= bci. The r5900 hybrid avoids
// porting call_vm + TimerTickStub + the stub queue (the ARM/i386 design): it
// instead flushes the VirtualStackFrame to memory (locals -> g_jlocals, any
// expression values -> g_jsp) so no Java value lives in a caller-saved register
// across the call, then makes an unconditional C-ABI call to jit_timer_tick,
// which replicates the interpreter's own check_timer_tick (the pending-tick test
// lives in C). Flushing here also keeps the Java frame consistent for a GC or
// thread switch that a taken tick may trigger. Correctness first; folding the
// tick test inline (fast path when no tick is pending) is a later optimization.
void CodeGenerator::check_timer_tick(JVM_SINGLE_ARG_TRAPS) {
  frame()->flush(JVM_SINGLE_ARG_NO_CHECK);
  mips_call_c(this, (address)jit_timer_tick);
}

// ---- inline-cache / compilation stubs ---------------------------------------

void CodeGenerator::check_cast_stub(CompilationQueueElement* cqe JVM_TRAPS) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::instance_of_stub(CompilationQueueElement* cqe JVM_TRAPS) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::new_object_stub(CompilationQueueElement* cqe JVM_TRAPS) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::new_type_array_stub(CompilationQueueElement* cqe JVM_TRAPS) {
  (void)cqe;
  SHOULD_NOT_REACH_HERE();
}

// ---- arraycopy ---------------------------------------------------------------

bool CodeGenerator::arraycopy(JVM_SINGLE_ARG_TRAPS) {
  SHOULD_NOT_REACH_HERE();
  return false;
}

bool CodeGenerator::unchecked_arraycopy(BasicType array_element_type JVM_TRAPS) {
  (void)array_element_type;
  SHOULD_NOT_REACH_HERE();
  return false;
}

#endif // ENABLE_COMPILER
