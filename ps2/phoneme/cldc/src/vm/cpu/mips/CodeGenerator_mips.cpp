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
extern "C" void jit_timer_tick(int bci); // Marco 3.2b: backward-branch timer check (+GC safepoint bci)
// Marco 3.4a: runtime-exception throw helpers. Thin wrappers over the C
// interpreter's own interpreter_throw_* (Interpreter_c.cpp, patch #44): allocate
// the exception, run find_exception_frame over the g_jfp chain jit_frame_enter
// built, and reposition g_jfp/g_jsp/g_jpc/g_jlocals to the handler frame. See the
// throw-path emission in mips_emit_runtime_throw and JIT_PLAN.md 7.
extern "C" void jit_throw_null_pointer();
extern "C" void jit_throw_array_index();
// Marco 3.6b-real: the real (non-inlined) resolved static call. Resolves the
// callee from the caller frame's cpool (index = compile-time constant, GC-safe),
// runs the class-init barrier, invokes, and drives an interpreted callee to
// completion with a nested dispatch loop bounded by the caller frame (a compiled
// callee returns via jit_return so the loop does not iterate). See the emission
// in CodeGenerator::invoke, the option-B fp-check in mips_emit_invoke_fp_check,
// and JIT_PLAN.md 7.
extern "C" void jit_invoke_static(int cpool_index, int invoker_size, int bci);
// Marco 3.6c-vtable: super.m()/private call (_fast_invokespecial). Same shape as
// jit_invoke_static, but the callee is resolved via the CPOOL class's vtable
// (vindex+klazz_id -> class -> ClassInfo -> get_method_from_ci), not a direct
// method pointer -- the static binding of invokespecial (e.g. the superclass for
// super.m()). See CodeGenerator::invoke's helper branch.
extern "C" void jit_invoke_special(int cpool_index, int invoker_size, int bci);
// Marco 3.6c-vtable 2/3: virtual call (_fast_invokevirtual). Dispatches DYNAMICALLY
// on the receiver's vtable (get_method_from_vtable), so an overriding subclass method
// is chosen at runtime. Same (cpool_index, invoker_size) shape. See invoke_virtual.
extern "C" void jit_invoke_virtual(int cpool_index, int invoker_size, int bci);
// Marco 3.6c-vtable 3/3: interface call (_fast_invokeinterface). Dynamic dispatch via
// the receiver's itable (linear search of the interface class_id). Takes num_params
// (not invoker_size -- the length 5 is fixed inside the helper). See invoke_interface.
extern "C" void jit_invoke_interface(int cpool_index, int num_params, int bci);

// Marco 3.7b: array-store type check for a compiled aastore. Mirrors the interp's
// aastore type_check (-> array_store_type_check via call_vm): raises ArrayStoreException
// if obj is not assignable to the element type of the array ref (null obj is always
// OK), repositioning g_jfp for the option-B unwind. See CodeGenerator::type_check.
extern "C" void jit_array_store_check(address ref, address obj, int idx, int bci);

// Marco 3.8: object allocation + type checks from compiled code. The shared closure
// resolves the class at COMPILE TIME and hands the backend a stable class_id (a small
// constant -> GC-safe, no moveable oop embedded in the native code); these helpers
// re-derive the class at runtime via get_class_by_id. jit_new allocates (may GC / may
// throw OutOfMemoryError); jit_checkcast throws ClassCastException on a failed subtype
// check; jit_instanceof returns 0/1 and never throws (the class is already resolved).
extern "C" address jit_new(int class_id, int bci);
extern "C" void    jit_checkcast(int class_id, int bci);
extern "C" jint    jit_instanceof(int class_id);

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

// --- Fase 3 (Marco 3.6b-real): option-B unwind fp-check after a real call ------
// Emitted after every real (non-inlined) invoke. jit_invoke_static drives the
// callee and returns here. If an exception unwound PAST this compiled method,
// find_exception_frame has repositioned g_jfp (s0) to a handler frame ABOVE this
// one, so s0 no longer equals the caller frame fp_A that method_entry saved at
// 4(sp). In that case eject via the bare native epilogue (restore $ra/$sp, jr ra;
// NO jit_return -- the Java frame is already unwound), propagating the unwind one
// native frame further, exactly like mips_emit_runtime_throw. When s0 == fp_A the
// call returned normally and we fall through to the continuation. The Java stack
// grows down (JavaStackDirection < 0), so a live callee frame has a SMALLER g_jfp;
// equality is the only "returned to me" signal, hence beq.
static void mips_emit_invoke_fp_check(BinaryAssembler* a) {
  a->emit(Assembler::encode_lw(Assembler::at, Assembler::sp, 4));   // at = fp_A
  BinaryAssembler::Label cont;
  // if g_jfp(s0) == fp_A -> normal return, skip the eject (delay slot = nop)
  a->emit_branch(Assembler::encode_beq(Assembler::fp, Assembler::at, 0), cont);
  // eject: bare native epilogue (identical to mips_emit_runtime_throw's tail)
  a->emit(Assembler::encode_lw(Assembler::ra, Assembler::sp, 0));
  a->emit(Assembler::encode_addiu(Assembler::sp, Assembler::sp, 16));
  a->emit(Assembler::encode_jr(Assembler::ra));
  a->emit(Assembler::encode_nop());
  a->bind(cont);
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
  // Marco 3.6b-real (option-B unwind): jit_frame_enter left s0 (g_jfp) = this
  // method's own Java frame (fp_A). Save it so the post-invoke fp-check can detect
  // an exception that unwound past us (s0 != fp_A). Harmless for leaves that never
  // invoke -- one extra store, and the fp-check is only emitted at real calls.
  emit(Assembler::encode_sw(Assembler::fp, Assembler::sp, 4));
}

// In the C-interpreter hybrid the Java expression stack is g_jsp (s1), owned by
// the interpreter and set up by jit_frame_enter. Marco 3.6a makes the physical
// stack pointer real: the shared VirtualStackFrame calls increment_stack_pointer_by
// (via set_stack_pointer) whenever the real stack pointer must catch up to the
// virtual one -- at a flush with live expression values (spill) or when
// materializing invoke arguments. Values stay register-resident; moving s1 only
// reserves/releases their memory homes so store_to_address (jsp-relative) and the
// C helpers that read g_jsp (jit_invoke/jit_timer_tick/jit_throw_*) see the right
// stack depth. The Java stack grows down (JavaStackDirection < 0), so a deeper
// virtual sp (positive adjustment) decreases g_jsp -- mirrors i386's
// leal(esp, [esp - elt*adjustment]). For the straight-line whitelisted methods of
// Marcos 3.1-3.5 the expression stack is empty at every flush, so adjustment is
// always 0 and nothing is emitted (byte-identical to the old no-op). Called in the
// normal compile flow -> must NOT bail out.
void CodeGenerator::increment_stack_pointer_by(int adjustment) {
  if (adjustment == 0) {
    return;
  }
  const int delta = -BytesPerStackElement * adjustment;
  if (delta >= -32768 && delta <= 32767) {
    emit(Assembler::encode_addiu(Assembler::jsp, Assembler::jsp, delta));
  } else {
    // Deep frames beyond the addiu imm16 range: materialize into $at (scratch,
    // outside the register pool) and add. Realistic method stacks never hit this.
    mips_li(this, Assembler::at, (juint)delta);
    emit(Assembler::encode_addu(Assembler::jsp, Assembler::jsp, Assembler::at));
  }
}

// No-op on the C-interpreter hybrid: jit_frame_enter already leaves s1 (g_jsp) at
// the callee's empty expression stack (SET_FRAME(stack_bottom_pointer, g_jsp)),
// which is the physical work i386's clear_stack does in its compiled prologue
// (leal esp, [ebp + stack_bottom_pointer_offset]). VirtualStackFrame::clear()
// resets the model's real stack pointer to max_locals-1 (empty stack) right after
// the method entry helper ran, so s1 already matches -- nothing to emit. (clear()
// is otherwise only reached from throw_simple_exception, a dead end.) Called in
// the normal compile flow -> must NOT bail out.
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
// stored directly. Two-word (long/double) still arrive with a later Marco.
// Marco 3.7a: an object/array (reference) store into the HEAP additionally emits
// a GC write barrier (see HeapAddress::write_barrier_* in Addressing_mips). A
// reference store to a LOCAL is a plain MemoryAddress whose barrier is a no-op.
void CodeGenerator::store_to_address(Value& value, BasicType type,
                                     MemoryAddress& address) {
  if (!value.is_present()) return;   // nothing to store
  GUARANTEE(stack_type_for(type) == value.stack_type(),
            "types must match (taking stack types into account)");

  // Resolve the source register first -- it is independent of the address, and the
  // write-barrier prolog (below) may reallocate the address into a fresh register.
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

  // Marco 3.7a: reference store into the heap needs a write barrier UNLESS the
  // value is provably not a heap pointer (e.g. a NULL constant). write_barrier is
  // a no-op on LocationAddress/StackAddress, so only field/array-element stores
  // take it. Mirrors i386 store_to_address (prolog -> store -> epilog).
  const bool needs_barrier =
      (type == T_OBJECT || type == T_ARRAY) && !value.not_on_heap();
  if (needs_barrier) {
    address.write_barrier_prolog();          // allocates addr_reg = &slot
  }

  // After the prolog, HeapAddress::address_for returns the address register (disp 0).
  const BinaryAssembler::Address addr = address.lo_address();
  const Assembler::Register base = addr.base();
  const int off = addr.disp();

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

  if (needs_barrier) {
    address.write_barrier_epilog();
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

// ============================================================================
// Fase 5 (bail-limpo): CLEAN COMPILE BAIL-OUT for not-yet-emitted operations.
//
// The methods below are the bytecodes/operations the r5900 backend does not yet
// emit (float/double/long arithmetic & conversions, array allocation, monitors,
// switch, athrow, arraycopy). The compile trigger's bytecode whitelist normally
// keeps a method containing any of them from being armed at all -- but so that a
// HOT method which slips one through (or a real game, once the whitelist is
// relaxed / hotness-driven) NEVER crashes, each aborts the active compilation
// cleanly instead of SHOULD_NOT_REACH_HERE (which is a NO-OP in PRODUCT ->
// silent corruption). Compiler::abort_active_compilation throws the internal
// out-of-memory signal (JVM_THROW returns from here); the Compiler catches it,
// discards the partial compiled code, and the method stays interpreted -- the
// exact primitive already used above for invoke-while-inlining and invoke_native.
// Bodies keep the (void) casts so the signatures stay documented and no
// unused-parameter warnings appear after the SHOULD_NOT_REACH_HERE is gone.
// Bytecode-entry points reachable from a real game convert; internal invariants
// (switch defaults over a known enum/type) and provably-dead code without a
// JVM_TRAPS arg (overflow, fpu_clear, the *_stub queue handlers) stay
// SHOULD_NOT_REACH_HERE.
//
// PERMANENT vs TRANSIENT: these operations will NEVER compile in this build, so
// they abort with is_permanent=TRUE (set_impossible_to_compile) -- the method
// bails ONCE and is thereafter interpreted without re-attempting. is_permanent=
// false would re-enter try_to_compile on every invocation, and its
// compiler_area_soft_collect can evict already-compiled methods (observed on real
// HW as hot invokes silently losing their compiled code). Only genuinely transient
// bails stay false: invoke-while-inlining (may compile without the inline next
// time) and uncommon_trap (an unresolved class may load later; shared
// CodeGenerator.cpp). See references/JIT_PLAN.md §7.
// ============================================================================

// if_then_else / if_iinc: only produced by OptimizeForwardBranches, which is
// forced off on MIPS (USE_OPT_FORWARD_BRANCH=0, patch #38b) -- kept as a bail-out
// for defense in depth.
void CodeGenerator::if_then_else(Value& result, BytecodeClosure::cond_op condition,
                                 Value& op1, Value& op2,
                                 ExtendedValue& result_true,
                                 ExtendedValue& result_false JVM_TRAPS) {
  (void)result; (void)condition; (void)op1; (void)op2;
  (void)result_true; (void)result_false;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::if_iinc(Value& result, BytecodeClosure::cond_op condition,
                            Value& op1, Value& op2,
                            Value& arg, int increment JVM_TRAPS) {
  (void)result; (void)condition; (void)op1; (void)op2; (void)arg; (void)increment;
  Compiler::abort_active_compilation(true JVM_THROW);
}

// tableswitch / lookupswitch: dense/sparse jump tables. Not yet emitted.
void CodeGenerator::table_switch(Value& index, jint table_index, jint default_dest,
                                 jint low, jint high JVM_TRAPS) {
  (void)index; (void)table_index; (void)default_dest; (void)low; (void)high;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::lookup_switch(Value& index, jint table_index, jint default_dest,
                                  jint num_of_pairs JVM_TRAPS) {
  (void)index; (void)table_index; (void)default_dest; (void)num_of_pairs;
  Compiler::abort_active_compilation(true JVM_THROW);
}

// ---- allocation / type checks ------------------------------------------------

// Fase 3 (Marco 3.8): allocate a fixed-size instance from compiled code. The shared
// BytecodeCompileClosure::new_object already resolved AND initialized the class at
// compile time (get_klass_or_null(index, /*needs_init=*/true); an unresolved or
// uninitialized/abstract/interface class takes the uncommon_trap path instead). We
// pass its class_id -- a stable constant, so no moveable oop is embedded (GC-safe) --
// to jit_new, which allocates via the interpreter's call_vm path (GC-safe; may throw
// OutOfMemoryError). Because that allocation can trigger a GC, flush the frame first
// so every live oop is memory-resident (a root) before the call. On OOM the interp's
// find_exception_frame repositions g_jfp, so run the option-B fp-check after the call
// to eject if the handler is an ancestor frame. The fresh instance comes back in v0.
void CodeGenerator::new_object(Value& result, JavaClass* klass JVM_TRAPS) {
  frame()->flush(JVM_SINGLE_ARG_CHECK);
  mips_li(this, Assembler::a0, (juint)klass->class_id());
  mips_li(this, Assembler::a1, (juint)bci());    // a1 = bci (GC safepoint: alloc may GC)
  mips_call_c(this, (address)jit_new);
  mips_emit_invoke_fp_check(this);               // OOM unwind (option B)
  result.set_register(RegisterAllocator::allocate());
  move(result.lo_register(), Assembler::v0);     // v0 = fresh instance
  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: new -> jit_new emitted (Marco 3.8)"); } }
}

// Array allocation (anewarray / newarray / multianewarray / static array init):
// not yet emitted (a fixed-size instance `new` is, via jit_new -- Marco 3.8).
// Bail cleanly; a game method allocating an array stays interpreted.
void CodeGenerator::new_object_array(Value& result, JavaClass* element_class,
                                     Value& length JVM_TRAPS) {
  (void)result; (void)element_class; (void)length;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::new_basic_array(Value& result, BasicType type,
                                    Value& length JVM_TRAPS) {
  (void)result; (void)type; (void)length;
  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 5: new_basic_array -> clean bail (abort compile)"); } }
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::new_multi_array(Value& result JVM_TRAPS) {
  (void)result;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::init_static_array(Value& array JVM_TRAPS) {
  (void)array;
  Compiler::abort_active_compilation(true JVM_THROW);
}

// Fase 3 (Marco 3.8): checkcast. The i386 backend inlines a subtype-cache probe with a
// slow-case stub; the C hybrid instead pushes the object onto the expression stack,
// flushes so it lands at the top of g_jsp, and lets jit_checkcast read it (OBJ_PEEK(0)),
// do the subtype check against the compile-time class_id, and throw ClassCastException
// on failure. A null object always passes. The throw uses the interp's own path
// (find_exception_frame -> g_jfp), so the option-B fp-check ejects if the CCE handler
// is an ancestor frame. The closure re-pushes the object afterwards (checkcast leaves
// its operand on the stack), so we pop it back into the Value here. `klass` (a moveable
// oop) is unused -- class_id drives the runtime resolve.
void CodeGenerator::check_cast(Value& object, Value& klass, int class_id JVM_TRAPS) {
  (void)klass;
  frame()->push(object);
  frame()->flush(JVM_SINGLE_ARG_CHECK);          // object -> g_jsp top (OBJ_PEEK(0))
  mips_li(this, Assembler::a0, (juint)class_id);
  mips_li(this, Assembler::a1, (juint)bci());    // a1 = bci (GC safepoint: may throw CCE)
  mips_call_c(this, (address)jit_checkcast);
  mips_emit_invoke_fp_check(this);               // CCE unwind (option B)
  frame()->pop(object);
  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: checkcast -> jit_checkcast emitted (Marco 3.8)"); } }
}

// Fase 3 (Marco 3.8): instanceof. Same push/flush so jit_instanceof can read the object
// (OBJ_PEEK(0)); it returns 0/1 in v0 and never throws for a resolved class, so there is
// no fp-check. The object is consumed (unlike checkcast) -- pop it off the stack and put
// the int result in a register for the closure to push.
void CodeGenerator::instance_of(Value& result, Value& object, Value& klass,
                                int class_id JVM_TRAPS) {
  (void)klass;
  frame()->push(object);
  frame()->flush(JVM_SINGLE_ARG_CHECK);
  mips_li(this, Assembler::a0, (juint)class_id);
  mips_call_c(this, (address)jit_instanceof);     // v0 = 0/1
  frame()->pop(object);
  result.set_register(RegisterAllocator::allocate());
  move(result.lo_register(), Assembler::v0);
  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: instanceof -> jit_instanceof emitted (Marco 3.8)"); } }
}

// Fase 3 (Marco 3.4b): null + bounds check for an array access. Mirrors the i386
// array_check (CodeGenerator_i386.cpp:651), adapted to the r5900 (no compare-
// against-memory, no flags): null_check reuses the Marco 3.4a inline throw; the
// length is loaded into a register and the bound is an UNSIGNED compare
// (`index < length`), which catches a negative index for free (as a huge
// unsigned). If the index is NOT in bounds, branch into the runtime-throw path
// (ArrayIndexOutOfBoundsException) -- same helper-C mechanism as null_check.
void CodeGenerator::array_check(Value& array, Value& index JVM_TRAPS) {
  maybe_null_check(array JVM_CHECK);
  GUARANTEE(array.in_register(), "array must be register-resident for the check");

  // length = *(array + Array::length_offset())
  const Assembler::Register len = RegisterAllocator::allocate();
  emit(Assembler::encode_lw(len, array.lo_register(), Array::length_offset()));

  // at = (index < length)  [unsigned]; in bounds iff at == 1.
  const Assembler::Register at = Assembler::at;
  if (index.is_immediate()) {
    mips_li(this, at, (juint)index.as_int());
    emit(Assembler::encode_sltu(at, at, len));
  } else {
    GUARANTEE(index.in_register(), "index must be immediate or in a register");
    emit(Assembler::encode_sltu(at, index.lo_register(), len));
  }
  RegisterAllocator::dereference(len);

  Label ok;
  emit_branch(Assembler::encode_bne(at, Assembler::zero, 0), ok);   // in bounds -> skip
  mips_emit_runtime_throw(this, ThrowExceptionStub::rte_array_index_out_of_bounds);
  bind(ok);
}

// Fase 3 (Marco 3.7b): the aastore array-store type check. The shared header names
// the parameters (object, array, index), but POSITIONALLY they are the array, the
// index, and the value being stored (see BytecodeCompileClosure::store_array's call
// `type_check(array, index, value)` and the i386 backend); we use semantic names.
// The interp raises ArrayStoreException when the value is not assignable to the
// array's element type; we mirror it with a C helper (jit_array_store_check ->
// interp type_check -> array_store_type_check via call_vm). Since that call can GC
// and can throw (repositioning g_jfp), we flush the frame first and run the option-B
// fp-check after -- exactly like a real invoke. The operands are re-pushed (mirrors
// i386) so the flush materializes them in g_jsp: the call clobbers t0-t8, so they
// must survive in memory (the popped Values reload from g_jsp when store_to_array
// runs next).
void CodeGenerator::type_check(Value& array, Value& index, Value& value JVM_TRAPS) {
  frame()->push(array);
  frame()->push(index);
  frame()->push(value);                        // value on top of the expression stack
  frame()->flush(JVM_SINGLE_ARG_CHECK);

  // After the flush, g_jsp (s1) points at the top slot: arg_offset_from_sp(0)=value,
  // (1)=index, (2)=array (BytesPerStackElement each), matching the invoke's receiver
  // read convention. Load ref/obj/idx into the C-arg registers for the helper.
  emit(Assembler::encode_lw(Assembler::a0, Assembler::jsp,
                            JavaFrame::arg_offset_from_sp(2)));   // a0 = ref (array)
  emit(Assembler::encode_lw(Assembler::a1, Assembler::jsp,
                            JavaFrame::arg_offset_from_sp(0)));   // a1 = obj (value)
  emit(Assembler::encode_lw(Assembler::a2, Assembler::jsp,
                            JavaFrame::arg_offset_from_sp(1)));   // a2 = idx (index)
  mips_li(this, Assembler::a3, (juint)bci());   // a3 = bci (GC safepoint: type_check may GC)
  mips_call_c(this, (address)jit_array_store_check);

  // Option-B unwind: if the check threw ArrayStoreException, find_exception_frame
  // repositioned g_jfp past this frame -> eject via the bare native epilogue.
  mips_emit_invoke_fp_check(this);

  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: aastore type_check -> jit_array_store_check "
                    "emitted (Marco 3.7b)"); } }

  frame()->pop(value);
  frame()->pop(index);
  frame()->pop(array);
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

// Monitors (monitorenter / monitorexit and the synchronized-method entry/exit
// bookkeeping): not yet emitted. A synchronized method is already refused by the
// arm trigger (JVM_ACC_SYNCHRONIZED), and a synchronized block carries an
// exception table (refused by has_no_exception_table); these bail cleanly as a
// backstop for any path the whitelist does not cover.
void CodeGenerator::monitor_enter(Value& object JVM_TRAPS) {
  (void)object;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::monitor_exit(Value& object JVM_TRAPS) {
  (void)object;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::check_monitors(JVM_SINGLE_ARG_TRAPS) {
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::unlock_activation(JVM_SINGLE_ARG_TRAPS) {
  Compiler::abort_active_compilation(true JVM_THROW);
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

// athrow in a method with no matching handler lowers to return_error (see
// BytecodeCompileClosure::throw_exception). Not yet emitted -> bail cleanly so a
// method that throws is interpreted (its exception then unwinds via the interp).
void CodeGenerator::return_error(Value& value JVM_TRAPS) {
  (void)value;
  Compiler::abort_active_compilation(true JVM_THROW);
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
// three-operand form. div/rem (idiv/irem) and reverse-subtract are not yet
// emitted; each switch default below now bails the compilation cleanly (Fase 5)
// instead of SHOULD_NOT_REACH_HERE, so a hot game method doing integer division
// stays interpreted rather than crashing / corrupting in PRODUCT.
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
      default:                                              // div/rem/rsub: Fase 5 bail
        Compiler::abort_active_compilation(true JVM_THROW);
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
    default:                                            // div/rem/rsub: Fase 5 bail
      { static bool _once = false; if (!_once) { _once = true;
          tty->print_cr("[PS2ME-JIT] Fase 5: int_binary_do div/rem -> clean bail "
                        "(abort compile)"); } }
      Compiler::abort_active_compilation(true JVM_THROW);
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
    default:                                            // div/rem/rsub: Fase 5 bail
      Compiler::abort_active_compilation(true JVM_THROW);
  }
  RegisterAllocator::dereference(rt);
}

// Marco 3.3: integer negate (ineg). una_abs is only produced by Math.abs
// inlining (an invoke); the default now bails cleanly (Fase 5) instead of
// SHOULD_NOT_REACH_HERE.
void CodeGenerator::int_unary_do(Value& result, Value& op1,
                                 BytecodeClosure::unary_op op JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  op1.writable_copy(result);
  const Assembler::Register rd = result.lo_register();
  switch (op) {
    case BytecodeClosure::una_neg:
      emit(Assembler::encode_subu(rd, Assembler::zero, rd)); return;  // rd = 0 - rd
    default:
      Compiler::abort_active_compilation(true JVM_THROW);
  }
}

// long (two-word integer) arithmetic / compare: not yet emitted. Bail cleanly.
void CodeGenerator::long_binary_do(Value& result, Value& op1, Value& op2,
                                   BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::long_unary_do(Value& result, Value& op1,
                                  BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::long_cmp(Value& result, Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)op1; (void)op2;
  Compiler::abort_active_compilation(true JVM_THROW);
}

// ---- float / double arithmetic (Fase 4) --------------------------------------
// Not yet emitted (FPU r5900 single non-IEEE for float; soft-float for double).
// Bail cleanly so a game method using float/double stays interpreted; the profile
// on real games decides whether Fase 4 comes next (JIT_PLAN §7).

void CodeGenerator::float_binary_do(Value& result, Value& op1, Value& op2,
                                    BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::float_unary_do(Value& result, Value& op1,
                                   BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::double_binary_do(Value& result, Value& op1, Value& op2,
                                     BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::double_unary_do(Value& result, Value& op1,
                                    BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::float_cmp(Value& result, BytecodeClosure::cond_op cond,
                              Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)cond; (void)op1; (void)op2;
  Compiler::abort_active_compilation(true JVM_THROW);
}

void CodeGenerator::double_cmp(Value& result, BytecodeClosure::cond_op cond,
                               Value& op1, Value& op2 JVM_TRAPS) {
  (void)result; (void)cond; (void)op1; (void)op2;
  Compiler::abort_active_compilation(true JVM_THROW);
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
// Conversions involving long/float/double: not yet emitted (Fase 4). i2b/i2c/i2s
// (narrowing int->int) are covered above (Marco 3.3). Bail cleanly.
void CodeGenerator::i2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::i2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::i2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::l2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::l2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::f2i(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::f2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::f2d(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::d2i(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::d2l(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}
void CodeGenerator::d2f(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; Compiler::abort_active_compilation(true JVM_THROW);
}

// ---- invokes -----------------------------------------------------------------

// Fase 3 (Marco 3.6b-real): the REAL (non-inlined) resolved static call. The
// shared front-end inlines small whitelisted callees (internal_compile_inlined,
// Marco 3.6b-inline) BEFORE reaching here; this emitter handles the ones it does
// NOT inline (callee too big / non-leaf -- bytecode_inline_prepass rejects them),
// which the widened whitelist now admits precisely because they never inline.
//
// Emission (mirrors i386, but the frame/call/unwind protocol lives in C helpers):
//   flush the VSF (args land in g_jsp memory; every value is marked flushed so the
//   post-call reloads come from memory -- the C helper clobbering t0-t8 is safe) ->
//   a0 = cpool index (compile-time constant, resolved at runtime against the caller
//   frame's cpool by jit_invoke_static -> GC-safe, no oop literal), a1 = invoker
//   size -> call jit_invoke_static -> adjust_for_invoke updates the model (pop
//   params, push the memory-resident result) -> option-B fp-check.
//
// Resolution: invokestatic and _fast_invokevirtual_final (Marco 3.6c: constructors
// <init> + final methods) both resolve the callee DIRECTLY from the cpool (a direct
// method pointer), so jit_invoke_static resolves them the same way at runtime -- its
// class-init barrier is a guaranteed no-op for the _final case (the holder is already
// initialized: a receiver / a just-constructed instance exists). The only extra work
// for _final is the receiver null-check (must_do_null_check). An invoke reached while
// inlining (the cpool index would not match the single runtime frame's cpool) is
// barred and bails cleanly.
//
// Marco 3.6c-vtable also routes _fast_invokespecial (super.m() non-init + private)
// HERE, because the shared fast_invoke_special closure lowers through __ invoke.
// But unlike the static/final forms it does NOT resolve to a direct method pointer
// -- the cpool holds vindex+klazz_id, so jit_invoke_static (which re-resolves with
// get_from_cpool) would fetch garbage. We branch on the invoke bytecode and emit
// jit_invoke_special instead (it re-resolves via the cpool class's vtable, mirroring
// bc_impl_fast_invokespecial). Everything else -- flush, receiver null-check,
// adjust_for_invoke, fp-check -- is identical. Vtable/itable-resolved forms that
// re-dispatch on the RECEIVER (_fast_invokevirtual / interface) stay in
// invoke_virtual/invoke_interface (still abort).
void CodeGenerator::invoke(const Method* method, bool must_do_null_check JVM_TRAPS) {
  if (Compiler::is_inlining()) {
    { static bool _once = false; if (!_once) { _once = true;
        tty->print_cr("[PS2ME-JIT] Fase 3: invoke while inlining -> abort compile"); } }
    Compiler::abort_active_compilation(false JVM_THROW);   // inlining is transient
  }

  // Read the cpool index from the current invoke bytecode's operand in the (root)
  // method being compiled. Not inlining here (guarded above) -> root == current.
  Method* caller = root_method();
  const jint at_bci = bci();
  const jubyte invoke_bc = caller->ubyte_at(at_bci);
  GUARANTEE(invoke_bc == Bytecodes::_fast_invokestatic ||
            invoke_bc == Bytecodes::_fast_init_invokestatic ||
            invoke_bc == Bytecodes::_fast_invokevirtual_final ||
            invoke_bc == Bytecodes::_fast_invokespecial,
            "Marco 3.6b/3.6c compile only cpool-resolved calls here");
  const jushort cp_index = (jushort)
    (((jint)caller->ubyte_at(at_bci + 1) << 8) | (jint)caller->ubyte_at(at_bci + 2));

  // Flush args + live temporaries to g_jsp memory (see header note on safety).
  frame()->flush(JVM_SINGLE_ARG_CHECK);

  // Marco 3.6c: receiver null-check for a call with a receiver (_final). After the
  // flush the receiver (the deepest arg) lives in g_jsp memory at
  // arg_offset_from_sp(size_of_parameters - 1); load it and null_check it (reuses the
  // 3.4a inline throw path -- on null it throws NPE and unwinds out of this frame).
  if (must_do_null_check) {
    Value receiver(T_OBJECT);
    receiver.set_register(RegisterAllocator::allocate());
    emit(Assembler::encode_lw(receiver.lo_register(), Assembler::jsp,
             JavaFrame::arg_offset_from_sp(method->size_of_parameters() - 1)));
    null_check(receiver JVM_CHECK);
  }   // receiver Value destructs here -> frees its register

  // a0 = cpool index, a1 = invoker size (3 = fast_invokestatic/_special/_final length),
  // a2 = bci. The bci is the GC safepoint: jit_set_safepoint_bcp in the helper sets
  // g_jpc = method_base + bci so a GC inside the callee scans THIS compiled frame with
  // the correct stackmap (the frame is otherwise stuck at bci=0 -> derived oops stale).
  mips_li(this, Assembler::a0, (juint)cp_index);
  mips_li(this, Assembler::a1, (juint)3);
  mips_li(this, Assembler::a2, (juint)at_bci);
  // Marco 3.6c-vtable: _fast_invokespecial re-resolves via the cpool class's vtable
  // (vindex+klazz_id), so it needs jit_invoke_special; the direct (cpool method
  // pointer) forms use jit_invoke_static.
  mips_call_c(this, (invoke_bc == Bytecodes::_fast_invokespecial)
                      ? (address)jit_invoke_special : (address)jit_invoke_static);

  // Model update: pop the parameter block, push the result (marked flushed to
  // memory -- return_internal PUSHed it onto g_jsp, so later reads reload it).
  Signature::Raw signature = method->signature();
  frame()->adjust_for_invoke(method->size_of_parameters(),
                             signature().return_type());

  // Option-B unwind: eject if the callee unwound past this frame.
  mips_emit_invoke_fp_check(this);

  if (invoke_bc == Bytecodes::_fast_invokespecial) {
    static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: _fast_invokespecial -> jit_invoke_special "
                    "emitted (Marco 3.6c-vtable)"); }
  } else {
    static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: non-inlined invoke -> REAL call emitted "
                    "(Marco 3.6b-real)"); }
  }
}

// Fase 3 (Marco 3.6c-vtable 2/3): the REAL virtual call (_fast_invokevirtual). The
// receiver's DYNAMIC type selects the method at runtime (unlike _final/special, which
// bind statically). This mirrors CodeGenerator::invoke's real-call emission exactly,
// but calls jit_invoke_virtual (which re-resolves via the receiver's vtable). The
// type-info devirtualization fast-path is gated OFF on MIPS (BytecodeCompileClosure.
// cpp), so fast_invoke_virtual ALWAYS lands here -- a real call, never an inline nor a
// CodeGenerator::invoke direct-call -- keeping the whitelist's 100%-coverage invariant.
// vtable_index is not needed by the emitter (the helper reads vindex from the cpool,
// GC-safe); the receiver null-check is mandatory for a virtual call.
void CodeGenerator::invoke_virtual(Method* method, int vtable_index,
                                   BasicType return_type JVM_TRAPS) {
  (void)vtable_index;
  if (Compiler::is_inlining()) {
    Compiler::abort_active_compilation(false JVM_THROW);   // inlining is transient
    return;
  }

  // Read the cpool index from the current _fast_invokevirtual bytecode's operand in
  // the (root) method being compiled (not inlining -> root == current).
  Method* caller = root_method();
  const jint at_bci = bci();
  GUARANTEE(caller->ubyte_at(at_bci) == Bytecodes::_fast_invokevirtual,
            "Marco 3.6c-vtable compiles _fast_invokevirtual here");
  const jushort cp_index = (jushort)
    (((jint)caller->ubyte_at(at_bci + 1) << 8) | (jint)caller->ubyte_at(at_bci + 2));

  // Flush args + live temporaries to g_jsp memory (same safety note as invoke()).
  frame()->flush(JVM_SINGLE_ARG_CHECK);

  // Receiver null-check (a virtual call always has a receiver). After the flush the
  // receiver (deepest arg) lives in g_jsp at arg_offset_from_sp(size_of_parameters-1);
  // load and null_check it (reuses the 3.4a inline throw path -> NPE unwinds out).
  {
    Value receiver(T_OBJECT);
    receiver.set_register(RegisterAllocator::allocate());
    emit(Assembler::encode_lw(receiver.lo_register(), Assembler::jsp,
             JavaFrame::arg_offset_from_sp(method->size_of_parameters() - 1)));
    null_check(receiver JVM_CHECK);
  }   // receiver Value destructs here -> frees its register

  // a0 = cpool index, a1 = invoker size (3 = fast_invokevirtual length), a2 = bci
  // (GC safepoint -- see jit_set_safepoint_bcp).
  mips_li(this, Assembler::a0, (juint)cp_index);
  mips_li(this, Assembler::a1, (juint)3);
  mips_li(this, Assembler::a2, (juint)at_bci);
  mips_call_c(this, (address)jit_invoke_virtual);

  // Model update: pop the parameter block, push the memory-resident result.
  frame()->adjust_for_invoke(method->size_of_parameters(), return_type);

  // Option-B unwind: eject if the callee unwound past this frame.
  mips_emit_invoke_fp_check(this);

  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: _fast_invokevirtual -> jit_invoke_virtual "
                    "emitted (Marco 3.6c-vtable 2/3)"); } }
}

// Fase 3 (Marco 3.6c-vtable 3/3): the REAL interface call (_fast_invokeinterface).
// Dynamic dispatch via the receiver's itable. Mirrors invoke_virtual's emission but
// calls jit_invoke_interface, and passes parameters_size (the interface bytecode's arg
// count) as the receiver-locating index -- the helper reads method_index/class_id from
// the cpool and computes the fixed invoker size 5 itself. klass/itable_index are not
// needed by the emitter (the helper resolves the itable at runtime, GC-safe).
void CodeGenerator::invoke_interface(JavaClass* klass, int itable_index,
                                     int parameters_size,
                                     BasicType return_type JVM_TRAPS) {
  (void)klass; (void)itable_index;
  if (Compiler::is_inlining()) {
    Compiler::abort_active_compilation(false JVM_THROW);   // inlining is transient
    return;
  }

  // Read the cpool index from the current _fast_invokeinterface bytecode's operand.
  Method* caller = root_method();
  const jint at_bci = bci();
  GUARANTEE(caller->ubyte_at(at_bci) == Bytecodes::_fast_invokeinterface,
            "Marco 3.6c-vtable compiles _fast_invokeinterface here");
  const jushort cp_index = (jushort)
    (((jint)caller->ubyte_at(at_bci + 1) << 8) | (jint)caller->ubyte_at(at_bci + 2));

  // Flush args + live temporaries to g_jsp memory (same safety note as invoke()).
  frame()->flush(JVM_SINGLE_ARG_CHECK);

  // Receiver null-check: after flush the receiver (deepest arg) lives in g_jsp at
  // arg_offset_from_sp(parameters_size - 1). parameters_size includes the receiver.
  {
    Value receiver(T_OBJECT);
    receiver.set_register(RegisterAllocator::allocate());
    emit(Assembler::encode_lw(receiver.lo_register(), Assembler::jsp,
             JavaFrame::arg_offset_from_sp(parameters_size - 1)));
    null_check(receiver JVM_CHECK);
  }   // receiver Value destructs here -> frees its register

  // a0 = cpool index, a1 = num_params (the helper uses the fixed invoker size 5),
  // a2 = bci (GC safepoint -- see jit_set_safepoint_bcp).
  mips_li(this, Assembler::a0, (juint)cp_index);
  mips_li(this, Assembler::a1, (juint)parameters_size);
  mips_li(this, Assembler::a2, (juint)at_bci);
  mips_call_c(this, (address)jit_invoke_interface);

  // Model update: pop the parameter block, push the memory-resident result.
  frame()->adjust_for_invoke(parameters_size, return_type);

  // Option-B unwind: eject if the callee unwound past this frame.
  mips_emit_invoke_fp_check(this);

  { static bool _once = false; if (!_once) { _once = true;
      tty->print_cr("[PS2ME-JIT] Fase 3: _fast_invokeinterface -> jit_invoke_interface "
                    "emitted (Marco 3.6c-vtable 3/3)"); } }
}

void CodeGenerator::invoke_native(BasicType return_kind, address entry JVM_TRAPS) {
  (void)return_kind; (void)entry;
  Compiler::abort_active_compilation(false JVM_THROW);   // native call: keep transient
}

// ---- exceptions / vm calls ---------------------------------------------------

// Fast in-line catch of a compile-time-known exception type (athrow-as-goto). Only
// reached for a method WITH an exception table, which the arm trigger already
// refuses (has_no_exception_table) -- bail cleanly as a backstop. Returns bool, so
// use JVM_THROW_(false) (the internal OOM signal returns `false` from here).
bool CodeGenerator::quick_catch_exception(const Value& value, JavaClass* catch_type,
                                          int handler_bci JVM_TRAPS) {
  (void)value; (void)catch_type; (void)handler_bci;
  Compiler::abort_active_compilation(true JVM_THROW_(false));
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

// call_vm_extra_arg always immediately precedes a call_vm (it just parks an extra
// argument register); since call_vm below bails the compilation, the parked arg is
// irrelevant. Make it a NO-OP (not SHOULD_NOT_REACH_HERE, which is a silent no-op
// in PRODUCT anyway) so the pair is harmless if ever reached. It has no JVM_TRAPS
// arg, so it cannot bail itself -- the following call_vm does.
void CodeGenerator::call_vm_extra_arg(const Register extra_arg) {
  (void)extra_arg;
}

// Generic VM call: the r5900 hybrid emits VM interactions through dedicated C
// helpers (jit_invoke_* / jit_throw_* / jit_timer_tick), never this generic path,
// which the ARM/i386 backends use for deoptimize / uncommon_trap / debugger
// athrow. uncommon_trap is already gated to abort on MIPS (shared CodeGenerator.
// cpp); this bail-out covers any remaining caller (e.g. go_to_interpreter's
// deoptimize) so it never crashes -- the method stays interpreted.
void CodeGenerator::call_vm(address entry, BasicType return_value_type JVM_TRAPS) {
  (void)entry; (void)return_value_type;
  Compiler::abort_active_compilation(true JVM_THROW);
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
  mips_li(this, Assembler::a0, (juint)bci());   // a0 = bci (GC safepoint: tick may switch/GC)
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

// System.arraycopy inlining: not yet emitted. Returns bool (true = handled inline);
// bailing the compilation leaves the method interpreted (arraycopy runs natively
// there). Use JVM_THROW_(false).
bool CodeGenerator::arraycopy(JVM_SINGLE_ARG_TRAPS) {
  Compiler::abort_active_compilation(true JVM_THROW_(false));
  return false;
}

bool CodeGenerator::unchecked_arraycopy(BasicType array_element_type JVM_TRAPS) {
  (void)array_element_type;
  Compiler::abort_active_compilation(true JVM_THROW_(false));
  return false;
}

#endif // ENABLE_COMPILER
