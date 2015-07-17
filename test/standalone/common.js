/*
 * Copyright (c) 2015, Joyent, Inc. All rights reserved.
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
	var arch = process.argch == 'x64' ? 'amd64' : 'ia32';
	return (mod_path.join(
	    __dirname, '..', '..', 'build', arch, 'mdb_v8.so'));
}
