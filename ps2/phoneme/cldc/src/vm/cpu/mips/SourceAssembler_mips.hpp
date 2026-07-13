/*
 *   PS2ME: MIPS r5900 JIT backend.  (see Assembler_mips.hpp header banner)
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * GPLv2 only; a copy is included at /legal/license.txt.
 */

// The SourceAssembler emits textual assembly and is used only by the interpreter
// generator / AOT source image generator. The PS2ME build keeps the pure-C
// interpreter (ENABLE_INTERPRETER_GENERATOR == false, ENABLE_SOURCE_IMAGE_
// GENERATOR == false), so this is an empty Fase-0 stub.
#if ENABLE_SOURCE_IMAGE_GENERATOR || ENABLE_INTERPRETER_GENERATOR

class SourceAssembler: public Assembler {
};

#endif
