/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

/*
 * tst.postmortem_basic.js: exercises code paths used to interpret as many
 * kinds of property values as we currently support.  That includes all kinds of
 * strings, numbers, oddball values, arrays, objects, dates, regular
 * expressions, functions, and booleans.
 */

var common = require('./common');
var assert = require('assert');
var os = require('os');
var path = require('path');
var util = require('util');

/*
 * This class sets a variety of properties that together use most of the kinds
 * of values that currently support.  As a result, using "findjsobjects" to
 * locate an instance of this class and then printing it out covers the code
 * responsible for interpreting at least some cases for each of these object
 * types.  (For many values, there are many different cases, and it's hard to
 * ensure that we've covered all of them.)  If the underlying V8 structures
 * change (or there's an mdb_v8 regression) and this test fails, it will likely
 * manifest as an object type that's not recognized any more (in which case
 * findjsobjects will consider it garbage and not list it by default) or the
 * "jsprint" output will be wrong.
 */
function Menagerie() {
	this.a_seqstring = 'my_string';
	this.a_consstring = this.a_seqstring + '_suffix';
	this.a_slicedstring = this.a_seqstring.slice(3, 5);

	this.a_number_smallint = 3;
	this.a_number_zero = 0;
	this.a_number_float = 4.7;
	this.a_number_large = Math.pow(2, 42);
	this.a_number_nan = NaN;

	this.a_null = null;
	this.a_undefined = undefined;

	this.a_array_empty = [];
	this.a_array_withstuff = [ 3, 5, null, this.a_seqstring ];
	this.a_array_withhole = this.a_array_withstuff.slice(0);
	delete (this.a_array_withhole[1]);

	this.a_date_recent = new Date('2016-03-17');
	this.a_date_zero = new Date(0);

	this.a_regexp = /animals walking upright/;
	this.a_func = function myFunc() {};

	this.a_bool_true = true;
	this.a_bool_false = false;
}


/*
 * This class is used to exercise a special case of double-precision
 * floating-point values with double unboxing enabled.  The goal is to have an
 * object with double-precision values beyond property 31.  We want to make sure
 * there are other non-double to make sure our bitwise logic is correct when
 * examining the layout descriptor.  See the comments in mdb_v8.c around unboxed
 * double values for details.
 *
 * Note that we cannot use a loop here because V8 will transform this into an
 * object with dictionary properties.
 */
function Zoo()
{
	this.prop_00 = 'value_00';
	this.prop_01 = 'value_01';
	this.prop_02 = 'value_02';
	this.prop_03 = 'value_03';
	this.prop_04 = 'value_04';
	this.prop_05 = 'value_05';
	this.prop_06 = 'value_06';
	this.prop_07 = 'value_07';
	this.prop_08 = 'value_08';
	this.prop_09 = 'value_09';
	this.prop_10 = 'value_10';
	this.prop_11 = 11.1111111;
	this.prop_12 = 'value_12';
	this.prop_13 = 'value_13';
	this.prop_14 = 'value_14';
	this.prop_15 = 'value_15';
	this.prop_16 = 'value_16';
	this.prop_17 = 17.1717171;
	this.prop_18 = 'value_18';
	this.prop_19 = 'value_19';
	this.prop_20 = 'value_20';
	this.prop_21 = 'value_21';
	this.prop_22 = 'value_22';
	this.prop_23 = 'value_23';
	this.prop_24 = 'value_24';
	this.prop_25 = 'value_25';
	this.prop_26 = 'value_26';
	this.prop_27 = 'value_27';
	this.prop_28 = 'value_28';
	this.prop_29 = 'value_29';
	this.prop_30 = 'value_30';
	this.prop_31 = 'value_31';
	this.prop_32 = 'value_32';
	this.prop_33 = 33.3333333;
	this.prop_34 = 'value_34';
	this.prop_35 = 'value_35';
	this.prop_36 = 'value_36';
}

var obj1 = new Menagerie();
var obj2 = new Zoo();



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

	var mdb = spawn('mdb', args, { stdio: 'pipe', 'env': { 'TZ': 'utc' } });

	mdb.on('exit', function (code2) {
		var retained = '; core retained as ' + corefile;

		if (code2 != 0) {
			console.error('mdb exited with code ' +
			    util.inspect(code2) + retained);
			process.exit(code2);
		}

		console.error(output);
		assert.equal([
		    '{',
		    '    "a_seqstring": "my_string",',
		    '    "a_consstring": "my_string_suffix",',
		    '    "a_slicedstring": "st",',
		    '    "a_number_smallint": 3,',
		    '    "a_number_zero": 0,',
		    '    "a_number_float": 4.700000e+00,',
		    '    "a_number_large": 4398046511104,',
		    '    "a_number_nan": NaN,',
		    '    "a_null": null,',
		    '    "a_undefined": undefined,',
		    '    "a_array_empty": [],',
		    '    "a_array_withstuff": [',
		    '        3,',
		    '        5,',
		    '        null,',
		    '        "my_string",',
		    '    ],',
		    '    "a_array_withhole": [',
		    '        3,',
		    '        hole,',
		    '        null,',
		    '        "my_string",',
		    '    ],',
		    '    "a_date_recent": 1458172800000 ' +
		        '(2016 Mar 17 00:00:00),',
		    '    "a_date_zero": 0 (1970 Jan  1 00:00:00),',
		    '    "a_regexp": JSRegExp: "animals walking upright",',
		    '    "a_bool_true": true,',
		    '    "a_bool_false": false,',
		    '}',
		    '{',
		    '    "prop_00": "value_00",',
		    '    "prop_01": "value_01",',
		    '    "prop_02": "value_02",',
		    '    "prop_03": "value_03",',
		    '    "prop_04": "value_04",',
		    '    "prop_05": "value_05",',
		    '    "prop_06": "value_06",',
		    '    "prop_07": "value_07",',
		    '    "prop_08": "value_08",',
		    '    "prop_09": "value_09",',
		    '    "prop_10": "value_10",',
		    '    "prop_11": 1.111111e+01,',
		    '    "prop_12": "value_12",',
		    '    "prop_13": "value_13",',
		    '    "prop_14": "value_14",',
		    '    "prop_15": "value_15",',
		    '    "prop_16": "value_16",',
		    '    "prop_17": 1.717172e+01,',
		    '    "prop_18": "value_18",',
		    '    "prop_19": "value_19",',
		    '    "prop_20": "value_20",',
		    '    "prop_21": "value_21",',
		    '    "prop_22": "value_22",',
		    '    "prop_23": "value_23",',
		    '    "prop_24": "value_24",',
		    '    "prop_25": "value_25",',
		    '    "prop_26": "value_26",',
		    '    "prop_27": "value_27",',
		    '    "prop_28": "value_28",',
		    '    "prop_29": "value_29",',
		    '    "prop_30": "value_30",',
		    '    "prop_31": "value_31",',
		    '    "prop_32": "value_32",',
		    '    "prop_33": 3.333333e+01,',
		    '    "prop_34": "value_34",',
		    '    "prop_35": "value_35",',
		    '    "prop_36": "value_36",',
		    '}',
		    ''
		].join('\n'), output,
		    'output mismatch' + retained);
		unlinkSync(corefile);
		process.exit(0);
	});

	mdb.stdout.on('data', function (data) {
		output += data;
	});

	mdb.stderr.on('data', function (data) {
		console.log('mdb stderr: ' + data);
	});

	var mod = util.format('::load %s ! cat > /dev/null\n',
	    common.dmodpath());
	mdb.stdin.write(mod);
	mdb.stdin.write('::findjsobjects -c Menagerie | ');
	mdb.stdin.write('::findjsobjects -p a_seqstring | ');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');
	mdb.stdin.write('::findjsobjects -c Zoo | ');
	mdb.stdin.write('::findjsobjects -p prop_01 |');
	mdb.stdin.write('::findjsobjects | ::jsprint\n');
	mdb.stdin.end();
});
