/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * mdb_v8_subr.c: utility functions used internally within mdb_v8.
 */

#include <assert.h>
#include <alloca.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

struct v8fixedarray {
	uintptr_t	v8fa_addr;
	int		v8fa_memflags;
	unsigned long	v8fa_nelts;
	uintptr_t	v8fa_elements;
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

	maybefree(arrayp, sizeof (*arrayp), arrayp->v8fa_memflags);
}

/*
 * Iterate the elements of this fixed array.
 *
 * This implementation is careful to avoid needing memory proportional to the
 * array size, as that makes it very difficult for end users to work with very
 * large arrays.
 */
int
v8fixedarray_iter_elements(v8fixedarray_t *arrayp,
    int (*func)(v8fixedarray_t *, unsigned int, uintptr_t, void *),
    void *uarg)
{
	int maxnpgelts = 1024;
	int curnpgelts;
	uintptr_t *buf;
	uintptr_t addr;
	unsigned int index, length, i;
	size_t maxpgsz, curpgsz;
	int rv = -1;

	length = v8fixedarray_length(arrayp);
	if (length == 0) {
		return (0);
	}

	maxpgsz = maxnpgelts * sizeof (buf[0]);
	buf = alloca(maxpgsz);

	addr = arrayp->v8fa_addr + V8_OFF_FIXEDARRAY_DATA;
	index = 0;

	do {
		curnpgelts = MIN(length - index, maxnpgelts);
		curpgsz = curnpgelts * sizeof (buf[0]);
		rv = mdb_vread(buf, curpgsz, addr);
		if (rv == -1) {
			v8_warn("failed to read array from index %d", index);
			break;
		}

		for (i = 0; i < curnpgelts; i++) {
			rv = func(arrayp, index + i, buf[i], uarg);
			if (rv != 0) {
				break;
			}
		}

		index += i;
		addr += curpgsz;
	} while (rv == 0 && index < length - 1);

	return (rv);
}

/*
 * Return a native array representing the contents of the FixedArray "arrayp".
 * NOTE: If possible, v8fixedarray_iter_elements() should be used instead of
 * this function.  That function requires only a constant amount of memory
 * regardless of the array size.  That can be a major performance improvement
 * when available memory is limited, and it makes it possible to work with
 * arbitrarily large arrays for which we can't necessarily allocate such a large
 * block.
 *
 * The length of the returned array is given by v8fixedarray_length().  The
 * caller must free this with "maybefree" using the same "memflags".
 * TODO This should probably either be a separate type, or maybe we should hang
 * it off the v8fixedarray_t (and keep it cached).
 */
uintptr_t *
v8fixedarray_as_array(v8fixedarray_t *arrayp, int memflags)
{
	uintptr_t *elts;
	size_t arraysz;

	if (arrayp->v8fa_nelts == 0) {
		return (NULL);
	}

	arraysz = arrayp->v8fa_nelts * sizeof (elts[0]);
	elts = mdb_zalloc(arraysz, memflags);
	if (elts == NULL) {
		return (NULL);
	}

	if (mdb_vread(elts, arraysz,
	    arrayp->v8fa_addr + V8_OFF_FIXEDARRAY_DATA) == -1) {
		maybefree(elts, arraysz, memflags);
		return (NULL);
	}

	return (elts);
}

/*
 * Returns the number of elements in the FixedArray "arrayp".
 */
size_t
v8fixedarray_length(v8fixedarray_t *arrayp)
{
	return (arrayp->v8fa_nelts);
}
