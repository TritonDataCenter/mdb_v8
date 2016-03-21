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

function LanguageH(chapter) { this.OBEY = 'CHAPTER ' + parseInt(chapter, 10); }
var obj = new LanguageH(1);

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

		var lines = output.split('\n');
		var found = 0, i;
		var expected = '"OBEY": "' + obj.OBEY + '"', nexpected = 2;

		for (i = 0; i < lines.length; i++) {
			if (lines[i].indexOf(expected) != -1)
				found++;
		}

		assert.equal(found, nexpected, 'expected ' + nexpected +
		    ' objects, found ' + found + retained);

		unlinkSync(corefile);
		process.exit(0);
	});

	mdb.stdout.on('data', function (data) {
		output += data;
	});

	mdb.stderr.on('data', function (data) {
		console.log('mdb stderr: ' + data);
	});

	var mod = util.format('::load %s\n', common.dmodpath());
	mdb.stdin.write(mod);
	mdb.stdin.write('::findjsobjects -c LanguageH | ');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');
	mdb.stdin.write('::findjsobjects -p OBEY | ');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');
	mdb.stdin.end();
});
