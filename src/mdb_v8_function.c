/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

/*
 * mdb_v8_function.c: implementations of functions used for working with
 * JSFunctions and related objects, including Context, ScopeInfo, and Code
 * objects.
 */

#include <assert.h>
#include <strings.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

/*
 * See mdb_v8_dbg.h for details on what these various structures represent.
 */

struct v8function {
	uintptr_t	v8func_addr;		/* address in target proc */
	int		v8func_memflags;	/* allocation flags */
	uintptr_t	v8func_shared;		/* SharedFunctionInfo */
};

struct v8funcinfo {
	uintptr_t	v8fi_addr;		/* address in target proc */
	int		v8fi_memflags;		/* allocation flags */
	uintptr_t	v8fi_script;		/* script object */
	uintptr_t	v8fi_funcname;		/* function name (string) */
	uintptr_t	v8fi_inferred_name;	/* inferred func name */
	uintptr_t	v8fi_scriptpath;	/* script file name (string) */
	uintptr_t	v8fi_tokenpos;		/* "function" token position */
	uintptr_t	v8fi_line_endings;	/* script line endings table */
	uintptr_t	v8fi_code;		/* "code" object */
};

struct v8code {
	uintptr_t	v8code_addr;		/* address in target proc */
	int		v8code_memflags;	/* allocation flags */
	uintptr_t	v8code_instr_start;	/* start of instructions */
	uintptr_t	v8code_instr_size;	/* size of instructions */
};

struct v8context {
	uintptr_t	v8ctx_addr;	/* context address in target process */
	int		v8ctx_memflags;	/* memory allocation flags */
	uintptr_t	*v8ctx_elts;	/* copied-in array of context slots */
	size_t		v8ctx_nelts;	/* count of context slots */
};

struct v8scopeinfo {
	uintptr_t	v8si_addr;	/* ScopeInfo address in target proc */
	int		v8si_memflags;	/* memory allocation flags */
	uintptr_t	*v8si_elts;	/* copied-in array of slots */
	size_t		v8si_nelts;	/* count of slots */
};

struct v8boundfunction {
	uintptr_t	v8bf_addr;	/* address of bound function */
	uintptr_t	v8bf_memflags;	/* memory allocation flags */

	/*
	 * As mentioned in mdb_v8_dbg.h, earlier versions of V8 use a plain
	 * JSFunction to represent bound functions.  In those versions, there's
	 * a "bindings" array that contains the values of the wrapped function,
	 * "this", and all of the bound arguments.  Newer versions of V8 use a
	 * separate class for bound functions that has first-class properties
	 * for these fields.
	 *
	 * It's pretty easy to normalize the target function and "this", so we
	 * do that at load time and store them into v8bf_target and v8bf_this.
	 */
	uintptr_t	v8bf_target;	/* target (wrapped) function */
	uintptr_t	v8bf_this;	/* bound value of "this" */

	/*
	 * Abstracting over the argument list is only slightly trickier.  In
	 * both cases, we have a V8 array, but in one case we need to skip the
	 * first two elements.
	 */
	uintptr_t	*v8bf_array;	/* bindings or arguments (see above) */
	uintptr_t	v8bf_arraylen;	/* length of "v8bf_array" */
	uintptr_t	v8bf_idx_arg0;	/* first valid argument in */
					/* "v8bf_args" */
};

/*
 * These constants describe the layout of the "bindings" array for bound
 * functions.  They could in principle come from postmortem metadata, but
 * they're already obselete in current versions of V8, so it's unlikely they'd
 * ever come from the binary anyway.
 */
static int V8_BINDINGS_INDEX_TARGET = 0;
static int V8_BINDINGS_INDEX_THIS = 1;
static int V8_BINDINGS_INDEX_ARGS_START = 2;

/*
 * This structure and array describe the statically-defined fields stored inside
 * each Context.  This is mainly useful for debugger tools that want to dump
 * everything inside the context.
 */
typedef struct {
	const char	*v8ctxf_label;	/* name of field */
	intptr_t	*v8ctxf_idxp;	/* ptr to index into context (array) */
} v8context_field_t;

static v8context_field_t v8context_fields[] = {
	{ "closure function",	&V8_CONTEXT_IDX_CLOSURE },
	{ "previous context",	&V8_CONTEXT_IDX_PREV },
	{ "extension",		&V8_CONTEXT_IDX_EXT },
};

static size_t v8context_nfields =
    sizeof (v8context_fields) / sizeof (v8context_fields[0]);

/*
 * This structure and array describe the layout of a ScopeInfo.  Each
 * vartype_info describes a certain kind of variable, and the structures below
 * include pointers to the field (inside a ScopeInfo) that stores the count of
 * that kind of variable.
 */
struct v8scopeinfo_var {
	size_t	v8siv_which;
	size_t	v8siv_realidx;
};

typedef struct {
	v8scopeinfo_vartype_t	v8vti_vartype;
	const char		*v8vti_label;
	intptr_t		*v8vti_idx_countp;
	intptr_t		*v8vti_offset;
} v8scopeinfo_vartype_info_t;

static v8scopeinfo_vartype_info_t v8scopeinfo_vartypes[] = {
	{ V8SV_PARAMS, "parameter", &V8_SCOPEINFO_IDX_NPARAMS },
	{ V8SV_STACKLOCALS, "stack local variable",
	    &V8_SCOPEINFO_IDX_NSTACKLOCALS, &V8_SCOPEINFO_OFFSET_STACK_LOCALS },
	{ V8SV_CONTEXTLOCALS, "context local variable",
	    &V8_SCOPEINFO_IDX_NCONTEXTLOCALS },
};

static size_t v8scopeinfo_nvartypes =
    sizeof (v8scopeinfo_vartypes) / sizeof (v8scopeinfo_vartypes[0]);


/*
 * Local utility function declarations.
 */

static uintptr_t v8context_elt(v8context_t *, unsigned int);
static v8scopeinfo_vartype_info_t *v8scopeinfo_vartype_lookup(
    v8scopeinfo_vartype_t);
static v8boundfunction_t *v8boundfunction_load_bindings(uintptr_t, int);
static v8boundfunction_t *v8boundfunction_load_direct(uintptr_t, int);


/*
 * JSFunction functions
 */

/*
 * Load a JSFunction object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8function_t *
v8function_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t shared;
	v8function_t *funcp;

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0) {
		v8_warn("%p: not a heap object\n", addr);
		return (NULL);
	}

	if (type != V8_TYPE_JSFUNCTION) {
		v8_warn("%p: not a JSFunction\n", addr);
		return (NULL);
	}

	if (read_heap_ptr(&shared, addr, V8_OFF_JSFUNCTION_SHARED) != 0) {
		v8_warn("%p: no SharedFunctionInfo\n", addr);
		return (NULL);
	}

	if ((funcp = mdb_zalloc(sizeof (*funcp), memflags)) == NULL) {
		return (NULL);
	}

	funcp->v8func_addr = addr;
	funcp->v8func_memflags = memflags;
	funcp->v8func_shared = shared;
	return (funcp);
}

/*
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8function_free(v8function_t *funcp)
{
	if (funcp == NULL) {
		return;
	}

	maybefree(funcp, sizeof (*funcp), funcp->v8func_memflags);
}

/*
 * Given a JSFunction in "funcp", load into "ctxp" the context associated with
 * this function.  This is a convenience function that finds the context and
 * calls v8context_load(), so see the notes about that function.
 *
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8context_t *
v8function_context(v8function_t *funcp, int memflags)
{
	uintptr_t addr, context;

	addr = funcp->v8func_addr;
	if (read_heap_ptr(&context, addr, V8_OFF_JSFUNCTION_CONTEXT) != 0) {
		v8_warn("%p: failed to read context\n", addr);
		return (NULL);
	}

	return (v8context_load(context, memflags));
}

/*
 * Given a JSFunction in "funcp", load the ScopeInfo associated with this
 * function.  This is a convenience function that ultimately calls
 * v8scopeinfo_load(), so see the notes about that function.
 *
 * See the patterns in mdb_v8_dbg.h for interface details.
 *
 * Note that this returns the ScopeInfo that's effectively defined by this
 * function.  Contexts created _within_ this function (e.g., nested functions)
 * use this ScopeInfo.  This function itself has a context with its own
 * ScopeInfo, and that's not the same as this one.  (For that, use
 * v8function_context() and then v8context_scopeinfo().)
 */
v8scopeinfo_t *
v8function_scopeinfo(v8function_t *funcp, int memflags)
{
	uintptr_t closure, shared, scopeinfo;

	if (V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO == -1) {
		v8_warn("could not find \"scope_info\"");
		return (NULL);
	}

	closure = funcp->v8func_addr;
	if (read_heap_ptr(&shared, closure, V8_OFF_JSFUNCTION_SHARED) != 0 ||
	    read_heap_ptr(&scopeinfo, shared,
	    V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO) != 0) {
		return (NULL);
	}

	return (v8scopeinfo_load(scopeinfo, memflags));
}

/*
 * Given a function, load the shared function information.
 *
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8funcinfo_t *
v8function_funcinfo(v8function_t *funcp, int memflags)
{
	return (v8funcinfo_load(funcp->v8func_shared, memflags));
}


/*
 * SharedFunctionInfo functions
 */

/*
 * Load a SharedFunctionInfo object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8funcinfo_t *
v8funcinfo_load(uintptr_t funcinfo, int memflags)
{
	v8funcinfo_t *fip;
	uintptr_t script, name, inferred_name, code;
	uintptr_t scriptpath, lineends, tokenpos;

	if (read_heap_maybesmi(&tokenpos, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION) != 0 ||
	    read_heap_ptr(&name, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_NAME) != 0 ||
	    read_heap_ptr(&script, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0 ||
	    read_heap_ptr(&scriptpath, script, V8_OFF_SCRIPT_NAME) != 0 ||
	    read_heap_ptr(&lineends, script, V8_OFF_SCRIPT_LINE_ENDS) != 0 ||
	    read_heap_ptr(&code, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_CODE) != 0) {
		return (NULL);
	}

	if (read_heap_ptr(&inferred_name, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER) != 0) {
		inferred_name = 0;
	}

	fip = mdb_zalloc(sizeof (*fip), memflags);
	if (fip == NULL) {
		return (NULL);
	}

	fip->v8fi_addr = funcinfo;
	fip->v8fi_memflags = memflags;
	fip->v8fi_funcname = name;
	fip->v8fi_inferred_name = inferred_name;
	fip->v8fi_script = script;
	fip->v8fi_scriptpath = scriptpath;
	fip->v8fi_tokenpos = tokenpos;
	fip->v8fi_line_endings = jsobj_is_undefined(lineends) ? NULL : lineends;
	fip->v8fi_code = code;
	return (fip);
}

/*
 * Free a SharedFunctionInfo object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8funcinfo_free(v8funcinfo_t *fip)
{
	if (fip == NULL) {
		return;
	}

	maybefree(fip, sizeof (*fip), fip->v8fi_memflags);
}

/*
 * Write a human-readable name for the function "fip" into the buffer "strb".
 * This function may produce the name the function was defined with or an
 * inferred name, and it may attempt to include information about where the
 * function was defined.
 */
int
v8funcinfo_funcname(v8funcinfo_t *fip, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t flags)
{
	v8string_t *strp;
	int rv = 0;

	/*
	 * First, try to load the proper function name.  If that works, we're
	 * done.
	 */
	strp = v8string_load(fip->v8fi_funcname, UM_SLEEP);
	if (strp == NULL) {
		mdbv8_strbuf_sprintf(strb, "<unknown>");
	} else if (v8string_length(strp) == 0) {
		mdbv8_strbuf_sprintf(strb, "<anonymous>");
		v8string_free(strp);
	} else {
		rv = v8string_write(strp, strb, flags, JSSTR_NUDE);
		v8string_free(strp);
		return (rv);
	}

	/*
	 * If that failed or was empty, then we printed a generic name, but try
	 * now to append the inferred name.
	 */
	if (fip->v8fi_inferred_name != NULL) {
		strp = v8string_load(fip->v8fi_inferred_name, UM_SLEEP);
		if (strp != NULL) {
			mdbv8_strbuf_sprintf(strb, " (as ");
			if (v8string_length(strp) == 0) {
				mdbv8_strbuf_sprintf(strb, "<anon>");
			} else {
				rv = v8string_write(strp, strb, flags,
				    JSSTR_NUDE);
			}

			mdbv8_strbuf_sprintf(strb, ")");
			v8string_free(strp);
		}
	}

	return (rv);
}

/*
 * Write the name of the script where the function "fip" was defined into the
 * buffer "strb".
 */
int
v8funcinfo_scriptpath(v8funcinfo_t *fip, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t flags)
{
	v8string_t *strp;
	int rv;

	strp = v8string_load(fip->v8fi_scriptpath, UM_SLEEP);
	if (strp == NULL)
		return (-1);

	rv = v8string_write(strp, strb, flags, JSSTR_NUDE);
	v8string_free(strp);
	return (rv);
}

/*
 * Write the location (within a script) where the function "fip" is defined.
 * More precisely, this is the location where the "function" JavaScript token
 * appears inside the file.  If line number information is available, the
 * location will include the line number.  Otherwise, it will include the
 * position in the file.  For Node.js programs, it's important to know that Node
 * itself prepends program files with a short header, which can cause the
 * position number to be slightly different than a developer might expect.  The
 * debugger's job is to report facts, not convenient lies.
 */
int
v8funcinfo_definition_location(v8funcinfo_t *fip, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t flags)
{
	uintptr_t tokpos, lower, upper, i;
	uintptr_t *data;
	v8fixedarray_t *arrayp;

	/*
	 * The "function" token position is an SMI, and has already been decoded
	 * at this point.  For both the -1 check and the binary search
	 * algorithm below, it's easier to compare this to other SMI-encoded
	 * values, so we re-encode the token position.
	 */
	tokpos = V8_VALUE_SMI(fip->v8fi_tokenpos);

	/*
	 * V8 maintains a table of line endings for each script, but it's lazily
	 * initialized.  If it's there, then we'll use that to map the
	 * function's token position to a line number.  Otherwise, we'll just
	 * print out the position itself (which is basically a character offset
	 * into the script).
	 */
	if (fip->v8fi_line_endings == NULL) {
		if (tokpos == V8_VALUE_SMI(-1)) {
			mdbv8_strbuf_sprintf(strb, "unknown position");
		} else {
			/*
			 * If we're printing out the actual position value
			 * (because we didn't find line number information),
			 * then use the decoded value again.
			 */
			mdbv8_strbuf_sprintf(strb, "position %d",
			    fip->v8fi_tokenpos);
		}

		return (0);
	}

	arrayp = v8fixedarray_load(fip->v8fi_line_endings, UM_NOSLEEP);
	if (arrayp == NULL) {
		return (-1);
	}

	data = v8fixedarray_elts(arrayp);
	lower = 0;
	upper = v8fixedarray_length(arrayp) - 1;
	if (tokpos > data[upper]) {
		mdbv8_strbuf_sprintf(strb, "position out of range");
	} else if (tokpos <= data[0]) {
		mdbv8_strbuf_sprintf(strb, "line 1");
	} else {
		i = 0;
		while (upper >= 1) {
			i = (lower + upper) >> 1;
			if (tokpos > data[i])
				lower = i + 1;
			else if (tokpos <= data[i - 1])
				upper = i - 1;
			else
				break;
		}

		mdbv8_strbuf_sprintf(strb, "line %d", i + 1);
	}

	v8fixedarray_free(arrayp);
	return (0);
}

/*
 * Loads the V8Code object function "fip".  This is useful for disassembling a
 * JavaScript function.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8code_t *
v8funcinfo_code(v8funcinfo_t *fip, int memflags)
{
	return (v8code_load(fip->v8fi_code, memflags));
}


/*
 * V8Code functions
 */

/*
 * Load a V8Code object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8code_t *
v8code_load(uintptr_t code, int memflags)
{
	uintptr_t start, size;
	v8code_t *codep;

	start = code + V8_OFF_CODE_INSTRUCTION_START;
	if (read_heap_ptr(&size, code, V8_OFF_CODE_INSTRUCTION_SIZE) != 0) {
		return (NULL);
	}

	if ((codep = mdb_zalloc(sizeof (*codep), memflags)) == NULL) {
		return (NULL);
	}

	codep->v8code_addr = code;
	codep->v8code_memflags = memflags;
	codep->v8code_instr_start = start;
	codep->v8code_instr_size = size;
	return (codep);
}

/*
 * Free a V8Code object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8code_free(v8code_t *codep)
{
	if (codep == NULL) {
		return;
	}

	maybefree(codep, sizeof (*codep), codep->v8code_memflags);
}

/*
 * Return the address in the target process of this V8Code object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
uintptr_t
v8code_addr(v8code_t *codep)
{
	return (codep->v8code_addr);
}

/*
 * Return the starting address for the native instructions that make up this
 * Code block.
 */
uintptr_t
v8code_instructions_start(v8code_t *codep)
{
	return (codep->v8code_instr_start);
}

/*
 * Return the size of the memory region containing the native instructions for
 * this Code block.
 */
uintptr_t
v8code_instructions_size(v8code_t *codep)
{
	return (codep->v8code_instr_size);
}


/*
 * Context functions
 */

/*
 * Given a V8 Context in "addr", load it into "ctxp".
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8context_t *
v8context_load(uintptr_t addr, int memflags)
{
	v8context_t *ctxp;

	if ((ctxp = mdb_zalloc(sizeof (*ctxp), memflags)) == NULL) {
		return (NULL);
	}

	ctxp->v8ctx_addr = addr;
	ctxp->v8ctx_memflags = memflags;
	if (read_heap_array(addr, &ctxp->v8ctx_elts,
	    &ctxp->v8ctx_nelts, memflags) != 0) {
		goto err;
	}

	if (ctxp->v8ctx_nelts < V8_CONTEXT_NCOMMON) {
		v8_warn("%p: context array is too short\n", addr);
		goto err;
	}

	return (ctxp);

err:
	v8context_free(ctxp);
	return (NULL);
}

/*
 * Free a loaded Context.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8context_free(v8context_t *ctxp)
{
	if (ctxp == NULL) {
		return;
	}

	maybefree(ctxp->v8ctx_elts,
	    ctxp->v8ctx_nelts * sizeof (ctxp->v8ctx_elts[0]),
	    ctxp->v8ctx_memflags);
	maybefree(ctxp, sizeof (*ctxp), ctxp->v8ctx_memflags);
}

/*
 * Returns the Context's address in the target program.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
uintptr_t
v8context_addr(v8context_t *ctxp)
{
	return (ctxp->v8ctx_addr);
}

/*
 * Returns the address of the closure associated with this context.
 * The closure is a JSFunction object.
 */
uintptr_t
v8context_closure(v8context_t *ctxp)
{
	return (v8context_elt(ctxp, V8_CONTEXT_IDX_CLOSURE));
}

/*
 * Returns the "previous" context for this context.
 */
uintptr_t
v8context_prev_context(v8context_t *ctxp)
{
	return (v8context_elt(ctxp, V8_CONTEXT_IDX_PREV));
}

/*
 * Returns in "*valptr" the value of JavaScript variable "i" in this context.
 * ("i" is an index, described by the context's ScopeInfo.)
 */
int
v8context_var_value(v8context_t *ctxp, unsigned int i, uintptr_t *valptr)
{
	unsigned int idx;

	idx = i + V8_CONTEXT_NCOMMON;
	if (i >= ctxp->v8ctx_nelts) {
		v8_warn("context %p: variable index %d is out of range\n",
		    ctxp->v8ctx_addr, i);
		return (-1);
	}

	*valptr = v8context_elt(ctxp, idx);
	return (0);
}

/*
 * Load scope information for this context into "sip".
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8scopeinfo_t *
v8context_scopeinfo(v8context_t *ctxp, int memflags)
{
	uintptr_t closure;
	v8function_t *funcp;
	v8scopeinfo_t *sip;

	closure = v8context_closure(ctxp);
	funcp = v8function_load(closure, memflags);
	if (funcp == NULL) {
		return (NULL);
	}
	sip = v8function_scopeinfo(funcp, memflags);
	v8function_free(funcp);
	return (sip);
}

/*
 * Private, low-level function for accessing individual slots of the underlying
 * array.
 */
static uintptr_t
v8context_elt(v8context_t *ctxp, unsigned int i)
{
	assert(i < ctxp->v8ctx_nelts);
	return (ctxp->v8ctx_elts[i]);
}

/*
 * Low-level context structure
 *
 * Iterate the statically-defined slots in this context.  These should
 * correspond to the static fields described above in v8context_fields, plus
 * either the global object slot or the native context slot depending on V8's
 * version.  With each slot, the caller gets the slot label and the value in
 * that slot.
 */
int
v8context_iter_static_slots(v8context_t *ctxp,
    int (*func)(v8context_t *, const char *, uintptr_t, void *), void *arg)
{
	unsigned int i;
	intptr_t idx;
	uintptr_t value;
	v8context_field_t *fp;
	int rv;

	rv = 0;
	for (i = 0; i < v8context_nfields; i++) {
		fp = &v8context_fields[i];
		idx = *(fp->v8ctxf_idxp);
		value = v8context_elt(ctxp, idx);
		rv = func(ctxp, fp->v8ctxf_label, value, arg);
		if (rv != 0) {
			break;
		}
	}

	/*
	 * If a value for V8_CONTEXT_IDX_NATIVE was found, it means we're
	 * inspecting a core file generated by a version of V8 that is recent
	 * enough to link to the native context instead of the global object. So
	 * use that value instead of the one that was found or on which we fell
	 * back for V8_CONTEXT_IDX_GLOBAL.
	 */
	if (V8_CONTEXT_IDX_NATIVE != -1) {
		rv = func(ctxp, "native context", v8context_elt(ctxp,
		    V8_CONTEXT_IDX_NATIVE), arg);
	} else {
		rv = func(ctxp, "global object", v8context_elt(ctxp,
		    V8_CONTEXT_IDX_GLOBAL), arg);
	}

	return (rv);
}

/*
 * Iterate the dynamically-defined slots in this context.  These correspond to
 * the values described in the context's ScopeInfo.  With each slot, the caller
 * gets the integer index of the slot (relative to the start of the dynamic
 * slots) and the value in that slot.  (This function does not assume that the
 * scope information has been loaded, so it only provides values by the integer
 * index.)
 */
int
v8context_iter_dynamic_slots(v8context_t *ctxp,
    int (*func)(v8context_t *, uint_t, uintptr_t, void *), void *arg)
{
	unsigned int nslots, i;
	int rv = 0;

	nslots = V8_CONTEXT_NCOMMON;
	for (i = nslots; i < ctxp->v8ctx_nelts; i++) {
		rv = func(ctxp, i - nslots, v8context_elt(ctxp, i), arg);
		if (rv != 0) {
			break;
		}
	}

	return (rv);
}


/*
 * ScopeInfo functions
 */

/*
 * Given a V8 ScopeInfo in "addr", load it into "sip".
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8scopeinfo_t *
v8scopeinfo_load(uintptr_t addr, int memflags)
{
	v8scopeinfo_t *sip;

	if ((sip = mdb_zalloc(sizeof (*sip), memflags)) == NULL) {
		return (NULL);
	}

	sip->v8si_memflags = memflags;
	sip->v8si_addr = addr;
	if (read_heap_array(addr,
	    &sip->v8si_elts, &sip->v8si_nelts, memflags) != 0) {
		goto err;
	}

	if (sip->v8si_nelts < V8_SCOPEINFO_IDX_FIRST_VARS) {
		v8_warn("array too short to be a ScopeInfo\n");
		goto err;
	}

	if (!V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NPARAMS]) ||
	    !V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NSTACKLOCALS]) ||
	    !V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NCONTEXTLOCALS])) {
		v8_warn("static ScopeInfo fields do not look like SMIs\n");
		goto err;
	}

	return (sip);

err:
	v8scopeinfo_free(sip);
	return (NULL);
}

/*
 * Free a loaded ScopeInfo object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8scopeinfo_free(v8scopeinfo_t *sip)
{
	if (sip == NULL) {
		return;
	}

	maybefree(sip->v8si_elts,
	    sip->v8si_nelts * sizeof (sip->v8si_elts[0]), sip->v8si_memflags);
	maybefree(sip, sizeof (*sip), sip->v8si_memflags);
}

/*
 * Returns the address in the target program of the ScopeInfo object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
uintptr_t
v8scopeinfo_addr(v8scopeinfo_t *sip)
{
	return (sip->v8si_addr);
}

/*
 * Iterate the vartypes in a ScopeInfo, which correspond to different kinds of
 * variable (e.g., "parameter", "stack-local variable", or "context-local
 * variable").  The caller gets an enum describing the vartype, which can be
 * used to get the vartype name and iterate variables of this type.
 */
int
v8scopeinfo_iter_vartypes(v8scopeinfo_t *sip,
    int (*func)(v8scopeinfo_t *, v8scopeinfo_vartype_t, void *), void *arg)
{
	int i, rv;
	v8scopeinfo_vartype_info_t *vtip;

	rv = 0;

	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		vtip = &v8scopeinfo_vartypes[i];
		rv = func(sip, vtip->v8vti_vartype, arg);
		if (rv != 0)
			break;
	}

	return (rv);
}

/*
 * Returns a human-readable label for a given kind of scope variable.  The scope
 * variable must be valid.
 */
const char *
v8scopeinfo_vartype_name(v8scopeinfo_vartype_t scopevartype)
{
	v8scopeinfo_vartype_info_t *vtip;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	return (vtip->v8vti_label);
}

/*
 * Returns the number of variables of this kind (e.g., the number of
 * context-local variables, when scopevartype is V8SV_CONTEXTLOCALS).
 */
size_t
v8scopeinfo_vartype_nvars(v8scopeinfo_t *sip,
    v8scopeinfo_vartype_t scopevartype)
{
	v8scopeinfo_vartype_info_t *vtip;
	uintptr_t value;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	value = sip->v8si_elts[*(vtip->v8vti_idx_countp)];
	assert(V8_IS_SMI(value));
	return (V8_SMI_VALUE(value));
}

/*
 * Iterate the variables of the kind specified by "scopevartype" (e.g.,
 * context-local variables, when scopevartype is V8SV_CONTEXTLOCALS).  With each
 * variable, the caller gets an opaque pointer that can be used to get the
 * variable's name and an index for retrieving its value from a given context.
 */
int
v8scopeinfo_iter_vars(v8scopeinfo_t *sip,
    v8scopeinfo_vartype_t scopevartype,
    int (*func)(v8scopeinfo_t *, v8scopeinfo_var_t *, void *), void *arg)
{
	int rv;
	size_t i, nvars, nskip, idx;
	v8scopeinfo_vartype_info_t *vtip, *ogrp;
	v8scopeinfo_var_t var;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	nvars = v8scopeinfo_vartype_nvars(sip, scopevartype);

	/*
	 * Skip to the start of the ScopeInfo's dynamic part. See mdb_v8_dbg.h
	 * for more details on the layout of ScopeInfo objects.
	 */
	nskip = V8_SCOPEINFO_IDX_FIRST_VARS;

	/*
	 * Iterate over variable types so that we can add the offset from the
	 * beginning of the actual data (the dynamic part) to the region of the
	 * dynamic part that is specific to the variable type we're interested
	 * in.
	 */
	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		ogrp = &v8scopeinfo_vartypes[i];

		/*
		 * In the variable/dynamic part of a ScopeInfo layout, some
		 * variable types have static metadata, e.g stack local entries
		 * have a StackLocalFirstSlot, before the actual data. Add that
		 * offset for each variable type, including for the one we're
		 * interested in.
		 */
		if (v8scopeinfo_vartypes[i].v8vti_offset != NULL &&
		    *(v8scopeinfo_vartypes[i].v8vti_offset) != -1) {
			nskip += *(v8scopeinfo_vartypes[i].v8vti_offset);
		}

		/*
		 * If the current variable type is the one we're interested in,
		 * do not add anything to the offset. We're done.
		 */
		if (*(ogrp->v8vti_idx_countp) == *(vtip->v8vti_idx_countp)) {
			break;
		}

		/*
		 * The data for the current variable type is before the one
		 * we're interested in in the variable part of the ScopeInfo
		 * layout. Add the number of entries for this variable type to
		 * the offset.
		 */
		nskip += v8scopeinfo_vartype_nvars(sip, ogrp->v8vti_vartype);
	}

	rv = 0;
	for (i = 0; i < nvars; i++) {
		idx = nskip + i;
		if (idx >= sip->v8si_nelts) {
			v8_warn("v8scopeinfo_iter_vars: short scopeinfo\n");
			return (-1);
		}

		var.v8siv_which = i;
		var.v8siv_realidx = idx;
		rv = func(sip, &var, arg);
		if (rv != 0) {
			break;
		}
	}

	return (rv);
}

/*
 * Returns the integer index for this variable.  This is used to extract the
 * value out of a context with this scope.
 */
size_t
v8scopeinfo_var_idx(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp)
{
	return (sivp->v8siv_which);
}

/*
 * Returns the name of of this variable (as a heap string).
 */
uintptr_t
v8scopeinfo_var_name(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp)
{
	assert(sivp->v8siv_realidx < sip->v8si_nelts);
	return (sip->v8si_elts[sivp->v8siv_realidx]);
}

/*
 * Look up our internal metadata for this vartype.
 */
static v8scopeinfo_vartype_info_t *
v8scopeinfo_vartype_lookup(v8scopeinfo_vartype_t scopevartype)
{
	int i;
	v8scopeinfo_vartype_info_t *vtip;

	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		vtip = &v8scopeinfo_vartypes[i];
		if (scopevartype == vtip->v8vti_vartype)
			return (vtip);
	}

	return (NULL);
}


/*
 * Bound functions
 *
 * Bound functions are functions returned from JavaScript's Function.bind()
 * method.  A bound function essentially wraps some other function, but
 * overrides the "this" and any number of initial arguments:
 *
 *     function myFunc(arg1, arg2) { ... }
 *
 *     var newctx = {};
 *     var bound = myFunc.bind(newctx, 'hello');
 *
 * In this example:
 *
 *     "bound" is the bound function.  Invoking it invokes "myFunc".
 *     "myFunc" is the target function.
 *     "newctx" is the value of "this" inside the "myFunc" invocation.
 *
 * See the comments in mdb_v8_dbg.h and in the definition of "struct
 * v8boundfunction" above for implementation notes.
 */

v8boundfunction_t *
v8boundfunction_load(uintptr_t addr, int memflags)
{
	if (V8_TYPE_JSBOUNDFUNCTION == -1) {
		/*
		 * This is Node prior to v6.  Load this as a v8function_t and
		 * use its bindings array.
		 */
		return (v8boundfunction_load_bindings(addr, memflags));
	} else {
		/*
		 * This is a more recent version of V8 that stores information
		 * directly in a JSBoundFunction.
		 */
		return (v8boundfunction_load_direct(addr, memflags));
	}
}

static v8boundfunction_t *
v8boundfunction_load_bindings(uintptr_t addr, int memflags)
{
	v8function_t *funcp;
	v8boundfunction_t *bfp;
	uintptr_t hints, bindingsp;
	v8funcinfo_t *fip;
	int err;

	funcp = v8function_load(addr, memflags);
	if (funcp == NULL) {
		return (NULL);
	}

	/*
	 * To determine whether this is even a bound function, we need to look
	 * at the compiler hints hanging off the SharedFunctionInfo.
	 */
	fip = v8function_funcinfo(funcp, memflags);
	v8function_free(funcp);
	if (fip == NULL) {
		return (NULL);
	}

	err = read_heap_maybesmi(&hints, fip->v8fi_addr,
	    V8_OFF_SHAREDFUNCTIONINFO_COMPILER_HINTS);
	v8funcinfo_free(fip);
	if (err != 0) {
		return (NULL);
	}

	if (!V8_HINT_BOUND(hints)) {
		v8_warn("%p: not a bound function\n", addr);
		return (NULL);
	}

	if (read_heap_ptr(&bindingsp, addr,
	    V8_OFF_JSFUNCTION_LITERALS_OR_BINDINGS) != 0) {
		v8_warn("%p: failed to load bindings\n", addr);
		return (NULL);
	}

	if ((bfp = mdb_zalloc(sizeof (*bfp), memflags)) == NULL) {
		return (NULL);
	}

	bfp->v8bf_addr = addr;
	bfp->v8bf_memflags = memflags;

	if (read_heap_array(bindingsp, &bfp->v8bf_array,
	    &bfp->v8bf_arraylen, memflags) != 0) {
		v8_warn("%p: failed to load bindings array\n", addr);
		v8boundfunction_free(bfp);
		return (NULL);
	}

	if (bfp->v8bf_arraylen < V8_BINDINGS_INDEX_ARGS_START) {
		v8_warn("%p: bindings array is too short\n", addr);
		v8boundfunction_free(bfp);
		return (NULL);
	}

	bfp->v8bf_target = bfp->v8bf_array[V8_BINDINGS_INDEX_TARGET];
	bfp->v8bf_this = bfp->v8bf_array[V8_BINDINGS_INDEX_THIS];
	bfp->v8bf_idx_arg0 = V8_BINDINGS_INDEX_ARGS_START;
	return (bfp);
}

static v8boundfunction_t *
v8boundfunction_load_direct(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t boundArgs;
	v8boundfunction_t *bfp;

	assert(V8_TYPE_JSBOUNDFUNCTION != -1);

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0) {
		v8_warn("%p: not a heap object\n", addr);
		return (NULL);
	}

	if (type != V8_TYPE_JSBOUNDFUNCTION) {
		v8_warn("%p: not a JSBoundFunction\n", addr);
		return (NULL);
	}

	if ((bfp = mdb_zalloc(sizeof (*bfp), memflags)) == NULL) {
		return (NULL);
	}

	if (read_heap_ptr(&bfp->v8bf_target, addr,
	    V8_OFF_JSBOUNDFUNCTION_BOUND_TARGET_FUNCTION) == -1 ||
	    read_heap_ptr(&bfp->v8bf_this, addr,
	    V8_OFF_JSBOUNDFUNCTION_BOUND_THIS) == -1 ||
	    read_heap_ptr(&boundArgs, addr,
	    V8_OFF_JSBOUNDFUNCTION_BOUND_ARGUMENTS) == -1 ||
	    read_heap_array(boundArgs, &bfp->v8bf_array,
	    &bfp->v8bf_arraylen, memflags) == -1) {
		v8_warn("%p: failed to read binding details\n", addr);
		v8boundfunction_free(bfp);
		return (NULL);
	}

	bfp->v8bf_addr = addr;
	bfp->v8bf_memflags = memflags;
	return (bfp);
}

/*
 * Returns the target of this bound function (i.e., the function that this
 * function wraps).
 */
uintptr_t
v8boundfunction_target(v8boundfunction_t *bfp)
{
	return (bfp->v8bf_target);
}

/*
 * Returns the value of "this" specified by this binding.
 */
uintptr_t
v8boundfunction_this(v8boundfunction_t *bfp)
{
	return (bfp->v8bf_this);
}

/*
 * Returns the number of arguments specified by this binding.
 */
size_t
v8boundfunction_nargs(v8boundfunction_t *bfp)
{
	return (bfp->v8bf_arraylen - bfp->v8bf_idx_arg0);
}

/*
 * Iterates the arguments specified by this binding.
 */
int
v8boundfunction_iter_args(v8boundfunction_t *bfp,
    int (*func)(v8boundfunction_t *, uint_t, uintptr_t, void *), void *arg)
{
	size_t argi, elti;
	int rv = 0;

	for (argi = 0, elti = bfp->v8bf_idx_arg0;
	    elti < bfp->v8bf_arraylen; argi++, elti++) {
		rv = func(bfp, argi, bfp->v8bf_array[elti], arg);
		if (rv != 0) {
			return (rv);
		}
	}

	assert(argi == v8boundfunction_nargs(bfp));
	return (0);
}

/*
 * Free a v8boundfunction_t object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8boundfunction_free(v8boundfunction_t *bfp)
{
	if (bfp == NULL) {
		return;
	}

	maybefree(bfp->v8bf_array,
	    bfp->v8bf_arraylen * sizeof (bfp->v8bf_array[0]),
	    bfp->v8bf_memflags);
	maybefree(bfp, sizeof (*bfp), bfp->v8bf_memflags);
}
