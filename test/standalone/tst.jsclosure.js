/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

var assert = require('assert');
var childprocess = require('child_process');
var os = require('os');
var path = require('path');
var util = require('util');

var common = require('./common');

var getRuntimeVersions = require('../lib/runtime-versions').getRuntimeVersions;
var compareV8Versions = require('../lib/runtime-versions').compareV8Versions;

var gcoreSelf = require('./gcore_self');

var RUNTIME_VERSIONS = getRuntimeVersions();
var V8_VERSION = RUNTIME_VERSIONS.V8;
var to, passed, mdb;
var output = '';
var sentinel = 'SENTINEL\n';

/*
 * We're going to look for this function in ::jsfunctions.
 */
function doStuff(str)
{
	var count = 10;
	function myClosure(elt, i) {
		/*
		 * Do something with "str" and "count" so that we can see them
		 * in ::jsclosure.
		 */
		if (str) {
			count--;
		}
	}

	[ 1 ].forEach(myClosure);
	to = setTimeout(myClosure, 30000);
	return (count);
}

var bindObject = {
    '__bind': doStuff.bind({ 'thisObj': true },
	'arg1value', 'arg2value', 'arg3value', 'arg4value')
};

doStuff('hello world');

process.on('exit', function () {
	assert.ok(passed);
	console.error('test passed');
});

gcoreSelf(function onGcore(err, corefile) {
	var unlinkSync = require('fs').unlinkSync;
	var args = [ '-S', corefile ];

	if (err) {
		console.error('failed to gcore self: %s', err.message);
		process.exit(1);
	}

	if (process.env.MDB_LIBRARY_PATH && process.env.MDB_LIBRARY_PATH != '')
		args = args.concat([ '-L', process.env.MDB_LIBRARY_PATH ]);

	mdb = childprocess.spawn('mdb', args, { stdio: 'pipe' });

	mdb.on('exit', function (code2) {
		var retained = '; core retained as ' + corefile;

		if (code2 != 0) {
			console.error('mdb exited with code ' +
			    util.inspect(code2) + retained);
			process.exit(code2);
		}

		unlinkSync(corefile);
		clearTimeout(to);
		/* process exit */
	});

	mdb.stdout.on('data', function (data) {
		output += data;
		while (output.indexOf(sentinel) != -1)
			doWork();
	});

	mdb.stderr.on('data', function (data) {
		console.log('mdb stderr: ' + data);
	});

	/*
	 * The '1000$w' sets the terminal width to a large value to keep MDB
	 * from inserting newlines at the default 80 columns.
	 */
	var mod = util.format('1000$w; ::load %s\n', common.dmodpath());
	doCmd(mod);
});

function doWork()
{
	var i, chunk;

	i = output.indexOf(sentinel);
	chunk = output.substr(0, i);
	output = output.substr(i + sentinel.length);
	console.error(chunk);
	processors[whichproc++](chunk);
}

function doCmd(str)
{
	process.stderr.write('> ' + str);
	mdb.stdin.write(str);
	mdb.stdin.write('!echo ' + sentinel);
}

var processors, whichproc;
var closure;
var bindPtr;

/*
 * This is effectively a pipeline of command response handlers.  Each one kicks
 * off the next command.
 */
whichproc = 0;
processors = [
    function waitForModuleLoad() {
	doCmd('::jsfunctions -n myClosure ! awk \'NR == 2{ print $1 }\'\n');
    },

    function gotClosurePointer(chunk) {
	closure = chunk.trim();
	assert.ok(closure.length > 0,
	    'did not find expected closure "myClosure"');
	doCmd(util.format('%s::jsclosure\n', closure));
    },

    function gotClosureInfo(chunk) {
	var lines = chunk.split(/\n/);
	assert.equal(3, lines.length);
	/* JSSTYLED */
	assert.ok(/^    "str": [a-z0-9]+: "hello world"/.test(lines[0]));
	/* JSSTYLED */
	assert.ok(/^    "count": [a-z0-9]+: 9$/.test(lines[1]));
	assert.equal(lines[2], '');

	doCmd(util.format('%s::v8function\n', closure));
    },

    function gotFunctionInfo(chunk) {
    	var context;

	chunk.split(/\n/).forEach(function (line) {
		var parts = line.split(/\s+/);
		if (parts[0] == 'context:')
			context = parts[1];
	});

	assert.ok(context !== undefined);
	doCmd(util.format('%s::v8context\n', context));
    },

    function gotContext(chunk) {
	var lines = chunk.split(/\n/);
	var required = [
		/* BEGIN JSSTYLED */
		/* jsl:ignore */
		/^closure function: [a-z0-9]+ \(JSFunction\)$/,
		/^previous context: [a-z0-9]+ \(FixedArray\)$/
		/* jsl:end */
		/* END JSSTYLED */
	];

	/*
	 * With V8 4.9.104 and later, the sentinel for context extension is "the
	 * hole" instead of a SMI with value 0. See
	 * https://codereview.chromium.org/1484723003.
	 */
	if (compareV8Versions(V8_VERSION,
		{major: 4, minor: 9, patch: 104}) >= 0) {
		/* BEGIN JSSTYLED */
		/* jsl:ignore */
		required.push(/^extension: [a-z0-9]+ \(Oddball: "hole"\)$/);
		/* jsl:end */
		/* END JSSTYLED */
	} else {
		required.push(/^extension: 0 \(SMI: value = 0\)$/);
	}

	/*
	 * With V8 4.9.88 and later, the reference to the global object (an
	 * instance of JSGlobalObject) was replaced in most cases by a reference
	 * to the native context, which is an instance of a subclass of
	 * FixedArray.
	 * See https://codereview.chromium.org/1480003002.
	 */
	if (compareV8Versions(V8_VERSION,
		{major: 4, minor: 9, patch: 88}) >= 0) {
		required.push(/^native context: [a-z0-9]+ \(FixedArray\)$/);
	} else {
		required.push(/^global object: [a-z0-9]+ \(JSGlobalObject\)$/);
	}

	required = required.concat([
		/* BEGIN JSSTYLED */
		/* jsl:ignore */
		/^    slot 0: [a-z0-9]+ \(.*\)$/,
		/^    slot 1: [a-z0-9]+ \(SMI: value = 9\)$/
		/* jsl:end */
		/* END JSSTYLED */
	]);

	var func;

	lines.forEach(function (line) {
		var i, parts;

		for (i = 0; i < required.length; i++) {
			if (required[i].test(line)) {
				required.splice(i, 1);
				break;
			}
		}

		if (/^closure function:/.test(line)) {
			parts = line.split(/\s+/);
			func = parts[2];
		}
	});

	assert.deepEqual([], required);
	doCmd(util.format('%s::v8function ! ' +
	    'awk \'/shared scope_info:/{ print $3 }\'\n', func));
    },

    function gotScopePtr(chunk) {
    	var ptr = chunk.trim();
	doCmd(util.format('%s::v8scopeinfo\n', ptr));
    },

    function gotScopeInfo(chunk) {
	var lines = chunk.split(/\n/);
	var required = [
	    /* BEGIN JSSTYLED */
	    /* jsl:ignore */
	    /^1 parameter$/,
	    /^    parameter 0: [a-z0-9]+ \("str"\)$/,
	    /^1 stack local variable$/,
	    /^    stack local variable 0: [a-z0-9]+ \("myClosure"\)$/,
	    /^2 context local variables$/,
	    /^    context local variable 0: [a-z0-9]+ \("str"\)$/,
	    /^    context local variable 1: [a-z0-9]+ \("count"\)$/
	    /* jsl:end */
	    /* END JSSTYLED */
	];

	lines.forEach(function (line) {
		var i;
		for (i = 0; i < required.length; i++) {
			if (required[i].test(line)) {
				required.splice(i, 1);
				break;
			}
		}
	});

	assert.deepEqual([], required);

	doCmd(util.format('::findjsobjects -p __bind | ::findjsobjects | ' +
	    '::jsprint -ad1 __bind\n'));
    },

    function gotBindCandidates(chunk) {
	var lines = chunk.split(/\n/);
	bindPtr = null;

	lines.forEach(function (l) {
		var parts = l.split(':');
		if (bindPtr === null && parts.length == 2 &&
		    /function/.test(parts[1])) {
			bindPtr = parts[0];
		}
	});

	assert.notStrictEqual(null, bindPtr,
	    'did not find matching pointer');

	doCmd(util.format('%s::jsfunction\n', bindPtr));
    },

    function gotJsFunction(chunk) {
	var lines, match, bindTarget, values, cmds;
	var i;

	lines = chunk.split(/\n/);
	if (lines.length != 7 ||
	    !/^bound function that wraps: [a-fA-F0-9]+/.test(lines[0]) ||
	    !/^with "this" = [a-fA-F0-9]+ \(.*Object\)/.test(lines[1]) ||
	    !/^      arg0  = [a-fA-F0-9]+ \(.*String\)/.test(lines[2]) ||
	    !/^      arg1  = [a-fA-F0-9]+ \(.*String\)/.test(lines[3]) ||
	    !/^      arg2  = [a-fA-F0-9]+ \(.*String\)/.test(lines[4]) ||
	    !/^      arg3  = [a-fA-F0-9]+ \(.*String\)/.test(lines[5])) {
		throw (new Error('::jsfunction output mismatch'));
	}

	match = lines[0].match(/^bound function that wraps: ([a-fA-F0-9]+)/);
	assert.notStrictEqual(match, null);
	bindTarget = match[1];

	values = {};
	for (i = 1; i < 6; i++) {
		match = lines[i].match(
		    /* JSSTYLED */
		    /\s+"?(arg\d+|this)"?\s+= ([a-fA-F0-9]+)/);
		assert.notStrictEqual(null, match);
		values[match[1]] = match[2];
	}

	console.error(values);
	cmds = [ bindTarget + '::jsfunction' ];
	Object.keys(values).forEach(function (k) {
		cmds.push(values[k] + '::jsprint');
	});

	doCmd(cmds.join(';') + '\n');
	mdb.stdin.end();
    },

    function gotJsFunctionValues(chunk) {
	var lines;

	lines = chunk.split(/\n/);
	assert.ok(/^defined at.*tst\.jsclosure\.js/.test(lines[1]));
	lines.splice(1, 1);

	assert.deepEqual(lines, [
	    'function: doStuff',
	    '{',
	    '    "thisObj": true,',
	    '}',
	    '"arg1value"',
	    '"arg2value"',
	    '"arg3value"',
	    '"arg4value"',
	    ''
	]);

	passed = true;
    }
];
