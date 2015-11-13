/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_subr.c: utility functions used internally within mdb_v8.
 */

#include <assert.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

struct v8fixedarray {
	uintptr_t	v8fa_addr;
	int		v8fa_memflags;
	unsigned long	v8fa_nelts;
	uintptr_t	*v8fa_elts;
};

/*
 * Load a V8 FixedArray object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8fixedarray_t *
v8fixedarray_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t nelts;
	uintptr_t *elts;
	size_t arraysz;
	v8fixedarray_t *arrayp;

	if (!V8_IS_HEAPOBJECT(addr) ||
	    read_typebyte(&type, addr) != 0 || type != V8_TYPE_FIXEDARRAY ||
	    read_heap_smi(&nelts, addr, V8_OFF_FIXEDARRAY_LENGTH) != 0 ||
	    (arrayp = mdb_zalloc(sizeof (*arrayp), memflags)) == NULL) {
		return (NULL);
	}

	arrayp->v8fa_addr = addr;
	arrayp->v8fa_memflags = memflags;
	arrayp->v8fa_nelts = nelts;

	if (arrayp->v8fa_nelts > 0) {
		arraysz = nelts * sizeof (elts[0]);
		elts = mdb_zalloc(arraysz, memflags);
		if (elts == NULL) {
			v8fixedarray_free(arrayp);
			return (NULL);
		}

		arrayp->v8fa_elts = elts;
		if (mdb_vread(elts, arraysz,
		    addr + V8_OFF_FIXEDARRAY_DATA) == -1) {
			v8fixedarray_free(arrayp);
			return (NULL);
		}
	}

	return (arrayp);
}

/*
 * Free a V8 FixedArray object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8fixedarray_free(v8fixedarray_t *arrayp)
{
	if (arrayp == NULL) {
		return;
	}

	maybefree(arrayp->v8fa_elts,
	    arrayp->v8fa_nelts * sizeof (arrayp->v8fa_elts[0]),
	    arrayp->v8fa_memflags);
	maybefree(arrayp, sizeof (*arrayp), arrayp->v8fa_memflags);
}

/*
 * Return a native array representing the contents of the FixedArray "arrayp".
 * The length of the array is given by v8fixedarray_length().
 */
uintptr_t *
v8fixedarray_elts(v8fixedarray_t *arrayp)
{
	return (arrayp->v8fa_elts);
}

/*
 * Returns the number of elements in the FixedArray "arrayp".
 */
size_t
v8fixedarray_length(v8fixedarray_t *arrayp)
{
	return (arrayp->v8fa_nelts);
}
