/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * mdb_v8_dbi.h: interfaces typically provided by the surrounding debugger
 * (i.e., mdb).
 */

#ifndef	_MDBV8DBI_H
#define	_MDBV8DBI_H

int dbi_ugrep(uintptr_t, int (*func)(uintptr_t, void *), void *);

#endif	/* _MDBV8DBI_H */
