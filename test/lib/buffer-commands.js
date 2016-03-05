var assert = require('assert');

var getRuntimeVersions = require('./runtime-versions').getRuntimeVersions;

function getFindBufferAddressCmd(opts) {
	assert.ok(typeof opts === 'object');
	assert.ok(typeof opts.propertyName === 'string');
	assert.ok(typeof opts.length === 'number');
	assert.ok(typeof opts.outputFile === 'string');

	var runtimeVersions = getRuntimeVersions();

	// Starting from node v4.0, buffers are actually Uint8Array instances,
	// and they don't have a "length" property
	if (runtimeVersions.node.major < 4) {
		return '::findjsobjects -p ' + opts.propertyName +
		' | ' + '::findjsobjects | ' + '::jsprint -b length ! ' +
		'awk -F: \'$2 == ' + opts.length +
	    '{ print $1 }\'' + '| head -1 > ' + opts.outputFile + '\n';
	} else {
		return '::findjsobjects -p ' + opts.propertyName +
		' | ' + '::findjsobjects | ' + '::jsprint -b ! ' +
	    'awk -F: \'index($2, "length ' + opts.length + '>") > 0 ' +
	    '{ print $1 }\'' + '| head -1 > ' + opts.outputFile + '\n';
	}
}

module.exports = {
	getFindBufferAddressCmd: getFindBufferAddressCmd
};
