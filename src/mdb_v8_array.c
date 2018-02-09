/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * mdb_v8_array.c: implementations of functions used for working with
 * JavaScript arrays.  These are not to be confused with V8's internal
 * FixedArray (upon which JavaScript arrays are built).
 */

#include <assert.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

/*
 * See mdb_v8_dbg.h for details on what these various structures represent.
 */
struct v8array {
	uintptr_t	v8array_addr;		/* address in target proc */
	int		v8array_memflags;	/* allocation flags */
	v8fixedarray_t	*v8array_elements;	/* elements array */
	size_t		v8array_length;		/* length */
};

/*
 * Private structure used for managing iteration over the array.
 */
typedef struct {
	v8array_t	*v8ai_array;
	int		(*v8ai_func)(v8array_t *,
	    unsigned int, uintptr_t, void *);
	void		*v8ai_uarg;
	int		v8ai_rv;
} v8array_iteration_t;


/*
 * Load a JSArray object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8array_t *
v8array_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t length, elements;
	v8array_t *ap;

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0) {
		v8_warn("%p: not a heap object\n", addr);
		return (NULL);
	}

	if (type != V8_TYPE_JSARRAY) {
		v8_warn("%p: not a JSArray\n", addr);
		return (NULL);
	}

	if (read_heap_smi(&length, addr, V8_OFF_JSARRAY_LENGTH) != 0) {
		v8_warn("%p: could not read JSArray length\n", addr);
		return (NULL);
	}

	if (read_heap_ptr(&elements, addr, V8_OFF_JSOBJECT_ELEMENTS) != 0) {
		v8_warn("%p: could not read JSArray elements\n", addr);
		return (NULL);
	}

	if ((ap = mdb_zalloc(sizeof (*ap), memflags)) == NULL) {
		return (NULL);
	}

	ap->v8array_addr = addr;
	ap->v8array_length = length;
	ap->v8array_memflags = memflags;

	if (ap->v8array_length > 0) {
		ap->v8array_elements = v8fixedarray_load(elements, memflags);
		if (ap->v8array_elements == NULL) {
			v8array_free(ap);
			return (NULL);
		}
	}

	return (ap);
}

/*
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8array_free(v8array_t *ap)
{
	if (ap == NULL) {
		return;
	}

	v8fixedarray_free(ap->v8array_elements);
	maybefree(ap, sizeof (*ap), ap->v8array_memflags);
}

size_t
v8array_length(v8array_t *ap)
{
	return (ap->v8array_length);
}

static int
v8array_iter_one(v8fixedarray_t *arrayp, unsigned int index,
    uintptr_t value, void *uarg)
{
	v8array_iteration_t *iterate_statep = uarg;

	/*
	 * The JSArray may have fewer elements than the underlying FixedArray
	 * that's used to store its contents.  In that case, stop early.
	 */
	if (index >= iterate_statep->v8ai_array->v8array_length) {
		return (-1);
	}

	iterate_statep->v8ai_rv = (iterate_statep->v8ai_func(
	    iterate_statep->v8ai_array, index, value,
	    iterate_statep->v8ai_uarg));
	return (iterate_statep->v8ai_rv == 0 ? 0 : -1);
}

int
v8array_iter_elements(v8array_t *ap,
    int (*func)(v8array_t *, unsigned int, uintptr_t, void *), void *uarg)
{
	v8array_iteration_t iterate_state;

	if (v8array_length(ap) == 0) {
		return (0);
	}

	iterate_state.v8ai_array = ap;
	iterate_state.v8ai_uarg = uarg;
	iterate_state.v8ai_func = func;
	iterate_state.v8ai_rv = 0;
	(void) v8fixedarray_iter_elements(ap->v8array_elements,
	    v8array_iter_one, &iterate_state);
	return (iterate_state.v8ai_rv);
}
