/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

var common = require('./common');
var assert = require('assert');
var os = require('os');
var path = require('path');
var util = require('util');

/*
 * Now we're going to fork ourselves to gcore
 */
var spawn = require('child_process').spawn;
var prefix = '/var/tmp/node';
var corefile = prefix + '.' + process.pid;
var gcore = spawn('gcore', [ '-o', prefix, process.pid + '' ]);
var output = '';
var unlinkSync = require('fs').unlinkSync;
var args = [ '-S', corefile ];

if (process.env.MDB_LIBRARY_PATH && process.env.MDB_LIBRARY_PATH != '')
	args = args.concat([ '-L', process.env.MDB_LIBRARY_PATH ]);

gcore.stderr.on('data', function (data) {
	console.log('gcore: ' + data);
});

gcore.on('exit', function (code) {
	if (code != 0) {
		console.error('gcore exited with code ' + code);
		process.exit(code);
	}

	var mdb = spawn('mdb', args, { stdio: 'pipe' });

	mdb.on('exit', function (code2) {
		var retained = '; core retained as ' + corefile;

		if (code2 != 0) {
			console.error('mdb exited with code ' +
			    util.inspect(code2) + retained);
			process.exit(code2);
		}

		unlinkSync(corefile);
		process.exit(0);
	});

	mdb.stderr.on('data', function (data) {
		assert(false,
		    'loading mdb_v8 should not generate any output on stderr');
	});

	var mod = util.format('::load %s\n', common.dmodpath());
	mdb.stdin.write(mod);
	mdb.stdin.end();
});
