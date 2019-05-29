/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright 2019 Joyent, Inc.
 */

/*
 * v8cfg.h: canned configurations for previous V8 versions
 */

#ifndef V8CFG_H
#define	V8CFG_H

#include <sys/types.h>
#include <sys/mdb_modapi.h>

typedef struct {
	const char	*v8cs_name;	/* symbol name */
	intptr_t	v8cs_value;	/* symbol value */
} v8_cfg_symbol_t;

typedef struct v8_cfg {
	const char	*v8cfg_name;	/* canned config name */
	const char	*v8cfg_label;	/* description */
	v8_cfg_symbol_t	*v8cfg_symbols;	/* actual symbol values */

	int (*v8cfg_iter)(struct v8_cfg *, int (*)(mdb_symbol_t *, void *),
	    void *);
	int (*v8cfg_readsym)(struct v8_cfg *, const char *, intptr_t *);
} v8_cfg_t;

extern v8_cfg_t v8_cfg_04;
extern v8_cfg_t v8_cfg_06;
extern v8_cfg_t v8_cfg_target;
extern v8_cfg_t *v8_cfgs[];

#endif /* V8CFG_H */
