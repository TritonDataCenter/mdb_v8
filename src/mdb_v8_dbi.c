/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * mdb_v8_dbi.c: implementation of interfaces typically provided by the
 * surrounding debugger (i.e., mdb).  Various low-level functions in mdb_v8.c
 * (e.g., read_heap_ptr() and related functions) ought to be polished and moved
 * into here.
 */

#include "mdb_v8_impl.h"

#include <libproc.h>

/*
 * dbi_ugrep(addr, func, arg): find references to "addr" in the address space
 * and invoke "func" for each one.  Specifically, scans all pointer-aligned
 * values in mapped memory and invokes "func" for each one whose value is
 * "addr".
 *
 * mdb provides a "::ugrep" dcmd that implements this sort of search (with much
 * more flexibility), but there's no way for us to use it here (except maybe via
 * an "::eval" that calls back into a private dcmd that we write).  Since it's
 * not that complicated to begin with, we essentially reimplement it here.
 */

/*
 * Describes the state of a "ugrep" operation.
 */
typedef struct ugrep_op {
	uintptr_t	ug_addr;	/* address we're searching for */
	int		ug_result;	/* ret code of the ugrep operation */
	int		(*ug_callback)(uintptr_t, void *);	/* user cb */
	void		*ug_cbarg;	/* user callback args */
	uintptr_t	*ug_buf;	/* buffer for reading memory */
	size_t		ug_bufsz;	/* size of "ug_buf" */
} ugrep_op_t;

static int ugrep_mapping(ugrep_op_t *, const prmap_t *, const char *);

int
dbi_ugrep(uintptr_t addr, int (*callback)(uintptr_t, void *), void *cbarg)
{
	struct ps_prochandle *Pr;
	ugrep_op_t ugrep;
	int err;

	if (mdb_get_xdata("pshandle", &Pr, sizeof (Pr)) == -1) {
		mdb_warn("couldn't read pshandle xdata");
		return (-1);
	}

	ugrep.ug_addr = addr;
	ugrep.ug_result = 0;
	ugrep.ug_callback = callback;
	ugrep.ug_cbarg = cbarg;
	ugrep.ug_bufsz = 4096;
	ugrep.ug_buf = mdb_zalloc(ugrep.ug_bufsz, UM_SLEEP);

	err = Pmapping_iter(Pr, (proc_map_f *)ugrep_mapping, &ugrep);
	mdb_free(ugrep.ug_buf, ugrep.ug_bufsz);

	return (err != 0 ? -1 : ugrep.ug_result);
}

/* ARGSUSED */
static int
ugrep_mapping(ugrep_op_t *ugrep, const prmap_t *pmp, const char *name)
{
	uintptr_t chunkbase, vaddr;
	uintptr_t *buf;
	size_t ntoread, bufsz, nptrs, i;

	buf = ugrep->ug_buf;
	bufsz = ugrep->ug_bufsz;

	for (chunkbase = pmp->pr_vaddr;
	    chunkbase < pmp->pr_vaddr + pmp->pr_size; chunkbase += bufsz) {
		ntoread = MIN(bufsz,
		    pmp->pr_size - (chunkbase - pmp->pr_vaddr));

		if (mdb_vread(buf, ntoread, chunkbase) == -1) {
			/*
			 * Some mappings are not present in core files.  This
			 * does not represent an error case here.
			 */
			continue;
		}

		nptrs = ntoread / sizeof (uintptr_t);
		for (i = 0; i < nptrs; i++) {
			if (buf[i] == ugrep->ug_addr) {
				vaddr = chunkbase + (i * sizeof (uintptr_t));
				ugrep->ug_result = ugrep->ug_callback(vaddr,
				    ugrep->ug_cbarg);
				if (ugrep->ug_result != 0) {
					return (-1);
				}
			}
		}
	}

	return (0);
}
