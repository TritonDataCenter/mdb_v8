/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_dbg.h: interface for working with V8 objects in a debugger.
 *
 * This file should contain types and functions useful for debugging Node.js
 * programs.  These functions may currently be implemented in terms of the MDB
 * module API, but this interface should not include any MDB-specific
 * functionality.  The expectation is that this could be implemented by another
 * backend, and that it could be used to implement a different user interface.
 *
 *
 * GENERAL NOTES
 *
 * Addresses in the target program are represented as "uintptr_t".  Most of
 * these are either V8 small integers (see V8_IS_SMI() and V8_SMI_VALUE()) or
 * other V8 heap objects.  A number of functions exists to inspect and dump
 * these, but they have not been abstracted here.
 *
 * Functions here fall into one of two categories: functions that return "int"
 * can generally fail because of a validation problem or a failure to read
 * information from the target's address space.  Other functions cannot fail
 * because it's assumed that whatever conditions they depend on have already
 * been validated.  They typically assert such conditions.  It's critical that
 * such conditions _have_ already been checked (e.g., in v8context_load() or by
 * the caller).  The debugger should not assume that the target's address space
 * is not arbitrarily corrupt.
 */

#ifndef _MDBV8DBG_H
#define	_MDBV8DBG_H

/*
 * Contexts, closures, and ScopeInfo objects
 *
 * Each JavaScript closure (an instance of the V8 "JSFunction" class) has its
 * own Context (another V8 heap object).  The Context contains values of
 * variables that are accessible from that context.  By looking at the Context
 * associated with a closure, we can see the values of variables accessible in
 * that closure.  (Contexts are also used for other facilities, like "with"
 * expressions, but there is no support here for dealing with other kinds of
 * Contexts.)
 *
 * The information about the layout of a Context is stored in a separate
 * ScopeInfo object.  The ScopeInfo describes, among other things, the names of
 * the variables accessible in that context.  All closures for a given function
 * (in the JavaScript source code) share the same ScopeInfo, and that ScopeInfo
 * is available on the SharedFunctionInfo object referenced by each JSFunction
 * object.  (This makes sense because all closures for a given function (in the
 * source code) share the same set of accessible variable names.)
 *
 * ScopeInfo objects also include information about parameters and stack-local
 * variables, but the values of these are not available from a Contexts.
 *
 * In order to commonize code around reading and validating context information,
 * we require that callers use v8context_load() in order to work with Contexts.
 * Similarly, we provide v8scopeinfo_load() in order to work with ScopeInfo
 * objects.  As a convenient special case, we provide v8context_scopeinfo() to
 * load a scopeinfo_t for a v8context_t.
 *
 * Inside V8, both Context and ScopeInfo objects are implemented as FixedArrays.
 * Both have a few statically-defined slots that describe the object, followed
 * by dynamic slots.  For Contexts, the dynamic slots are described by the
 * corresponding ScopeInfo.  For ScopeInfo objects, the dynamic slots are
 * described by the initial statically-defined slots.
 *
 * For more on Context internals, see src/context.h in the V8 source.  For more
 * information on ScopeInfo internals, see the declaration of the ScopeInfo
 * class in src/objects.h in the V8 source.
 *
 * TODO in the future, we will likely want to provide free() functions for
 * v8context and v8scopeinfo, and those are going to need to know whether UM_GC
 * was passed at load-time.  Today, we assume callers passed UM_GC, and we don't
 * have a way to free these.
 */

/*
 * Working with Contexts
 */

/* XXX This type should be opaque. */
typedef struct {
	uintptr_t	v8ctx_addr;
	uintptr_t	*v8ctx_elts;
	size_t		v8ctx_nelts;
	int		v8ctx_error;
} v8context_t;

int v8context_load(uintptr_t, v8context_t *, int);
uintptr_t v8context_closure(v8context_t *);
uintptr_t v8context_prev_context(v8context_t *);
int v8context_var_value(v8context_t *, unsigned int, uintptr_t *);

/*
 * Low-level Context details
 */
int v8context_iter_static_slots(v8context_t *,
    int (*)(v8context_t *, const char *, uintptr_t, void *), void *);
int v8context_iter_dynamic_slots(v8context_t *,
    int (*func)(v8context_t *, uint_t, uintptr_t, void *), void *);

/*
 * Working with ScopeInfo objects
 */

/* XXX This type should be opaque. */
typedef struct {
	uintptr_t	v8si_addr;
	uintptr_t	*v8si_elts;
	size_t		v8si_nelts;
	int		v8si_error;
} v8scopeinfo_t;

typedef enum {
	V8SV_PARAMS,
	V8SV_STACKLOCALS,
	V8SV_CONTEXTLOCALS
} v8scopeinfo_vartype_t;

struct v8scopeinfo_var;
typedef struct v8scopeinfo_var v8scopeinfo_var_t;

int v8scopeinfo_load(uintptr_t, v8scopeinfo_t *, int);
int v8context_scopeinfo(v8context_t *, v8scopeinfo_t *, int);

int v8scopeinfo_iter_groups(v8scopeinfo_t *,
    int (*)(v8scopeinfo_t *, v8scopeinfo_vartype_t, void *), void *);
const char *v8scopeinfo_group_name(v8scopeinfo_vartype_t);

size_t v8scopeinfo_group_nvars(v8scopeinfo_t *, v8scopeinfo_vartype_t);
int v8scopeinfo_iter_vars(v8scopeinfo_t *, v8scopeinfo_vartype_t,
    int (*)(v8scopeinfo_t *, v8scopeinfo_var_t *, void *), void *);
size_t v8scopeinfo_var_idx(v8scopeinfo_t *, v8scopeinfo_var_t *);
uintptr_t v8scopeinfo_var_name(v8scopeinfo_t *, v8scopeinfo_var_t *);

int v8function_context(uintptr_t, v8context_t *, int);
int v8function_scopeinfo(uintptr_t, v8scopeinfo_t *, int);

#endif /* _MDBV8DBG_H */
