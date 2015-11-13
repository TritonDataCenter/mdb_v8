/*
 * issue-35.js: mdbv8diff module for the changes for issue 35.
 *
 * This file contains logic that's used when diff'ing mdb_v8 output from two
 * versions, one before the change for issue #35, and one after.  This is
 * basically useful only for regression testing.  Issue #35 rewrote a bunch of
 * internal interfaces in a way that was mostly logically equivalent, but fixed
 * several existing issues.  This module attempts to distinguish between diffs
 * where output changed in an expected way and those where it may have
 * regressed.
 */

exports.ignoreDiff = ignoreDiff;
exports.ignoreAddress = ignoreAddress;

/*
 * Returns true if mdbv8diff should treat "value1" and "value2" (which are known
 * not to be the exact same string already) as equivalent.  (That would be
 * because they changed in a known-good way.)
 */
function ignoreDiff(stream, value1, value2)
{
	var fixed;

	/*
	 * This function has a lot of regular expressions that confuse jsstyle.
	 */
	/* BEGIN JSSTYLED */

	/*
	 * Many of the difference across the fix for issue #35 result from more
	 * consistent (usually more specific) error messages.
	 */
	if (/<could not read type>/.test(value1) ||
	    /<not a string>/.test(value1) ||
	    /<string \(failed to read length\)/.test(value1) ||
	    /<external string \(failed to read ExternalString data\)/.test(
	    value1) ||
	    /<external string \(failed to read ExternalString ascii data\)/.
	    test(value1) ||
	    /<sliced string \(failed to read offset\)/.test(value1) ||
	    /<sliced string \(failed to read length\)/.test(value1) ||
	    /<external two-byte string>/.test(value1) ||
	    /<external string \(failed to read node external pointer /.
	    test(value1)) {
		if (/<string \(failed to read cons ptrs\)>/.test(value2) ||
		    /<string \(failed to load string\)>/.test(value2) ||
		    /<string \(failed to read offset\)>/.test(value2) ||
		    /<string \(failed to read data\)>/.test(value2)) {
			stream.vsCounterBump('diff_more_specific_string_error');
			return (true);
		}
	}

	if (/<external string \(failed to read ExternalString ascii data\)>/.
	    test(value1) &&
	    /"<string \(contents looks invalid\)>"/.test(value2)) {
		stream.vsCounterBump('diff_external_string_error');
		return (true);
	}

	/*
	 * The following two checks build on each other.  Do not change the
	 * order without reading the code carefully.
	 */
	fixed = value1.replace(
	    /<sliced string \(failed to read parent type\)>/,
	    '<sliced string (failed to load parent)>');
	if (fixed != value1 && value2 == fixed) {
		stream.vsCounterBump('diff_sliced_parent');
		return (true);
	}

	fixed = fixed.replace(
	    /<sliced string \(failed to load parent\)>/,
	    '"<sliced string (failed to load parent)>"');
	if (fixed != value1 && value2 == fixed) {
		stream.vsCounterBump('diff_sliced_parent_quoted');
		return (true);
	}

	/*
	 * Similarly, the following two checks build on each other.
	 */
	fixed = value1.replace(
	    /<sliced string \(parent is not a sequential string\)>/,
	    '<sliced string (failed to load parent)>');
	if (fixed != value1 && value2 == fixed) {
		stream.vsCounterBump('diff_sliced_parent_seq');
		return (true);
	}

	fixed = fixed.replace(
	    /<sliced string \(failed to load parent\)>/,
	    '"<sliced string (failed to load parent)>"');
	if (fixed != value1 && value2 == fixed) {
		stream.vsCounterBump('diff_sliced_parent_seq_quote');
		return (true);
	}

	/*
	 * Some of those differences just relate to quoting.
	 */
	fixed = value2.replace(
	    /"<sliced string \(parent is not a sequential string\)>"/,
	    '<sliced string (parent is not a sequential string)>');
	if (fixed != value2 && value1 == fixed) {
		stream.vsCounterBump('diff_proper_quoting');
	    	return (true);
	}

	fixed = value2.replace(
	    /"<external two-byte string>"/,
	    '<external two-byte string>');
	if (fixed != value2 && value1 == fixed) {
		stream.vsCounterBump('diff_proper_quoting');
	    	return (true);
	}

	/* END JSSTYLED */

	/*
	 * Check for changes in string truncation.
	 */
	if (differOnlyInTruncation(stream, value1, value2)) {
		stream.vsCounterBump('diff_truncation');
		return (true);
	}

	return (false);
}

/*
 * Checks whether the two values differ only in the way either the strings
 * themselves or any substrings used therein have been truncated.  When mdb_v8
 * truncates strings, it inserts a marker "[...]".  This has changed across
 * versions:
 *
 *    - Prior to the implementation of issue #35, mdb_v8 would often truncate
 *      strings one character shorter than necessary.  Output for the same
 *      string from old and new versions of mdb_v8 may be off-by-one in the
 *      position of the truncation marker:
 *
 *         actual string: "foo_bartholomew"
 *         old:           "foo_bar[...]"
 *         new:           "foo_bart[...]"
 *
 *    - As a result of the same behavioral difference, if this 1-byte difference
 *      is right at the border of the string's length, this could result in a
 *      string that isn't truncated at all on newer versions:
 *
 *          actual string: "foo_bart"
 *          old:           "foo_bar[...]"
 *          new:           "foo_bart"
 *
 *    - In either of the previous two cases, in some cases the older mdb_v8
 *      would leave out the trailing closing quote ('"') from the output.
 *
 *    - Prior to the implementation of issue #35, mdb_v8 would NOT insert the
 *      "[...]" marker when truncating external ASCII strings.
 *
 * To summarize:
 *
 *    - either string may have any number of truncation markers
 *
 *    - either string may have truncation markers that the other doesn't
 *
 *    - the second string may have an arbitrary extra character before the
 *      truncation marker
 *
 *    - the second string may have an extra '"' character before the truncation
 *      marker
 *
 * Our goal here is to ignore these differences (and _only_ these differences).
 * This implementation is exponential in the number of markers in both strings,
 * but in practice that's extremely small.
 */
function differOnlyInTruncation(stream, value1, value2)
{
	var i, p, q, reason;
	var v1before, v1after;
	var v2before, v2after;

	p = value1.indexOf('[...]');
	q = value2.indexOf('[...]');
	if (p == -1) {
		if (q == -1) {
			/*
			 * Neither string has a truncation.  This is a base
			 * case.  The differ logically only if they actually
			 * differ.
			 */
			return (value1 == value2);
		}

		/*
		 * Truncations in the second string are more constrained than
		 * truncations in the first string -- there are no funny edge
		 * cases with the surrounding characters, and the part of
		 * "value1" that corresponds to the truncation marker must be
		 * the same length as the marker.  (If it were shorter, it
		 * wouldn't have been truncated on newer versions.  If it were
		 * longer, it wouldn't have fit in the same buffer as the
		 * marker.)
		 */
		v2before = value2.substr(0, q);
		v1before = value1.substr(0, q);
		if (v1before != v2before) {
			return (false);
		}

		/*
		 * We always need to make a recursive call for the tails of the
		 * strings in case there are more truncation markers.
		 */
		v2after = value2.substr(q + '[...]'.length);
		v1after = value1.substr(q + '[...]'.length);
		return (differOnlyInTruncation(stream, v1after, v2after));
	}

	v1before = value1.substr(0, p);
	v1after = value1.substr(p + '[...]'.length);

	if (q != -1 && (q == p || q == p + 1 || q == p + 2)) {
		/*
		 * Both strings have a truncation marker, and they're within 2
		 * characters of the same position.  They must represent the
		 * same truncation.
		 *
		 * We basically want to compare what's before and after the
		 * marker, but there are edge cases on either side, so it's
		 * easier to break these checks out separately.
		 *
		 * First, compare the substrings before the "[...]" marker.
		 * Remember, the second substring may have an extra character or
		 * two, in which case we'll ignore it.
		 */
		v2before = value2.substr(0, p);

		if (v1before != v2before) {
			/*
			 * The strings differ in more than just truncation
			 * because the parts before the truncation mark don't
			 * match up (even allowing for the possible extra
			 * characters in the second string).
			 */
			return (false);
		}

		/*
		 * Now, compare the substrings after the "[...]" marker.
		 * Remember, the second substring may have an extra '"'
		 * character.
		 */
		v2after = value2.substr(q + '[...]'.length);
		if (v2after.charAt(0) == '"' && v1after.charAt(0) != '"') {
			v2after = value2.substr(q + '[...]"'.length);
			reason = 'trunc_accurate_truncation_plus_quote';
		} else {
			reason = 'trunc_accurate_truncation';
		}

		if (!differOnlyInTruncation(stream, v1after, v2after)) {
			return (false);
		}

		stream.vsCounterBump(reason);
		return (true);
	}

	/*
	 * The second string either does not have a truncation marker or it
	 * appears to be much later in the string.  In either case, we're going
	 * to ignore it until the recursive call.
	 *
	 * As above, we'll first check everything up to the truncation mark in
	 * the first string.
	 */
	v2before = value2.substr(0, p);
	if (v1before != v2before) {
		/*
		 * The strings don't match up even before we get to the
		 * truncation marker.
		 */
		return (false);
	}

	/*
	 * Now check that the tails of the strings match.  There may be one or
	 * two extra characters, and sadly, we have to check both.
	 */
	for (i = 1; i <= 2; i++) {
		v2after = value2.substr(p + '[...]'.length + i);
		if (v2after.charAt(0) == '"' && v1after.charAt(0) != '"') {
			v2after = v2after.substr(1);
			reason = 'trunc_unneccessary_truncation_plus_quote';
		} else {
			reason = 'trunc_unneccessary_truncation';
		}

		if (differOnlyInTruncation(stream, v1after, v2after)) {
			stream.vsCounterBump(reason);
			return (true);
		}
	}

	return (false);
}

/*
 * This function is even more specific: it assumes a particular core file that's
 * used for regression testing.  That's not ideal, but it _is_ actually useful
 * for these known cases to be documented somewhere.
 */
function ignoreAddress(stream, addr)
{
	var ignore = false;

	switch (addr) {
	case '82741231':
		/*
		 * Subproperty fd268e71 ("_events") is a SlicedString whose
		 * slice is way past the end of its parent string.  The value
		 * should be the empty string, but old versions erroneously
		 * printed some characters here.
		 */
		ignore = true;
		stream.vsCounterBump('ignored_SlicedString');
		break;

	case '827b6d4d':
	case '88df1c99':
		/*
		 * In previous versions of mdb_v8, 827b6d4d ran into a now-fixed
		 * bug in read_heap_dict() that would emit a bogus property.
		 * 827b6d4d refers to 88df1c99, so its output has the same
		 * problem.
		 */
		ignore = true;
		stream.vsCounterBump('ignored_read_heap_dict');
		break;

	case 'b44f2651':
		/*
		 * b44f2651 runs into mdb_v8 issue #49.  It's broken in older
		 * code, and it's broken in newer code, but the failure mode is
		 * slightly different, so it shows up spuriously in the diff.
		 * Ignore it.
		 */
		ignore = true;
		stream.vsCounterBump('ignored_issue-49');
		break;

	default:
		break;
	}

	return (ignore);
}
