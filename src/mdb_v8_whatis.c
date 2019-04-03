/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * mdb_v8_whatis.c: implementation of "v8whatis" functionality.
 */

#include <assert.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

/*
 * v8whatis() attempts to find the V8 heap object that contains "addr" by
 * looking at up to "maxoffset" bytes leading up to "addr" for the specific
 * signature that indicates a V8 heap object, and then interpreting any possible
 * heap object to see if the target address is indeed contained within it.
 * Results are stored into "whatisp", and any errors are returned as a
 * "v8whatis_error_t".  Note that as many fields of "whatisp" are populated as
 * possible, so even if you get V8W_ERR_DOESNTCONTAIN (which indicates that we
 * found an object, but it doesn't seem to contain the target), then
 * v8w_baseaddr and v8w_basetype are still valid.
 */
v8whatis_error_t
v8whatis(uintptr_t addr, size_t maxoffset, v8whatis_t *whatisp)
{
	uintptr_t origaddr, curaddr, curvalue, ptrlowbits;
	size_t curoffset;
	boolean_t contained;
	uint8_t typebyte;

	origaddr = addr;
	whatisp->v8w_origaddr = origaddr;

	/*
	 * Objects will always be stored at pointer-aligned addresses.  If we're
	 * given an address that's not pointer-aligned, clear the low bits to
	 * find the pointer-sized value containing the address given.
	 */
	ptrlowbits = sizeof (uintptr_t) - 1;
	addr &= ~ptrlowbits;
	assert(addr <= origaddr && origaddr - addr < sizeof (uintptr_t));

	/*
	 * On top of that, set the heap object tag bits.  Recall that most
	 * mdb_v8 operations interpret values the same way as V8: if the tag
	 * bits are set, then this is a heap object; otherwise, it's not.  And
	 * this command only makes sense for heap objects, so one might expect
	 * that we would bail if we're given something else.  But in practice,
	 * this command is expected to be chained with `::ugrep` or some other
	 * command that reports heap objects without the tag bits set, so it
	 * makes sense to just assume they were supposed to be set.
	 */
	addr |= V8_HeapObjectTag;
	whatisp->v8w_addr = addr;

	/*
	 * At this point, we walk backwards from the address we're given looking
	 * for something that looks like a V8 heap object.
	 */
	for (curoffset = 0; curoffset < maxoffset;
	    curoffset += sizeof (uintptr_t)) {
		curaddr = addr - curoffset;
		assert(V8_IS_HEAPOBJECT(curaddr));

		if (read_heap_ptr(&curvalue, curaddr,
		    V8_OFF_HEAPOBJECT_MAP) != 0 ||
		    read_typebyte(&typebyte, curvalue) != 0) {
			/*
			 * The address we're looking at was either unreadable,
			 * or we could not follow its Map pointer to find the
			 * type byte.  This cannot be a valid heap object
			 * because every heap object has a Map pointer as its
			 * first field.
			 */
			continue;
		}

		if (typebyte != V8_TYPE_MAP) {
			/*
			 * The address we're looking at refers to something
			 * other than a Map.  Again, this cannot be the address
			 * of a valid heap object.
			 */
			continue;
		}

		/*
		 * We've found what looks like a valid Map object.  See if we
		 * can read its type byte, too.  If not, this is likely garbage.
		 */
		if (read_typebyte(&typebyte, curaddr) != 0) {
			continue;
		}

		break;
	}

	if (curoffset >= maxoffset) {
		return (V8W_ERR_NOTFOUND);
	}

	whatisp->v8w_baseaddr = curaddr;
	whatisp->v8w_basetype = typebyte;

	/*
	 * At this point, check to see if the address that we were given might
	 * be contained in this object.  If not, that means we found a Map for a
	 * heap object that doesn't contain our target address.  We could have
	 * checked this in the loop above so that we'd keep walking backwards in
	 * this case, but we assume that Map objects aren't likely to appear
	 * inside the middle of other valid objects, and thus that if we found a
	 * Map and its heap object doesn't contain our target address, then
	 * we're done -- there is no heap object containing our target.
	 */
	if (v8contains(curaddr, typebyte, addr, &contained) == 0 &&
	    !contained) {
		return (V8W_ERR_DOESNTCONTAIN);
	}

	return (V8W_OK);
}
