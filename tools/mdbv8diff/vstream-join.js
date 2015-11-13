/*
 * XXX This should be extracted into a separate Node module.
 */
/*
 * A JoinStream takes as constructor inputs a group of sorted object-mode
 * streams and joins them.  For example, two input streams might contain:
 *
 *     INPUT STREAM 1		INPUT STREAM 2
 *     [ id1,  7 ]		[ id2, 35 ]
 *     [ id2, 18 ]		[ id3, 12 ]
 *
 * If you join these on the the first column, the output of the stream would
 * look like this:
 *
 *     OUTPUT STREAM
 *     [ id1, [ id1,  7 ], null      ]
 *     [ id2, [ id2, 18 ], [id2, 35] ]
 *     [ id3,        null, [id3, 12] ]
 *
 * The stream identifies mis-sorts and emits an error.
 */

var mod_assertplus = require('assert-plus');
var mod_jsprim = require('jsprim');
var mod_stream = require('stream');
var mod_util = require('util');
var mod_vstream = require('vstream');
var VError = require('verror');

module.exports = JoinStream;

/*
 * Joins N object-mode streams that emit arrays, one element of which is the
 * join key.  Values are assumed to be sorted by the join key.  This stream
 * emits joined objects of the form:
 *
 *     [ join key, value from stream 1, value from stream 2, ... ]
 *
 * Named arguments include:
 *
 *     joinOnIndex	integer, index of each input on which to join
 *
 *     sources		list of streams to read from.  Each stream should emit
 *     			an array that has the joinOnIndex value.
 *
 *     [streamOptions]	options to pass to stream constructor
 */
function JoinStream(args)
{
	var streamoptions;
	var self = this;

	mod_assertplus.object(args, 'args');
	mod_assertplus.number(args.joinOnIndex, 'args.joinOnIndex');
	mod_assertplus.arrayOfObject(args.sources, 'args.sources');
	mod_assertplus.optionalObject(args.streamOptions, 'args.streamOptions');
	mod_assertplus.ok(args.sources.length > 0);

	streamoptions = mod_jsprim.mergeObjects(args.streamOptions,
	    { 'objectMode': true }, { 'highWaterMark': 0 });
	mod_stream.Readable.call(this, streamoptions);
	mod_vstream.wrapStream(this);

	this.js_joinidx = args.joinOnIndex;
	this.js_waiting = false;
	this.js_working = false;
	this.js_sources = args.sources.map(function (s) {
		var src = {
		    'src_stream': s,
		    'src_next': null,
		    'src_next_join': null,
		    'src_last_join': null,
		    'src_done': false
		};

		s.on('readable', function sourceReadable() {
			self.kickIfWaiting();
		});

		s.on('end', function sourceEnded() {
			src.src_done = true;
			self.kickIfWaiting();
		});

		return (src);
	});
}

mod_util.inherits(JoinStream, mod_stream.Readable);

/*
 * Initially, js_waiting is false.  js_waiting becomes true when we run out of
 * data from our upstream sources, but we still have room to emit more data
 * ourselves.  When an upstream source becomes readable or ended AND js_waiting
 * is set, then js_waiting is cleared and we read as much as possible and push
 * until either we block again (and js_waiting is set again) or we run out of
 * output buffer space.
 *
 * The framework only invokes _read() if it previously indicated we were out of
 * buffer space.  In that case, js_waiting should not generally be set, since
 * that means we last came to rest because of buffer space exhaustion rather
 * than waiting for upstream data.  Nevertheless, Node appears to do this
 * sometimes, so we defensively just do nothing in that case.  This is a little
 * scary, since incorrectness here can result in the program exiting zero
 * without having read all the data, but the surrounding program verifies that
 * this doesn't happen.
 */
JoinStream.prototype._read = function ()
{
	if (this.js_working || this.js_waiting) {
		return;
	}

	this.kick();
};

/*
 * kickIfWaiting() is invoked when an upstream source either becomes readable or
 * ended.  If we're currently waiting for upstream data, then we kick ourselves
 * to try to process whatever we've got.  Otherwise, we do nothing.
 */
JoinStream.prototype.kickIfWaiting = function ()
{
	if (!this.js_waiting) {
		return;
	}

	this.js_waiting = false;
	this.kick();
};

/*
 * kick() is invoked when we may have more data to process.  This happens in one
 * of two cases:
 *
 *     o When the framework wants the next datum from the stream.  This happens
 *       when the consumer first calls read() (since there is no data buffered)
 *       and when the consumer subsequently calls read() and the buffer is
 *       empty.  In principle, this could also happen even with some data
 *       already buffered if the framework just decides it wants to buffer more
 *       data in advance of a subsequent read.
 *
 *       The important thing in this case is that we're transitioning from being
 *       at rest because the framework wants no more data (i.e., because of
 *       backpressure) to actively reading data from our upstream sources.
 *
 *     o When we were previously blocked on an upstream source, and an upstream
 *       source now has data (or end-of-stream) available.  In this case, we're
 *       transitioning from being at rest because we were blocked on an upstream
 *       read to actively reading because at least one upstream source is now
 *       ready.
 *
 * This function decides what to do based on the current state.  If there's data
 * (or end-of-stream) available from all sources, it constructs the next output
 * and emits it.  It repeats this until either we get backpressure from the
 * framework or one of the upstream sources has no data available.
 *
 * In principle, it would be possible to invoke this at any time, but for
 * tightness, we're careful to keep track of which case we're in.  We maintain
 * the invariant that when kick() is called, js_waiting must be cleared.
 */
JoinStream.prototype.kick = function ()
{
	var i, src, datum, blocked;
	var joinkey, output, err;
	var keepgoing;

	mod_assertplus.ok(!this.js_waiting);
	mod_assertplus.ok(!this.js_working);

	/*
	 * Iterate the input streams and make sure we have the next row
	 * available from each of them.  If we don't, we'll be blocked and won't
	 * be able to make forward progress until we do.
	 */
	blocked = false;
	for (i = 0; i < this.js_sources.length; i++) {
		src = this.js_sources[i];
		if (src.src_next !== null || src.src_done) {
			continue;
		}

		datum = src.src_stream.read();
		if (datum === null) {
			blocked = blocked || !src.src_done;
			continue;
		}

		mod_assertplus.ok(Array.isArray(datum));
		src.src_next = datum;
		src.src_next_join = datum[this.js_joinidx];

		if (src.src_next_join < src.src_last_join) {
			err = new VError('source %d: out of order', i);
			this.vsWarn(err, 'out of order');
			this.emit('error', err);
			return;
		}
	}

	if (blocked) {
		this.js_waiting = true;
		return;
	}

	/*
	 * By this point, we have the next row from each source.  Figure out the
	 * earliest join key among them.
	 */
	joinkey = null;
	for (i = 0; i < this.js_sources.length; i++) {
		src = this.js_sources[i];
		if (src.src_next_join !== null &&
		    (joinkey === null || joinkey > src.src_next_join)) {
			joinkey = src.src_next_join;
		}
	}

	if (joinkey === null) {
		mod_assertplus.equal(0, this.js_sources.filter(
		    function (s) { return (!s.src_done); }).length);
		this.js_working = true;
		this.push(null);
		this.js_working = false;
		return;
	}

	/*
	 * Now, emit one array value which contains:
	 *
	 * o the join key itself
	 * o the arrays representing the corresponding values from each source,
	 *   or "null" for sources that don't contain the join key
	 */
	output = [ joinkey ];
	for (i = 0; i < this.js_sources.length; i++) {
		src = this.js_sources[i];
		if (joinkey == src.src_next_join) {
			output.push(src.src_next);
			src.src_last_join = src.src_next_join;
			src.src_next_join = null;
			src.src_next = null;
		} else {
			output.push(null);
		}
	}

	this.js_working = true;
	keepgoing = this.push(output);
	this.js_working = false;

	if (keepgoing) {
		this.kick();
	}
};
