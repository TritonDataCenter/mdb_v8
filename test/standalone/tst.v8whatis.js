/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * tst.v8whatis.js: exercises the ::v8whatis dcmd.
 *
 * Like most of the standalone tests, this test works by creating a bunch of
 * structures in memory, using gcore(1M) to save a core file of the current
 * process, and then using an MDB session against the core file to pull out
 * those structures and verify that the debugger interprets them correctly.
 */

var assert = require('assert');
var jsprim = require('jsprim');
var util = require('util');
var VError = require('verror');

var common = require('./common');

/*
 * "testObject" is the root object from which we will hang the objects used for
 * our test cases.  Because this test mostly involves walking backwards, the
 * actual "test object" that we find with "::findjsobjects" is the one inside
 * "myArray".  (See the call to finalizeTestObject().)
 */
var testObject = {
    'myArray': [ 1, { 'aTest': 'hello_world' } ]
};

/*
 * Addresses found in the core file for "testObject" itself as well as the
 * arrays hanging off of it.
 */
var addrTestObjectTest;

function main()
{
	var testFuncs;
	var addrFixedArray, addrJsArray, addrJsObject;
	var addrJsObjectPlus12, addrEndMapping;
	var sizeMapping;

	testFuncs = [];

	/*
	 * First, exercise a few simple cases of invalid input.
	 */
	testFuncs.push(function badInputNoAddr(mdb, callback) {
		console.error('test: bad input: no address');
		mdb.runCmd('::v8whatis\n', function (output, erroutput) {
			assert.strictEqual(output, '');
			assert.ok(/must specify address for ::v8whatis/.test(
			    erroutput));
			callback();
		});
	});
	testFuncs.push(function badInputNoDarg(mdb, callback) {
		console.error('test: bad input: no address');
		mdb.runCmd('0::v8whatis -d\n', function (output, erroutput) {
			assert.strictEqual(output, '');
			assert.ok(/option requires an argument/.test(
			    erroutput));
			callback();
		});
	});

	/*
	 * Now, locate our test object for the rest of the tests.
	 */
	testFuncs.push(function findTestObjectAddress(mdb, callback) {
		common.findTestObject(mdb, function (err, addr) {
			addrTestObjectTest = addr;
			callback(err);
		});
	});

	/*
	 * Walk backwards from the test object.   Since it's contained inside an
	 * array, the immediate parent should be a FixedArray.
	 */
	testFuncs.push(function walkToFixedArray(mdb, callback) {
		console.error(
		    'test: walking back from array element to FixedArray');
		walkOneStep(addrTestObjectTest, mdb, function (err, step) {
			if (err) {
				callback(err);
				return;
			}

			assert.equal('string', typeof (step.parentBase));
			assert.strictEqual(step.parentType, 'FixedArray');
			addrFixedArray = step.parentBase;
			callback();
		});
	});

	/*
	 * Walk backwards again from the FixedArray.  This should take us to the
	 * JSArray that contains it.
	 */
	testFuncs.push(function eltDoUgrep(mdb, callback) {
		console.error(
		    'test: walking back from FixedArray to JSArray');
		walkOneStep(addrFixedArray, mdb, function (err, step) {
			if (err) {
				callback(err);
				return;
			}

			assert.equal('string', typeof (step.parentBase));
			assert.strictEqual(step.parentType, 'JSArray');
			addrJsArray = step.parentBase;
			callback();
		});
	});

	/*
	 * Now dump the array contents to prove it contains our test object.
	 */
	testFuncs.push(function checkArrayContents(mdb, callback) {
		var cmdstr;
		cmdstr = addrJsArray + '::jsarray\n';
		mdb.runCmd(cmdstr, function (output) {
			var lines;
			lines = common.splitMdbLines(output, { 'count': 2 });
			assert.equal(lines[1], addrTestObjectTest);
			callback();
		});
	});

	/*
	 * At this point, we've verified a couple of types of references:
	 * JSArrays and FixedArrays.  Let's check object properties by walking
	 * back once more.  Note that this could generate false positives, if V8
	 * has decided to organize this object differently than it usually does
	 * (e.g., with a separate "properties" array), but that doesn't seem
	 * likely here.  If it becomes a problem, we can make this test case
	 * more flexible.
	 */
	testFuncs.push(function propUgrep1(mdb, callback) {
		console.error('test: walking back from JSArray to JSObject');
		walkOneStep(addrJsArray, mdb, function (err, step) {
			if (err) {
				callback(err);
				return;
			}

			assert.equal('string', typeof (step.parentBase));
			assert.strictEqual(step.parentType, 'JSObject');
			addrJsObject = step.parentBase;
			callback();
		});
	});

	/*
	 * Now, print the object contents, following object properties and array
	 * elements to get back to our original test object, proving that these
	 * backwards references we found correspond to legitimate forward
	 * references.
	 */
	testFuncs.push(function checkObjectContents(mdb, callback) {
		var cmdstr;
		cmdstr = addrJsObject + '::jsprint myArray[1].aTest\n';
		mdb.runCmd(cmdstr, function (output) {
			var lines;
			lines = common.splitMdbLines(output, { 'count': 1 });
			assert.ok(/hello_world/.test(lines[0]),
			    'bad output for "::jsprint ..."');
			callback();
		});
	});

	/*
	 * This next two tests take the address that we now know refers to the
	 * base of a JSObject and try several pointer values within the object
	 * that should report the same base address.
	 */
	testFuncs.push(function objCheckBase(mdb, callback) {
		console.error('test: providing base address to ::v8whatis');
		runWhatisVerbose(addrJsObject, mdb, function (step) {
			assert.strictEqual(step.parentType, 'JSObject');
			assert.strictEqual(step.parentBase, addrJsObject);
			assert.strictEqual(step.symbolicOffset,
			    addrJsObject + '-0x0');
			callback();
		});
	});

	testFuncs.push(function objCheckBasePlus12(mdb, callback) {
		console.error('test: providing address+12 to ::v8whatis');
		addrJsObjectPlus12 = jsprim.parseInteger(addrJsObject, {
		    'base': 16,
		    'allowSign': false,
		    'allowImprecise': false,
		    'allowPrefix': false,
		    'allowTrailing': false,
		    'trimWhitespace': false,
		    'leadingZeroIsOctal': false
		});
		if (addrJsObjectPlus12 instanceof Error) {
			callback(new VError(addrJsObjectPlus12,
			    'could not parse address of test object: %s',
			    addrTestObjectTest));
			return;
		}

		addrJsObjectPlus12 = (addrJsObjectPlus12 + 12).toString(16);
		runWhatisVerbose(addrJsObjectPlus12, mdb, function (step) {
			assert.strictEqual(step.parentType, 'JSObject');
			assert.strictEqual(step.parentBase, addrJsObject);
			assert.strictEqual(step.symbolicOffset,
			    addrJsObjectPlus12 + '-0xc');
			callback();
		});
	});

	/*
	 * On the other hand, if we take this last address (12 bytes into the
	 * JSObject) and limit our search to only 4 bytes, we should not find a
	 * parent reference.  This exercises the error case where we didn't
	 * search back far enough.
	 */
	testFuncs.push(function objCheckBasePlus12Limit4(mdb, callback) {
		console.error('test: providing address+12 with limit of 4');
		mdb.runCmd(addrJsObjectPlus12 + '::v8whatis -v -d4\n',
		    function (output, erroutput) {
			assert.ok(/no heap object found in previous 4 bytes/.
			    test(erroutput));
			assert.strictEqual(output, '');
			callback();
		    });
	});

	/*
	 * Now test the case where we find a V8 heap object, but it doesn't seem
	 * to contain our target address.  Since V8 often allocates objects
	 * sequentially without gaps, it can be a little tricky to locate an
	 * address that we can use to test this case.  Here's how we do it: we
	 * take one of the known-good addresses, find the address at the end of
	 * its virtual memory mapping, and use that address.  We'll use the size
	 * of the mapping as an argument to "-d".  By construction, we know
	 * there exists a heap object within the specified range, and we know
	 * that it won't contain our address because it's not even in the same
	 * mapping (if our address is even mapped at all).
	 */
	testFuncs.push(function getEndOfMapping(mdb, callback) {
		console.error('test: end-of-mapping address');
		mdb.runCmd(addrJsObjectPlus12 + '$m\n', function (output) {
			var lines, parts;

			lines = common.splitMdbLines(output, { 'count': 2 });
			parts = lines[1].trim().split(/\s+/);
			assert.ok(parts.length >= 3, 'garbled mapping line');
			addrEndMapping = parts[1];
			sizeMapping = parts[2];
			callback();
		});
	});
	testFuncs.push(function useEndOfMapping(mdb, callback) {
		mdb.runCmd(addrEndMapping + '::v8whatis -v -d ' +
		    sizeMapping + '\n', function (output, erroutput) {
			assert.ok(
			    /heap object found.*does not appear to contain/.
			    test(erroutput));
			assert.strictEqual(output, '');
			callback();
		});
	});

	testFuncs.push(function (mdb, callback) {
		mdb.checkMdbLeaks(callback);
	});

	common.finalizeTestObject(testObject['myArray'][1]);
	common.standaloneTest(testFuncs, function (err) {
		if (err) {
			throw (err);
		}

		console.log('%s passed', process.argv[1]);
	});
}

function walkOneStep(addr, mdb, callback)
{
	var rv = {
	    'addr': addr,		/* address itself */
	    'ugrep': null,		/* where address is referenced */
	    'parentBase': null,		/* base addr of containing V8 object */
	    'parentType': null,		/* type of containing V8 object */
	    'parentRaw': null,		/* raw verbose output */
	    'symbolicOffset': null	/* offset from "addr" to "parentBase" */
	};

	mdb.runCmd(addr + '::ugrep\n', function (uoutput) {
		var lines;

		lines = common.splitMdbLines(uoutput, { 'count': 1 });
		rv.ugrep = lines[0].trim();

		mdb.runCmd(rv.ugrep + '::v8whatis\n',
		    function (woutput, werroutput) {
			assert.strictEqual(werroutput.length, 0);
			lines = common.splitMdbLines(woutput, { 'count': 1 });
			rv.parentBase = lines[0].trim();

			runWhatisVerbose(rv.ugrep, mdb, function (whatis) {
				assert.strictEqual(rv.parentBase,
				    whatis.parentBase);
				rv.parentType = whatis.parentType;
				rv.parentRaw = whatis.parentRaw;
				rv.symbolicOffset = whatis.symbolicOffset;
				callback(null, rv);
			});
		    });
	});
}

function runWhatisVerbose(addr, mdb, callback)
{
	var rv;

	rv = {
	    'parentBase': null,
	    'parentType': null,
	    'parentRaw': null,
	    'symbolicOffset': null
	};

	mdb.runCmd(addr + '::v8whatis -v\n', function (output) {
		var lines, rex, match;

		lines = common.splitMdbLines(output, { 'count': 1 });
		rv.parentRaw = lines[0];
		rex = new RegExp('^([a-z0-9]+) \\(found Map at ' +
		    '[a-z0-9]+ \\((.*)\\) for type ([a-zA-Z]+)\\)$');
		match = rv.parentRaw.match(rex);
		assert.notStrictEqual(match, null, 'garbled verbose output');
		rv.parentBase = match[1];
		rv.symbolicOffset = match[2];
		rv.parentType = match[3];
		callback(rv);
	});
}

main();
