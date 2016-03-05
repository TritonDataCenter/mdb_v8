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

	NODE_VERSIONS = process.versions.node.split('.');

	NODE_MAJOR = Number(NODE_VERSIONS[0]);
	assert.equal(isNaN(NODE_MAJOR), false);

	NODE_MINOR = Number(NODE_VERSIONS[1]);
	assert.equal(isNaN(NODE_MINOR), false);

	NODE_PATCH = Number(NODE_VERSIONS[2]);
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

module.exports = {
	getNodeVersions: getNodeVersions,
	getV8Versions: getV8Versions,
	getRuntimeVersions: getRuntimeVersions
};
