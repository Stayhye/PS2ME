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

// Emit an EE C-ABI call to an absolute code address: materialize it in t9 (the
// MIPS PIC call register), jalr, and fill the branch delay slot with a nop.
// (ra is clobbered by jalr; the caller must have saved the real return address.)
static void mips_call_c(BinaryAssembler* a, address target) {
  mips_li(a, Assembler::t9, (juint)(unsigned long)target);
  a->emit(Assembler::encode_jalr(Assembler::ra, Assembler::t9));
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

void CodeGenerator::increment_stack_pointer_by(int adjustment) {
  (void)adjustment;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::clear_stack() {
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::bytecode_prolog() {
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::flush_epilogue(JVM_SINGLE_ARG_TRAPS) {
  SHOULD_NOT_REACH_HERE();
}

// ---- loads / stores / moves --------------------------------------------------

void CodeGenerator::load_from_address(Value& result, BasicType type,
                                      MemoryAddress& address, Condition cond) {
  (void)result; (void)type; (void)address; (void)cond;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::store_to_address(Value& value, BasicType type,
                                     MemoryAddress& address) {
  (void)value; (void)type; (void)address;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::move(const Value& dst, const Value& src, const Condition cond) {
  (void)dst; (void)src; (void)cond;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::move(Value& dst, Oop* obj, Condition cond) {
  (void)dst; (void)obj; (void)cond;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::move(Assembler::Register dst, Assembler::Register src,
                         Condition cond) {
  (void)dst; (void)src; (void)cond;
  SHOULD_NOT_REACH_HERE();
}

// ---- comparisons / conditional control flow ----------------------------------

void CodeGenerator::cmp_values(Value& op1, Value& op2) {
  (void)op1; (void)op2;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::conditional_jump_do(BytecodeClosure::cond_op condition,
                                        Label& destination) {
  (void)condition; (void)destination;
  SHOULD_NOT_REACH_HERE();
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

void CodeGenerator::null_check(const Value& object JVM_TRAPS) {
  (void)object;
  SHOULD_NOT_REACH_HERE();
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

void CodeGenerator::int_binary_do(Value& result, Value& op1, Value& op2,
                                  BytecodeClosure::binary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op2; (void)op;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::int_unary_do(Value& result, Value& op1,
                                 BytecodeClosure::unary_op op JVM_TRAPS) {
  (void)result; (void)op1; (void)op;
  SHOULD_NOT_REACH_HERE();
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

void CodeGenerator::i2b(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::i2c(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
}
void CodeGenerator::i2s(Value& result, Value& value JVM_TRAPS) {
  (void)result; (void)value; SHOULD_NOT_REACH_HERE();
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

void CodeGenerator::throw_simple_exception(int rte JVM_TRAPS) {
  (void)rte;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::call_vm_extra_arg(const Register extra_arg) {
  (void)extra_arg;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::call_vm(address entry, BasicType return_value_type JVM_TRAPS) {
  (void)entry; (void)return_value_type;
  SHOULD_NOT_REACH_HERE();
}

void CodeGenerator::check_timer_tick(JVM_SINGLE_ARG_TRAPS) {
  SHOULD_NOT_REACH_HERE();
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
