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

doStuff('hello world');

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
var sentinel = 'SENTINEL\n';
var mdb;
var passed;
var to;

process.on('exit', function () {
	assert.ok(passed);
	console.error('test passed');
});

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

	mdb = spawn('mdb', args, { stdio: 'pipe' });

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

	var mod = util.format('::load %s\n', common.dmodpath());
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
	    /^previous context: [a-z0-9]+ \(FixedArray\)$/,
	    /^extension: 0 \(SMI: value = 0\)$/,
	    /^global object: [a-z0-9]+ \(JSGlobalObject\)$/,
	    /^    slot 0: [a-z0-9]+ \(.*\)$/,
	    /^    slot 1: [a-z0-9]+ \(SMI: value = 9\)$/
	    /* jsl:end */
	    /* END JSSTYLED */
	];
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
	mdb.stdin.end();
    },

    function last(chunk) {
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
	passed = true;
    }
];
