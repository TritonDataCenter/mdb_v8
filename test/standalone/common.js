/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * test/standalone/common.js: common functions for standalone JavaScript tests
 */

var mod_path = require('path');

/* Public interface */
exports.dmodpath = dmodpath;

/*
 * Returns the path to the built dmod, for loading into mdb during testing.
 */
function dmodpath()
{
	var arch = process.arch == 'x64' ? 'amd64' : 'ia32';
	return (mod_path.join(
	    __dirname, '..', '..', 'build', arch, 'mdb_v8.so'));
}
