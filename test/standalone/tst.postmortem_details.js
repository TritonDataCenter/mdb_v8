/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */


var assert = require('assert');
var os = require('os');
var path = require('path');
var util = require('util');

var bufferCommands = require('../lib/buffer-commands');
var common = require('./common');
var getRuntimeVersions = require('../lib/runtime-versions').getRuntimeVersions;

var RUNTIME_VERSIONS = getRuntimeVersions();
var V8_VERSION = RUNTIME_VERSIONS.V8;
var NODE_VERSION = RUNTIME_VERSIONS.node;

/*
 * We're going to look specifically for this function and buffer in the core
 * file.
 */
function myTestFunction()
{
	[ 1 ].forEach(function myIterFunction(t) {});
	return (new Buffer(bufstr));
}

var bufstr, mybuffer, slicedBuffer, slicedBufferLength;

/*
 * Run myTestFunction() three times to create multiple instances of
 * myIterFunction.
 */
bufstr = 'Hello, test suite!';
mybuffer = myTestFunction(bufstr);
mybuffer = myTestFunction(bufstr);
mybuffer = myTestFunction(bufstr);
mybuffer.my_buffer = true;

slicedBufferLength = 5;
slicedBuffer = mybuffer.slice(0, slicedBufferLength);
slicedBuffer.is_sliced_buffer = true;

var OBJECT_KINDS = ['dict', 'inobject', 'numeric', 'props'];

/*
 * Now we're going to fork ourselves to gcore
 */
var spawn = require('child_process').spawn;
var prefix = '/var/tmp/node';
var corefile = prefix + '.' + process.pid;
var tmpfile = '/var/tmp/node-postmortem-func' + '.' + process.pid;
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
		unlinkSync(tmpfile);
		var retained = '; core retained as ' + corefile;

		if (code2 != 0) {
			console.error('mdb exited with code ' +
			    util.inspect(code2) + retained);
			process.exit(code2);
		}

		var lines = output.split(/\n/);
		var current = null, testname = null;
		var whichtest = -1;
		var i;

		for (i = 0; i < lines.length; i++) {
			if (lines[i].indexOf('test: ') === 0) {
				if (current !== null) {
					console.error('verifying ' + testname +
					    ' using ' +
					    verifiers[whichtest].name);
					verifiers[whichtest](current);
				}
				whichtest++;
				current = [];
				testname = lines[i];
				continue;
			}

			if (current !== null)
				current.push(lines[i]);
		}

		console.error('verifying ' + testname + ' using ' +
		    verifiers[whichtest].name);
		verifiers[whichtest](current);

		unlinkSync(corefile);
		process.exit(0);
	});

	mdb.stdout.on('data', function (data) {
		output += data;
	});

	mdb.stderr.on('data', function (data) {
		console.log('mdb stderr: ' + data);
	});

	var verifiers = [];
	var bufferAddress, slicedBufferAddress;
	verifiers.push(function verifyConstructor(testlines) {
		if (NODE_VERSION.major < 4) {
			assert.deepEqual(testlines, [ 'Buffer' ]);
		} else {
			assert.deepEqual(testlines, [ 'Uint8Array' ]);
		}
	});
	verifiers.push(function verifyNodebuffer(testlines) {
		assert.equal(testlines.length, 1);
		assert.ok(/^[0-9a-fA-F]+$/.test(testlines[0]));
		bufferAddress = testlines[0];
	});
	verifiers.push(function verifyBufferContents(testlines) {
		assert.equal(testlines.length, 1);
		assert.equal(testlines[0], '0x' + bufferAddress +
			':      Hello');
	});
	// Buffer instances are implemented as typed arrays in Node
	// versions that ship with V8 >= 4.6. Typed arrays in these versions
	// of V8 do not *directly* store their underlying buffer as an
	// "internal" element, so ::v8internal would not output its address.
	// It would instead output the address of a FixedTypedArrayBase
	// instance. Thus, skip the test.
	if (V8_VERSION.major < 4 || V8_VERSION.major === 4 &&
		V8_VERSION.minor < 6) {
		verifiers.push(function verifyV8internal(testlines) {
			assert.deepEqual(testlines, [ bufferAddress ]);
		});
	}
	verifiers.push(function verifySlicedBufferConstructor(testlines) {
		if (NODE_VERSION.major >= 4) {
			assert.deepEqual(testlines, [ 'Uint8Array' ]);
		} else if (NODE_VERSION.major === 0 &&
			NODE_VERSION.minor === 12) {
			assert.deepEqual(testlines, [ 'NativeBuffer' ]);
		} else {
			assert.deepEqual(testlines, [ 'Buffer' ]);
		}
	});
	verifiers.push(function verifySlicedNodebuffer(testlines) {
		assert.equal(testlines.length, 1);
		assert.ok(/^[0-9a-fA-F]+$/.test(testlines[0]));
		slicedBufferAddress = testlines[0];
	});
	verifiers.push(function verifySlicedBufferContents(testlines) {
		assert.equal(testlines.length, 1);
		assert.equal(testlines[0], '0x' + slicedBufferAddress +
			':      Hello');
	});

	// Buffer instances are implemented as typed arrays in Node
	// versions that ship with V8 >= 4.6. Typed arrays in these versions
	// of V8 do not *directly* store their underlying buffer as an
	// "internal" element, so ::v8internal would not output its address.
	// It would instead output the address of a FixedTypedArrayBase
	// instance. Thus, skip the test.
	if (V8_VERSION.major < 4 || V8_VERSION.major === 4 &&
		V8_VERSION.minor < 6) {
		verifiers.push(function verifySliceBufferV8internal(testlines) {
			assert.deepEqual(testlines, [ slicedBufferAddress ]);
		});
	}

	verifiers.push(function verifyJsfunctionN(testlines) {
		assert.equal(testlines.length, 2);
		var parts = testlines[1].trim().split(/\s+/);
		assert.equal(parts[1], 1);
		assert.equal(parts[2], 'myTestFunction');
		assert.ok(parts[3].indexOf('tst.postmortem_details.js') != -1);
	});
	verifiers.push(function verifyJsfunctionS(testlines) {
		var foundtest = false, founditer = false;
		assert.ok(testlines.length > 1);
		testlines.forEach(function (line) {
			var parts = line.trim().split(/\s+/);
			if (parts[2] == 'myIterFunction') {
				assert.equal(parts[1], '3');
				founditer = true;
			} else if (parts[2] == 'myTestFunction') {
				foundtest = true;
				assert.equal(parts[1], '1');
			}
		});
		assert.ok(foundtest);
		assert.ok(founditer);
	});
	verifiers.push(function verifyJssource(testlines) {
		var content = testlines.join('\n');
		assert.ok(testlines[0].indexOf('tst.postmortem_details.js')
		    != -1);
		assert.ok(content.indexOf('function myTestFunction()\n')
		    != -1);
		assert.ok(content.indexOf('return (new Buffer(bufstr));\n')
		    != -1);
	});
	OBJECT_KINDS.forEach(function (kind) {
		verifiers.push(function verifyFindObjectsKind(testLines) {
			// There should be at least one object for
			// every kind of objects (except for the special cases
			// below)
			var expectedMinimumObjs = 1;

			if (kind === 'props') {
				// On versions > 0.10.x, currently there's no
				// object with the kind 'props'. There should
				// be, but it's a minor issue we're or to live
				// with for now.
				expectedMinimumObjs = 0;
			}

			assert.ok(testLines.length >= expectedMinimumObjs);
		});
	});

	var mod = util.format('::load %s\n', common.dmodpath());
	mdb.stdin.write(mod);
	mdb.stdin.write('!echo test: jsconstructor\n');
	mdb.stdin.write(bufferCommands.getFindBufferAddressCmd({
		propertyName: 'my_buffer',
		length: bufstr.length,
		outputFile: tmpfile
	}));
	mdb.stdin.write('::cat ' + tmpfile + ' | ::jsconstructor\n');
	mdb.stdin.write('!echo test: nodebuffer\n');
	mdb.stdin.write('::cat ' + tmpfile + ' | ::nodebuffer\n');
	mdb.stdin.write('!echo test: nodebuffer contents\n');
	mdb.stdin.write('::cat ' + tmpfile +
	    ' | ::nodebuffer | ::eval "./ccccc"\n');

	// Buffer instances are implemented as typed arrays in Node
	// versions that ship with V8 >= 4.6. Typed arrays in these versions
	// of V8 do not *directly* store their underlying buffer as an
	// "internal" element, so ::v8internal would not output its address.
	// It would instead output the address of a FixedTypedArrayBase
	// instance. Thus, skip the test.
	if (V8_VERSION.major < 4 || V8_VERSION.major === 4 &&
		V8_VERSION.minor < 6) {
		mdb.stdin.write('!echo test: v8internal\n');
		mdb.stdin.write('::cat ' + tmpfile +
		    ' | ::v8print ! awk \'$2 == "elements"{' +
		    'print $4 }\' > ' + tmpfile + '\n');
		mdb.stdin.write('::cat ' + tmpfile + ' | ::v8internal 0\n');
	}
	// Tests that sliced buffers can be inspected properly. See related
	// issue: https://github.com/joyent/mdb_v8/issues/58.
	mdb.stdin.write('!echo test: sliced buffer\n');
	mdb.stdin.write(bufferCommands.getFindBufferAddressCmd({
		propertyName: 'is_sliced_buffer',
		length: slicedBufferLength,
		outputFile: tmpfile
	}));
	mdb.stdin.write('::cat ' + tmpfile + ' | ::jsconstructor\n');
	mdb.stdin.write('!echo test: sliced nodebuffer\n');
	mdb.stdin.write('::cat ' + tmpfile + ' | ::nodebuffer\n');
	mdb.stdin.write('!echo test: sliced nodebuffer contents\n');
	mdb.stdin.write('::cat ' + tmpfile +
	    ' | ::nodebuffer | ::eval "./ccccc"\n');

	// Buffer instances are implemented as typed arrays in Node
	// versions that ship with V8 >= 4.6. Typed arrays in these versions
	// of V8 do not *directly* store their underlying buffer as an
	// "internal" element, so ::v8internal would not output its address.
	// It would instead output the address of a FixedTypedArrayBase
	// instance. Thus, skip the test.
	if (V8_VERSION.major < 4 || V8_VERSION.major === 4 &&
		V8_VERSION.minor < 6) {
		mdb.stdin.write('!echo test: v8internal\n');
		mdb.stdin.write('::cat ' + tmpfile +
		    ' | ::v8print ! awk \'$2 == "elements"{' +
		    'print $4 }\' > ' + tmpfile + '\n');
		mdb.stdin.write('::cat ' + tmpfile + ' | ::v8internal 0\n');
	}

	mdb.stdin.write('!echo test: jsfunctions -n\n');
	mdb.stdin.write('::jsfunctions -n myTestFunction ! cat\n');
	mdb.stdin.write('!echo test: jsfunctions -s\n');
	mdb.stdin.write('::jsfunctions -s tst.postmortem_details.js ! cat\n');
	mdb.stdin.write('!echo test: jssource\n');
	mdb.stdin.write('::jsfunctions -n myTestFunction ! ' +
	    'awk \'NR == 2 {print $1}\' | head -1 > ' + tmpfile + '\n');
	mdb.stdin.write('::cat ' + tmpfile + ' | ::jssource -n 0\n');
	OBJECT_KINDS.forEach(function (kind) {
		mdb.stdin.write(util.format(
		    '!echo test: findjsobjects -k %s\n', kind));
		mdb.stdin.write(util.format('::findjsobjects -k %s\n', kind));
	});
	mdb.stdin.end();
});
