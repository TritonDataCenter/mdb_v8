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

function Foo() {}
function LanguageH(chapter) {
	this.OBEY = 'CHAPTER ' + parseInt(chapter, 10);
	// This reference is used for the ::findjsobjects -r test below
	this.foo = new Foo();
}

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

function verifyTest(testName, verifiers, verifierIndex, testOutput) {
	assert.ok(typeof (testName) === 'string',
		'testName must be a string');
	assert.ok(Array.isArray(verifiers), 'verifiers must be an array');
	assert.ok(typeof (verifierIndex) === 'number' &&
		isFinite(verifierIndex),
		'verifierIndex must be a finite number');
	assert.ok(Array.isArray(testOutput),
		'testOutput must be an array');

	var verifier = verifiers[verifierIndex];
	assert.ok(verifier, 'verifier for test ' + testName
		+ ' must exists');
	console.error('verifying ' + testName + ' using '
		+ verifier.name);
	verifier(testOutput);
}

gcore.on('exit', function (code) {
	var verifiers = [];

	if (code != 0) {
		console.error('gcore exited with code ' + code);
		process.exit(code);
	}

	var mdb = spawn('mdb', args, { stdio: 'pipe' });

	mdb.on('exit', function (code2) {
		var verifierIndex = -1;
		var testName;
		var currentTestOutput = null;
		var i;

		var retained = '; core retained as ' + corefile;

		if (code2 != 0) {
			console.error('mdb exited with code ' +
			    util.inspect(code2) + retained);
			process.exit(code2);
		}

		var lines = output.split('\n');

		for (i = 0; i < lines.length; i++) {
			// Found a new test
			if (lines[i].indexOf('test: ') === 0) {
				// If we already were parsing a previous test,
				// run the verifier for this previous test.
				if (currentTestOutput !== null) {
					verifyTest(testName, verifiers,
					    verifierIndex, currentTestOutput);
				}

				// Move to the next verifier function and reset
				// the test output and name for this new test.
				++verifierIndex;
				currentTestOutput = [];
				testName = lines[i];
				continue;
			}

			// Accumulate test output for the current test.
			if (currentTestOutput !== null) {
				currentTestOutput.push(lines[i]);
			}
		}

		// Verify the last test
		verifyTest(testName, verifiers, verifierIndex,
		    currentTestOutput);

		unlinkSync(corefile);
		process.exit(0);
	});

	mdb.stdout.on('data', function (data) {
		output += data;
	});

	mdb.stderr.on('data', function (data) {
		console.log('mdb stderr: ' + data);
	});

	verifiers.push(function verifyFindjsobjectsByConstructor(cmdOutput) {
		var expectedOutputLine = '"OBEY": "' + obj.OBEY + '"';
		assert.ok(cmdOutput.some(function findExpectedLine(line) {
			return (line.indexOf(expectedOutputLine) !== -1);
		}));
	});

	verifiers.push(function verifyFindjsobjectsByProperty(cmdOutput) {
		var expectedOutputLine = '"OBEY": "' + obj.OBEY + '"';
		assert.ok(cmdOutput.some(function findExpectedLine(line) {
			return (line.indexOf(expectedOutputLine) !== -1);
		}));
	});

	verifiers.push(function verifyFindjsobjectsByReference(cmdOutput) {
		var refRegexp =
		    /^[0-9a-fA-F]+ referred to by\s([0-9a-fA-F]+).foo/;
		assert.ok(cmdOutput.length > 0,
			'::findjsobjects -r should output at least one line');

		// Finding just one line that outputs a reference from .foo is
		// enough, since ::findjsobjects -c Foo | ::findjsobjects could
		// output addresses that do not represent the actual instance
		// referenced by the .foo property.
		assert.ok(cmdOutput.some(function findReferenceOutput(line) {
			return (line.match(refRegexp) !== null);
		}), '::findjsobjects -r output should match ' + refRegexp);
	});

	var mod = util.format('::load %s\n', common.dmodpath());
	mdb.stdin.write(mod);

	mdb.stdin.write('!echo test: findjsobjects by constructor\n');
	mdb.stdin.write('::findjsobjects -c LanguageH | ');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');

	mdb.stdin.write('!echo test: findjsobjects by property\n');
	mdb.stdin.write('::findjsobjects -p OBEY | ');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');

	mdb.stdin.write('!echo test: findjsobjects by reference\n');
	mdb.stdin.write('::findjsobjects -c Foo | ::findjsobjects');
	mdb.stdin.write('| ::findjsobjects -r\n');

	mdb.stdin.end();
});
