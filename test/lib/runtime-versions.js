var assert = require('assert');

var cachedVersions = {};

function getNodeVersions() {
	var NODE_VERSIONS;
	var NODE_MAJOR;
	var NODE_MINOR;
	var NODE_PATCH;

	if (cachedVersions.node) {
		return cachedVersions.node;
	}

	NODE_VERSIONS =
		process.versions.node.match(/^(\d+)\.(\d+)\.(\d+)(\-\w+)?$/);
	assert.ok(NODE_VERSIONS);

	NODE_MAJOR = Number(NODE_VERSIONS[1]);
	assert.equal(isNaN(NODE_MAJOR), false);

	NODE_MINOR = Number(NODE_VERSIONS[2]);
	assert.equal(isNaN(NODE_MINOR), false);

	NODE_PATCH = Number(NODE_VERSIONS[3]);
	assert.equal(isNaN(NODE_PATCH), false);

	cachedVersions.node = {
		major: NODE_MAJOR,
		minor: NODE_MINOR,
		patch: NODE_PATCH
	};

	return cachedVersions.node;
}

function getV8Versions() {
	var V8_VERSIONS;
	var V8_MAJOR;
	var V8_MINOR;
	var V8_BUILD;
	var V8_PATCH;

	if (cachedVersions.V8) {
		return cachedVersions.V8;
	}

	V8_VERSIONS = process.versions.v8.split('.');

	V8_MAJOR = Number(V8_VERSIONS[0]);
	assert.equal(isNaN(V8_MAJOR), false);

	V8_MINOR = Number(V8_VERSIONS[1]);
	assert.equal(isNaN(V8_MINOR), false);

	V8_BUILD = Number(V8_VERSIONS[2]);
	assert.equal(isNaN(V8_BUILD), false);

	V8_PATCH = Number(V8_VERSIONS[3]);
	assert.equal(isNaN(V8_PATCH), false);

	cachedVersions.V8 = {
		major: V8_MAJOR,
		minor: V8_MINOR,
		build: V8_BUILD,
		patch: V8_PATCH
	};

	return cachedVersions.V8;
}

function getRuntimeVersions() {
	return {
		node: getNodeVersions(),
		V8: getV8Versions()
	};
}

/*
 * Compares two objects representing V8 versions. Returns 1 if versionA >
 * versionB, -1 if versionA < versionB and 0 otherwise.
 */
function compareV8Versions(versionA, versionB) {
	assert(typeof (versionA) === 'object', 'versionA must be an object');
	assert(typeof (versionA.major) === 'number',
		'versionA.major must be a number');
	assert(typeof (versionA.minor) === 'number',
		'versionA.minor must be a number');
	assert(typeof (versionA.patch) === 'number',
		'versionA.patch must be a number');
	assert(versionA.build === undefined || typeof (versionA.build) === 'number',
		'versionA.build must be a number or undefined');

	assert(typeof (versionB) === 'object', 'versionB must be an object');
	assert(typeof (versionB.major) === 'number',
		'versionB.major must be a number');
	assert(typeof (versionB.minor) === 'number',
		'versionB.minor must be a number');
	assert(typeof (versionB.patch) === 'number',
		'versionB.patch must be a number');
	assert(versionB.build === undefined || typeof (versionB.build) === 'number',
		'versionB.build must be a number');

	var versionLevelIndex;
	var versionLevels = ['major', 'minor', 'patch', 'build'];

	for (versionLevelIndex in versionLevels) {
		var versionLevel = versionLevels[versionLevelIndex];
		if (versionA[versionLevel] !== undefined &&
			versionB[versionLevel] !== undefined &&
			versionA[versionLevel] != versionB[versionLevel]) {
			return versionA[versionLevel] > versionB[versionLevel] ? 1 : -1;
		}
	}

	return 0;
}

module.exports = {
	getNodeVersions: getNodeVersions,
	getV8Versions: getV8Versions,
	getRuntimeVersions: getRuntimeVersions,
	compareV8Versions: compareV8Versions
};
