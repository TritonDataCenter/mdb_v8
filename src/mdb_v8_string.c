/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_string.c: interface for working with V8 (JavaScript) string values.
 * This differs from mdb_v8_strbuf.[hc], which is a general-purpose interface
 * within mdb_v8 for working with C strings.
 */

#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"
#include "v8dbg.h"

#include <assert.h>
#include <ctype.h>

#define	JSSTR_DEPTH(f)		((f) & ((1 << JSSTR_FLAGSHIFT) - 1))
#define	JSSTR_BUMPDEPTH(f)	((f) + 1)

struct v8string {
	uintptr_t	v8s_addr;
	size_t		v8s_len;
	uint8_t		v8s_type;
	int		v8s_memflags;
	union		{
		struct {
			uintptr_t	v8s_cons_p1;
			uintptr_t	v8s_cons_p2;
		} v8s_consinfo;

		struct {
			uintptr_t	v8s_sliced_parent;
			uintptr_t	v8s_sliced_offset;
		} v8s_slicedinfo;

		struct {
			uintptr_t	v8s_external_data;
			uintptr_t	v8s_external_nodedata;
		} v8s_external;
	} v8s_info;
};

static int v8string_write_seq(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t, size_t, ssize_t);
static int v8string_write_cons(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);
static int v8string_write_ext(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);
static int v8string_write_sliced(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);

static const char *v8s_truncate_marker = "[...]";
static size_t v8s_truncate_marker_bytes = sizeof ("[...]") - 1;

/*
 * Loads a V8 String object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
v8string_t *
v8string_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t length;
	v8string_t *strp;

	if (read_typebyte(&type, addr) != 0) {
		v8_warn("could not read type for string: %p\n", addr);
		return (NULL);
	}

	if (!V8_TYPE_STRING(type)) {
		v8_warn("not a string: %p\n", addr);
		return (NULL);
	}

	if (!V8_STRREP_SEQ(type) && !V8_STRREP_CONS(type) &&
	    !V8_STRREP_EXT(type) && !V8_STRREP_SLICED(type)) {
		v8_warn("unsupported string representation: %p\n", addr);
		return (NULL);
	}

	if (read_heap_smi(&length, addr, V8_OFF_STRING_LENGTH) != 0) {
		v8_warn("failed to read string length: %p\n", addr);
		return (NULL);
	}

	if ((strp = mdb_zalloc(sizeof (*strp), memflags)) == NULL) {
		return (NULL);
	}

	strp->v8s_addr = addr;
	strp->v8s_len = length;
	strp->v8s_type = type;
	strp->v8s_memflags = memflags;

	if (V8_STRREP_CONS(type)) {
		if (read_heap_ptr(&strp->v8s_info.v8s_consinfo.v8s_cons_p1,
		    addr, V8_OFF_CONSSTRING_FIRST) != 0 ||
		    read_heap_ptr(&strp->v8s_info.v8s_consinfo.v8s_cons_p2,
		    addr, V8_OFF_CONSSTRING_SECOND) != 0) {
			v8_warn("failed to read cons ptrs: %p\n", addr);
			goto fail;
		}
	} else if (V8_STRREP_SLICED(type)) {
		if (read_heap_ptr(
		    &strp->v8s_info.v8s_slicedinfo.v8s_sliced_parent,
		    addr, V8_OFF_SLICEDSTRING_PARENT) != 0 ||
		    read_heap_smi(
		    &strp->v8s_info.v8s_slicedinfo.v8s_sliced_offset,
		    addr, V8_OFF_SLICEDSTRING_OFFSET) != 0) {
			v8_warn("failed to read slice info: %p\n", addr);
			goto fail;
		}
	} else if (V8_STRREP_EXT(type)) {
		if (read_heap_ptr(
		    &strp->v8s_info.v8s_external.v8s_external_data,
		    addr, V8_OFF_EXTERNALSTRING_RESOURCE) != 0 ||
		    read_heap_ptr(
		    &strp->v8s_info.v8s_external.v8s_external_nodedata,
		    strp->v8s_info.v8s_external.v8s_external_data,
		    NODE_OFF_EXTSTR_DATA) != 0) {
			v8_warn("failed to read node string: %p\n", addr);
			goto fail;
		}
	}

	return (strp);

fail:
	v8string_free(strp);
	return (NULL);
}

/*
 * Frees a V8 String object.
 * See the patterns in mdb_v8_dbg.h for interface details.
 */
void
v8string_free(v8string_t *strp)
{
	if (strp == NULL) {
		return;
	}

	maybefree(strp, sizeof (*strp), strp->v8s_memflags);
}

/*
 * Returns the length (in characters) of a V8 string.  This may differ from the
 * number of bytes used to represent it (in the case of two-byte strings), as
 * well as the number of bytes used when writing it to a string buffer (since
 * with some flags, some characters may be escaped).
 */
size_t
v8string_length(v8string_t *strp)
{
	return (strp->v8s_len);
}

/*
 * Write the contents of the JavaScript string "strp" into the string buffer
 * "strb".  "strflags" allows the caller to specify properties related to the
 * string buffer (e.g., that non-ASCII characters should be sanitized, or that
 * the string is being written as JSON and certain characters should be
 * escaped).  "v8flags" allows the caller to specify properties related to the
 * V8 String itself, like whether it should be quoted, or verbose details should
 * be printed.
 */
int
v8string_write(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	int err;
	uint8_t type;
	boolean_t quoted;

	/*
	 * XXX For verbose, need to write obj_jstype() replacement that uses
	 * mdbv8_strbuf_t.
	 * XXX also consider whether the verbose stuff should go to the same
	 * stream
	 * XXX also consider whether we want to restore the indenting behavior.
	 */
	if (JSSTR_DEPTH(v8flags) > JSSTR_MAXDEPTH) {
		mdbv8_strbuf_sprintf(strb, "<maximum depth exceeded>");
		return (-1);
	}

	type = strp->v8s_type;
	if (V8_STRENC_ASCII(type))
		v8flags |= JSSTR_ISASCII;
	else
		v8flags &= ~JSSTR_ISASCII;

	quoted = (v8flags & JSSTR_QUOTED) != 0;
	if (quoted) {
		mdbv8_strbuf_appendc(strb, '"', strflags);
		v8flags &= ~JSSTR_QUOTED;
		mdbv8_strbuf_reserve(strb, 1);
	}

	v8flags = JSSTR_BUMPDEPTH(v8flags) & (~JSSTR_QUOTED);
	if (V8_STRREP_SEQ(type)) {
		err = v8string_write_seq(strp, strb, strflags, v8flags, 0, -1);
	} else if (V8_STRREP_CONS(type)) {
		err = v8string_write_cons(strp, strb, strflags, v8flags);
	} else if (V8_STRREP_EXT(type)) {
		err = v8string_write_ext(strp, strb, strflags, v8flags);
	} else {
		/* Types are checked in v8string_load(). */
		assert(V8_STRREP_SLICED(type));
		err = v8string_write_sliced(strp, strb, strflags, v8flags);
	}

	if (quoted) {
		mdbv8_strbuf_reserve(strb, -1);
		mdbv8_strbuf_appendc(strb, '"', strflags);
	}

	return (err);
}

/*
 * This structure is used to keep track of state while writing out a sequential
 * string.
 */
typedef struct {
	v8string_t	*v8sw_strp;		/* input string */
	v8string_flags_t v8sw_v8flags;		/* string flags */
	uintptr_t	v8sw_charsp;		/* start of raw string data */
	size_t		v8sw_readoff;		/* current posn in "charsp" */
	size_t		v8sw_inbytesperchar;	/* bytes of input per char */
	size_t		v8sw_nreadchars;	/* characters read so far */

	/*
	 * Unlike the arguments to v8string_write_seq(), the slice fields below
	 * have been normalized to the correct values.  (The same-named
	 * arguments to the function may contain default values that require
	 * computation of the actual values, which will be stored here.)
	 */
	size_t		v8sw_sliceoffset;	/* initial offset (nchars) */
	size_t		v8sw_slicelen;		/* write length (nchars) */

	mdbv8_strbuf_t	*v8sw_strb;		/* output buffer */
	mdbv8_strappend_flags_t v8sw_strflags;	/* output flags */

	char		*v8sw_chunk;		/* raw data (input) buffer */
	size_t		v8sw_chunksz;		/* raw data buffer size */
	size_t		v8sw_chunki;		/* position in raw buffer */
	boolean_t	v8sw_chunklast;		/* this is the last chunk */
	boolean_t	v8sw_done;		/* finished the write */
	boolean_t	v8sw_asciicheck;	/* bail out on non-ASCII */
} v8string_write_t;

/*
 * See v8string_write_sizecheck().
 */
typedef enum {
	V8SC_DONTKNOW,
	V8SC_WILLFIT,
	V8SC_WONTFIT,
	V8SC_NODANGER
} v8string_sizecheck_t;

static v8string_sizecheck_t v8string_write_sizecheck(v8string_write_t *);
static int v8string_write_seq_chunk(v8string_write_t *);

/*
 * Implementation of v8string_write() for sequential strings.  "usliceoffset"
 * and "uslicelen" denote the a range of characters in the string to write.
 */
static int
v8string_write_seq(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags,
    size_t usliceoffset, ssize_t uslicelen)
{
	size_t sliceoffset;	/* actual slice offset */
	size_t slicelen;	/* actual slice length */
	size_t nstrchrs;	/* characters in the string */
	size_t inbytesperchar;	/* bytes per character */
	uintptr_t charsp;	/* start of string */
	size_t bufsz;		/* internal buffer size */
	char buf[8192];		/* internal buffer */
	int err;

	v8string_write_t write;	/* write state */

	bufsz = sizeof (buf);
	nstrchrs = v8string_length(strp);

	/*
	 * This function operates on a slice of the string, identified by
	 * initial offset ("sliceoffset") and length ("slicelen").  The special
	 * length value "-1" denotes the range from "sliceoffset" to the end of
	 * the string.  Thus, to denote the entire string, the caller would
	 * specify "sliceoffset" 0 and "slicelen" -1.  We normalize the slice
	 * offset and length here and store these values separately from the
	 * caller-provided values for debugging purposes.
	 */
	if (usliceoffset > nstrchrs) {
		sliceoffset = nstrchrs;
	} else {
		sliceoffset = usliceoffset;
	}

	if (uslicelen == -1) {
		/*
		 * The caller asked for everything from the offset to the end of
		 * the string.  Calculate the actual value here.
		 */
		slicelen = nstrchrs - sliceoffset;
	} else if (uslicelen > nstrchrs - sliceoffset) {
		/*
		 * The caller specified a length that would run past the end of
		 * the string.  Truncate it to the end of the string.
		 */
		slicelen = nstrchrs - sliceoffset;
	} else {
		slicelen = uslicelen;
	}

	assert(sliceoffset <= nstrchrs);
	assert(slicelen <= nstrchrs);
	assert(sliceoffset + slicelen <= nstrchrs);

	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdb_printf("str %p: length %d chars, slice %d length %d "
		    "(actually %d length %d)\n", strp->v8s_addr, nstrchrs,
		    usliceoffset, uslicelen, sliceoffset, slicelen);
	}

	/*
	 * We're going to read through the string's raw data, starting at the
	 * requested offset.  The specific addresses depend on whether we're
	 * looking at an ASCII or "two-byte" string.
	 */
	if ((v8flags & JSSTR_ISASCII) != 0) {
		inbytesperchar = 1;
		charsp = strp->v8s_addr + V8_OFF_SEQASCIISTR_CHARS;
	} else {
		inbytesperchar = 2;
		charsp = strp->v8s_addr + V8_OFF_SEQTWOBYTESTR_CHARS;
	}

	/*
	 * At this point, we've computed everything we need to start reading the
	 * input string into our data buffer in chunks and then write those
	 * chunks out to the output buffer.  We store this information into a
	 * structure so we can implement logical pieces of the algorithm below
	 * in separate functions.
	 */
	write.v8sw_strp = strp;
	write.v8sw_v8flags = v8flags;
	write.v8sw_charsp = charsp;
	write.v8sw_readoff = sliceoffset * inbytesperchar;
	write.v8sw_inbytesperchar = inbytesperchar;
	write.v8sw_nreadchars = 0;
	write.v8sw_sliceoffset = sliceoffset;
	write.v8sw_slicelen = slicelen;
	write.v8sw_strb = strb;
	write.v8sw_strflags = strflags;
	write.v8sw_chunk = &buf[0];
	write.v8sw_chunksz = bufsz;
	write.v8sw_chunki = 0;
	write.v8sw_done = slicelen == 0;
	write.v8sw_asciicheck = B_FALSE;
	err = 0;

	while (!write.v8sw_done) {
		err = v8string_write_seq_chunk(&write);
		if (err != 0) {
			break;
		}
	}

	return (err);
}

static int
v8string_write_seq_chunk(v8string_write_t *writep)
{
	size_t inbytesleft, nbytestoread;
	v8string_sizecheck_t sizecheck;

	inbytesleft = writep->v8sw_inbytesperchar *
	    (writep->v8sw_slicelen - writep->v8sw_nreadchars);
	if (writep->v8sw_chunksz < inbytesleft) {
		nbytestoread = writep->v8sw_chunksz;
		writep->v8sw_chunklast = B_FALSE;
	} else {
		nbytestoread = inbytesleft;
		writep->v8sw_chunklast = B_TRUE;
	}

	if (mdb_vread(writep->v8sw_chunk, nbytestoread,
	    writep->v8sw_charsp + writep->v8sw_readoff) == -1) {
		mdbv8_strbuf_sprintf(writep->v8sw_strb,
		    "<string (failed to read data)>");
		writep->v8sw_done = B_TRUE;
		return (0);
	}

	/*
	 * For external strings, we proactively check that it appears to be
	 * ASCII.  This is just a heuristic to avoid dumping a bunch of garbage,
	 * and we do it as much for historical reasons as anything else.
	 */
	if (writep->v8sw_asciicheck) {
		char firstchar = writep->v8sw_chunk[0];
		if (firstchar != '\0' && !isascii(firstchar)) {
			mdbv8_strbuf_sprintf(writep->v8sw_strb,
			    "<string (contents looks invalid)>");
			writep->v8sw_done = B_TRUE;
			return (0);
		}

		writep->v8sw_asciicheck = B_FALSE;
	}

	writep->v8sw_chunki = 0;
	while (writep->v8sw_nreadchars < writep->v8sw_slicelen &&
	    writep->v8sw_chunki < nbytestoread) {
		sizecheck = v8string_write_sizecheck(writep);
		if (sizecheck == V8SC_WONTFIT) {
			/*
			 * XXX It would be nice if callers could know whether
			 * the string was truncated or not.  Maybe this whole
			 * interface would be cleaner if we first calculated the
			 * number of bytes required to store the result of this
			 * string.  Then we _could_ calculate ahead of time how
			 * many of the string's characters to print.  And if we
			 * had that interface, callers could make sure the
			 * buffer was large enough or know that the string was
			 * truncated.  However, that will require two passes,
			 * each of which requires a bunch of mdb_vreads().
			 * XXX that applies to the similar block in
			 * v8string_write_ext() too.
			 */
			mdbv8_strbuf_appends(writep->v8sw_strb,
			    v8s_truncate_marker, writep->v8sw_strflags);
			writep->v8sw_done = B_TRUE;
			return (0);
		}

		if (sizecheck == V8SC_DONTKNOW) {
			/*
			 * This can't happen at the beginning of a chunk, and if
			 * it did, it would mean we stopped making forward
			 * progress.
			 */
			assert(writep->v8sw_chunki != 0);
			return (0);
		}

		assert(sizecheck == V8SC_WILLFIT || sizecheck == V8SC_NODANGER);
		writep->v8sw_readoff += writep->v8sw_inbytesperchar;

		if ((writep->v8sw_v8flags & JSSTR_ISASCII) != 0) {
			mdbv8_strbuf_appendc(
			    writep->v8sw_strb,
			    writep->v8sw_chunk[writep->v8sw_chunki],
			    writep->v8sw_strflags);
		} else {
			uint16_t chrval;
			assert(writep->v8sw_chunki % 2 == 0);
			chrval = *((uint16_t *)(
			    writep->v8sw_chunk + writep->v8sw_chunki));
			mdbv8_strbuf_appendc(
			    writep->v8sw_strb, chrval,
			    writep->v8sw_strflags);
		}

		writep->v8sw_nreadchars++;
		writep->v8sw_chunki += writep->v8sw_inbytesperchar;
	}

	assert(writep->v8sw_nreadchars <= writep->v8sw_slicelen);
	if (writep->v8sw_nreadchars == writep->v8sw_slicelen) {
		writep->v8sw_done = B_TRUE;
	}

	return (0);
}

static v8string_sizecheck_t
v8string_write_sizecheck(v8string_write_t *writep)
{
	size_t outbytesleft;
	size_t maxoutbytesperchar = 2;
	size_t i, noutbytes;
	uint16_t chrval;
	size_t firstcharbytes, nreadchars;

	/*
	 * If writing this character clearly leaves us with enough output bytes
	 * to write the truncate marker, we don't need to worry about this yet.
	 */
	outbytesleft = mdbv8_strbuf_bytesleft(writep->v8sw_strb);
	if (outbytesleft > maxoutbytesperchar &&
	    outbytesleft - maxoutbytesperchar >= v8s_truncate_marker_bytes) {
		return (V8SC_NODANGER);
	}

	/*
	 * It's going to be close, so we've got to walk through the rest of the
	 * chunk and count the number of bytes to figure out if we're going to
	 * make it.
	 */
	i = writep->v8sw_chunki;
	assert(i < writep->v8sw_chunksz);
	assert(writep->v8sw_nreadchars < writep->v8sw_slicelen);
	noutbytes = 0;
	nreadchars = 0;
	firstcharbytes = 0;
	while (i < writep->v8sw_chunksz &&
	    writep->v8sw_nreadchars + nreadchars < writep->v8sw_slicelen) {
		if ((writep->v8sw_v8flags & JSSTR_ISASCII) != 0) {
			chrval = writep->v8sw_chunk[i];
		} else {
			chrval = *((uint16_t *)(writep->v8sw_chunk + i));
		}

		noutbytes += mdbv8_strbuf_nbytesforchar(
		    chrval, writep->v8sw_strflags);
		if (i == writep->v8sw_chunki) {
			firstcharbytes = noutbytes;
		}
		i += writep->v8sw_inbytesperchar;
		nreadchars++;
	}

	/*
	 * As above, if we'll still have enough bytes to write the marker even
	 * if we write the first character, then return V8SC_NODANGER.
	 */
	if (outbytesleft > v8s_truncate_marker_bytes &&
	    firstcharbytes <= outbytesleft - v8s_truncate_marker_bytes) {
		return (V8SC_NODANGER);
	}

	if (noutbytes > outbytesleft) {
		return (V8SC_WONTFIT);
	}

	if (i == writep->v8sw_chunksz && !writep->v8sw_chunklast) {
		return (V8SC_DONTKNOW);
	}

	return (V8SC_WILLFIT);
}

/*
 * Implementation of v8string_write() for ConsStrings.
 */
static int
v8string_write_cons(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	uintptr_t str1addr, str2addr;
	v8string_t *str1p, *str2p;
	v8string_flags_t flags;
	int rv = 0;

	str1addr = strp->v8s_info.v8s_consinfo.v8s_cons_p1;
	str2addr = strp->v8s_info.v8s_consinfo.v8s_cons_p2;
	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdb_printf("str %p: cons of %p and %p\n",
		    strp->v8s_addr, str1addr, str2addr);
	}

	str1p = v8string_load(
	    strp->v8s_info.v8s_consinfo.v8s_cons_p1, strp->v8s_memflags);
	str2p = v8string_load(
	    strp->v8s_info.v8s_consinfo.v8s_cons_p2, strp->v8s_memflags);

	if (str1p == NULL || str2p == NULL) {
		mdbv8_strbuf_sprintf(strb,
		    "<string (failed to read cons ptrs)>");
	} else {
		flags = JSSTR_BUMPDEPTH(v8flags);
		rv = v8string_write(str1p, strb, strflags, flags);
		if (rv == 0)
			rv = v8string_write(str2p, strb, strflags, flags);
	}

	v8string_free(str1p);
	v8string_free(str2p);
	return (rv);
}

/*
 * Implementation of v8string_write() for SlicedStrings.
 */
static int
v8string_write_sliced(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	uintptr_t parent, offset, length;
	v8string_t *pstrp;
	v8string_flags_t flags;
	int rv = 0;

	parent = strp->v8s_info.v8s_slicedinfo.v8s_sliced_parent;
	offset = strp->v8s_info.v8s_slicedinfo.v8s_sliced_offset;
	length = v8string_length(strp);

	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdb_printf("str %p: slice of %p from %d of length %d\n",
		    strp->v8s_addr, parent, offset, length);
	}

	pstrp = v8string_load(parent, strp->v8s_memflags);
	if (pstrp == NULL) {
		mdbv8_strbuf_sprintf(strb,
		    "<sliced string (failed to load parent)>");
		goto out;
	}

	if (!V8_STRREP_SEQ(pstrp->v8s_type)) {
		mdbv8_strbuf_sprintf(strb,
		    "<sliced string (parent is not a sequential string)>");
		goto out;
	}

	flags = JSSTR_BUMPDEPTH(v8flags);
	rv = v8string_write_seq(pstrp, strb, strflags, flags,
	    offset, length);

out:
	v8string_free(pstrp);
	return (rv);
}

/*
 * Implementation of v8string_write() for ExternalStrings.  This implementation
 * assumes that all external strings are Node strings.
 */
static int
v8string_write_ext(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	char buf[8192];
	size_t ntotal;
	uintptr_t charsp;
	v8string_write_t write;
	int err;

	charsp = strp->v8s_info.v8s_external.v8s_external_nodedata;
	ntotal = v8string_length(strp);

	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdbv8_strbuf_sprintf(strb,
		    "external string: %p "
		    "(assuming node.js string (length %d))\n",
		    strp->v8s_addr, ntotal);
	}

	if ((v8flags & JSSTR_ISASCII) == 0) {
		mdbv8_strbuf_sprintf(strb, "<external two-byte string>");
		return (0);
	}

	bzero(&write, sizeof (write));
	write.v8sw_strp = strp;
	write.v8sw_v8flags = v8flags;
	write.v8sw_charsp = charsp;
	write.v8sw_readoff = 0;
	write.v8sw_inbytesperchar = 1;
	write.v8sw_nreadchars = 0;
	write.v8sw_sliceoffset = 0;
	write.v8sw_slicelen = ntotal;
	write.v8sw_strb = strb;
	write.v8sw_strflags = strflags;
	write.v8sw_chunk = buf;
	write.v8sw_chunksz = sizeof (buf);
	write.v8sw_chunki = 0;
	write.v8sw_chunklast = B_FALSE;
	write.v8sw_done = ntotal == 0;
	write.v8sw_asciicheck = B_TRUE;
	err = 0;

	while (!write.v8sw_done) {
		err = v8string_write_seq_chunk(&write);
		if (err != 0) {
			break;
		}
	}

	return (err);
}
