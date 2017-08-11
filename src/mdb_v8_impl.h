/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

/*
 * mdb_v8_impl.h: common functions used in the implementation of this module.
 * This file is a work in progress.  For now, it contains both MDB-specific and
 * MDB-agnostic definitions, but the hope is to eventually make this
 * MDB-agnostic.
 */

#ifndef	_MDBV8IMPL_H
#define	_MDBV8IMPL_H

#include <stdarg.h>

/*
 * We hard-code our MDB_API_VERSION to be 3 to allow this module to be
 * compiled on systems with higher version numbers, but still allow the
 * resulting binary object to be used on older systems.  (We do not make use
 * of functionality present in versions later than 3.)  This is particularly
 * important for mdb_v8 because (1) it's used in particular to debug
 * application-level software and (2) it has a history of rapid evolution.
 */
#define	MDB_API_VERSION		3

#include <sys/mdb_modapi.h>

/*
 * XXX Cleanup work to be done on these:
 * - prefix these function names
 * - normalize their names, calling patterns, and argument types
 * - add the other related functions
 */
void maybefree(void *, size_t, int);
int read_heap_array(uintptr_t, uintptr_t **, size_t *, int);
int read_heap_maybesmi(uintptr_t *, uintptr_t, ssize_t);
int read_heap_ptr(uintptr_t *, uintptr_t, ssize_t);
int read_heap_smi(uintptr_t *, uintptr_t, ssize_t);
int read_typebyte(uint8_t *, uintptr_t);
void v8_warn(const char *, ...);
boolean_t jsobj_is_undefined(uintptr_t);

/*
 * We need to find a better way of exposing this information.  For now, these
 * represent all the metadata constants used by multiple C files.
 */
extern intptr_t V8_TYPE_JSFUNCTION;
extern intptr_t V8_TYPE_JSBOUNDFUNCTION;
extern intptr_t V8_TYPE_FIXEDARRAY;

extern intptr_t V8_IsNotStringMask;
extern intptr_t V8_StringTag;
extern intptr_t V8_NotStringTag;
extern intptr_t V8_StringEncodingMask;
extern intptr_t V8_TwoByteStringTag;
extern intptr_t V8_AsciiStringTag;
extern intptr_t V8_OneByteStringTag;
extern intptr_t V8_StringRepresentationMask;
extern intptr_t V8_SeqStringTag;
extern intptr_t V8_ConsStringTag;
extern intptr_t V8_SlicedStringTag;
extern intptr_t V8_ExternalStringTag;
extern intptr_t V8_CompilerHints_BoundFunction;

extern ssize_t V8_OFF_CODE_INSTRUCTION_SIZE;
extern ssize_t V8_OFF_CODE_INSTRUCTION_START;
extern ssize_t V8_OFF_CONSSTRING_FIRST;
extern ssize_t V8_OFF_CONSSTRING_SECOND;
extern ssize_t V8_OFF_EXTERNALSTRING_RESOURCE;
extern ssize_t V8_OFF_FIXEDARRAY_DATA;
extern ssize_t V8_OFF_FIXEDARRAY_LENGTH;
extern ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_ARGUMENTS;
extern ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_TARGET_FUNCTION;
extern ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_THIS;
extern ssize_t V8_OFF_JSFUNCTION_CONTEXT;
extern ssize_t V8_OFF_JSFUNCTION_LITERALS_OR_BINDINGS;
extern ssize_t V8_OFF_JSFUNCTION_SHARED;
extern ssize_t V8_OFF_SCRIPT_LINE_ENDS;
extern ssize_t V8_OFF_SCRIPT_NAME;
extern ssize_t V8_OFF_SEQASCIISTR_CHARS;
extern ssize_t V8_OFF_SEQONEBYTESTR_CHARS;
extern ssize_t V8_OFF_SEQTWOBYTESTR_CHARS;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_CODE;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_COMPILER_HINTS;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_INFERRED_NAME;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_NAME;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_SCRIPT;
extern ssize_t V8_OFF_SLICEDSTRING_PARENT;
extern ssize_t V8_OFF_SLICEDSTRING_OFFSET;
extern ssize_t V8_OFF_STRING_LENGTH;

extern intptr_t V8_CONTEXT_IDX_CLOSURE;
extern intptr_t V8_CONTEXT_IDX_EXT;
extern intptr_t V8_CONTEXT_IDX_GLOBAL;
extern intptr_t V8_CONTEXT_IDX_NATIVE;
extern intptr_t V8_CONTEXT_IDX_PREV;
extern intptr_t V8_CONTEXT_NCOMMON;

extern intptr_t V8_SCOPEINFO_IDX_FIRST_VARS;
extern intptr_t V8_SCOPEINFO_IDX_NCONTEXTLOCALS;
extern intptr_t V8_SCOPEINFO_IDX_NPARAMS;
extern intptr_t V8_SCOPEINFO_IDX_NSTACKLOCALS;
extern intptr_t V8_SCOPEINFO_OFFSET_STACK_LOCALS;

extern intptr_t V8_HeapObjectTag;
extern intptr_t V8_HeapObjectTagMask;
extern intptr_t V8_SmiTag;
extern intptr_t V8_SmiTagMask;
extern intptr_t V8_SmiValueShift;
extern intptr_t V8_SmiShiftSize;

/* see node_string.h */
#define	NODE_OFF_EXTSTR_DATA		sizeof (uintptr_t)

#endif	/* _MDBV8IMPL_H */
