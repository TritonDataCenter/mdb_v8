/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_impl.h: common functions used in the implementation of this module.
 * This file is a work in progress.  For now, it contains both MDB-specific and
 * MDB-agnostic definitions, but the hope is to eventually make this
 * MDB-agnostic.
 */

#ifndef _MDBV8IMPL_H
#define	_MDBV8IMPL_H

/*
 * XXX Cleanup work to be done on these:
 * - prefix these function names
 * - normalize their names, calling patterns, and argument types
 * - add the other related functions
 */
int read_heap_array(uintptr_t, uintptr_t **, size_t *, int);
int read_heap_ptr(uintptr_t *, uintptr_t, ssize_t);
int read_typebyte(uint8_t *, uintptr_t);
void v8_warn(const char *, ...);

/*
 * We need to find a better way of exposing this information.  For now, these
 * represent all the metadata constants used by multiple C files.
 */
extern intptr_t V8_TYPE_JSFUNCTION;

extern ssize_t V8_OFF_JSFUNCTION_CONTEXT;
extern ssize_t V8_OFF_JSFUNCTION_SHARED;
extern ssize_t V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO;

extern intptr_t V8_CONTEXT_IDX_CLOSURE;
extern intptr_t V8_CONTEXT_IDX_EXT;
extern intptr_t V8_CONTEXT_IDX_GLOBAL;
extern intptr_t V8_CONTEXT_IDX_PREV;
extern intptr_t V8_CONTEXT_NCOMMON;

extern intptr_t V8_SCOPEINFO_IDX_FIRST_VARS;
extern intptr_t V8_SCOPEINFO_IDX_NCONTEXTLOCALS;
extern intptr_t V8_SCOPEINFO_IDX_NPARAMS;
extern intptr_t V8_SCOPEINFO_IDX_NSTACKLOCALS;

extern intptr_t V8_HeapObjectTag;
extern intptr_t V8_HeapObjectTagMask;
extern intptr_t V8_SmiTag;
extern intptr_t V8_SmiTagMask;
extern intptr_t V8_SmiValueShift;
extern intptr_t V8_SmiShiftSize;

#endif /* _MDBV8IMPL_H */
