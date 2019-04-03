/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
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
 * Addresses in the target program are represented as "uintptr_t".  Most of
 * these are either V8 small integers (see V8_IS_SMI() and V8_SMI_VALUE()) or
 * other V8 heap objects.  A number of functions exists to inspect and dump
 * these, but they have not yet been abstracted here.
 *
 *
 * RETURN VALUES
 *
 * Functions here fall into one of two categories: functions that return "int"
 * (or a pointer that may be NULL) can generally fail because of a validation
 * problem or a failure to read information from the target's address space.
 * Other functions cannot fail because it's assumed that whatever conditions
 * they depend on have already been validated.  They typically assert such
 * conditions.  It's critical that such conditions _have_ already been checked
 * (e.g., in v8context_load() or by the caller).  The debugger should not assume
 * that the target's address space is not arbitrarily corrupt.
 *
 *
 * USAGE PATTERNS
 *
 * Most of the objects below have a few common patterns of functions:
 *
 *     CLASSNAME_t *CLASSNAME_load(uintptr_t addr, int memflags)
 *
 *         Given an address in the target program "addr" and memory allocation
 *         flags "memflags", validate that the address refers to an object of
 *         the appropriate class, load basic information about the object, and
 *         return a handle for working with it.
 *
 *         For example, v8function_load() takes a pointer to a JSFunction object
 *         in the target program, validates it, and loads pointers to the
 *         function's name, JavaScript source code, and so on.  Once you've
 *         loaded an object like this, most other functions will not fail,
 *         because they've already validated the things that can go wrong.
 *
 *     void CLASSNAME_free(CLASSNAME_t *)
 *
 *         Frees an object of type CLASSNAME, if appropriate.  Note that the
 *         memflags specified when the object was loaded may specify that the
 *         object should be GC'd later, in which case this will do nothing, but
 *         in general callers should not assume this unless they were the ones
 *         that called CLASSNAME_load() in the first place.
 *
 *     uintptr_t CLASSNAME_addr(CLASSNAME_t *);
 *
 *         Returns the address in the target process for this object (as
 *         initially passed to CLASSNAME_load()).
 *
 * There are a few other patterns used here:
 *
 *     CLASSNAME2_t *CLASSNAME1_CLASSNAME2(CLASSNAME1_t *);
 *
 *         This is used when objects of one class refer to objects of another.
 *         For example, every v8function_t refers to a v8funcinfo_t with shared
 *         function information.  Given a v8function_t, you can get to the
 *         v8funcinfo_t using v8function_funcinfo().
 */

#ifndef	_MDBV8DBG_H
#define	_MDBV8DBG_H

#include <stdarg.h>
#include <sys/types.h>

/*
 * Basic types
 */

typedef struct {
	char	*ms_buf;	/* full buffer */
	size_t	ms_bufsz;	/* full buffer size */
	char	*ms_curbuf;	/* current position in buffer */
	size_t	ms_curbufsz;	/* current buffer size left */
	size_t	ms_reservesz;	/* bytes reserved */
	int	ms_flags;	/* buffer flags */
	int	ms_memflags;	/* memory allocation flags */
} mdbv8_strbuf_t;

typedef struct v8fixedarray v8fixedarray_t;
typedef struct v8string v8string_t;

typedef struct v8array v8array_t;
typedef struct v8function v8function_t;
typedef struct v8boundfunction v8boundfunction_t;
typedef struct v8code v8code_t;
typedef struct v8funcinfo v8funcinfo_t;
typedef struct v8context v8context_t;
typedef struct v8scopeinfo v8scopeinfo_t;
typedef struct v8scopeinfo_var v8scopeinfo_var_t;

typedef enum {
	MSF_ASCIIONLY	= 0x1,			/* replace non-ASCII */
	MSF_JSON	= MSF_ASCIIONLY | 0x2,	/* partial JSON string */
} mdbv8_strappend_flags_t;

typedef enum {
	V8SV_PARAMS,
	V8SV_STACKLOCALS,
	V8SV_CONTEXTLOCALS
} v8scopeinfo_vartype_t;

typedef enum {
	JSSTR_NONE,
	JSSTR_NUDE	= JSSTR_NONE,

	JSSTR_FLAGSHIFT = 16,
	JSSTR_VERBOSE   = (0x1 << JSSTR_FLAGSHIFT),
	JSSTR_QUOTED    = (0x2 << JSSTR_FLAGSHIFT),
	JSSTR_ISASCII   = (0x4 << JSSTR_FLAGSHIFT),

	JSSTR_MAXDEPTH  = 512
} v8string_flags_t;


/*
 * Working with ASCII strings.  These string buffers are used for most
 * operations that turn anything into a string for printing to the user.
 */

mdbv8_strbuf_t *mdbv8_strbuf_alloc(size_t, int);
void mdbv8_strbuf_free(mdbv8_strbuf_t *);
void mdbv8_strbuf_init(mdbv8_strbuf_t *, char *, size_t);
void mdbv8_strbuf_legacy_update(mdbv8_strbuf_t *, char **, size_t *);

size_t mdbv8_strbuf_bufsz(mdbv8_strbuf_t *);
size_t mdbv8_strbuf_bytesleft(mdbv8_strbuf_t *);

void mdbv8_strbuf_rewind(mdbv8_strbuf_t *);
void mdbv8_strbuf_reserve(mdbv8_strbuf_t *, ssize_t);
void mdbv8_strbuf_appendc(mdbv8_strbuf_t *, uint16_t, mdbv8_strappend_flags_t);
void mdbv8_strbuf_appends(mdbv8_strbuf_t *, const char *,
    mdbv8_strappend_flags_t);
void mdbv8_strbuf_sprintf(mdbv8_strbuf_t *, const char *, ...);
void mdbv8_strbuf_vsprintf(mdbv8_strbuf_t *, const char *, va_list);
const char *mdbv8_strbuf_tocstr(mdbv8_strbuf_t *);

size_t mdbv8_strbuf_nbytesforchar(uint16_t, mdbv8_strappend_flags_t);


/*
 * Working with JavaScript arrays.
 */
v8array_t *v8array_load(uintptr_t, int);
void v8array_free(v8array_t *);
size_t v8array_length(v8array_t *);
int v8array_iter_elements(v8array_t *,
    int (*)(v8array_t *, unsigned int, uintptr_t, void *), void *);


/*
 * Working with V8 FixedArrays.  These are plain arrays used within V8 for a
 * variety of higher-level structures.  Most of these structures apply their own
 * semantics to the elements of the array.  Contexts and ScopeInfos are examples
 * of higher-level objects that are just FixedArrays with additional semantics.
 */

v8fixedarray_t *v8fixedarray_load(uintptr_t, int);
void v8fixedarray_free(v8fixedarray_t *);

int v8fixedarray_iter_elements(v8fixedarray_t *,
    int (*)(v8fixedarray_t *, unsigned int, uintptr_t, void *), void *);
uintptr_t *v8fixedarray_as_array(v8fixedarray_t *, int);
size_t v8fixedarray_length(v8fixedarray_t *);


/*
 * Working with JavaScript strings.
 */

v8string_t *v8string_load(uintptr_t, int);
void v8string_free(v8string_t *);

size_t v8string_length(v8string_t *);
int v8string_write(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);


/*
 * Functions, contexts, closures, and ScopeInfo objects
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
 */

/*
 * Working with JSFunction objects.
 *
 * JSFunction objects represent closures, rather than a single instance of the
 * function in the source code.  There may be many JSFunction objects for what
 * programmers would typically call a "function" -- one for each active closure.
 *
 * If you're working with a bound function, use v8boundfunction_t.
 */
v8function_t *v8function_load(uintptr_t, int);
void v8function_free(v8function_t *);

v8context_t *v8function_context(v8function_t *, int);
v8scopeinfo_t *v8function_scopeinfo(v8function_t *, int);
v8funcinfo_t *v8function_funcinfo(v8function_t *, int);

/*
 * Working with SharedFunctionInfo objects.
 *
 * Since v8function_t objects denote individual closures, many instances may
 * refer to the same function name, source code, native instructions, and so on.
 * These are represented by one instance of v8funcinfo_t.
 */
v8funcinfo_t *v8funcinfo_load(uintptr_t, int);
void v8funcinfo_free(v8funcinfo_t *);
int v8funcinfo_funcname(v8funcinfo_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t);
int v8funcinfo_scriptpath(v8funcinfo_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t);
int v8funcinfo_definition_location(v8funcinfo_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t);
v8code_t *v8funcinfo_code(v8funcinfo_t *, int);

/*
 * Working with V8Code objects.  These objects represent blocks of native
 * instructions.  We wouldn't bother to abstract this separately from
 * v8funcinfo_t, except that there are code blocks that aren't part of a
 * function (e.g., various trampolines) that it's sometimes useful to inspect.
 */
v8code_t *v8code_load(uintptr_t, int);
void v8code_free(v8code_t *);
uintptr_t v8code_addr(v8code_t *);
uintptr_t v8code_instructions_start(v8code_t *);
uintptr_t v8code_instructions_size(v8code_t *);

/*
 * Working with Contexts
 */

v8context_t *v8context_load(uintptr_t, int);
void v8context_free(v8context_t *);
uintptr_t v8context_addr(v8context_t *);
uintptr_t v8context_closure(v8context_t *);
uintptr_t v8context_prev_context(v8context_t *);
int v8context_var_value(v8context_t *, unsigned int, uintptr_t *);
v8scopeinfo_t *v8context_scopeinfo(v8context_t *, int);

int v8context_iter_static_slots(v8context_t *,
    int (*)(v8context_t *, const char *, uintptr_t, void *), void *);
int v8context_iter_dynamic_slots(v8context_t *,
    int (*func)(v8context_t *, uint_t, uintptr_t, void *), void *);

/*
 * Working with ScopeInfo objects
 */

v8scopeinfo_t *v8scopeinfo_load(uintptr_t, int);
void v8scopeinfo_free(v8scopeinfo_t *);
uintptr_t v8scopeinfo_addr(v8scopeinfo_t *);

int v8scopeinfo_iter_vartypes(v8scopeinfo_t *,
    int (*)(v8scopeinfo_t *, v8scopeinfo_vartype_t, void *), void *);
const char *v8scopeinfo_vartype_name(v8scopeinfo_vartype_t);

size_t v8scopeinfo_vartype_nvars(v8scopeinfo_t *, v8scopeinfo_vartype_t);
int v8scopeinfo_iter_vars(v8scopeinfo_t *, v8scopeinfo_vartype_t,
    int (*)(v8scopeinfo_t *, v8scopeinfo_var_t *, void *), void *);
size_t v8scopeinfo_var_idx(v8scopeinfo_t *, v8scopeinfo_var_t *);
uintptr_t v8scopeinfo_var_name(v8scopeinfo_t *, v8scopeinfo_var_t *);

/*
 * Working with bound functions.
 *
 * In versions of V8 used in Node v4 and earlier, bound functions have their own
 * valid JSFunction instance (with shared function info, name, and the usual
 * properties), and you will be able to load them with v8function_load() (as
 * well as v8boundfunction_load()).  However, later V8 versions use a separate
 * JSBoundFunction class.  In these versions, there will not be a JSFunction
 * instance for bound functions, and you will not be able to load them with
 * v8function_load().  You can use v8boundfunction_load() in all cases.
 */
v8boundfunction_t *v8boundfunction_load(uintptr_t, int);
uintptr_t v8boundfunction_target(v8boundfunction_t *);
uintptr_t v8boundfunction_this(v8boundfunction_t *);
size_t v8boundfunction_nargs(v8boundfunction_t *);
int v8boundfunction_iter_args(v8boundfunction_t *,
    int (*)(v8boundfunction_t *, uint_t, uintptr_t, void *), void *);
void v8boundfunction_free(v8boundfunction_t *);


/*
 * Higher-level functions.
 */

/*
 * v8contains() attempts to determine whether a given V8 heap object contains a
 * target address.
 */
int v8contains(uintptr_t, uint8_t, uintptr_t, boolean_t *);

/*
 * v8whatis() attempts to find the V8 heap object that contains the target
 * address.
 */
typedef enum {
	V8W_OK = 0,
	V8W_ERR_NOTFOUND = 1,
	V8W_ERR_DOESNTCONTAIN = 2
} v8whatis_error_t;

typedef struct {
	uintptr_t	v8w_addr;	/* user-supplied address */
	uintptr_t	v8w_origaddr;	/* adjusted address */
	uintptr_t	v8w_baseaddr;	/* address of containing V8 object */
	uint8_t		v8w_basetype;	/* type of containing V8 object */
} v8whatis_t;

v8whatis_error_t v8whatis(uintptr_t, size_t, v8whatis_t *);

#endif	/* _MDBV8DBG_H */
