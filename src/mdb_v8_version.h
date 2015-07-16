/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2015, Joyent, Inc. All rights reserved.
 */

/*
 * mdb_v8_version.h: declares version number values
 */

#ifndef	_MDBV8_VERSION_H
#define	_MDBV8_VERSION_H

/*
 * These constants are defined in mdb_v8_version.c, which is generated as part
 * of the build.
 */
extern int mdbv8_vers_major;
extern int mdbv8_vers_minor;
extern int mdbv8_vers_micro;

#endif /* _MDBV8_VERSION_H */
