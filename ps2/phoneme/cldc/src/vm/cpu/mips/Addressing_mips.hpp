/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

#if ENABLE_COMPILER

// Address abstractions for the r5900. A MIPS memory operand is base+disp16, so
// (unlike i386) there is no index/scale. Structure mirrors the i386 backend.

class MemoryAddress: public GenericAddress {
 public:
  MemoryAddress(BasicType type) : GenericAddress(type) { }

  BinaryAssembler::Address lo_address() { return address_for(lo_offset()); }
  BinaryAssembler::Address hi_address() {
    GUARANTEE(is_two_word(), "sanity check");
    return address_for(hi_offset());
  }

 protected:
  virtual BinaryAssembler::Address address_for(jint address_offset)
     JVM_PURE_VIRTUAL_((BinaryAssembler::Address)0);
};

class HeapAddress: public MemoryAddress {
 public:
  HeapAddress(BasicType type) : MemoryAddress(type) { clear_address_register(); }
 ~HeapAddress();

  virtual void write_barrier_prolog();
  virtual void write_barrier_epilog();

 protected:
  virtual BinaryAssembler::Address address_for(jint address_offset);
  virtual BinaryAssembler::Address compute_address_for(jint address_offset)
     JVM_PURE_VIRTUAL_((BinaryAssembler::Address)0);

 private:
  BinaryAssembler::Register _address_register;

  BinaryAssembler::Register address_register() const { return _address_register; }
  void set_address_register(BinaryAssembler::Register value) {
    _address_register = value;
  }
  bool has_address_register() const {
    return address_register() != BinaryAssembler::no_reg;
  }
  void clear_address_register() {
    set_address_register(BinaryAssembler::no_reg);
  }
};

class FieldAddress: public HeapAddress {
 public:
  FieldAddress(Value& object, jint offset, BasicType type) :
    HeapAddress(type), _object(&object), _offset(offset) { }
 protected:
  virtual BinaryAssembler::Address compute_address_for(jint address_offset);
 private:
  Value* _object;
  jint   _offset;
  Value* object() const { return _object; }
  jint   offset() const { return _offset; }
};

class IndexedAddress: public HeapAddress {
 public:
  IndexedAddress(Value& array, Value& index, BasicType type)
    : HeapAddress(type), _array(&array), _index(&index) { }
 protected:
  virtual BinaryAssembler::Address compute_address_for(jint address_offset);
 private:
  Value* _array;
  Value* _index;
  Value* array() const { return _array; }
  Value* index() const { return _index; }
  jint   index_shift() const { return jvm_log2(byte_size_for(type())); }
};

class StackAddress: public MemoryAddress {
 public:
  StackAddress(BinaryAssembler::Register base, BasicType type)
    : MemoryAddress(type), _base(base) { }
  BinaryAssembler::Address tag_address()  { return address_for(tag_offset());  }
  BinaryAssembler::Address tag2_address() { return address_for(tag2_offset()); }

 protected:
  virtual BinaryAssembler::Address address_for(jint address_offset);

  virtual jint compute_base_offset() {
    return JavaFrame::arg_offset_from_sp(is_two_word() ? 1 : 0);
  }
  virtual jint tag_offset() const {
    GUARANTEE(TaggedJavaStack, "Shouldn't be getting tag_offset()");
    return -BytesPerWord;
  }
  virtual jint tag2_offset() const {
    GUARANTEE(is_two_word() && TaggedJavaStack, "sanity");
    return -(BytesPerStackElement + BytesPerWord);
  }
  virtual jint lo_offset() const {
    return (is_two_word() ? -BytesPerStackElement : 0);
  }
  virtual jint hi_offset() const {
    GUARANTEE(is_two_word(), "sanity");
    return 0;
  }

 private:
  BinaryAssembler::Register _base;
  BinaryAssembler::Register base() const { return _base; }
};

class LocationAddress: public StackAddress {
 public:
  LocationAddress(jint index, BasicType type)
    : StackAddress(base_for(index), type), _index(index) { }
 protected:
  virtual jint compute_base_offset();
 private:
  jint _index;
  jint index() const { return _index; }
  bool is_local() const { return is_local_index(index()); }
  static bool is_local_index(jint index);
  // In the C-interpreter hybrid, locals and the Java expression stack live in
  // interpreter memory addressed by dedicated global-reg pointers, NOT the EE
  // $sp: `locals` (s3 == g_jlocals) bases locals, `jsp` (s1 == g_jsp) bases
  // expression-stack slots -- matching Interpreter_c.cpp's GET_LOCAL/stack.
  static BinaryAssembler::Register base_for(jint index) {
    return is_local_index(index) ?
      BinaryAssembler::locals : BinaryAssembler::jsp;
  }
};

#endif // ENABLE_COMPILER
