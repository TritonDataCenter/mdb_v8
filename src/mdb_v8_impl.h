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
