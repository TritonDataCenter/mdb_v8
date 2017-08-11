/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

/*
 * mdb(1M) module for debugging the V8 JavaScript engine.  This implementation
 * makes heavy use of metadata defined in the V8 binary for inspecting in-memory
 * structures.  Canned configurations can be manually loaded for V8 binaries
 * that predate this metadata.  See mdb_v8_cfg.c for details.
 *
 * NOTE: This dmod implementation (including this file and related headers and C
 * files) have existed in the mdb_v8, Node.js, and SmartOS source trees.  The
 * copy in the mdb_v8 repository is the canonical source.  For details, see that
 * repository.
 */

#include "mdb_v8_impl.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <libproc.h>
#include <sys/avl.h>
#include <alloca.h>

#include "v8dbg.h"
#include "v8cfg.h"
#include "mdb_v8_version.h"
#include "mdb_v8_dbg.h"

#define	offsetof(s, m)	((size_t)(&(((s *)0)->m)))

#ifndef MDBV8_VERS_TAG
#error build must define MDBV8_VERS_TAG
#endif
const char *mdbv8_vers_tag = MDBV8_VERS_TAG;

/*
 * The "v8_class" and "v8_field" structures describe the C++ classes used to
 * represent V8 heap objects.
 */
typedef struct v8_class {
	struct v8_class *v8c_next;	/* list linkage */
	struct v8_class *v8c_parent;	/* parent class (inheritance) */
	struct v8_field *v8c_fields;	/* array of class fields */
	size_t		v8c_start;	/* offset of first class field */
	size_t		v8c_end;	/* offset of first subclass field */
	char		v8c_name[64];	/* heap object class name */
} v8_class_t;

typedef struct v8_field {
	struct v8_field	*v8f_next;	/* list linkage */
	ssize_t		v8f_offset;	/* field offset */
	char 		v8f_name[64];	/* field name */
	boolean_t	v8f_isbyte;	/* 1-byte int field */
	boolean_t	v8f_isstr;	/* NUL-terminated string */
} v8_field_t;

/*
 * Similarly, the "v8_enum" structure describes an enum from V8.
 */
typedef struct {
	char 	v8e_name[64];
	uint_t	v8e_value;
} v8_enum_t;

/*
 * During configuration, the dmod updates these globals with the actual set of
 * classes, types, and frame types based on the debug metadata.
 */
static v8_class_t	*v8_classes;

static v8_enum_t	v8_types[128];
static int 		v8_next_type;

static v8_enum_t 	v8_frametypes[16];
static int 		v8_next_frametype;

static int		v8_warnings;
static int		v8_silent;

/*
 * The following constants describe offsets from the frame pointer that are used
 * to inspect each stack frame.  They're initialized from the debug metadata.
 */
static ssize_t	V8_OFF_FP_CONTEXT;
static ssize_t	V8_OFF_FP_MARKER;
static ssize_t	V8_OFF_FP_FUNCTION;
static ssize_t	V8_OFF_FP_ARGS;
static ssize_t	V8_OFF_FP_CONTEXT_OR_FRAME_TYPE;

/*
 * The following constants are used by macros defined in heap-dbg-common.h to
 * examine the types of various V8 heap objects.  In general, the macros should
 * be preferred to using the constants directly.  The values of these constants
 * are initialized from the debug metadata.
 */
intptr_t V8_FirstNonstringType;
intptr_t V8_IsNotStringMask;
intptr_t V8_StringTag;
intptr_t V8_NotStringTag;
intptr_t V8_StringEncodingMask;
intptr_t V8_TwoByteStringTag;
intptr_t V8_AsciiStringTag;
intptr_t V8_OneByteStringTag;
intptr_t V8_StringRepresentationMask;
intptr_t V8_SeqStringTag;
intptr_t V8_ConsStringTag;
intptr_t V8_SlicedStringTag;
intptr_t V8_ExternalStringTag;
intptr_t V8_FailureTag;
intptr_t V8_FailureTagMask;
intptr_t V8_HeapObjectTag;
intptr_t V8_HeapObjectTagMask;
intptr_t V8_SmiTag;
intptr_t V8_SmiTagMask;
intptr_t V8_SmiValueShift;
intptr_t V8_SmiShiftSize;
intptr_t V8_PointerSizeLog2;
intptr_t V8_CompilerHints_BoundFunction;

static intptr_t	V8_ISSHARED_SHIFT;
static intptr_t	V8_DICT_SHIFT;
static intptr_t	V8_DICT_PREFIX_SIZE;
static intptr_t	V8_DICT_ENTRY_SIZE;
static intptr_t	V8_DICT_START_INDEX;
static intptr_t	V8_PROPINDEX_MASK;
static intptr_t	V8_PROPINDEX_SHIFT;
static intptr_t	V8_PROP_IDX_CONTENT;
static intptr_t	V8_PROP_IDX_FIRST;
static intptr_t	V8_PROP_TYPE_FIELD;
static intptr_t	V8_PROP_TYPE_MASK;
static intptr_t	V8_PROP_DESC_KEY;
static intptr_t	V8_PROP_DESC_DETAILS;
static intptr_t	V8_PROP_DESC_VALUE;
static intptr_t	V8_PROP_DESC_SIZE;
static intptr_t	V8_TRANSITIONS_IDX_DESC;

intptr_t V8_TYPE_ACCESSORINFO = -1;
intptr_t V8_TYPE_ACCESSORPAIR = -1;
intptr_t V8_TYPE_EXECUTABLEACCESSORINFO = -1;
intptr_t V8_TYPE_JSOBJECT = -1;
intptr_t V8_TYPE_JSARRAY = -1;
intptr_t V8_TYPE_JSFUNCTION = -1;
intptr_t V8_TYPE_JSBOUNDFUNCTION = -1;
intptr_t V8_TYPE_JSDATE = -1;
intptr_t V8_TYPE_JSREGEXP = -1;
intptr_t V8_TYPE_HEAPNUMBER = -1;
intptr_t V8_TYPE_MUTABLEHEAPNUMBER = -1;
intptr_t V8_TYPE_ODDBALL = -1;
intptr_t V8_TYPE_FIXEDARRAY = -1;
intptr_t V8_TYPE_MAP = -1;
intptr_t V8_TYPE_JSTYPEDARRAY = -1;

static intptr_t V8_ELEMENTS_KIND_SHIFT;
static intptr_t V8_ELEMENTS_KIND_BITCOUNT;
static intptr_t V8_ELEMENTS_FAST_ELEMENTS;
static intptr_t V8_ELEMENTS_FAST_HOLEY_ELEMENTS;
static intptr_t V8_ELEMENTS_DICTIONARY_ELEMENTS;

intptr_t V8_CONTEXT_NCOMMON;
intptr_t V8_CONTEXT_IDX_CLOSURE;
intptr_t V8_CONTEXT_IDX_PREV;
intptr_t V8_CONTEXT_IDX_EXT;
intptr_t V8_CONTEXT_IDX_GLOBAL;
intptr_t V8_CONTEXT_IDX_NATIVE;

intptr_t V8_SCOPEINFO_IDX_NPARAMS;
intptr_t V8_SCOPEINFO_IDX_NSTACKLOCALS;
intptr_t V8_SCOPEINFO_OFFSET_STACK_LOCALS;
intptr_t V8_SCOPEINFO_IDX_NCONTEXTLOCALS;
intptr_t V8_SCOPEINFO_IDX_FIRST_VARS;

/*
 * Although we have this information in v8_classes, the following offsets are
 * defined explicitly because they're used directly in code below.
 */
ssize_t V8_OFF_CODE_INSTRUCTION_SIZE;
ssize_t V8_OFF_CODE_INSTRUCTION_START;
ssize_t V8_OFF_CONSSTRING_FIRST;
ssize_t V8_OFF_CONSSTRING_SECOND;
ssize_t V8_OFF_EXTERNALSTRING_RESOURCE;
ssize_t V8_OFF_FIXEDARRAY_DATA;
ssize_t V8_OFF_FIXEDARRAY_LENGTH;
ssize_t V8_OFF_HEAPNUMBER_VALUE;
ssize_t V8_OFF_HEAPOBJECT_MAP;
ssize_t V8_OFF_JSARRAY_LENGTH;
ssize_t V8_OFF_JSDATE_VALUE;
ssize_t V8_OFF_JSREGEXP_DATA;
ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_ARGUMENTS;
ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_TARGET_FUNCTION;
ssize_t V8_OFF_JSBOUNDFUNCTION_BOUND_THIS;
ssize_t V8_OFF_JSFUNCTION_CONTEXT;
ssize_t V8_OFF_JSFUNCTION_LITERALS_OR_BINDINGS;
ssize_t V8_OFF_JSFUNCTION_SHARED;
ssize_t V8_OFF_JSOBJECT_ELEMENTS;
ssize_t V8_OFF_JSOBJECT_PROPERTIES;
ssize_t V8_OFF_JSRECEIVER_PROPERTIES;
ssize_t V8_OFF_MAP_CONSTRUCTOR;
ssize_t V8_OFF_MAP_CONSTRUCTOR_OR_BACKPOINTER;
ssize_t V8_OFF_MAP_INOBJECT_PROPERTIES;
ssize_t V8_OFF_MAP_INOBJECT_PROPERTIES_OR_CTOR_FUN_INDEX;
ssize_t V8_OFF_MAP_INSTANCE_ATTRIBUTES;
ssize_t V8_OFF_MAP_INSTANCE_DESCRIPTORS;
ssize_t V8_OFF_MAP_INSTANCE_SIZE;
ssize_t V8_OFF_MAP_LAYOUT_DESCRIPTOR;
ssize_t V8_OFF_MAP_BIT_FIELD;
ssize_t V8_OFF_MAP_BIT_FIELD2;
ssize_t V8_OFF_MAP_BIT_FIELD3;
ssize_t V8_OFF_MAP_TRANSITIONS;
ssize_t V8_OFF_ODDBALL_TO_STRING;
ssize_t V8_OFF_SCRIPT_LINE_ENDS;
ssize_t V8_OFF_SCRIPT_NAME;
ssize_t V8_OFF_SCRIPT_SOURCE;
ssize_t V8_OFF_SEQASCIISTR_CHARS;
ssize_t V8_OFF_SEQONEBYTESTR_CHARS;
ssize_t V8_OFF_SEQTWOBYTESTR_CHARS;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_CODE;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_COMPILER_HINTS;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_END_POSITION;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_INFERRED_NAME;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_LENGTH;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_SCRIPT;
ssize_t V8_OFF_SHAREDFUNCTIONINFO_NAME;
ssize_t V8_OFF_SLICEDSTRING_PARENT;
ssize_t V8_OFF_SLICEDSTRING_OFFSET;
ssize_t V8_OFF_STRING_LENGTH;
ssize_t V8_OFF_JSTYPEDARRAY_LENGTH;
ssize_t V8_OFF_JSARRAYBUFFER_BACKINGSTORE;
ssize_t V8_OFF_JSARRAYBUFFERVIEW_BUFFER;
ssize_t V8_OFF_JSARRAYBUFFERVIEW_CONTENT_OFFSET;

#define	V8_CONSTANT_OPTIONAL		1
#define	V8_CONSTANT_HASFALLBACK		2
#define	V8_CONSTANT_REMOVED		4
#define	V8_CONSTANT_ADDED		8

#define	V8_CONSTANT_MAJORSHIFT		4
#define	V8_CONSTANT_MAJORMASK		((1 << 4) - 1)
#define	V8_CONSTANT_MAJOR(flags)	\
	(((flags) >> V8_CONSTANT_MAJORSHIFT) & V8_CONSTANT_MAJORMASK)

#define	V8_CONSTANT_MINORSHIFT		8
#define	V8_CONSTANT_MINORMASK		((1 << 9) - 1)
#define	V8_CONSTANT_MINOR(flags)	\
	(((flags) >> V8_CONSTANT_MINORSHIFT) & V8_CONSTANT_MINORMASK)

#define	V8_CONSTANT_FALLBACK(maj, min) \
	(V8_CONSTANT_OPTIONAL | V8_CONSTANT_HASFALLBACK | \
	((maj) << V8_CONSTANT_MAJORSHIFT) | ((min) << V8_CONSTANT_MINORSHIFT))

#define	V8_CONSTANT_REMOVED_SINCE(maj, min) \
	(V8_CONSTANT_REMOVED | \
	((maj) << V8_CONSTANT_MAJORSHIFT) | ((min) << V8_CONSTANT_MINORSHIFT))

#define	V8_CONSTANT_ADDED_SINCE(maj, min)	\
	(V8_CONSTANT_ADDED | \
	((maj) << V8_CONSTANT_MAJORSHIFT) | ((min) << V8_CONSTANT_MINORSHIFT))

/*
 * Table of constants used directly by this file.
 */
typedef struct v8_constant {
	intptr_t	*v8c_valp;
	const char	*v8c_symbol;
	uint32_t	v8c_flags;
	intptr_t	v8c_fallback;
} v8_constant_t;

static v8_constant_t v8_constants[] = {
	{ &V8_OFF_FP_CONTEXT_OR_FRAME_TYPE,
		"v8dbg_off_fp_context_or_frame_type",
#ifdef _LP64
		V8_CONSTANT_FALLBACK(5, 1), -0x8 },
#else
		V8_CONSTANT_FALLBACK(5, 1), -0x4 },
#endif
	{ &V8_OFF_FP_CONTEXT,		"v8dbg_off_fp_context"		},
	{ &V8_OFF_FP_FUNCTION,		"v8dbg_off_fp_function"		},
	{ &V8_OFF_FP_MARKER,		"v8dbg_off_fp_marker",
	    V8_CONSTANT_REMOVED_SINCE(5, 1)		},
	{ &V8_OFF_FP_ARGS,		"v8dbg_off_fp_args"		},

	{ &V8_FirstNonstringType,	"v8dbg_FirstNonstringType"	},
	{ &V8_IsNotStringMask,		"v8dbg_IsNotStringMask"		},
	{ &V8_StringTag,		"v8dbg_StringTag"		},
	{ &V8_NotStringTag,		"v8dbg_NotStringTag"		},
	{ &V8_StringEncodingMask,	"v8dbg_StringEncodingMask"	},
	{ &V8_TwoByteStringTag,		"v8dbg_TwoByteStringTag"	},
	{ &V8_AsciiStringTag,		"v8dbg_AsciiStringTag",
	    V8_CONSTANT_REMOVED_SINCE(3, 29) },
	{ &V8_OneByteStringTag,		"v8dbg_OneByteStringTag",
	    V8_CONSTANT_ADDED_SINCE(3, 29) },
	{ &V8_StringRepresentationMask,	"v8dbg_StringRepresentationMask" },
	{ &V8_SeqStringTag,		"v8dbg_SeqStringTag"		},
	{ &V8_ConsStringTag,		"v8dbg_ConsStringTag"		},
	{ &V8_SlicedStringTag,		"v8dbg_SlicedStringTag",
	    V8_CONSTANT_FALLBACK(0, 0), 0x3 },
	{ &V8_ExternalStringTag,	"v8dbg_ExternalStringTag"	},
	{ &V8_FailureTag,		"v8dbg_FailureTag",
		V8_CONSTANT_REMOVED_SINCE(3, 28) },
	{ &V8_FailureTagMask,		"v8dbg_FailureTagMask",
		V8_CONSTANT_REMOVED_SINCE(3, 28) },
	{ &V8_HeapObjectTag,		"v8dbg_HeapObjectTag"		},
	{ &V8_HeapObjectTagMask,	"v8dbg_HeapObjectTagMask"	},
	{ &V8_SmiTag,			"v8dbg_SmiTag"			},
	{ &V8_SmiTagMask,		"v8dbg_SmiTagMask"		},
	{ &V8_SmiValueShift,		"v8dbg_SmiValueShift"		},
	{ &V8_SmiShiftSize,		"v8dbg_SmiShiftSize",
#ifdef _LP64
	    V8_CONSTANT_FALLBACK(0, 0), 31 },
#else
	    V8_CONSTANT_FALLBACK(0, 0), 0 },
#endif
	{ &V8_PointerSizeLog2,		"v8dbg_PointerSizeLog2"		},

	{ &V8_DICT_SHIFT,		"v8dbg_bit_field3_dictionary_map_shift",
	    V8_CONSTANT_FALLBACK(3, 13), 24 },
	{ &V8_DICT_PREFIX_SIZE,		"v8dbg_dict_prefix_size",
	    V8_CONSTANT_FALLBACK(3, 11), 2 },
	{ &V8_DICT_ENTRY_SIZE,		"v8dbg_dict_entry_size",
	    V8_CONSTANT_FALLBACK(3, 11), 3 },
	{ &V8_DICT_START_INDEX,		"v8dbg_dict_start_index",
	    V8_CONSTANT_FALLBACK(3, 11), 3 },
	{ &V8_PROPINDEX_MASK,		"v8dbg_prop_index_mask",
	    V8_CONSTANT_FALLBACK(3, 26), 0x3ff00000 },
	{ &V8_PROPINDEX_SHIFT,		"v8dbg_prop_index_shift",
	    V8_CONSTANT_FALLBACK(3, 26), 20 },
	{ &V8_ISSHARED_SHIFT,		"v8dbg_isshared_shift",
	    V8_CONSTANT_FALLBACK(3, 11), 0 },
	{ &V8_PROP_IDX_FIRST,		"v8dbg_prop_idx_first"		},
	{ &V8_PROP_TYPE_FIELD,		"v8dbg_prop_type_field"		},
	{ &V8_PROP_TYPE_MASK,		"v8dbg_prop_type_mask"		},
	{ &V8_PROP_IDX_CONTENT,		"v8dbg_prop_idx_content",
	    V8_CONSTANT_OPTIONAL },
	{ &V8_PROP_DESC_KEY,		"v8dbg_prop_desc_key",
	    V8_CONSTANT_FALLBACK(0, 0), 0 },
	{ &V8_PROP_DESC_DETAILS,	"v8dbg_prop_desc_details",
	    V8_CONSTANT_FALLBACK(0, 0), 1 },
	{ &V8_PROP_DESC_VALUE,		"v8dbg_prop_desc_value",
	    V8_CONSTANT_FALLBACK(0, 0), 2 },
	{ &V8_PROP_DESC_SIZE,		"v8dbg_prop_desc_size",
	    V8_CONSTANT_FALLBACK(0, 0), 3 },
	{ &V8_TRANSITIONS_IDX_DESC,	"v8dbg_transitions_idx_descriptors",
	    V8_CONSTANT_OPTIONAL },

	{ &V8_ELEMENTS_KIND_SHIFT,	"v8dbg_elements_kind_shift",
	    V8_CONSTANT_FALLBACK(0, 0), 3 },
	{ &V8_ELEMENTS_KIND_BITCOUNT,	"v8dbg_elements_kind_bitcount",
	    V8_CONSTANT_FALLBACK(0, 0), 5 },
	{ &V8_ELEMENTS_FAST_ELEMENTS,
	    "v8dbg_elements_fast_elements",
	    V8_CONSTANT_FALLBACK(0, 0), 2 },
	{ &V8_ELEMENTS_FAST_HOLEY_ELEMENTS,
	    "v8dbg_elements_fast_holey_elements",
	    V8_CONSTANT_FALLBACK(0, 0), 3 },
	{ &V8_ELEMENTS_DICTIONARY_ELEMENTS,
	    "v8dbg_elements_dictionary_elements",
	    V8_CONSTANT_FALLBACK(0, 0), 6 },

	{ &V8_CONTEXT_NCOMMON, "v8dbg_context_ncommon",
	    V8_CONSTANT_FALLBACK(0, 0), 4 },
	{ &V8_CONTEXT_IDX_CLOSURE, "v8dbg_context_idx_closure",
	    V8_CONSTANT_FALLBACK(0, 0), 0 },
	{ &V8_CONTEXT_IDX_PREV, "v8dbg_context_idx_prev",
	    V8_CONSTANT_FALLBACK(0, 0), 1 },
	{ &V8_CONTEXT_IDX_EXT, "v8dbg_context_idx_ext",
	    V8_CONSTANT_FALLBACK(0, 0), 2 },
	{ &V8_CONTEXT_IDX_GLOBAL, "v8dbg_context_idx_global",
	    V8_CONSTANT_FALLBACK(0, 0), 3 },
	/*
	 * https://codereview.chromium.org/1480003002, which replaces the link
	 * from a context to the global object with a link to the native
	 * context, landed in V8 4.9.88.
	 */
	{ &V8_CONTEXT_IDX_NATIVE, "v8dbg_context_idx_native",
	    V8_CONSTANT_FALLBACK(4, 9), 3 },

	{ &V8_SCOPEINFO_IDX_NPARAMS, "v8dbg_scopeinfo_idx_nparams",
	    V8_CONSTANT_FALLBACK(3, 7), 1 },
	{ &V8_SCOPEINFO_IDX_NSTACKLOCALS, "v8dbg_scopeinfo_idx_nstacklocals",
	    V8_CONSTANT_FALLBACK(3, 7), 2 },
	{ &V8_SCOPEINFO_OFFSET_STACK_LOCALS,
		"v8dbg_scopeinfo_offset_stack_locals",
	    V8_CONSTANT_FALLBACK(4, 4), 1 },
	{ &V8_SCOPEINFO_IDX_NCONTEXTLOCALS,
	    "v8dbg_scopeinfo_idx_ncontextlocals",
	    V8_CONSTANT_FALLBACK(3, 7), 3 },
	{ &V8_SCOPEINFO_IDX_FIRST_VARS, "v8dbg_scopeinfo_idx_first_vars",
	    V8_CONSTANT_FALLBACK(4, 5), 6 },
};

static int v8_nconstants = sizeof (v8_constants) / sizeof (v8_constants[0]);

typedef struct v8_offset {
	ssize_t		*v8o_valp;
	const char	*v8o_class;
	const char	*v8o_member;
	boolean_t	v8o_optional;
	uint32_t	v8o_flags;
	intptr_t	v8o_fallback;
} v8_offset_t;

static v8_offset_t v8_offsets[] = {
	{ &V8_OFF_CODE_INSTRUCTION_SIZE,
	    "Code", "instruction_size" },
	{ &V8_OFF_CODE_INSTRUCTION_START,
	    "Code", "instruction_start" },
	{ &V8_OFF_CONSSTRING_FIRST,
	    "ConsString", "first" },
	{ &V8_OFF_CONSSTRING_SECOND,
	    "ConsString", "second" },
	{ &V8_OFF_EXTERNALSTRING_RESOURCE,
	    "ExternalString", "resource" },
	{ &V8_OFF_FIXEDARRAY_DATA,
	    "FixedArray", "data" },
	{ &V8_OFF_FIXEDARRAY_LENGTH,
	    "FixedArray", "length" },
	{ &V8_OFF_HEAPNUMBER_VALUE,
	    "HeapNumber", "value" },
	{ &V8_OFF_HEAPOBJECT_MAP,
	    "HeapObject", "map" },
	{ &V8_OFF_JSARRAY_LENGTH,
	    "JSArray", "length" },
	{ &V8_OFF_JSDATE_VALUE,
	    "JSDate", "value", B_TRUE },

	{ &V8_OFF_JSBOUNDFUNCTION_BOUND_ARGUMENTS,
	    "JSBoundFunction", "bound_arguments", B_FALSE,
	    V8_CONSTANT_ADDED_SINCE(4, 9) },
	{ &V8_OFF_JSBOUNDFUNCTION_BOUND_TARGET_FUNCTION,
	    "JSBoundFunction", "bound_target_function", B_FALSE,
	    V8_CONSTANT_ADDED_SINCE(4, 9) },
	{ &V8_OFF_JSBOUNDFUNCTION_BOUND_THIS,
	    "JSBoundFunction", "bound_this", B_FALSE,
	    V8_CONSTANT_ADDED_SINCE(4, 9) },

	{ &V8_OFF_JSFUNCTION_CONTEXT,
	    "JSFunction", "context", B_TRUE },
	{ &V8_OFF_JSFUNCTION_LITERALS_OR_BINDINGS,
	    "JSFunction", "literals_or_bindings", B_FALSE,
	    V8_CONSTANT_REMOVED_SINCE(4, 9) },
	{ &V8_OFF_JSFUNCTION_SHARED,
	    "JSFunction", "shared" },
	{ &V8_OFF_JSOBJECT_ELEMENTS,
	    "JSObject", "elements" },
	/*
	 * https://codereview.chromium.org/1575423002 which landed in V8 4.9.353
	 * moved the properties from JSObject to JSReceiver.
	 */
	{ &V8_OFF_JSOBJECT_PROPERTIES,
	    "JSObject", "properties", B_FALSE,
		V8_CONSTANT_REMOVED_SINCE(4, 9) },
	{ &V8_OFF_JSRECEIVER_PROPERTIES,
	    "JSReceiver", "properties", B_FALSE,
		V8_CONSTANT_ADDED_SINCE(4, 9) },
	{ &V8_OFF_JSREGEXP_DATA,
	    "JSRegExp", "data", B_TRUE },
	{ &V8_OFF_MAP_CONSTRUCTOR,
	    "Map", "constructor",
	    B_FALSE, V8_CONSTANT_REMOVED_SINCE(4, 3)},
	{ &V8_OFF_MAP_CONSTRUCTOR_OR_BACKPOINTER,
	    "Map", "constructor_or_backpointer",
	    B_FALSE, V8_CONSTANT_ADDED_SINCE(4, 3)},
	{ &V8_OFF_MAP_INOBJECT_PROPERTIES,
	    "Map", "inobject_properties",
	    B_FALSE, V8_CONSTANT_REMOVED_SINCE(4, 6) },
#ifdef _LP64
	{ &V8_OFF_MAP_INOBJECT_PROPERTIES_OR_CTOR_FUN_INDEX,
	    "Map", "inobject_properties_or_constructor_function_index",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 8 },
#else
	{ &V8_OFF_MAP_INOBJECT_PROPERTIES_OR_CTOR_FUN_INDEX,
	    "Map", "inobject_properties_or_constructor_function_index",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 4 },
#endif
	{ &V8_OFF_MAP_INSTANCE_ATTRIBUTES,
	    "Map", "instance_attributes" },
	{ &V8_OFF_MAP_INSTANCE_DESCRIPTORS,
	    "Map", "instance_descriptors", B_TRUE },
	{ &V8_OFF_MAP_LAYOUT_DESCRIPTOR,
	    "Map", "layout_descriptor", B_TRUE },
	{ &V8_OFF_MAP_TRANSITIONS,
	    "Map", "transitions", B_TRUE },
	{ &V8_OFF_MAP_INSTANCE_SIZE,
	    "Map", "instance_size" },
	{ &V8_OFF_MAP_BIT_FIELD2,
	    "Map", "bit_field2", B_TRUE },
	{ &V8_OFF_MAP_BIT_FIELD3,
	    "Map", "bit_field3", B_TRUE },
	{ &V8_OFF_ODDBALL_TO_STRING,
	    "Oddball", "to_string" },
	{ &V8_OFF_SCRIPT_LINE_ENDS,
	    "Script", "line_ends" },
	{ &V8_OFF_SCRIPT_NAME,
	    "Script", "name" },
	{ &V8_OFF_SCRIPT_SOURCE,
	    "Script", "source" },
	{ &V8_OFF_SEQASCIISTR_CHARS,
	    "SeqAsciiString", "chars", B_TRUE },
	{ &V8_OFF_SEQONEBYTESTR_CHARS,
	    "SeqOneByteString", "chars", B_TRUE },
	{ &V8_OFF_SEQTWOBYTESTR_CHARS,
	    "SeqTwoByteString", "chars", B_TRUE },
	{ &V8_OFF_SHAREDFUNCTIONINFO_CODE,
	    "SharedFunctionInfo", "code" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_COMPILER_HINTS,
	    "SharedFunctionInfo", "compiler_hints" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_END_POSITION,
	    "SharedFunctionInfo", "end_position" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION,
	    "SharedFunctionInfo", "function_token_position" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_INFERRED_NAME,
	    "SharedFunctionInfo", "inferred_name",
		V8_CONSTANT_REMOVED_SINCE(5, 1) },
#ifdef _LP64
	{ &V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER,
	    "SharedFunctionInfo", "function_identifier",
		V8_CONSTANT_FALLBACK(5, 1), 79},
#else
	{ &V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER,
	    "SharedFunctionInfo", "function_identifier",
		V8_CONSTANT_FALLBACK(5, 1), 39},
#endif
	{ &V8_OFF_SHAREDFUNCTIONINFO_LENGTH,
	    "SharedFunctionInfo", "length" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_NAME,
	    "SharedFunctionInfo", "name" },
	{ &V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO,
	    "SharedFunctionInfo", "scope_info", B_TRUE },
	{ &V8_OFF_SHAREDFUNCTIONINFO_SCRIPT,
	    "SharedFunctionInfo", "script" },
	{ &V8_OFF_SLICEDSTRING_OFFSET,
	    "SlicedString", "offset" },
	{ &V8_OFF_SLICEDSTRING_PARENT,
	    "SlicedString", "parent", B_TRUE },
	{ &V8_OFF_STRING_LENGTH,
	    "String", "length" },
#ifdef _LP64
	{ &V8_OFF_JSTYPEDARRAY_LENGTH,
	    "JSTypedArray", "length",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 5), 55 },
#else
	{ &V8_OFF_JSTYPEDARRAY_LENGTH,
	    "JSTypedArray", "length",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 5), 27 },
#endif
#ifdef _LP64
	{ &V8_OFF_JSARRAYBUFFER_BACKINGSTORE,
	    "JSArrayBuffer", "backing_store",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 23 },
#else
	{ &V8_OFF_JSARRAYBUFFER_BACKINGSTORE,
	    "JSArrayBuffer", "backing_store",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 11 },
#endif
#ifdef _LP64
	{ &V8_OFF_JSARRAYBUFFERVIEW_BUFFER,
	    "JSArrayBufferView", "buffer",
	    B_FALSE, V8_CONSTANT_FALLBACK(3, 20), 23 },
#else
	{ &V8_OFF_JSARRAYBUFFERVIEW_BUFFER,
	    "JSArrayBufferView", "buffer",
	    B_FALSE, V8_CONSTANT_FALLBACK(3, 20), 11 },
#endif
#ifdef _LP64
	{ &V8_OFF_JSARRAYBUFFERVIEW_CONTENT_OFFSET,
	    "JSArrayBufferView", "byte_offset",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 31 },
#else
	{ &V8_OFF_JSARRAYBUFFERVIEW_CONTENT_OFFSET,
	    "JSArrayBufferView", "byte_offset",
	    B_FALSE, V8_CONSTANT_FALLBACK(4, 6), 15 },
#endif
};

static int v8_noffsets = sizeof (v8_offsets) / sizeof (v8_offsets[0]);

static uintptr_t v8_major;
static uintptr_t v8_minor;
static uintptr_t v8_build;
static uintptr_t v8_patch;

static int autoconf_iter_symbol(mdb_symbol_t *, void *);
static v8_class_t *conf_class_findcreate(const char *);
static v8_field_t *conf_field_create(v8_class_t *, const char *, size_t);
static char *conf_next_part(char *, char *);
static int conf_update_parent(const char *);
static int conf_update_field(v8_cfg_t *, const char *);
static int conf_update_enum(v8_cfg_t *, const char *, const char *,
    v8_enum_t *);
static int conf_update_type(v8_cfg_t *, const char *);
static int conf_update_frametype(v8_cfg_t *, const char *);
static void conf_class_compute_offsets(v8_class_t *);

static int heap_offset(const char *, const char *, ssize_t *);
static int jsfunc_name(uintptr_t, char **, size_t *);


/*
 * When iterating properties, it's useful to keep track of what kinds of
 * properties were found.  This is useful for developers to identify objects of
 * different kinds in order to debug them.
 */
typedef enum {
	JPI_NONE = 0,

	/*
	 * Indicates how properties are stored in this object.  There can be
	 * both numeric properties and some of the other kinds.
	 */
	JPI_NUMERIC	= 0x01,	/* numeric-named properties in "elements" */
	JPI_DICT	= 0x02,	/* dictionary properties */
	JPI_INOBJECT	= 0x04,	/* properties stored inside object */
	JPI_PROPS	= 0x08,	/* "properties" array */

	/* error-like cases */
	JPI_SKIPPED   = 0x10,	/* some properties were skipped */
	JPI_BADLAYOUT = 0x20,	/* we didn't recognize the layout at all */
	JPI_BADPROPS  = 0x40,	/* property values don't look valid */
	JPI_MAYBE_GARBAGE = (JPI_SKIPPED | JPI_BADLAYOUT | JPI_BADPROPS),
	JPI_UNDEFPROPNAME = 0x80,	/* found "undefined" for prop name */

	/* fallback cases */
	JPI_HASTRANSITIONS	= 0x100, /* found a transitions array */
	JPI_HASCONTENT		= 0x200, /* found a separate content array */
} jspropinfo_t;

typedef struct jsobj_print {
	char **jsop_bufp;
	size_t *jsop_lenp;
	int jsop_indent;
	uint64_t jsop_depth;
	boolean_t jsop_printaddr;
	uintptr_t jsop_baseaddr;
	int jsop_nprops;
	const char *jsop_member;
	size_t jsop_maxstrlen;
	boolean_t jsop_found;
	boolean_t jsop_descended;
	jspropinfo_t jsop_propinfo;
} jsobj_print_t;

static int jsobj_print_number(uintptr_t, jsobj_print_t *);
static int jsobj_print_oddball(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsobject(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsarray(uintptr_t, jsobj_print_t *);
static int jsobj_print_jstyped_array(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsfunction(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsboundfunction(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsdate(uintptr_t, jsobj_print_t *);
static int jsobj_print_jsregexp(uintptr_t, jsobj_print_t *);


/*
 * Layout descriptors: see jsobj_layout_load() for details.
 */

typedef enum {
	JL_F_HASLAYOUT	= 0x1,	/* map has any layout descriptor */
	JL_F_ALLTAGGED	= 0x2,	/* no properties are untagged */
	JL_F_ARRAY	= 0x4,	/* layout descriptors larger than SMI */
} jsobj_layout_flags_t;

/*
 * A layout descriptor is a bit vector with one bit per object property, grouped
 * into 32-bit "words" (even on LP64).  It's more straightforward to support a
 * small number of these words, and even with only 8 words supported, we can
 * access objects having 256 properties.  With that many properties, it's likely
 * V8 will have converted the object to dictionary-mode by that point, in which
 * case this appears not to be relevant.
 */
#define	JL_MAXBITVECS 8

typedef struct {
	jsobj_layout_flags_t	jl_flags;
	uintptr_t		jl_descriptor;
	size_t			jl_length;
	uint32_t		jl_bitvecs[JL_MAXBITVECS];
} jsobj_layout_t;

static int jsobj_layout_load(jsobj_layout_t *, uintptr_t);
static boolean_t jsobj_layout_untagged(jsobj_layout_t *, uintptr_t);

/*
 * Returns 1 if the V8 version v8_major.v8.minor is strictly older than
 * the V8 version represented by "flags".
 * Returns 0 otherwise.
 */
static int
v8_version_older(uintptr_t v8_major, uintptr_t v8_minor, uint32_t flags) {
	return (v8_major < V8_CONSTANT_MAJOR(flags) ||
	    (v8_major == V8_CONSTANT_MAJOR(flags) &&
	    v8_minor < V8_CONSTANT_MINOR(flags)));
}

/*
 * Returns 1 if the V8 version v8_major.v8.minor is newer or equal than
 * the V8 version represented by "flags".
 * Returns 0 otherwise.
 */
static int
v8_version_at_least(uintptr_t v8_major, uintptr_t v8_minor, uint32_t flags) {
	return (v8_major > V8_CONSTANT_MAJOR(flags) ||
	    (v8_major == V8_CONSTANT_MAJOR(flags) &&
	    v8_minor >= V8_CONSTANT_MINOR(flags)));
}

/*
 * Returns true if the version of V8 inside this process or core file is older
 * than the specified version.
 */
static boolean_t
v8_version_current_older(uintptr_t major, uintptr_t minor, uintptr_t build,
    uintptr_t patch)
{
	return (v8_major < major || (v8_major == major &&
	    (v8_minor < minor || (v8_minor == minor &&
	    (v8_build < build || (v8_build == build &&
	    v8_patch < patch))))));
}

/*
 * Invoked when this dmod is initially loaded to load the set of classes, enums,
 * and other constants from the metadata in the target binary.
 */
static int
autoconfigure(v8_cfg_t *cfgp)
{
	v8_class_t *clp;
	v8_enum_t *ep;
	struct v8_constant *cnp;
	int ii;
	int failed = 0;
	int constant_optional, constant_removed, constant_added;
	int offset_optional, offset_removed, offset_added;
	int offset_fallback;
	int v8_older, v8_at_least;

	assert(v8_classes == NULL);

	/*
	 * Iterate all global symbols looking for metadata.
	 */
	if (cfgp->v8cfg_iter(cfgp, autoconf_iter_symbol, cfgp) != 0) {
		mdb_warn("failed to autoconfigure V8 support\n");
		return (-1);
	}

	/*
	 * By now we've configured all of the classes so we can update the
	 * "start" and "end" fields in each class with information from its
	 * parent class.
	 */
	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next) {
		if (clp->v8c_end != (size_t)-1)
			continue;

		conf_class_compute_offsets(clp);
	};

	/*
	 * Load various constants used directly in the module.
	 */
	for (ii = 0; ii < v8_nconstants; ii++) {
		cnp = &v8_constants[ii];

		if (cfgp->v8cfg_readsym(cfgp,
		    cnp->v8c_symbol, cnp->v8c_valp) != -1) {
			continue;
		}

		constant_optional = cnp->v8c_flags & V8_CONSTANT_OPTIONAL;
		constant_removed = cnp->v8c_flags & V8_CONSTANT_REMOVED;
		constant_added = cnp->v8c_flags & V8_CONSTANT_ADDED;
		v8_older = v8_version_older(v8_major, v8_minor, cnp->v8c_flags);
		v8_at_least = v8_version_at_least(v8_major,
		    v8_minor, cnp->v8c_flags);

		if (!constant_optional &&
		    (!constant_removed || v8_older) &&
		    (!constant_added || v8_at_least)) {
			mdb_warn("failed to read \"%s\"", cnp->v8c_symbol);
			failed++;
			continue;
		}

		if (!(cnp->v8c_flags & V8_CONSTANT_HASFALLBACK) ||
		    v8_major < V8_CONSTANT_MAJOR(cnp->v8c_flags) ||
		    (v8_major == V8_CONSTANT_MAJOR(cnp->v8c_flags) &&
		    v8_minor < V8_CONSTANT_MINOR(cnp->v8c_flags))) {
			*cnp->v8c_valp = -1;
			continue;
		}

		/*
		 * We have a fallback -- and we know that the version satisfies
		 * the fallback's version constraints; use the fallback value.
		 */
		*cnp->v8c_valp = cnp->v8c_fallback;
	}

	/*
	 * Load type values for well-known classes that we use a lot.
	 */
	for (ep = v8_types; ep->v8e_name[0] != '\0'; ep++) {
		if (strcmp(ep->v8e_name, "JSObject") == 0)
			V8_TYPE_JSOBJECT = ep->v8e_value;

		if (strcmp(ep->v8e_name, "JSArray") == 0)
			V8_TYPE_JSARRAY = ep->v8e_value;

		/*
		 * JSBoundFunction is only used in relatively modern versions of
		 * V8 (those used in Node v6 and later).
		 */
		if (strcmp(ep->v8e_name, "JSBoundFunction") == 0)
			V8_TYPE_JSBOUNDFUNCTION = ep->v8e_value;

		if (strcmp(ep->v8e_name, "JSFunction") == 0)
			V8_TYPE_JSFUNCTION = ep->v8e_value;

		if (strcmp(ep->v8e_name, "FixedArray") == 0)
			V8_TYPE_FIXEDARRAY = ep->v8e_value;

		if (strcmp(ep->v8e_name, "AccessorInfo") == 0)
			V8_TYPE_ACCESSORINFO = ep->v8e_value;

		if (strcmp(ep->v8e_name, "AccessorPair") == 0)
			V8_TYPE_ACCESSORPAIR = ep->v8e_value;

		if (strcmp(ep->v8e_name, "ExecutableAccessorInfo") == 0)
			V8_TYPE_EXECUTABLEACCESSORINFO = ep->v8e_value;

		if (strcmp(ep->v8e_name, "HeapNumber") == 0)
			V8_TYPE_HEAPNUMBER = ep->v8e_value;

		if (strcmp(ep->v8e_name, "MutableHeapNumber") == 0)
			V8_TYPE_MUTABLEHEAPNUMBER = ep->v8e_value;

		if (strcmp(ep->v8e_name, "JSDate") == 0)
			V8_TYPE_JSDATE = ep->v8e_value;

		if (strcmp(ep->v8e_name, "JSRegExp") == 0)
			V8_TYPE_JSREGEXP = ep->v8e_value;

		if (strcmp(ep->v8e_name, "Oddball") == 0)
			V8_TYPE_ODDBALL = ep->v8e_value;

		if (strcmp(ep->v8e_name, "Map") == 0)
			V8_TYPE_MAP = ep->v8e_value;

		if (strcmp(ep->v8e_name, "JSTypedArray") == 0)
			V8_TYPE_JSTYPEDARRAY = ep->v8e_value;
	}

	if (V8_TYPE_JSOBJECT == -1) {
		mdb_warn("couldn't find JSObject type\n");
		failed++;
	}

	if (V8_TYPE_JSARRAY == -1) {
		mdb_warn("couldn't find JSArray type\n");
		failed++;
	}

	if (V8_TYPE_JSFUNCTION == -1) {
		mdb_warn("couldn't find JSFunction type\n");
		failed++;
	}

	if (V8_TYPE_FIXEDARRAY == -1) {
		mdb_warn("couldn't find FixedArray type\n");
		failed++;
	}

	/*
	 * It's non-fatal if we can't find HeapNumber, JSDate, JSRegExp, or
	 * Oddball because they're only used for heuristics.  It's not even a
	 * warning if we don't find the Accessor-related fields for the same
	 * reason, and they change too much to even bother warning.
	 */
	if (V8_TYPE_HEAPNUMBER == -1)
		mdb_warn("couldn't find HeapNumber type\n");

	if (V8_TYPE_JSDATE == -1)
		mdb_warn("couldn't find JSDate type\n");

	if (V8_TYPE_JSREGEXP == -1)
		mdb_warn("couldn't find JSRegExp type\n");

	if (V8_TYPE_ODDBALL == -1)
		mdb_warn("couldn't find Oddball type\n");

	/*
	 * The MutableHeapNumber type was added in the V8 delivered with Node
	 * v4.  It functions just like HeapNumber.  However, because there is no
	 * separate MutableHeapNumber class, the postmortem metadata generation
	 * script does not emit a type for MutableHeapNumber, so we have to
	 * infer it.  In the version delivered with Node 4, at least the
	 * MutableHeapNumber type is assigned directly after HeapNumber's.
	 * By V8 version 4.6.85.23 (and possibly earlier), this type value
	 * changed to appear directly after the type for CODE.  Clearly, we'll
	 * need a better way to reliably obtain this value.
	 */
	if (V8_TYPE_HEAPNUMBER != -1 && V8_TYPE_MUTABLEHEAPNUMBER == -1) {
		if (v8_version_current_older(4, 6, 85, 23)) {
			V8_TYPE_MUTABLEHEAPNUMBER = V8_TYPE_HEAPNUMBER + 1;
		} else {
			for (ep = v8_types; ep->v8e_name[0] != '\0'; ep++) {
				if (strcmp(ep->v8e_name, "Code") == 0) {
					break;
				}
			}

			if (ep->v8e_name[0] != '\0') {
				V8_TYPE_MUTABLEHEAPNUMBER = ep->v8e_value + 1;
			} else {
				mdb_warn("couldn't find type for "
				    "MutableHeapNumber\n");
			}
		}
	}

	/*
	 * Finally, load various class offsets.
	 */
	for (ii = 0; ii < v8_noffsets; ii++) {
		struct v8_offset *offp = &v8_offsets[ii];
		const char *klass = offp->v8o_class;

again:
		if (heap_offset(klass, offp->v8o_member, offp->v8o_valp) == 0)
			continue;

		if (strcmp(klass, "FixedArray") == 0) {
			/*
			 * The V8 included in node v0.6 uses a FixedArrayBase
			 * class to contain the "length" field, while the one
			 * in v0.4 has no such base class and stores the field
			 * directly in FixedArray; if we failed to derive
			 * the offset from FixedArray, try FixedArrayBase.
			 */
			klass = "FixedArrayBase";
			goto again;
		}

		if (offp->v8o_optional) {
			*offp->v8o_valp = -1;
			continue;
		}

		offset_optional = offp->v8o_flags & V8_CONSTANT_OPTIONAL;
		offset_removed = offp->v8o_flags & V8_CONSTANT_REMOVED;
		offset_added = offp->v8o_flags & V8_CONSTANT_ADDED;
		v8_older = v8_version_older(v8_major,
		    v8_minor, offp->v8o_flags);
		v8_at_least = v8_version_at_least(v8_major,
		    v8_minor, offp->v8o_flags);

		if (!offset_optional &&
		    (!offset_removed || v8_older) &&
		    (!offset_added || v8_at_least)) {
			mdb_warn("couldn't find class \"%s\", field \"%s\"\n",
			    offp->v8o_class, offp->v8o_member);
			failed++;
		}

		offset_fallback = offp->v8o_flags & V8_CONSTANT_HASFALLBACK;
		if (!offset_fallback ||
		    v8_major < V8_CONSTANT_MAJOR(offp->v8o_flags) ||
		    (v8_major == V8_CONSTANT_MAJOR(offp->v8o_flags) &&
		    v8_minor < V8_CONSTANT_MINOR(offp->v8o_flags))) {
			*offp->v8o_valp = -1;
			continue;
		}

		/*
		 * We have a fallback -- and we know that the version satisfies
		 * the fallback's version constraints; use the fallback value.
		 */
		*offp->v8o_valp = offp->v8o_fallback;
	}

	if (!((V8_OFF_SEQASCIISTR_CHARS != -1) ^
	    (V8_OFF_SEQONEBYTESTR_CHARS != -1))) {
		mdb_warn("expected exactly one of SeqAsciiString and "
		    "SeqOneByteString to be defined\n");
		failed++;
	}

	if (V8_OFF_SEQONEBYTESTR_CHARS != -1)
		V8_OFF_SEQASCIISTR_CHARS = V8_OFF_SEQONEBYTESTR_CHARS;

	if (V8_OFF_SEQTWOBYTESTR_CHARS == -1)
		V8_OFF_SEQTWOBYTESTR_CHARS = V8_OFF_SEQASCIISTR_CHARS;

	if (V8_OFF_SLICEDSTRING_PARENT == -1)
		V8_OFF_SLICEDSTRING_PARENT = V8_OFF_SLICEDSTRING_OFFSET -
		    sizeof (uintptr_t);

	if (V8_OFF_JSFUNCTION_CONTEXT == -1)
		V8_OFF_JSFUNCTION_CONTEXT = V8_OFF_JSFUNCTION_SHARED +
		    sizeof (uintptr_t);

	if (V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO == -1) {
		if (heap_offset("SharedFunctionInfo", "optimized_code_map",
		    &V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO) == -1) {
			V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO = -1;
		} else {
			V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO +=
			    sizeof (uintptr_t);
		}
	}

	/*
	 * If we don't have bit_field/bit_field2 for Map, we know that they're
	 * the second and third byte of instance_attributes.
	 */
	if (V8_OFF_MAP_BIT_FIELD == -1)
		V8_OFF_MAP_BIT_FIELD = V8_OFF_MAP_INSTANCE_ATTRIBUTES + 2;

	if (V8_OFF_MAP_BIT_FIELD2 == -1)
		V8_OFF_MAP_BIT_FIELD2 = V8_OFF_MAP_INSTANCE_ATTRIBUTES + 3;

	/*
	 * V8_SCOPEINFO_IDX_FIRST_VARS' value was 4 in V8 3.7 and up,
	 * then 5 when StrongModeFreeVariableCount was added with
	 * https://codereview.chromium.org/1005063002, and 6 when
	 * ContextGlobalCount was added with
	 * https://codereview.chromium.org/1218783005.
	 * Since the current V8_CONSTANT_FALLBACK macro doesn't allow
	 * us to specify different values for different V8 versions,
	 * these are hardcoded below.
	 */
	if (V8_SCOPEINFO_IDX_FIRST_VARS == -1) {
		if (v8_major > 4 || (v8_major == 4 && v8_minor >= 3)) {
			V8_SCOPEINFO_IDX_FIRST_VARS = 5;
		} else if (v8_major > 3 || (v8_major == 3 && v8_minor >= 7)) {
			V8_SCOPEINFO_IDX_FIRST_VARS = 4;
		}
	}

	/*
	 * With V8 version 4.3, a new "constructor_or_backpointer" field
	 * replaces the original "constructor" field. Both of them can't
	 * exist at the same time, and the data they point have similar
	 * semantics (at least similar enough so that the current code can
	 * handle either of them transparently). So if the new field exists,
	 * copy its value into the variable used to hold the old field to
	 * allow the code to be more concise.
	 */
	if (V8_OFF_MAP_CONSTRUCTOR_OR_BACKPOINTER != -1)
		V8_OFF_MAP_CONSTRUCTOR = V8_OFF_MAP_CONSTRUCTOR_OR_BACKPOINTER;

	if (V8_OFF_MAP_INOBJECT_PROPERTIES_OR_CTOR_FUN_INDEX != -1)
		V8_OFF_MAP_INOBJECT_PROPERTIES =
		    V8_OFF_MAP_INOBJECT_PROPERTIES_OR_CTOR_FUN_INDEX;

	if (V8_OFF_JSOBJECT_PROPERTIES == -1) {
		V8_OFF_JSOBJECT_PROPERTIES = V8_OFF_JSRECEIVER_PROPERTIES;
	}

	/*
	 * Starting with V8 5.1.71 (and node v6.5.0), the value that identifies
	 * the type of an internal frame is stored at offset
	 * V8_OFF_FP_CONTEXT_OR_FRAME_TYPE. See
	 * https://codereview.chromium.org/1696043002. If the value of that
	 * offset is -1, it means we're in the presence of an older version of
	 * V8, and we fall back to the previous offset used to retrieve that
	 * value, which is V8_OFF_FP_MARKER.
	 */
	if (V8_OFF_FP_CONTEXT_OR_FRAME_TYPE == -1) {
		V8_OFF_FP_CONTEXT_OR_FRAME_TYPE = V8_OFF_FP_MARKER;
	}

	/*
	 * Starting with V8 5.1.162 (and node v6.5.0), the SharedFunctionInfo's
	 * "inferred_name"" field was renamed to "function_identifier" (See
	 * https://codereview.chromium.org/1801023002). If the value of
	 * V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER is -1, it means we're in the
	 * presence of an older V8 version, and we should use the original
	 * offset.
	 */
	if (V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER == -1) {
		V8_OFF_SHAREDFUNCTIONINFO_IDENTIFIER =
		    V8_OFF_SHAREDFUNCTIONINFO_INFERRED_NAME;
	}

	/*
	 * The appropriate values for the "kBoundFunction" bit that lives within
	 * the SharedFunctionInfo "compiler_hints" field have changed over time.
	 * It's unlikely we'll ever have metadata in the binary for this field,
	 * since current versions of V8 and Node (at least V8 4.9.385 and later
	 * and Node 6.8 and later) don't use it at all.
	 *
	 * Important versions:
	 *
	 *	Node	V8
	 *	0.12.0	3.28.73.0
	 *	0.12.16	3.28.71.19 (note: earlier V8 than that in v0.12.0)
	 *	4.0.0	4.5.103.30
	 *	6.0.0	5.0.71.35 (can detect V8_TYPE_JSBOUNDFUNCTION)
	 */
	if (V8_TYPE_JSBOUNDFUNCTION == -1) {
		if (v8_version_current_older(3, 28, 71, 19)) {
			/* Node 0.10 */
			V8_CompilerHints_BoundFunction = 13;
		} else if (v8_version_current_older(4, 5, 103, 30)) {
			/* Node 0.12 */
			V8_CompilerHints_BoundFunction = 8;
		} else {
			/* Node v4 */
			V8_CompilerHints_BoundFunction = 10;
		}
	}

	return (failed ? -1 : 0);
}

/* ARGSUSED */
static int
autoconf_iter_symbol(mdb_symbol_t *symp, void *arg)
{
	v8_cfg_t *cfgp = arg;

	if (strncmp(symp->sym_name, "v8dbg_parent_",
	    sizeof ("v8dbg_parent_") - 1) == 0)
		return (conf_update_parent(symp->sym_name));

	if (strncmp(symp->sym_name, "v8dbg_class_",
	    sizeof ("v8dbg_class_") - 1) == 0)
		return (conf_update_field(cfgp, symp->sym_name));

	if (strncmp(symp->sym_name, "v8dbg_type_",
	    sizeof ("v8dbg_type_") - 1) == 0)
		return (conf_update_type(cfgp, symp->sym_name));

	if (strncmp(symp->sym_name, "v8dbg_frametype_",
	    sizeof ("v8dbg_frametype_") - 1) == 0)
		return (conf_update_frametype(cfgp, symp->sym_name));

	return (0);
}

/*
 * Extracts the next field of a string whose fields are separated by "__" (as
 * the V8 metadata symbols are).
 */
static char *
conf_next_part(char *buf, char *start)
{
	char *pp;

	if ((pp = strstr(start, "__")) == NULL) {
		mdb_warn("malformed symbol name: %s\n", buf);
		return (NULL);
	}

	*pp = '\0';
	return (pp + sizeof ("__") - 1);
}

static v8_class_t *
conf_class_findcreate(const char *name)
{
	v8_class_t *clp, *iclp, *prev = NULL;
	int cmp;

	for (iclp = v8_classes; iclp != NULL; iclp = iclp->v8c_next) {
		if ((cmp = strcmp(iclp->v8c_name, name)) == 0)
			return (iclp);

		if (cmp > 0)
			break;

		prev = iclp;
	}

	if ((clp = mdb_zalloc(sizeof (*clp), UM_NOSLEEP)) == NULL)
		return (NULL);

	(void) strlcpy(clp->v8c_name, name, sizeof (clp->v8c_name));
	clp->v8c_end = (size_t)-1;
	clp->v8c_next = iclp;

	if (prev != NULL) {
		prev->v8c_next = clp;
	} else {
		v8_classes = clp;
	}

	return (clp);
}

static v8_field_t *
conf_field_create(v8_class_t *clp, const char *name, size_t offset)
{
	v8_field_t *flp, *iflp;

	if ((flp = mdb_zalloc(sizeof (*flp), UM_NOSLEEP)) == NULL)
		return (NULL);

	(void) strlcpy(flp->v8f_name, name, sizeof (flp->v8f_name));
	flp->v8f_offset = offset;

	if (clp->v8c_fields == NULL || clp->v8c_fields->v8f_offset > offset) {
		flp->v8f_next = clp->v8c_fields;
		clp->v8c_fields = flp;
		return (flp);
	}

	for (iflp = clp->v8c_fields; iflp->v8f_next != NULL;
	    iflp = iflp->v8f_next) {
		if (iflp->v8f_next->v8f_offset > offset)
			break;
	}

	flp->v8f_next = iflp->v8f_next;
	iflp->v8f_next = flp;
	return (flp);
}

/*
 * Given a "v8dbg_parent_X__Y", symbol, update the parent of class X to class Y.
 * Note that neither class necessarily exists already.
 */
static int
conf_update_parent(const char *symbol)
{
	char *pp, *qq;
	char buf[128];
	v8_class_t *clp, *pclp;

	(void) strlcpy(buf, symbol, sizeof (buf));
	pp = buf + sizeof ("v8dbg_parent_") - 1;
	qq = conf_next_part(buf, pp);

	if (qq == NULL)
		return (-1);

	clp = conf_class_findcreate(pp);
	pclp = conf_class_findcreate(qq);

	if (clp == NULL || pclp == NULL) {
		mdb_warn("mdb_v8: out of memory\n");
		return (-1);
	}

	clp->v8c_parent = pclp;
	return (0);
}

/*
 * Given a "v8dbg_class_CLASS__FIELD__TYPE", symbol, save field "FIELD" into
 * class CLASS with the offset described by the symbol.  Note that CLASS does
 * not necessarily exist already.
 */
static int
conf_update_field(v8_cfg_t *cfgp, const char *symbol)
{
	v8_class_t *clp;
	v8_field_t *flp;
	intptr_t offset;
	char *pp, *qq, *tt;
	char buf[128];
	int is_map_bitfield3_smi = 0;
	int is_v8_bitfield3_actually_int = 0;

	(void) strlcpy(buf, symbol, sizeof (buf));

	pp = buf + sizeof ("v8dbg_class_") - 1;
	qq = conf_next_part(buf, pp);

	if (qq == NULL || (tt = conf_next_part(buf, qq)) == NULL)
		return (-1);

	if (cfgp->v8cfg_readsym(cfgp, symbol, &offset) == -1) {
		mdb_warn("failed to read symbol \"%s\"", symbol);
		return (-1);
	}

	if ((clp = conf_class_findcreate(pp)) == NULL ||
	    (flp = conf_field_create(clp, qq, (size_t)offset)) == NULL)
		return (-1);

	is_map_bitfield3_smi = strcmp(pp, "Map") == 0 &&
	    strcmp(qq, "bit_field3") == 0 && strcmp(tt, "SMI") == 0;

	/*
	 * V8 versions between 3.28 and 4.7 were released without the proper
	 * post-mortem metadata for Map's bit_field3 type, so we hardcode its
	 * actual type when V8's version is between the version that introduced
	 * the change from SMI to int and the version where the proper metadata
	 * was added (4.7 inclusive, since several V8 4.7 versions exist and
	 * some of them have the proper metadata, some of them don't).
	 */
	if (is_map_bitfield3_smi)
		is_v8_bitfield3_actually_int =
		    (v8_major == 3 && v8_minor >= 28) ||
		    (v8_major == 4 && v8_minor <= 7);

	if (strcmp(tt, "int") == 0 ||
	    (is_map_bitfield3_smi && is_v8_bitfield3_actually_int)) {
		flp->v8f_isbyte = B_TRUE;
	} else if (strcmp(tt, "char") == 0) {
		flp->v8f_isstr = B_TRUE;
	}

	return (0);
}

static int
conf_update_enum(v8_cfg_t *cfgp, const char *symbol, const char *name,
    v8_enum_t *enp)
{
	intptr_t value;

	if (cfgp->v8cfg_readsym(cfgp, symbol, &value) == -1) {
		mdb_warn("failed to read symbol \"%s\"", symbol);
		return (-1);
	}

	enp->v8e_value = (int)value;
	(void) strlcpy(enp->v8e_name, name, sizeof (enp->v8e_name));
	return (0);
}

/*
 * Given a "v8dbg_type_TYPENAME" constant, save the type name in v8_types.  Note
 * that this enum has multiple integer values with the same string label.
 */
static int
conf_update_type(v8_cfg_t *cfgp, const char *symbol)
{
	char *klass;
	v8_enum_t *enp;
	char buf[128];

	if (v8_next_type > sizeof (v8_types) / sizeof (v8_types[0])) {
		mdb_warn("too many V8 types\n");
		return (-1);
	}

	(void) strlcpy(buf, symbol, sizeof (buf));

	klass = buf + sizeof ("v8dbg_type_") - 1;
	if (conf_next_part(buf, klass) == NULL)
		return (-1);

	enp = &v8_types[v8_next_type++];
	return (conf_update_enum(cfgp, symbol, klass, enp));
}

/*
 * Given a "v8dbg_frametype_TYPENAME" constant, save the frame type in
 * v8_frametypes.
 */
static int
conf_update_frametype(v8_cfg_t *cfgp, const char *symbol)
{
	const char *frametype;
	v8_enum_t *enp;

	if (v8_next_frametype >
	    sizeof (v8_frametypes) / sizeof (v8_frametypes[0])) {
		mdb_warn("too many V8 frame types\n");
		return (-1);
	}

	enp = &v8_frametypes[v8_next_frametype++];
	frametype = symbol + sizeof ("v8dbg_frametype_") - 1;
	return (conf_update_enum(cfgp, symbol, frametype, enp));
}

/*
 * Now that all classes have been loaded, update the "start" and "end" fields of
 * each class based on the values of its parent class.
 */
static void
conf_class_compute_offsets(v8_class_t *clp)
{
	v8_field_t *flp;

	assert(clp->v8c_start == 0);
	assert(clp->v8c_end == (size_t)-1);

	if (clp->v8c_parent != NULL) {
		if (clp->v8c_parent->v8c_end == (size_t)-1)
			conf_class_compute_offsets(clp->v8c_parent);

		clp->v8c_start = clp->v8c_parent->v8c_end;
	}

	if (clp->v8c_fields == NULL) {
		clp->v8c_end = clp->v8c_start;
		return;
	}

	for (flp = clp->v8c_fields; flp->v8f_next != NULL; flp = flp->v8f_next)
		;

	if (flp == NULL)
		clp->v8c_end = clp->v8c_start;
	else
		clp->v8c_end = flp->v8f_offset + sizeof (uintptr_t);
}

/*
 * Utility functions
 */

static int jsstr_print(uintptr_t, uint_t, char **, size_t *);
static boolean_t jsobj_is_hole(uintptr_t addr);
static boolean_t jsobj_maybe_garbage(uintptr_t addr);

static const char *
enum_lookup_str(v8_enum_t *enums, int val, const char *dflt)
{
	v8_enum_t *ep;

	for (ep = enums; ep->v8e_name[0] != '\0'; ep++) {
		if (ep->v8e_value == val)
			return (ep->v8e_name);
	}

	return (dflt);
}

static void
enum_print(v8_enum_t *enums)
{
	v8_enum_t *itp;

	for (itp = enums; itp->v8e_name[0] != '\0'; itp++)
		mdb_printf("%-30s = 0x%02x\n", itp->v8e_name, itp->v8e_value);
}

/*
 * b[v]snprintf behave like [v]snprintf(3c), except that they update the buffer
 * and length arguments based on how much buffer space is used by the operation.
 * This makes it much easier to combine multiple calls in sequence without
 * worrying about buffer overflow.
 */
static size_t
bvsnprintf(char **bufp, size_t *buflenp, const char *format, va_list alist)
{
	size_t rv, len;

	if (*buflenp == 0)
		return (vsnprintf(NULL, 0, format, alist));

	rv = vsnprintf(*bufp, *buflenp, format, alist);

	len = MIN(rv, *buflenp);
	*buflenp -= len;
	*bufp += len;

	return (len);
}

static size_t
bsnprintf(char **bufp, size_t *buflenp, const char *format, ...)
{
	va_list alist;
	size_t rv;

	va_start(alist, format);
	rv = bvsnprintf(bufp, buflenp, format, alist);
	va_end(alist);

	return (rv);
}

void
maybefree(void *arg, size_t sz, int memflags)
{
	if (!(memflags & UM_GC))
		mdb_free(arg, sz);
}

void
v8_warn(const char *format, ...)
{
	char buf[512];
	va_list alist;
	int len;

	if (!v8_warnings || v8_silent)
		return;

	va_start(alist, format);
	(void) vsnprintf(buf, sizeof (buf), format, alist);
	va_end(alist);

	/*
	 * This is made slightly annoying because we need to effectively
	 * preserve the original format string to allow for mdb to use the
	 * new-line at the end to indicate that strerror should be elided.
	 */
	if ((len = strlen(format)) > 0 && format[len - 1] == '\n') {
		buf[strlen(buf) - 1] = '\0';
		mdb_warn("%s\n", buf);
	} else {
		mdb_warn("%s", buf);
	}
}

static v8_field_t *
conf_field_lookup(const char *klass, const char *field)
{
	v8_class_t *clp;
	v8_field_t *flp;

	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next) {
		if (strcmp(klass, clp->v8c_name) == 0)
			break;
	}

	if (clp == NULL)
		return (NULL);

	for (flp = clp->v8c_fields; flp != NULL; flp = flp->v8f_next) {
		if (strcmp(field, flp->v8f_name) == 0)
			break;
	}

	return (flp);
}

/*
 * Property values
 *
 * In V8, JavaScript values are represented using word-sized native values.
 * The native value may represent a small integer, in which case the integer
 * value is encoded directly in the native value; or it may represent some other
 * kind of JavaScript value, in which case the native value encodes a pointer to
 * a more complex structure describing the value.  You can tell whether a native
 * value represents a small integer or a pointer to something else by looking at
 * the low bits of the native value.  These bits are called the _tag_, and
 * JavaScript values encoded this way are called _tagged_ objects.
 *
 * Until the V8 version used in Node v4, all JavaScript values were tagged in
 * this way.  As a result, most interfaces in mdb_v8 that operate on JavaScript
 * values of any kind historically used a "uintptr_t" to refer to arbitrary
 * program values.  That was sufficient because an interface could always figure
 * out from the tag what kind of value it was looking at, no matter where the
 * value came from.
 *
 * As of Node v4, V8 uses a feature called unboxed doubles, where in some cases,
 * rather than wrapping a double-precision floating-point value in a HeapNumber
 * class and then referencing it using a tagged pointer to the HeapNumber, V8
 * instead writes the raw double value directly where it would have written the
 * (tagged) pointer to the HeapNumber.  Such values are called _untagged_
 * because the low bits (the tag bits) are not meaningful.  In order to
 * interpret such values, the caller *must* know already whether it's looking at
 * a tagged value or an unboxed double, since a double can have any bit pattern
 * in the tag bits.
 *
 * There are more details about this in the comment above jsobj_layout_load().
 * Suffice it here to say that this optimization is only known to be used when a
 * value is used as an object property.  As a result, property values cannot be
 * represented with a uintptr_t, but need a combination of uintptr_t and a bit
 * indicating whether the uintptr actually contains an unboxed double.
 *
 * Because this interface change is pretty recent, we've done this in a way that
 * minimizes the change to existing internal interfaces, although the result is
 * something of a kludge.  These interfaces need some refactoring to look more
 * like the interfaces in mdb_v8_dbg.h.
 */
typedef struct {
	boolean_t v8v_isboxeddouble;
	union {
		double		v8vu_double;
		uintptr_t	v8vu_addr;
	} v8v_u;
} v8propvalue_t;

/*
 * Initialize a v8propvalue_t to represent a tagged value represented by "addr".
 */
static void
jsobj_propvalue_addr(v8propvalue_t *valp, uintptr_t addr)
{
	bzero(valp, sizeof (*valp));
	valp->v8v_isboxeddouble = B_FALSE;
	valp->v8v_u.v8vu_addr = addr;
}

/*
 * Initialize a v8propvalue_t to represent an untagged, unboxed double value
 * represented by "d".
 */
static void
jsobj_propvalue_double(v8propvalue_t *valp, double d)
{
	bzero(valp, sizeof (*valp));
	valp->v8v_isboxeddouble = B_TRUE;
	valp->v8v_u.v8vu_double = d;
}


/*
 * Returns in "offp" the offset of field "field" in C++ class "klass".
 */
static int
heap_offset(const char *klass, const char *field, ssize_t *offp)
{
	v8_field_t *flp;

	flp = conf_field_lookup(klass, field);

	if (flp == NULL)
		return (-1);

	*offp = V8_OFF_HEAP(flp->v8f_offset);
	return (0);
}

/*
 * Assuming "addr" is an instance of the C++ heap class "klass", read into *valp
 * the pointer-sized value of field "field".
 */
int
read_heap_ptr(uintptr_t *valp, uintptr_t addr, ssize_t off)
{
	if (mdb_vread(valp, sizeof (*valp), addr + off) == -1) {
		v8_warn("failed to read offset %d from %p", off, addr);
		return (-1);
	}

	return (0);
}

/*
 * Like read_heap_ptr, but assume the field is an SMI and store the actual value
 * into *valp rather than the encoded representation.
 */
int
read_heap_smi(uintptr_t *valp, uintptr_t addr, ssize_t off)
{
	if (read_heap_ptr(valp, addr, off) != 0)
		return (-1);

	if (!V8_IS_SMI(*valp)) {
		v8_warn("expected SMI, got %p\n", *valp);
		return (-1);
	}

	*valp = V8_SMI_VALUE(*valp);

	return (0);
}

static int
read_heap_double(double *valp, uintptr_t addr, ssize_t off)
{
	if (mdb_vread(valp, sizeof (*valp), addr + off) == -1) {
		v8_warn("failed to read heap value at %p", addr + off);
		return (-1);
	}

	return (0);
}

/*
 * Assuming "addr" refers to a FixedArray, return a newly-allocated array
 * representing its contents.
 *
 * TODO This function is deprecated.  See v8fixedarray_load().
 */
int
read_heap_array(uintptr_t addr, uintptr_t **retp, size_t *lenp, int flags)
{
	uint8_t type;
	uintptr_t len;

	if (!V8_IS_HEAPOBJECT(addr))
		return (-1);

	if (read_typebyte(&type, addr) != 0)
		return (-1);

	if (type != V8_TYPE_FIXEDARRAY)
		return (-1);

	if (read_heap_smi(&len, addr, V8_OFF_FIXEDARRAY_LENGTH) != 0)
		return (-1);

	*lenp = len;

	if (len == 0) {
		*retp = NULL;
		return (0);
	}

	if ((*retp = mdb_zalloc(len * sizeof (uintptr_t), flags)) == NULL)
		return (-1);

	if (mdb_vread(*retp, len * sizeof (uintptr_t),
	    addr + V8_OFF_FIXEDARRAY_DATA) == -1) {
		maybefree(*retp, len * sizeof (uintptr_t), flags);
		*retp = NULL;
		return (-1);
	}

	return (0);
}

static int
read_heap_byte(uint8_t *valp, uintptr_t addr, ssize_t off)
{
	if (mdb_vread(valp, sizeof (*valp), addr + off) == -1) {
		v8_warn("failed to read heap value at %p", addr + off);
		return (-1);
	}

	return (0);
}

/*
 * This is truly horrific.  Inside the V8 Script and SharedFunctionInfo classes
 * are a number of small-integer fields like the function_token_position (an
 * offset into the script's text where the "function" token appears).  For
 * 32-bit processes, V8 stores these as a sequence of SMI fields, which we know
 * how to interpret well enough.  For 64-bit processes, "to avoid wasting
 * space", they use a different trick: each 8-byte word contains two integer
 * fields.  The low word is represented like an SMI: shifted left by one.  They
 * don't bother shifting the high word, since its low bit will never be looked
 * at (since it's not word-aligned).
 *
 * This function is used for cases where we would use read_heap_smi(), except
 * that this is one of those fields that might be encoded or might not be,
 * depending on whether the address is word-aligned.
 */
int
read_heap_maybesmi(uintptr_t *valp, uintptr_t addr, ssize_t off)
{
#ifdef _LP64
	uint32_t readval;

	if (mdb_vread(&readval, sizeof (readval), addr + off) == -1) {
		*valp = -1;
		v8_warn("failed to read offset %d from %p", off, addr);
		return (-1);
	}

	/*
	 * If this was the low half-word, it needs to be shifted right.
	 */
	if ((addr + off) % sizeof (uintptr_t) == 0)
		readval >>= 1;

	*valp = (uintptr_t)readval;
	return (0);
#else
	return (read_heap_smi(valp, addr, off));
#endif
}

/*
 * Given a heap object, returns in *valp the byte describing the type of the
 * object.  This is shorthand for first retrieving the Map at the start of the
 * heap object and then retrieving the type byte from the Map object.
 */
int
read_typebyte(uint8_t *valp, uintptr_t addr)
{
	uintptr_t mapaddr;
	ssize_t off = V8_OFF_HEAPOBJECT_MAP;

	if (mdb_vread(&mapaddr, sizeof (mapaddr), addr + off) == -1) {
		v8_warn("failed to read type of %p", addr);
		return (-1);
	}

	if (!V8_IS_HEAPOBJECT(mapaddr)) {
		v8_warn("object map is not a heap object\n");
		return (-1);
	}

	if (read_heap_byte(valp, mapaddr, V8_OFF_MAP_INSTANCE_ATTRIBUTES) == -1)
		return (-1);

	return (0);
}

/*
 * Given a heap object, returns in *valp the size of the object.  For
 * variable-size objects, returns an undefined value.
 */
static int
read_size(size_t *valp, uintptr_t addr)
{
	uintptr_t mapaddr;
	uint8_t size;

	if (read_heap_ptr(&mapaddr, addr, V8_OFF_HEAPOBJECT_MAP) != 0)
		return (-1);

	if (!V8_IS_HEAPOBJECT(mapaddr)) {
		v8_warn("heap object map is not itself a heap object\n");
		return (-1);
	}

	if (read_heap_byte(&size, mapaddr, V8_OFF_MAP_INSTANCE_SIZE) != 0)
		return (-1);

	*valp = size << V8_PointerSizeLog2;
	return (0);
}

/*
 * Assuming "addr" refers to a FixedArray that is implementing a
 * StringDictionary, iterate over its contents calling the specified function
 * with key and value.
 */
static int
read_heap_dict(uintptr_t addr,
    int (*func)(const char *, v8propvalue_t *, void *), void *arg,
    jspropinfo_t *propinfo)
{
	uint8_t type;
	uintptr_t len;
	char buf[512];
	char *bufp;
	int rval = -1;
	uintptr_t *dict, ndict, i;
	v8propvalue_t value;

	if (read_heap_array(addr, &dict, &ndict, UM_SLEEP) != 0)
		return (-1);

	if (V8_DICT_ENTRY_SIZE < 2) {
		v8_warn("dictionary entry size (%d) is too small for a "
		    "key and value\n", V8_DICT_ENTRY_SIZE);
		goto out;
	}

	for (i = V8_DICT_START_INDEX + V8_DICT_PREFIX_SIZE; i + 1 < ndict;
	    i += V8_DICT_ENTRY_SIZE) {
		/*
		 * The layout here is key, value, details. (This is hardcoded
		 * in Dictionary<Shape, Key>::SetEntry().)
		 */
		if (jsobj_is_undefined(dict[i]))
			continue;

		if (V8_IS_SMI(dict[i])) {
			intptr_t val = V8_SMI_VALUE(dict[i]);
			(void) snprintf(buf, sizeof (buf), "%" PRIdPTR, val);
		} else {
			if (jsobj_is_hole(dict[i])) {
				/*
				 * In some cases, the key can (apparently) be a
				 * hole, in which case we skip over it.
				 */
				continue;
			}

			if (read_typebyte(&type, dict[i]) != 0)
				goto out;

			if (!V8_TYPE_STRING(type))
				goto out;

			bufp = buf;
			len = sizeof (buf);

			if (jsstr_print(dict[i], JSSTR_NUDE, &bufp, &len) != 0)
				goto out;
		}

		if (propinfo != NULL && jsobj_maybe_garbage(dict[i + 1]))
			*propinfo |= JPI_BADPROPS;

		jsobj_propvalue_addr(&value, dict[i + 1]);
		if (func(buf, &value, arg) == -1)
			goto out;
	}

	rval = 0;
out:
	mdb_free(dict, ndict * sizeof (uintptr_t));

	return (rval);
}

/*
 * Given a uintptr_t whose contents represent a double, convert it to a double.
 * This should only be used in an LP64 context.  See jsobj_layout_load() for
 * details.
 */
static double
makedouble(uintptr_t ptr)
{
	union {
		double d;
		uintptr_t p;
	} u;

	u.p = ptr;
	return (u.d);
}

/*
 * Given a JavaScript object's Map "map", stores the object's constructor
 * in valp. Returns 0 if it succeeded, -1 otherwise.
 */
static int
get_map_constructor(uintptr_t *valp, uintptr_t map) {
	uintptr_t constructor_candidate;
	int constructor_found = 0;
	uint8_t type;

	if (V8_OFF_MAP_CONSTRUCTOR == -1)
		return (-1);

	/*
	 * https://codereview.chromium.org/950283002, which landed in V8 4.3.x,
	 * makes the "constructor" and "backpointer to transitions" field
	 * overlap. In order to retrieve the constructor from a Map, we follow
	 * the chain of "constructor_or_backpointer" pointers until we find an
	 * object that is _not_ a Map. This is the same algorithm as
	 * Map::GetConstructor in src/objects-inl.h.
	 */
	while (constructor_found == 0) {
		if (read_heap_ptr(&constructor_candidate,
		    map, V8_OFF_MAP_CONSTRUCTOR) != 0)
			return (-1);

		if (read_typebyte(&type, constructor_candidate) != 0)
			return (-1);

		if (type != V8_TYPE_MAP) {
			constructor_found = 1;
			*valp = constructor_candidate;
		}

		map = constructor_candidate;
	}

	if (constructor_found == 1)
		return (0);

	return (-1);
}

/*
 * Given an object, returns in "buf" the name of the constructor function.  With
 * "verbose", prints the pointer to the JSFunction object.  Given anything else,
 * returns an error (and warns the user why).
 */
static int
obj_jsconstructor(uintptr_t addr, char **bufp, size_t *lenp, boolean_t verbose)
{
	uint8_t type;
	uintptr_t map, consfunc, funcinfop;
	const char *constype;

	if (!V8_IS_HEAPOBJECT(addr) ||
	    read_typebyte(&type, addr) != 0 ||
	    (type != V8_TYPE_JSOBJECT &&
	    type != V8_TYPE_JSARRAY &&
	    type != V8_TYPE_JSTYPEDARRAY)) {
		mdb_warn("%p is not a JSObject\n", addr);
		return (-1);
	}

	if (mdb_vread(&map, sizeof (map), addr + V8_OFF_HEAPOBJECT_MAP) == -1 ||
	    get_map_constructor(&consfunc, map) == -1) {
		mdb_warn("unable to read object map\n");
		return (-1);
	}

	if (read_typebyte(&type, consfunc) != 0)
		return (-1);

	constype = enum_lookup_str(v8_types, type, "");
	if (strcmp(constype, "Oddball") == 0) {
		jsobj_print_t jsop;
		bzero(&jsop, sizeof (jsop));
		jsop.jsop_bufp = bufp;
		jsop.jsop_lenp = lenp;
		return (jsobj_print_oddball(consfunc, &jsop));
	}

	if (strcmp(constype, "JSFunction") != 0) {
		mdb_warn("constructor: expected JSFunction, found %s\n",
		    constype);
		return (-1);
	}

	if (read_heap_ptr(&funcinfop, consfunc, V8_OFF_JSFUNCTION_SHARED) != 0)
		return (-1);

	if (jsfunc_name(funcinfop, bufp, lenp) != 0)
		return (-1);

	if (verbose)
		bsnprintf(bufp, lenp, " (JSFunction: %p)", consfunc);

	return (0);
}

/*
 * Returns in "buf" a description of the type of "addr" suitable for printing.
 */
static int
obj_jstype(uintptr_t addr, char **bufp, size_t *lenp, uint8_t *typep)
{
	uint8_t typebyte;
	uintptr_t strptr, map, consfunc, funcinfop;
	const char *typename;

	if (V8_IS_FAILURE(addr)) {
		if (typep)
			*typep = 0;
		(void) bsnprintf(bufp, lenp, "'Failure' object");
		return (0);
	}

	if (V8_IS_SMI(addr)) {
		if (typep)
			*typep = 0;
		(void) bsnprintf(bufp, lenp, "SMI: value = %d",
		    V8_SMI_VALUE(addr));
		return (0);
	}

	if (read_typebyte(&typebyte, addr) != 0)
		return (-1);

	if (typep)
		*typep = typebyte;

	typename = enum_lookup_str(v8_types, typebyte, NULL);

	/*
	 * For not-yet-diagnosed reasons, we don't seem to be able to match the
	 * type byte for various string classes in Node v0.12 and later.
	 * However, we can tell from the tag which type of string it is, and
	 * we're generally able to print them.
	 */
	if (typename == NULL && V8_TYPE_STRING(typebyte)) {
		typename = "<unknown subclass>: String";
	} else if (typename == NULL) {
		typename = "<unknown>";
	}
	(void) bsnprintf(bufp, lenp, typename);

	if (strcmp(typename, "Oddball") == 0) {
		if (read_heap_ptr(&strptr, addr,
		    V8_OFF_ODDBALL_TO_STRING) != -1) {
			(void) bsnprintf(bufp, lenp, ": \"");
			(void) jsstr_print(strptr, JSSTR_NUDE, bufp, lenp);
			(void) bsnprintf(bufp, lenp, "\"");
		}
	}

	if (strcmp(typename, "JSObject") == 0 &&
	    mdb_vread(&map, sizeof (map), addr + V8_OFF_HEAPOBJECT_MAP) != -1 &&
	    get_map_constructor(&consfunc, map) != -1 &&
	    read_typebyte(&typebyte, consfunc) == 0 &&
	    strcmp(enum_lookup_str(v8_types, typebyte, ""),
	    "JSFunction") == 0 &&
	    mdb_vread(&funcinfop, sizeof (funcinfop),
	    consfunc + V8_OFF_JSFUNCTION_SHARED) != -1) {
		(void) bsnprintf(bufp, lenp, ": ");
		(void) jsfunc_name(funcinfop, bufp, lenp);
	}

	return (0);
}

/*
 * V8 allows implementers (like Node) to store pointer-sized values into
 * internal fields within V8 heap objects.  Implementors access these values by
 * 0-based index (e.g., SetInternalField(0, value)).  These values are stored as
 * an array directly after the last actual C++ field in the C++ object.
 *
 * Node uses internal fields to refer to handles.  For example, a socket's C++
 * HandleWrap object is typically stored as internal field 0 in the JavaScript
 * Socket object.  Similarly, the native-heap-allocated chunk of memory
 * associated with a Node Buffer is referenced by field 0 in the External array
 * pointed-to by the Node Buffer JSObject.
 */
static int
obj_v8internal(uintptr_t addr, uint_t idx, uintptr_t *valp)
{
	char *bufp;
	size_t len;
	const char *rqclass;
	ssize_t off;
	uint8_t type;

	v8_class_t *clp;
	char buf[256];

	bufp = buf;
	len = sizeof (buf);
	if (obj_jstype(addr, &bufp, &len, &type) != 0)
		return (DCMD_ERR);

	if (type == 0) {
		mdb_warn("%p: unsupported type\n", addr);
		return (DCMD_ERR);
	}

	if ((rqclass = enum_lookup_str(v8_types, type, NULL)) == NULL) {
		mdb_warn("%p: unknown type\n");
		return (DCMD_ERR);
	}

	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next) {
		if (strcmp(rqclass, clp->v8c_name) == 0)
			break;
	}

	if (clp == NULL) {
		mdb_warn("%p: didn't find expected class\n", addr);
		return (DCMD_ERR);
	}

	off = clp->v8c_end + (idx * sizeof (uintptr_t)) - 1;
	if (read_heap_ptr(valp, addr, off) != 0) {
		mdb_warn("%p: failed to read from %p\n", addr, addr + off);
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

/*
 * Print out the fields of the given object that come from the given class.
 */
static int
obj_print_fields(uintptr_t baddr, v8_class_t *clp)
{
	v8_field_t *flp;
	uintptr_t addr, value;
	int rv;
	char *bufp;
	size_t len;
	uint8_t type;
	char buf[256];

	for (flp = clp->v8c_fields; flp != NULL; flp = flp->v8f_next) {
		bufp = buf;
		len = sizeof (buf);

		addr = baddr + V8_OFF_HEAP(flp->v8f_offset);

		if (flp->v8f_isstr) {
			if (mdb_readstr(buf, sizeof (buf), addr) == -1) {
				mdb_printf("%p %s (unreadable)\n",
				    addr, flp->v8f_name);
				continue;
			}

			mdb_printf("%p %s = \"%s\"\n",
			    addr, flp->v8f_name, buf);
			continue;
		}

		if (flp->v8f_isbyte) {
			uint8_t sv;
			if (mdb_vread(&sv, sizeof (sv), addr) == -1) {
				mdb_printf("%p %s (unreadable)\n",
				    addr, flp->v8f_name);
				continue;
			}

			mdb_printf("%p %s = 0x%x\n", addr, flp->v8f_name, sv);
			continue;
		}

		rv = mdb_vread((void *)&value, sizeof (value), addr);

		if (rv != sizeof (value) ||
		    obj_jstype(value, &bufp, &len, &type) != 0) {
			mdb_printf("%p %s (unreadable)\n", addr, flp->v8f_name);
			continue;
		}

		if (type != 0 && V8_TYPE_STRING(type)) {
			(void) bsnprintf(&bufp, &len, ": ");
			(void) jsstr_print(value, JSSTR_QUOTED, &bufp, &len);
		}

		mdb_printf("%p %s = %p (%s)\n", addr, flp->v8f_name, value,
		    buf);
	}

	return (DCMD_OK);
}

/*
 * Print out all fields of the given object, starting with the root of the class
 * hierarchy and working down the most specific type.
 */
static int
obj_print_class(uintptr_t addr, v8_class_t *clp)
{
	int rv = 0;

	/*
	 * If we have no fields, we just print a simple inheritance hierarchy.
	 * If we have fields but our parent doesn't, our header includes the
	 * inheritance hierarchy.
	 */
	if (clp->v8c_end == 0) {
		mdb_printf("%s ", clp->v8c_name);

		if (clp->v8c_parent != NULL) {
			mdb_printf("< ");
			(void) obj_print_class(addr, clp->v8c_parent);
		}

		return (0);
	}

	mdb_printf("%p %s", addr, clp->v8c_name);

	if (clp->v8c_start == 0 && clp->v8c_parent != NULL) {
		mdb_printf(" < ");
		(void) obj_print_class(addr, clp->v8c_parent);
	}

	mdb_printf(" {\n");
	(void) mdb_inc_indent(4);

	if (clp->v8c_start > 0 && clp->v8c_parent != NULL)
		rv = obj_print_class(addr, clp->v8c_parent);

	rv |= obj_print_fields(addr, clp);
	(void) mdb_dec_indent(4);
	mdb_printf("}\n");

	return (rv);
}

/*
 * Print the ASCII string for the given JS string, expanding ConsStrings and
 * ExternalStrings as needed.
 *
 * This is an internal legacy interface.  Callers should use v8string_load() and
 * v8string_write() instead.
 */
static int
jsstr_print(uintptr_t addr, uint_t flags, char **bufp, size_t *lenp)
{
	mdbv8_strbuf_t strbuf;
	v8string_t *strp;
	int rv;

	mdbv8_strbuf_init(&strbuf, *bufp, *lenp);
	strp = v8string_load(addr, UM_SLEEP);
	if (strp == NULL) {
		mdbv8_strbuf_appends(&strbuf,
		    "<string (failed to load string)>", flags);
		rv = -1;
	} else {
		rv = v8string_write(strp, &strbuf, MSF_ASCIIONLY, flags);
		v8string_free(strp);
	}

	mdbv8_strbuf_legacy_update(&strbuf, bufp, lenp);
	return (rv);
}

/*
 * Returns true if the given address refers to the named oddball object (e.g.
 * "undefined").  Returns false on failure (since we shouldn't fail on the
 * actual "undefined" value).
 */
static boolean_t
jsobj_is_oddball(uintptr_t addr, char *oddball)
{
	uint8_t type;
	uintptr_t strptr;
	const char *typename;
	char buf[16];
	char *bufp = buf;
	size_t len = sizeof (buf);

	v8_silent++;

	if (read_typebyte(&type, addr) != 0) {
		v8_silent--;
		return (B_FALSE);
	}

	v8_silent--;
	typename = enum_lookup_str(v8_types, type, "<unknown>");
	if (strcmp(typename, "Oddball") != 0)
		return (B_FALSE);

	if (read_heap_ptr(&strptr, addr, V8_OFF_ODDBALL_TO_STRING) == -1)
		return (B_FALSE);

	if (jsstr_print(strptr, JSSTR_NUDE, &bufp, &len) != 0)
		return (B_FALSE);

	return (strcmp(buf, oddball) == 0);
}

boolean_t
jsobj_is_undefined(uintptr_t addr)
{
	return (jsobj_is_oddball(addr, "undefined"));
}

static boolean_t
jsobj_is_hole(uintptr_t addr)
{
	return (jsobj_is_oddball(addr, "hole"));
}

/*
 * Returns true if the value at "addr" appears to be invalid (as an object that
 * has been partially garbage-collected).  This heuristic only eliminates heap
 * objects with types that are not types printable by obj_jsprint().  We also
 * avoid marking accessors as garbage, even though obj_jsprint() doesn't support
 * them.
 */
static boolean_t
jsobj_maybe_garbage(uintptr_t addr)
{
	uint8_t type;

	return (!V8_IS_SMI(addr) &&
	    (read_typebyte(&type, addr) != 0 ||
	    (!V8_TYPE_STRING(type) &&
	    type != V8_TYPE_ACCESSORINFO &&
	    type != V8_TYPE_ACCESSORPAIR &&
	    type != V8_TYPE_EXECUTABLEACCESSORINFO &&
	    type != V8_TYPE_HEAPNUMBER &&
	    type != V8_TYPE_MUTABLEHEAPNUMBER &&
	    type != V8_TYPE_ODDBALL &&
	    type != V8_TYPE_JSOBJECT &&
	    type != V8_TYPE_JSARRAY &&
	    type != V8_TYPE_JSFUNCTION &&
	    (V8_TYPE_JSBOUNDFUNCTION == -1 ||
	    type != V8_TYPE_JSBOUNDFUNCTION) &&
	    type != V8_TYPE_JSDATE &&
	    type != V8_TYPE_JSREGEXP &&
	    type != V8_TYPE_JSTYPEDARRAY)));
}

/*
 * Iterate the properties of a JavaScript object "addr".
 *
 * Every heap object refers to a Map that describes how that heap object is laid
 * out.  The Map includes information like the constructor function used to
 * create the object, how many bytes each object uses, and how many properties
 * are stored inside the object.  (A single Map object can be shared by many
 * objects of the same general type, which is why this information is encoded by
 * reference rather than contained in each object.)
 *
 * V8 knows about lots of different kinds of properties:
 *
 *    o properties with numeric names (e.g., array elements)
 *    o dictionary properties
 *    o "fast" properties stored inside each object, much like a C struct
 *    o properties stored in the separate "properties" array
 *    o getters, setters, and other magic (not supported by this module)
 *
 * While property lookup in JavaScript involves traversing an object's prototype
 * chain, this module only iterates the properties local to the object itself.
 *
 *
 * Numeric properties
 *
 * Properties having numeric indexes are stored in the "elements" array attached
 * to each object.  Objects with numeric properties can also have other
 * properties.
 *
 *
 * Dictionary properties
 *
 * An object with dictionary properties is identified by one of the bits in
 * "bitfield3" in the object's Map.  For details on slow properties, see
 * read_heap_dict().
 *
 *
 * Other properties
 *
 * The Map object refers to an array of "instance descriptors".  This array has
 * a few metadata entries at the front, followed by groups of three entries for
 * each property.  In Node v0.10 and later, it looks roughly like this:
 *
 *               +--------------+         +----------------------+
 *               | JSObject     |    +--> | Map                  |
 *               +--------------|    |    +----------------------+
 *               | map          | ---+    | ...                  |
 *               | ...          |         | instance_descriptors | --+
 *  in-object    | [prop 0 val] |         | ...                  |   |
 *  properties   | [prop 1 val] |         +----------------------+   |
 *  (not for all | ...          |                                    |
 *  objects)     | [prop N val] |                                    |
 *               +--------------+                                    |
 *                 +------------------------------------------------+
 *                 |
 *                 +----> +------------------------------+
 *                        | FixedArray                   |
 *                        +------------------------------+
 *                        | ...                          |
 *                        | prop 0 "key" descriptor      |
 *                        | prop 0 "details" descriptor  |
 *                        | prop 0 "value" descriptor    |
 *                        | prop 1 "key" descriptor      |
 *                        | prop 1 "details" descriptor  |
 *                        | prop 1 "value" descriptor    |
 *                        | ...                          |
 *                        | prop N "key" descriptor      |
 *                        | prop N "details" descriptor  |
 *                        | prop N "value" descriptor    |
 *                        +------------------------------+
 *
 * In versions of Node prior to 0.10, there's an extra level of indirection.
 * The Map refers to a "transitions" array, which has an entry that points to
 * the instance descriptors.  In both cases, the descriptors look roughly the
 * same.
 *
 * Each property is described by three pointer-sized entries:
 *
 *    o key: a string denoting the name of the property
 *    o details: a bitfield describing attributes of this property
 *    o value: an integer describing where this property's value is stored
 *
 * "key" is straightforward: it's just the name of the property as the
 * JavaScript programmer knows it.
 *
 * In versions prior to Node 0.12, "value" is an integer.  If "value" is less
 * than the number of properties stored inside the object (which is also
 * recorded in the Map), then it denotes which of the in-object property value
 * slots (shown above inside the JSObject object) stores the value for this
 * property.  If "value" is greater than the number of properties stored inside
 * the object, then it denotes which index into the separate "properties" array
 * (a separate field in the JSObject, not shown above) contains the value for
 * this property.
 *
 * In Node 0.12, for properties that are stored inside the object, the offset is
 * obtained not using "value", but using a bitfield from the "details" part of
 * the descriptor.
 *
 * Terminology notes: it's important to keep straight the different senses of
 * "object" and "property" here.  We use "JavaScript objects" to refer to the
 * things that JavaScript programmers would call objects, including instances of
 * Object and Array and subclasses of those.  These are a subset of V8 heap
 * objects, since V8 uses its heap to manage lots of other objects that
 * JavaScript programmers don't think about.  This function iterates JavaScript
 * properties of these JavaScript objects, not internal properties of heap
 * objects in general.
 *
 * Relatedly, while JavaScript programmers frequently interchange the notions of
 * property names, property values, and property configurations (e.g., getters
 * and setters, read-only or not, hidden or not), these are all distinct in the
 * implementation of the VM, and "property" typically refers to the whole
 * configuration, which may include a way to get the property name and value.
 *
 * The canonical source of the information used here is the implementation of
 * property lookup in the V8 source code, currently in Object::GetProperty.
 */

static int
jsobj_properties(uintptr_t addr,
    int (*func)(const char *, v8propvalue_t *, void *), void *arg,
    jspropinfo_t *propinfop)
{
	uintptr_t ptr, map, elements;
	uintptr_t *props = NULL, *descs = NULL, *content = NULL, *trans, *elts;
	size_t size, nprops, ndescs, ncontent, ntrans, len;
	ssize_t ii, rndescs;
	uint8_t type, ninprops;
	int rval = -1;
	size_t ps = sizeof (uintptr_t);
	ssize_t off;
	jspropinfo_t propinfo = JPI_NONE;
	jsobj_layout_t layout;
	v8propvalue_t value;
	boolean_t untagged;

	/*
	 * First, check if the JSObject's "properties" field is a FixedArray.
	 * If not, then this is something we don't know how to deal with, and
	 * we'll just pass the caller a NULL value.
	 */
	if (mdb_vread(&ptr, ps, addr + V8_OFF_JSOBJECT_PROPERTIES) == -1)
		return (-1);

	if (read_typebyte(&type, ptr) != 0)
		return (-1);

	if (type != V8_TYPE_FIXEDARRAY) {
		char buf[256];
		(void) mdb_snprintf(buf, sizeof (buf), "<%s>",
		    enum_lookup_str(v8_types, type, "unknown"));
		if (propinfop != NULL)
			*propinfop = JPI_BADLAYOUT;
		return (func(buf, NULL, arg));
	}

	/*
	 * As described above, we need the Map to figure out how to iterate the
	 * properties for this object.
	 */
	if (mdb_vread(&map, ps, addr + V8_OFF_HEAPOBJECT_MAP) == -1)
		goto err;

	/*
	 * Check to see if our elements member is an array and non-zero; if
	 * so, it contains numerically-named properties.  Whether or not there
	 * are any numerically-named properties, there may be other kinds of
	 * properties.
	 * Do not consider instances of JSTypedArray, as they use the elements
	 * member to store their external data, not numerically-named
	 * properties.
	 */
	if (V8_ELEMENTS_KIND_SHIFT != -1 &&
	    type != V8_TYPE_JSTYPEDARRAY &&
	    read_heap_ptr(&elements, addr, V8_OFF_JSOBJECT_ELEMENTS) == 0 &&
	    read_heap_array(elements, &elts, &len, UM_SLEEP) == 0 && len != 0) {
		uint8_t bit_field2, kind;
		size_t sz = len * sizeof (uintptr_t);

		if (mdb_vread(&bit_field2, sizeof (bit_field2),
		    map + V8_OFF_MAP_BIT_FIELD2) == -1) {
			mdb_free(elts, sz);
			goto err;
		}

		kind = bit_field2 >> V8_ELEMENTS_KIND_SHIFT;
		kind &= (1 << V8_ELEMENTS_KIND_BITCOUNT) - 1;
		propinfo |= JPI_NUMERIC;

		if (kind == V8_ELEMENTS_FAST_ELEMENTS ||
		    kind == V8_ELEMENTS_FAST_HOLEY_ELEMENTS) {
			for (ii = 0; ii < len; ii++) {
				char name[10];

				if (kind == V8_ELEMENTS_FAST_HOLEY_ELEMENTS &&
				    jsobj_is_hole(elts[ii]))
					continue;

				snprintf(name, sizeof (name), "%" PRIdPTR, ii);

				/*
				 * If the property value doesn't look like a
				 * valid JavaScript object, mark this object as
				 * dubious.
				 */
				if (jsobj_maybe_garbage(elts[ii]))
					propinfo |= JPI_BADPROPS;

				jsobj_propvalue_addr(&value, elts[ii]);
				if (func(name, &value, arg) != 0) {
					mdb_free(elts, sz);
					goto err;
				}
			}
		} else if (kind == V8_ELEMENTS_DICTIONARY_ELEMENTS) {
			propinfo |= JPI_DICT;
			if (read_heap_dict(elements, func, arg,
			    &propinfo) != 0) {
				mdb_free(elts, sz);
				goto err;
			}
		}

		mdb_free(elts, sz);
	}

	if (V8_DICT_SHIFT != -1) {
		v8_field_t *flp;
		uintptr_t bit_field3;

		/*
		 * If dictionary properties are supported (the V8_DICT_SHIFT
		 * offset is not -1), then bitfield 3 tells us if the properties
		 * for this object are stored in "properties" field of the
		 * object using a Dictionary representation.
		 *
		 * Versions of V8 prior to Node 0.12 treated bit_field3 as an
		 * SMI, so it was pointer-sized, and it has to be converted from
		 * an SMI before using it.  In 0.12, it's treated as a raw
		 * uint32_t, meaning it's always int-sized and it should not be
		 * converted.  We can tell which case we're in because the debug
		 * constant (v8dbg_class_map__bit_field3__TYPE) tells us whether
		 * the TYPE is "SMI" or "int".
		 */

		flp = conf_field_lookup("Map", "bit_field3");
		if (flp == NULL || flp->v8f_isbyte) {
			/*
			 * v8f_isbyte indicates the type is "int", so we're in
			 * the int-sized not-a-SMI world.
			 */
			unsigned int bf3_value;
			if (mdb_vread(&bf3_value, sizeof (bf3_value),
			    map + V8_OFF_MAP_BIT_FIELD3) == -1)
				goto err;
			bit_field3 = (uintptr_t)bf3_value;
		} else {
			/* The metadata indicates this is an SMI. */
			if (mdb_vread(&bit_field3, sizeof (bit_field3),
			    map + V8_OFF_MAP_BIT_FIELD3) == -1)
					goto err;
			bit_field3 = V8_SMI_VALUE(bit_field3);
		}

		if (bit_field3 & (1 << V8_DICT_SHIFT)) {
			propinfo |= JPI_DICT;
			if (propinfop != NULL)
				*propinfop = propinfo;
			return (read_heap_dict(ptr, func, arg, propinfop));
		}
	} else if (V8_OFF_MAP_INSTANCE_DESCRIPTORS != -1) {
		uintptr_t bit_field3;

		if (mdb_vread(&bit_field3, sizeof (bit_field3),
		    map + V8_OFF_MAP_INSTANCE_DESCRIPTORS) == -1)
			goto err;

		if (V8_SMI_VALUE(bit_field3) == (1 << V8_ISSHARED_SHIFT)) {
			/*
			 * On versions of V8 prior to that used in 0.10,
			 * the instance descriptors were overloaded to also
			 * be bit_field3 -- and there was no way from that
			 * field to infer a dictionary type.  Because we
			 * can't determine if the map is actually the
			 * hash_table_map, we assume that if it's an object
			 * that has kIsShared set, that it is in fact a
			 * dictionary -- an assumption that is assuredly in
			 * error in some cases.
			 */
			propinfo |= JPI_DICT;
			if (propinfop != NULL)
				*propinfop = propinfo;
			return (read_heap_dict(ptr, func, arg, propinfop));
		}
	}

	if (read_heap_array(ptr, &props, &nprops, UM_SLEEP) != 0)
		goto err;

	/*
	 * Check if we're looking at an older version of V8, where the instance
	 * descriptors are stored not directly in the Map, but in the
	 * "transitions" array that's stored in the Map.
	 */
	if (V8_OFF_MAP_INSTANCE_DESCRIPTORS == -1) {
		if (V8_OFF_MAP_TRANSITIONS == -1 ||
		    V8_TRANSITIONS_IDX_DESC == -1 ||
		    V8_PROP_IDX_CONTENT != -1) {
			mdb_warn("missing instance_descriptors, but did "
			    "not find expected transitions array metadata; "
			    "cannot read properties\n");
			goto err;
		}

		propinfo |= JPI_HASTRANSITIONS;
		off = V8_OFF_MAP_TRANSITIONS;
		if (mdb_vread(&ptr, ps, map + off) == -1)
			goto err;

		if (read_heap_array(ptr, &trans, &ntrans, UM_SLEEP) != 0)
			goto err;

		ptr = trans[V8_TRANSITIONS_IDX_DESC];
		mdb_free(trans, ntrans * sizeof (uintptr_t));
	} else {
		off = V8_OFF_MAP_INSTANCE_DESCRIPTORS;
		if (mdb_vread(&ptr, ps, map + off) == -1)
			goto err;
	}

	/*
	 * Either way, at this point "ptr" should refer to the descriptors
	 * array.
	 */
	if (read_heap_array(ptr, &descs, &ndescs, UM_SLEEP) != 0)
		goto err;

	/*
	 * For cases where property values are stored directly inside the object
	 * ("fast properties"), we need to know the whole size of the object and
	 * the number of properties in the object in order to calculate the
	 * correct offset for each property.
	 */
	if (read_size(&size, addr) != 0)
		size = 0;
	if (mdb_vread(&ninprops, ps,
	    map + V8_OFF_MAP_INOBJECT_PROPERTIES) == -1)
		goto err;

	if (V8_PROP_IDX_CONTENT == -1) {
		/*
		 * On node v0.8 and later, the content is not stored in a
		 * separate FixedArray, but rather with the descriptors.  The
		 * number of actual properties is the length of the array minus
		 * the first (non-property) elements divided by the number of
		 * elements per property.
		 */
		content = descs;
		ncontent = ndescs;
		rndescs = ndescs > V8_PROP_IDX_FIRST ?
		    (ndescs - V8_PROP_IDX_FIRST) / V8_PROP_DESC_SIZE : 0;
	} else {
		/*
		 * On older versions, the content is stored in a separate array,
		 * and there's one entry per property (rather than three).
		 */
		if (V8_PROP_IDX_CONTENT < ndescs &&
		    read_heap_array(descs[V8_PROP_IDX_CONTENT], &content,
		    &ncontent, UM_SLEEP) != 0)
			goto err;

		rndescs = ndescs - V8_PROP_IDX_FIRST;
		propinfo |= JPI_HASCONTENT;
	}

	/*
	 * The last thing we need to work out is whether each property is stored
	 * as an untagged value or not.
	 */
	if (jsobj_layout_load(&layout, map) == -1)
		goto err;

	/*
	 * At this point, we've read all the pieces we need to process the list
	 * of instance descriptors.
	 */
	for (ii = 0; ii < rndescs; ii++) {
		intptr_t keyidx, validx, detidx, baseidx, propaddr, propidx;
		char buf[1024];
		intptr_t val;
		size_t len = sizeof (buf);
		char *c = buf;

		if (V8_PROP_IDX_CONTENT != -1) {
			/*
			 * In node versions prior to v0.8, this was hardcoded
			 * in the V8 implementation, so we hardcode it here
			 * as well.
			 */
			keyidx = ii + V8_PROP_IDX_FIRST;
			validx = ii << 1;
			detidx = (ii << 1) + 1;
		} else {
			baseidx = V8_PROP_IDX_FIRST + (ii * V8_PROP_DESC_SIZE);
			keyidx = baseidx + V8_PROP_DESC_KEY;
			validx = baseidx + V8_PROP_DESC_VALUE;
			detidx = baseidx + V8_PROP_DESC_DETAILS;
		}

		/*
		 * Ignore cases where our understanding doesn't appear to match
		 * what's here.
		 */
		if (detidx >= ncontent) {
			propinfo |= JPI_SKIPPED;
			v8_warn("property descriptor %d: detidx (%d) "
			    "out of bounds for content array (length %d)\n",
			    ii, detidx, ncontent);
			continue;
		}

		/*
		 * We only process fields.  There are other entries here
		 * (notably: transitions) that we don't care about (and these
		 * are not errors).
		 */
		if (!V8_DESC_ISFIELD(content[detidx]))
			continue;

		if (keyidx >= ndescs) {
			propinfo |= JPI_SKIPPED;
			v8_warn("property descriptor %d: keyidx (%d) "
			    "out of bounds for descriptor array (length %d)\n",
			    ii, keyidx, ndescs);
			continue;
		}

		if (jsstr_print(descs[keyidx], JSSTR_NUDE, &c, &len) != 0) {
			if (jsobj_is_undefined(descs[keyidx])) {
				/*
				 * In some cases, we've encountered objects that
				 * look basically fine, but have a bunch of
				 * extra "undefined" values in the instance
				 * descriptors.  Just ignore these, but mark
				 * them in case a developer wants to find them
				 * later.
				 */
				propinfo |= JPI_UNDEFPROPNAME;
			} else {
				propinfo |= JPI_SKIPPED;
				v8_warn("property descriptor %d: could not "
				    "print %p as a string\n", ii,
				    descs[keyidx]);
			}
			continue;
		}

		/*
		 * There are two possibilities at this point: the property may
		 * be stored directly inside the object (like a C struct), or it
		 * may be stored inside the attached "properties" array.  The
		 * details vary whether we're looking at the V8 bundled with
		 * v0.10 or v0.12.  The specific V8 version affects not only how
		 * to tell which kind of property is used, but also how to
		 * compute the in-object address from the information available.
		 */
		propaddr = 0;
		ptr = 0;
		if (v8_major > 3 || (v8_major == 3 && v8_minor >= 26)) {
			/*
			 * In Node v0.12, the property's 0-based index is stored
			 * in a bitfield in the "property details" word.  These
			 * constants are literal here because they're literal in
			 * the V8 source itself.  We use the heuristic that if
			 * the property index refers to something obviously not
			 * in the object, then it must be part of the
			 * "properties" array.
			 */
			propidx = V8_PROP_FIELDINDEX(content[detidx]);
			if (propidx < ninprops) {
				/* The property is stored inside the object. */
				propaddr = addr + V8_OFF_HEAP(
				    size - (ninprops - propidx) * ps);
			}
		} else {
			/*
			 * In v0.10 and earlier, the "value" part of each
			 * property descriptor tells us whether the property
			 * value is stored directly in the object or in the
			 * related "props" array.  See
			 * JSObject::RawFastPropertyAt() in the V8 source.
			 */
			val = (intptr_t)content[validx];
			if (!V8_IS_SMI(val)) {
				propinfo |= JPI_SKIPPED;
				v8_warn("object %p: property descriptor %d: "
				    "value index is not an SMI: %p\n", addr,
				    ii, val);
				continue;
			}

			propidx = V8_SMI_VALUE(val) - ninprops;
			if (propidx < 0) {
				/*
				 * The property is stored directly inside the
				 * object.  In Node 0.10, "val - ninprops" is
				 * the (negative) index of the property counted
				 * from the end of the object.  In that context,
				 * -1 refers to the last word in the object; -2
				 * refers to the second-last word, and so on.
				 */
				propaddr = addr +
				    V8_OFF_HEAP(size + propidx * ps);
			}
		}

		/*
		 * Now that we've figured out what kind of property it is and
		 * where it's located, read the value into "ptr".
		 */
		if (propaddr != 0) {
			/* This is an in-object property. */
			if (mdb_vread(&ptr, sizeof (ptr), propaddr) == -1) {
				propinfo |= JPI_SKIPPED;
				v8_warn("object %p: failed to read in-object "
				    "property at %p", addr, propaddr);
				continue;
			}

			propinfo |= JPI_INOBJECT;
		} else if (propidx >= 0 && propidx < nprops) {
			/* Valid "properties" array property found. */
			ptr = props[propidx];
			propinfo |= JPI_PROPS;
		} else {
			/*
			 * Invalid "properties" array property found.  This can
			 * happen when properties are deleted.  If this value
			 * isn't obviously corrupt, we'll just silently ignore
			 * it.
			 */
			if (propidx < rndescs)
				continue;

			propinfo |= JPI_SKIPPED;
			v8_warn("object %p: property descriptor %d: "
			    "value index value out of bounds (%d)\n",
			    addr, ii, nprops);
			goto err;
		}

		/*
		 * If the property value doesn't look like a valid JavaScript
		 * object, mark this object as dubious.
		 */
		untagged = jsobj_layout_untagged(&layout, propidx);
		if (!untagged && jsobj_maybe_garbage(ptr))
			propinfo |= JPI_BADPROPS;
		if (untagged) {
			jsobj_propvalue_double(&value, makedouble(ptr));
		} else {
			jsobj_propvalue_addr(&value, ptr);
		}

		if (func(buf, &value, arg) != 0)
			goto err;
	}

	rval = 0;
	if (propinfop != NULL)
		*propinfop = propinfo;

err:
	if (props != NULL)
		mdb_free(props, nprops * sizeof (uintptr_t));

	if (descs != NULL)
		mdb_free(descs, ndescs * sizeof (uintptr_t));

	if (content != NULL && V8_PROP_IDX_CONTENT != -1)
		mdb_free(content, ncontent * sizeof (uintptr_t));

	return (rval);
}

/*
 * Layout descriptors
 *
 * See the comment above v8propvalue_t for important historical context about
 * the representation of JavaScript values in V8.  You really need to read that
 * to understand this, but the summary is that for objects whose properties are
 * stored directly inside the object, V8 may represent JavaScript property
 * values as either tagged values (which is what most of mdb_v8 knows about) or
 * untagged, unboxed doubles.  It's not possible to know from the native value
 * alone whether it represents a tagged value or an unboxed double, so the
 * object itself (via its Map) needs an additional bit of per-property metadata
 * to distinguish these cases.  That metadata is stored in a _layout
 * descriptor_, and it's generally implemented using a bit vector with one bit
 * per property; however, since the whole point of this is to save the tiniest
 * morsels of both heap space and computation time, V8 avoids using any space or
 * indirection that it doesn't absolutely need, so there are several cases.
 *
 *     (1) When double unboxing is disabled, rather than storing a layout
 *         descriptor indicating that all values are tagged, there's no layout
 *         descriptor in the Map at all.  As a result, we need to know whether
 *         double unboxing is enabled so that we can avoid checking the
 *         descriptor when this behavior is disabled.  There's no direct way for
 *         us to tell this, but double unboxing is currently enabled for all
 *         64-bit builds and disabled for all 32-bit builds.
 *
 *     (2) When the layout descriptor bitfield fits within a 31-bit value, the
 *         layout descriptor itself is represented as an SMI.  Bit N of the
 *         decoded SMI indicates whether the value for field N is tagged or not.
 *         Take this Map object:
 *
 *           +------------------------------------------+
 *           | Map                                      |
 *           | ---                                      |
 *           | ...                                      |
 *           | layout_descriptor: SMI value 0x4800001   |
 *           | ...                                      |
 *           +------------------------------------------+
 *
 *         0x4800001 is the SMI-decoded value.  The actual encoding for
 *         0x4800001 is different for 32-bit and 64-bit programs (though only
 *         64-bit programs currently use double unboxing).  Either way, we can
 *         reliably tell whether the value in the layout_descriptor is
 *         SMI-encoded based on its tag bits.
 *
 *         The binary representation for that decoded value looks like this:
 *
 *          bits 210987654321098765432109876543210
 *             +-----------------------------------+
 *             | 000000100100000000000000000000001 |  SMI representation
 *             +-------|--|----------------------|-+
 *                     |  |                      +--- field  0 is untagged
 *                     |  +-------------------------- field 23 is untagged
 *                     +----------------------------- field 26 is untagged
 *
 *         This indicates that the values for fields 0, 23, and 26 are unboxed
 *         doubles (i.e., the value stored inside the object is an actual double
 *         value).  The values for all other fields are normal tagged values
 *         (i.e., SMIs or HeapObjects).
 *
 *     (3) When the layout descriptor bitfield cannot fit within a 31-bit value,
 *         the layout descriptor itself is a tagged pointer to a FixedTypedArray
 *         of 32-bit integers, each of which represents the metadata for 32
 *         fields.
 *
 *
 *           +--------------------+           +----------------------------+
 *           | Map                |     +---> | FixedTypedArray<uint32_t>  |
 *           | ---                |     |     | -------------------------  |
 *           | ...                |     |     | ...                        |
 *           | layout_descriptor -------+     | length                     |
 *           | ...                |           | ...                        |
 *           +--------------------+           | 32-bit int 0               |
 *                                            | 32-bit int 1               |
 *                                            | 32-bit int ...             |
 *                                            +----------------------------+
 *
 *         Each of the 32-bit ints is interpreted just as the decoded value in
 *         case (2) above.
 *
 *     (4) Since untagged values are pretty uncommon, V8 avoids allocating
 *         32-bit integers to expand the bitfield only to store all zeroes.  The
 *         implementation then assumes that if it's asked whether field N is
 *         tagged, and there is no bit for field N (because its bit logically
 *         extends past the allocated bit vector), then field N is tagged.
 *
 *         To make this concrete, let's revisit case (2) above, where we had an
 *         object with 27 fields, and only fields 0, 23, and 26 are untagged.
 *         Any number of tagged fields can be added to this object without
 *         changing the layout_descriptor.  If it grows to contain 42 fields,
 *         but all of these new fields are tagged, then V8 knows the last fields
 *         are tagged because there are no bits for them.  If a 43rd field is
 *         added that's untagged, then V8 switches to case (3) using a
 *         two-element array.  Even then, any number of tagged fields can
 *         continue to be added without changing the representation.
 *
 * To summarize: the property bitfield is spread across a number of 32-bit
 * integers, and high-order zero-filled words are not stored.  Given that, if
 * only 31 bits are required, then the contents of that single 31-bit word are
 * encoded as an SMI and stored in the layout_descriptor directly.  Otherwise,
 * the layout_descriptor stores a pointer to an array of 32-bit integers.  In
 * both cases, you access the bit for property N by finding the appropriate word
 * (as N / 32) and then the appropriate bit in that word (N % 32).  If there
 * aren't that many words, then the value is assumed to be tagged.  This
 * algorithm is implemented in jsobj_layout_untagged().
 */
static int
jsobj_layout_load(jsobj_layout_t *layoutp, uintptr_t map)
{
#ifdef _LP64
	ssize_t off;
#endif

	bzero(layoutp, sizeof (*layoutp));

#ifdef _LP64
	if (V8_OFF_MAP_LAYOUT_DESCRIPTOR == -1) {
		assert(!(layoutp->jl_flags & JL_F_HASLAYOUT));
		return (0);
	}

	if (read_heap_ptr(&layoutp->jl_descriptor, map,
	    V8_OFF_MAP_LAYOUT_DESCRIPTOR) != 0) {
		return (-1);
	}

	if (V8_IS_SMI(layoutp->jl_descriptor)) {
		layoutp->jl_flags |= JL_F_HASLAYOUT;
		layoutp->jl_length = 1;
		layoutp->jl_bitvecs[0] = V8_SMI_VALUE(layoutp->jl_descriptor);
		if (layoutp->jl_bitvecs[0] == 0) {
			layoutp->jl_flags |= JL_F_ALLTAGGED;
		}

		return (0);
	}

	/*
	 * This is an incredibly cheesy implementation of a FixedTypedArray, but
	 * we don't have a lot of sample cases with which to test a more
	 * complete implementation.  If this becomes more widely used in V8, we
	 * should first-class this data structure so that we have crisper
	 * interfaces for working with it.
	 */
	if (heap_offset("FixedTypedArrayBase", "base_pointer", &off) == -1) {
		v8_warn("large-style layout descriptor: failed to configure\n");
		return (-1);
	}

	/*
	 * On V8 prior to 4.6.85.23, the data for a FixedTypedArray starts at
	 * the first double-aligned address after the base pointer.  With that
	 * V8 version (and possibly earlier), there's an extra pointer-sized
	 * value that we need to skip.
	 */
	off += sizeof (uintptr_t);
	if (!v8_version_current_older(4, 6, 85, 23))
		off += sizeof (uintptr_t);
	off += (sizeof (double) - 1);
	off &= ~(sizeof (double) - 1);

	if (read_heap_smi(&layoutp->jl_length, layoutp->jl_descriptor,
	    V8_OFF_FIXEDARRAY_LENGTH) != 0) {
		v8_warn("large-style layout descriptor: "
		    "failed to read length\n");
		return (-1);
	}

	if (layoutp->jl_length > JL_MAXBITVECS) {
		v8_warn("large-style layout descriptor: "
		    "length too large (%d)\n", layoutp->jl_length);
		return (-1);
	}

	if (mdb_vread(layoutp->jl_bitvecs,
	    layoutp->jl_length * sizeof (uint32_t),
	    V8_OFF_HEAP(layoutp->jl_descriptor + off)) == -1) {
		v8_warn("large-style layout descriptor: failed to read array");
		return (-1);
	}

	layoutp->jl_flags |= JL_F_HASLAYOUT | JL_F_ARRAY;
#endif

	return (0);
}

/*
 * Returns whether 0-indexed field "propidx" is an untagged field according to
 * the layout descriptor "layoutp".  The implementation is described in the
 * comment above jsobj_layout_load().
 */
static boolean_t
jsobj_layout_untagged(jsobj_layout_t *layoutp, uintptr_t propidx)
{
	int nbitsperword = 32;
	int mask, whichword, whichbit;
	uint32_t word;

	/*
	 * If there was no layout descriptor, then all fields are tagged.
	 */
	if (!(layoutp->jl_flags & JL_F_HASLAYOUT)) {
		return (B_FALSE);
	}

	if (layoutp->jl_flags & JL_F_ALLTAGGED) {
		return (B_FALSE);
	}

	/*
	 * jsobj_layout_load() normalizes the two main cases by turning the
	 * SMI-based representation into a one-element array.
	 */
	assert((layoutp->jl_flags & JL_F_ARRAY) != 0 ||
	    layoutp->jl_length == 1);
	whichword = propidx / nbitsperword;
	whichbit = propidx % nbitsperword;
	if (whichword >= layoutp->jl_length) {
		/*
		 * If the property's index is not contained in the layout
		 * descriptor, that means it's tagged.
		 */
		return (B_FALSE);
	}

	assert(whichword < JL_MAXBITVECS);
	word = layoutp->jl_bitvecs[whichword];
	mask = 1 << whichbit;
	return ((mask & word) != 0);
}

/*
 * Given the line endings table in "lendsp", computes the line number for the
 * given token position and print the result into "buf".  If "lendsp" is
 * undefined, prints the token position instead.
 */
static int
jsfunc_lineno(uintptr_t lendsp, uintptr_t tokpos,
    char *buf, size_t buflen, int *lineno)
{
	uintptr_t size, bufsz, lower, upper, ii = 0;
	uintptr_t *data;

	if (lineno != NULL)
		*lineno = -1;

	if (jsobj_is_undefined(lendsp)) {
		/*
		 * The token position is an SMI, but it comes in as its raw
		 * value so we can more easily compare it to values in the line
		 * endings table.  If we're just printing the position directly,
		 * we must convert it here, unless we're checking against the
		 * "-1" sentinel.
		 */
		if (tokpos == V8_VALUE_SMI(-1))
			mdb_snprintf(buf, buflen, "unknown position");
		else
			mdb_snprintf(buf, buflen, "position %d",
			    V8_SMI_VALUE(tokpos));

		if (lineno != NULL)
			*lineno = 0;

		return (0);
	}

	if (read_heap_smi(&size, lendsp, V8_OFF_FIXEDARRAY_LENGTH) != 0)
		return (-1);

	bufsz = size * sizeof (data[0]);

	if ((data = mdb_alloc(bufsz, UM_NOSLEEP)) == NULL) {
		v8_warn("failed to alloc %d bytes for FixedArray data", bufsz);
		return (-1);
	}

	if (mdb_vread(data, bufsz, lendsp + V8_OFF_FIXEDARRAY_DATA) != bufsz) {
		v8_warn("failed to read FixedArray data");
		mdb_free(data, bufsz);
		return (-1);
	}

	lower = 0;
	upper = size - 1;

	if (tokpos > data[upper]) {
		(void) strlcpy(buf, "position out of range", buflen);
		mdb_free(data, bufsz);

		if (lineno != NULL)
			*lineno = 0;

		return (0);
	}

	if (tokpos <= data[0]) {
		(void) strlcpy(buf, "line 1", buflen);
		mdb_free(data, bufsz);

		if (lineno != NULL)
			*lineno = 1;

		return (0);
	}

	while (upper >= 1) {
		ii = (lower + upper) >> 1;
		if (tokpos > data[ii])
			lower = ii + 1;
		else if (tokpos <= data[ii - 1])
			upper = ii - 1;
		else
			break;
	}

	if (lineno != NULL)
		*lineno = ii + 1;

	(void) mdb_snprintf(buf, buflen, "line %d", ii + 1);
	mdb_free(data, bufsz);
	return (0);
}

/*
 * Given a Script object, prints nlines on either side of lineno, with each
 * line prefixed by prefix (if non-NULL).
 */
static void
jsfunc_lines(uintptr_t scriptp,
    uintptr_t start, uintptr_t end, int nlines, char *prefix)
{
	uintptr_t src;
	char *buf, *bufp;
	size_t bufsz = 1024, len;
	int i, line, slop = 10;
	boolean_t newline = B_TRUE;
	int startline = -1, endline = -1;

	if (read_heap_ptr(&src, scriptp, V8_OFF_SCRIPT_SOURCE) != 0)
		return;

	for (;;) {
		if ((buf = mdb_zalloc(bufsz, UM_NOSLEEP)) == NULL) {
			mdb_warn("failed to allocate source code "
			    "buffer of size %d", bufsz);
			return;
		}

		bufp = buf;
		len = bufsz;

		if (jsstr_print(src, JSSTR_NUDE, &bufp, &len) != 0) {
			mdb_free(buf, bufsz);
			return;
		}

		if (len > slop)
			break;

		mdb_free(buf, bufsz);
		bufsz <<= 1;
	}

	if (end >= bufsz)
		return;

	/*
	 * First, take a pass to determine where our lines actually start.
	 */
	for (i = 0, line = 1; buf[i] != '\0'; i++) {
		if (buf[i] == '\n')
			line++;

		if (i == start)
			startline = line;

		if (i == end) {
			endline = line;
			break;
		}
	}

	if (startline == -1 || endline == -1) {
		mdb_warn("for script %p, could not determine startline/endline"
		    " (start %ld, end %ld, nlines %d)\n",
		    scriptp, start, end, nlines);
		mdb_free(buf, bufsz);
		return;
	}

	for (i = 0, line = 1; buf[i] != '\0'; i++) {
		if (buf[i] == '\n') {
			line++;
			newline = B_TRUE;
		}

		if (line < startline - nlines)
			continue;

		if (line > endline + nlines)
			break;

		mdb_printf("%c", buf[i]);

		if (newline) {
			if (line >= startline && line <= endline)
				mdb_printf("%<b>");

			if (prefix != NULL)
				mdb_printf(prefix, line);

			if (line >= startline && line <= endline)
				mdb_printf("%</b>");

			newline = B_FALSE;
		}
	}

	mdb_printf("\n");

	if (line == endline)
		mdb_printf("%</b>");

	mdb_free(buf, bufsz);
}

/*
 * Given a SharedFunctionInfo object, prints into bufp a name of the function
 * suitable for printing.  This function attempts to infer a name for anonymous
 * functions.
 *
 * This is an internal legacy interface.  Callers should use v8funcinfo_load()
 * and related functions instead.
 */
static int
jsfunc_name(uintptr_t funcinfop, char **bufp, size_t *lenp)
{
	mdbv8_strbuf_t strbuf;
	v8funcinfo_t *fip;
	int rv;

	mdbv8_strbuf_init(&strbuf, *bufp, *lenp);
	fip = v8funcinfo_load(funcinfop, UM_SLEEP);
	if (fip == NULL) {
		return (-1);
	}

	rv = v8funcinfo_funcname(fip, &strbuf, MSF_ASCIIONLY);
	v8funcinfo_free(fip);
	mdbv8_strbuf_legacy_update(&strbuf, bufp, lenp);
	return (rv);
}

/*
 * JavaScript-level object printing
 */

static void
jsobj_print_double(char **bufp, size_t *lenp, double numval)
{
	if (numval == (long long)numval)
		(void) bsnprintf(bufp, lenp, "%lld", (long long)numval);
	else
		(void) bsnprintf(bufp, lenp, "%e", numval);
}

static int
jsobj_print_value(v8propvalue_t *valp, jsobj_print_t *jsop)
{
	uint8_t type;
	const char *klass;
	uintptr_t addr;
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;

	const struct {
		char *name;
		int (*func)(uintptr_t, jsobj_print_t *);
	} table[] = {
		{ "HeapNumber", jsobj_print_number },
		{ "Oddball", jsobj_print_oddball },
		{ "JSObject", jsobj_print_jsobject },
		{ "JSArray", jsobj_print_jsarray },
		{ "JSTypedArray", jsobj_print_jstyped_array },
		{ "JSFunction", jsobj_print_jsfunction },
		{ "JSBoundFunction", jsobj_print_jsboundfunction },
		{ "JSDate", jsobj_print_jsdate },
		{ "JSRegExp", jsobj_print_jsregexp },
		{ NULL }
	}, *ent;

	if (jsop->jsop_baseaddr != NULL && jsop->jsop_member == NULL)
		(void) bsnprintf(bufp, lenp, "%p: ", jsop->jsop_baseaddr);

	if (jsop->jsop_printaddr && jsop->jsop_member == NULL)
		(void) bsnprintf(bufp, lenp, "%p: ",
		    valp == NULL ? NULL : valp->v8v_u.v8vu_addr);

	if (valp != NULL && valp->v8v_isboxeddouble) {
		jsobj_print_double(bufp, lenp, valp->v8v_u.v8vu_double);
		return (0);
	}

	addr = valp == NULL ? NULL : valp->v8v_u.v8vu_addr;
	if (V8_IS_SMI(addr)) {
		(void) bsnprintf(bufp, lenp, "%d", V8_SMI_VALUE(addr));
		return (0);
	}

	if (!V8_IS_HEAPOBJECT(addr)) {
		(void) bsnprintf(bufp, lenp, "<not a heap object>");
		return (-1);
	}

	if (read_typebyte(&type, addr) != 0) {
		(void) bsnprintf(bufp, lenp, "<couldn't read type>");
		return (-1);
	}

	if (V8_TYPE_STRING(type)) {
		size_t omax, maxstrlen;
		int rv;

		/*
		 * The undocumented -N option to ::jsprint puts an artificial
		 * limit on the length of strings printed out.  We implement
		 * this here by passing a smaller length to jsstr_print(), and
		 * then updating the real buffer length to match.
		 *
		 * This is mainly intended for dmod developers, as when printing
		 * out every object in a core file.  Many strings contain entire
		 * source code files and are largely not interesting.
		 */
		if (jsop->jsop_maxstrlen == 0 ||
		    jsop->jsop_maxstrlen >= *lenp) {
			maxstrlen = *lenp;
		} else {
			maxstrlen = jsop->jsop_maxstrlen;
		}

		omax = maxstrlen;
		rv = jsstr_print(addr, JSSTR_QUOTED, bufp, &maxstrlen);
		assert(maxstrlen <= omax);
		*lenp -= omax - maxstrlen;
		return (rv);
	}

	/*
	 * MutableHeapNumbers behave just like HeapNumbers, but do not have a
	 * separate class.
	 */
	if (type == V8_TYPE_MUTABLEHEAPNUMBER)
		type = V8_TYPE_HEAPNUMBER;

	klass = enum_lookup_str(v8_types, type, "<unknown>");

	for (ent = &table[0]; ent->name != NULL; ent++) {
		if (strcmp(klass, ent->name) == 0) {
			jsop->jsop_descended = B_TRUE;
			return (ent->func(addr, jsop));
		}
	}

	(void) bsnprintf(bufp, lenp,
	    "<unknown JavaScript object type \"%s\">", klass);
	return (-1);
}

static int
jsobj_print(uintptr_t addr, jsobj_print_t *jsop)
{
	v8propvalue_t value;
	jsobj_propvalue_addr(&value, addr);
	return (jsobj_print_value(&value, jsop));
}

static int
jsobj_print_number(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	double numval;

	if (read_heap_double(&numval, addr, V8_OFF_HEAPNUMBER_VALUE) == -1)
		return (-1);

	jsobj_print_double(bufp, lenp, numval);
	return (0);
}

static int
jsobj_print_oddball(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	uintptr_t strptr;

	if (read_heap_ptr(&strptr, addr, V8_OFF_ODDBALL_TO_STRING) != 0)
		return (-1);

	return (jsstr_print(strptr, JSSTR_NUDE, bufp, lenp));
}

static int
jsobj_print_prop(const char *desc, v8propvalue_t *val, void *arg)
{
	jsobj_print_t *jsop = arg, descend;
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;

	(void) bsnprintf(bufp, lenp, "%s\n%*s\"%s\": ", jsop->jsop_nprops == 0 ?
	    "{" : "", jsop->jsop_indent + 4, "", desc);

	descend = *jsop;
	descend.jsop_depth--;
	descend.jsop_indent += 4;

	(void) jsobj_print_value(val, &descend);
	(void) bsnprintf(bufp, lenp, ",");

	jsop->jsop_nprops++;

	return (0);
}

static int
jsobj_print_prop_member(const char *desc, v8propvalue_t *val, void *arg)
{
	jsobj_print_t *jsop = arg, descend;
	const char *member = jsop->jsop_member, *next = member;
	int rv;

	for (; *next != '\0' && *next != '.' && *next != '['; next++)
		continue;

	if (*member == '[') {
		mdb_warn("cannot use array indexing on an object\n");
		return (-1);
	}

	if (strncmp(member, desc, next - member) != 0)
		return (0);

	if (desc[next - member] != '\0')
		return (0);

	/*
	 * This property matches the desired member; descend.
	 */
	descend = *jsop;

	if (*next == '\0') {
		descend.jsop_member = NULL;
		descend.jsop_found = B_TRUE;
	} else {
		descend.jsop_member = *next == '.' ? next + 1 : next;
	}

	rv = jsobj_print_value(val, &descend);
	jsop->jsop_found = descend.jsop_found;

	return (rv);
}

static int
jsobj_print_jsobject(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;

	if (jsop->jsop_member != NULL)
		return (jsobj_properties(addr, jsobj_print_prop_member,
		    jsop, &jsop->jsop_propinfo));

	if (jsop->jsop_depth == 0) {
		(void) bsnprintf(bufp, lenp, "[...]");
		return (0);
	}

	jsop->jsop_nprops = 0;

	if (jsobj_properties(addr, jsobj_print_prop, jsop,
	    &jsop->jsop_propinfo) != 0)
		return (-1);

	if (jsop->jsop_nprops > 0) {
		(void) bsnprintf(bufp, lenp, "\n%*s", jsop->jsop_indent, "");
	} else if (jsop->jsop_nprops == 0) {
		(void) bsnprintf(bufp, lenp, "{");
	} else {
		(void) bsnprintf(bufp, lenp, "{ /* unknown property */ ");
	}

	(void) bsnprintf(bufp, lenp, "}");

	return (0);
}

static int
jsobj_print_jsarray_member(uintptr_t addr, jsobj_print_t *jsop)
{
	uintptr_t *elts;
	jsobj_print_t descend;
	uintptr_t ptr;
	const char *member = jsop->jsop_member, *end, *p;
	size_t elt = 0, place = 1, len, rv;
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;

	if (read_heap_ptr(&ptr, addr, V8_OFF_JSOBJECT_ELEMENTS) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<array member (failed to read elements)>");
		return (-1);
	}

	if (read_heap_array(ptr, &elts, &len, UM_SLEEP | UM_GC) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<array member (failed to read array)>");
		return (-1);
	}

	if (*member != '[') {
		mdb_warn("expected bracketed array index; "
		    "found '%s'\n", member);
		return (-1);
	}

	if ((end = strchr(member, ']')) == NULL) {
		mdb_warn("missing array index terminator\n");
		return (-1);
	}

	/*
	 * We know where our array index ends; convert it to an integer
	 * by stepping through it from least significant digit to most.
	 */
	for (p = end - 1; p > member; p--) {
		if (*p < '0' || *p > '9') {
			mdb_warn("illegal array index at '%c'\n", *p);
			return (-1);
		}

		elt += (*p - '0') * place;
		place *= 10;
	}

	if (place == 1) {
		mdb_warn("missing array index\n");
		return (-1);
	}

	if (elt >= len) {
		mdb_warn("array index %d exceeds size of %d\n", elt, len);
		return (-1);
	}

	descend = *jsop;

	switch (*(++end)) {
	case '\0':
		descend.jsop_member = NULL;
		descend.jsop_found = B_TRUE;
		break;

	case '.':
		descend.jsop_member = end + 1;
		break;

	case '[':
		descend.jsop_member = end;
		break;

	default:
		mdb_warn("illegal character '%c' following "
		    "array index terminator\n", *end);
		return (-1);
	}

	rv = jsobj_print(elts[elt], &descend);
	jsop->jsop_found = descend.jsop_found;

	return (rv);
}

static int
jsobj_print_jsarray(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	int indent = jsop->jsop_indent;
	jsobj_print_t descend;
	uintptr_t ptr;
	uintptr_t *elts;
	size_t ii, len;

	if (jsop->jsop_member != NULL)
		return (jsobj_print_jsarray_member(addr, jsop));

	if (jsop->jsop_depth == 0) {
		(void) bsnprintf(bufp, lenp, "[...]");
		return (0);
	}

	if (read_heap_ptr(&ptr, addr, V8_OFF_JSOBJECT_ELEMENTS) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<array (failed to read elements)>");
		return (-1);
	}

	if (read_heap_array(ptr, &elts, &len, UM_SLEEP | UM_GC) != 0) {
		(void) bsnprintf(bufp, lenp, "<array (failed to read array)>");
		return (-1);
	}

	if (len == 0) {
		(void) bsnprintf(bufp, lenp, "[]");
		return (0);
	}

	descend = *jsop;
	descend.jsop_depth--;
	descend.jsop_indent += 4;

	if (len == 1) {
		(void) bsnprintf(bufp, lenp, "[ ");
		(void) jsobj_print(elts[0], &descend);
		(void) bsnprintf(bufp, lenp, " ]");
		return (0);
	}

	(void) bsnprintf(bufp, lenp, "[\n");

	for (ii = 0; ii < len && *lenp > 0; ii++) {
		(void) bsnprintf(bufp, lenp, "%*s", indent + 4, "");
		(void) jsobj_print(elts[ii], &descend);
		(void) bsnprintf(bufp, lenp, ",\n");
	}

	(void) bsnprintf(bufp, lenp, "%*s", indent, "");
	(void) bsnprintf(bufp, lenp, "]");

	return (0);
}

static int
jsobj_print_jstyped_array(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	uintptr_t length;

	if (V8_OFF_JSTYPEDARRAY_LENGTH == -1 ||
	    read_heap_smi(&length, addr, V8_OFF_JSTYPEDARRAY_LENGTH) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<array (failed to read jstypedarray length)>");
		return (-1);
	}

	(void) bsnprintf(bufp, lenp, "<Typed array of length ");
	(void) bsnprintf(bufp, lenp, "%d>", (int)length);

	return (0);
}

static int
jsobj_print_jsfunction(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	uintptr_t shared;

	if (read_heap_ptr(&shared, addr, V8_OFF_JSFUNCTION_SHARED) != 0)
		return (-1);

	(void) bsnprintf(bufp, lenp, "function ");
	return (jsfunc_name(shared, bufp, lenp) != 0);
}

static int
jsobj_print_jsboundfunction(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;

	(void) bsnprintf(bufp, lenp, "<bound function>");
	return (0);
}

static int
jsobj_print_jsdate(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	char buf[128];
	uintptr_t value;
	uint8_t type;
	double numval;

	if (V8_OFF_JSDATE_VALUE == -1) {
		(void) bsnprintf(bufp, lenp, "<JSDate>", buf);
		return (0);
	}

	if (read_heap_ptr(&value, addr, V8_OFF_JSDATE_VALUE) != 0) {
		(void) bsnprintf(bufp, lenp, "<JSDate (failed to read value)>");
		return (-1);
	}

	if (V8_IS_SMI(value)) {
		numval = V8_SMI_VALUE(value);
	} else {
		if (read_typebyte(&type, value) != 0) {
			(void) bsnprintf(bufp, lenp,
			    "<JSDate (failed to read type)>");
			return (-1);
		}

		if (strcmp(enum_lookup_str(v8_types, type, ""),
		    "HeapNumber") != 0) {
			(void) bsnprintf(bufp, lenp,
			    "<JSDate (value has unexpected type)>");
			return (-1);
		}

		if (read_heap_double(&numval, value,
		    V8_OFF_HEAPNUMBER_VALUE) == -1) {
			(void) bsnprintf(bufp, lenp,
			    "<JSDate (failed to read num)>");
			return (-1);
		}
	}

	mdb_snprintf(buf, sizeof (buf), "%Y",
	    (time_t)((long long)numval / MILLISEC));
	(void) bsnprintf(bufp, lenp, "%lld (%s)", (long long)numval, buf);

	return (0);
}

static int
jsobj_print_jsregexp(uintptr_t addr, jsobj_print_t *jsop)
{
	char **bufp = jsop->jsop_bufp;
	size_t *lenp = jsop->jsop_lenp;
	uintptr_t datap, source;
	uintptr_t *data;
	size_t datalen;
	int source_index = 1;

	if (V8_OFF_JSREGEXP_DATA == -1) {
		(void) bsnprintf(bufp, lenp, "<JSRegExp>");
		return (0);
	}

	if (read_heap_ptr(&datap, addr, V8_OFF_JSREGEXP_DATA) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<JSRegExp (failed to read data)>");
		return (-1);
	}

	if (read_heap_array(datap, &data, &datalen, UM_SLEEP | UM_GC) != 0) {
		(void) bsnprintf(bufp, lenp,
		    "<JSRegExp (failed to read array)>");
		return (-1);
	}

	/*
	 * The value for "source_index" here is unchanged from Node v0.6 through
	 * Node v0.12, but should ideally come from v8 debug metadata.
	 */
	if (datalen < source_index + 1) {
		(void) bsnprintf(bufp, lenp, "<JSRegExp (array too small)>");
		return (-1);
	}

	source = data[source_index];
	(void) bsnprintf(bufp, lenp, "JSRegExp: ");
	(void) jsstr_print(source, JSSTR_QUOTED, bufp, lenp);
	return (0);
}

/*
 * dcmd implementations
 */

/* ARGSUSED */
static int
dcmd_v8classes(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8_class_t *clp;

	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next)
		mdb_printf("%s\n", clp->v8c_name);

	return (DCMD_OK);
}

static int
do_v8code(v8code_t *codep, boolean_t opt_d)
{
	uintptr_t instrstart, instrsize;

	instrstart = v8code_instructions_start(codep);
	instrsize = v8code_instructions_size(codep);

	mdb_printf("code: %p\n", v8code_addr(codep));
	mdb_printf("instructions: [%p, %p)\n", instrstart,
	    instrstart + instrsize);

	if (!opt_d)
		return (DCMD_OK);

	mdb_set_dot(instrstart);

	do {
		(void) mdb_inc_indent(8); /* gets reset by mdb_eval() */

		/*
		 * This is absolutely awful. We want to disassemble the above
		 * range of instructions.  Because we don't know how many there
		 * are, we can't use "::dis".  We resort to evaluating "./i",
		 * but then we need to advance "." by the size of the
		 * instruction just printed.  The only way to do that is by
		 * printing out "+", but we don't want that to show up, so we
		 * redirect it to /dev/null.
		 */
		if (mdb_eval("/i") != 0 ||
		    mdb_eval("+=p ! cat > /dev/null") != 0) {
			(void) mdb_dec_indent(8);
			v8_warn("failed to disassemble at %p", mdb_get_dot());
			return (DCMD_ERR);
		}
	} while (mdb_get_dot() < instrstart + instrsize);

	(void) mdb_dec_indent(8);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8code(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	boolean_t opt_d = B_FALSE;
	v8code_t *codep;
	int rv;

	if (mdb_getopts(argc, argv, 'd', MDB_OPT_SETBITS, B_TRUE, &opt_d,
	    NULL) != argc)
		return (DCMD_USAGE);

	codep = v8code_load(addr, UM_NOSLEEP | UM_GC);
	if (codep == NULL) {
		return (DCMD_ERR);
	}

	rv = do_v8code(codep, opt_d);
	v8code_free(codep);
	return (rv);
}

/* ARGSUSED */
static int
dcmd_v8function(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	boolean_t opt_d = B_FALSE;
	v8function_t *fp = NULL;
	v8context_t *ctxp = NULL;
	v8scopeinfo_t *sip = NULL;
	v8funcinfo_t *fip = NULL;
	v8code_t *codep = NULL;
	mdbv8_strbuf_t *strb = NULL;
	int rv = DCMD_ERR;

	if (mdb_getopts(argc, argv, 'd', MDB_OPT_SETBITS, B_TRUE, &opt_d,
	    NULL) != argc)
		return (DCMD_USAGE);

	v8_warnings++;

	if ((fp = v8function_load(addr, UM_NOSLEEP)) == NULL ||
	    (ctxp = v8function_context(fp, UM_NOSLEEP)) == NULL ||
	    (fip = v8function_funcinfo(fp, UM_NOSLEEP)) == NULL ||
	    (codep = v8funcinfo_code(fip, UM_NOSLEEP)) == NULL ||
	    (strb = mdbv8_strbuf_alloc(512, UM_NOSLEEP)) == NULL) {
		goto out;
	}

	mdbv8_strbuf_sprintf(strb, "%p: JSFunction: ", addr);
	(void) v8funcinfo_funcname(fip, strb, MSF_ASCIIONLY);
	mdbv8_strbuf_sprintf(strb, "\n");

	mdbv8_strbuf_sprintf(strb, "defined at ");
	(void) v8funcinfo_scriptpath(fip, strb, MSF_ASCIIONLY);
	mdbv8_strbuf_sprintf(strb, " ");
	(void) v8funcinfo_definition_location(fip, strb, MSF_ASCIIONLY);
	mdb_printf("%s\n", mdbv8_strbuf_tocstr(strb));

	mdb_printf("context: %p\n", v8context_addr(ctxp));
	sip = v8function_scopeinfo(fp, UM_NOSLEEP);
	if (sip == NULL) {
		mdb_printf("shared scope_info not available\n");
	} else {
		mdb_printf("shared scope_info: %p\n", v8scopeinfo_addr(sip));
	}

	rv = do_v8code(codep, opt_d);

out:
	v8code_free(codep);
	v8funcinfo_free(fip);
	v8scopeinfo_free(sip);
	v8context_free(ctxp);
	v8function_free(fp);
	mdbv8_strbuf_free(strb);
	v8_warnings--;
	return (rv);
}

/*
 * Access an internal field of a V8 object.
 */
/* ARGSUSED */
static int
dcmd_v8internal(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t idx;
	uintptr_t fieldaddr;

	if (mdb_getopts(argc, argv, NULL) != argc - 1 ||
	    argv[argc - 1].a_type != MDB_TYPE_STRING)
		return (DCMD_USAGE);

	idx = mdb_strtoull(argv[argc - 1].a_un.a_str);
	if (obj_v8internal(addr, idx, &fieldaddr) != 0)
		return (DCMD_ERR);

	mdb_printf("%p\n", fieldaddr);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8frametypes(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	enum_print(v8_frametypes);
	return (DCMD_OK);
}

static void
dcmd_v8print_help(void)
{
	mdb_printf(
	    "Prints out \".\" (a V8 heap object) as an instance of its C++\n"
	    "class.  With no arguments, the appropriate class is detected\n"
	    "automatically.  The 'class' argument overrides this to print an\n"
	    "object as an instance of the given class.  The list of known\n"
	    "classes can be viewed with ::jsclasses.");
}

/* ARGSUSED */
static int
dcmd_v8print(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	const char *rqclass;
	v8_class_t *clp;
	char *bufp;
	size_t len;
	uint8_t type;
	char buf[256];

	if (argc < 1) {
		/*
		 * If no type was specified, determine it automatically.
		 */
		bufp = buf;
		len = sizeof (buf);
		if (obj_jstype(addr, &bufp, &len, &type) != 0)
			return (DCMD_ERR);

		if (type == 0) {
			/* For SMI or Failure, just print out the type. */
			mdb_printf("%s\n", buf);
			return (DCMD_OK);
		}

		if ((rqclass = enum_lookup_str(v8_types, type, NULL)) == NULL) {
			v8_warn("object has unknown type\n");
			return (DCMD_ERR);
		}
	} else {
		if (argv[0].a_type != MDB_TYPE_STRING)
			return (DCMD_USAGE);

		rqclass = argv[0].a_un.a_str;
	}

	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next) {
		if (strcmp(rqclass, clp->v8c_name) == 0)
			break;
	}

	if (clp == NULL) {
		v8_warn("unknown class '%s'\n", rqclass);
		return (DCMD_USAGE);
	}

	return (obj_print_class(addr, clp));
}

static int do_v8scopeinfo_vartype_print(v8scopeinfo_t *, v8scopeinfo_vartype_t,
    void *);
static int do_v8scopeinfo_var_print(v8scopeinfo_t *, v8scopeinfo_var_t *,
    void *);

/* ARGSUSED */
static int
dcmd_v8scopeinfo(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8scopeinfo_t *sip;

	if ((sip = v8scopeinfo_load(addr, UM_SLEEP | UM_GC)) == NULL) {
		mdb_warn("failed to load ScopeInfo");
		return (DCMD_ERR);
	}

	if (v8scopeinfo_iter_vartypes(sip, do_v8scopeinfo_vartype_print,
	    NULL) != 0) {
		mdb_warn("failed to walk scope info");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static int
do_v8scopeinfo_vartype_print(v8scopeinfo_t *sip,
    v8scopeinfo_vartype_t scopevartype, void *arg)
{
	size_t nvars;
	const char *label;

	nvars = v8scopeinfo_vartype_nvars(sip, scopevartype);
	label = v8scopeinfo_vartype_name(scopevartype);
	mdb_printf("%d %s%s\n", nvars, label, nvars == 1 ? "" : "s");
	return (v8scopeinfo_iter_vars(sip, scopevartype,
	    do_v8scopeinfo_var_print, (void *)label));
}

static int
do_v8scopeinfo_var_print(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp, void *arg)
{
	const char *label = arg;
	uintptr_t namestr;
	char buf[64];
	char *bufp;
	size_t buflen;

	namestr = v8scopeinfo_var_name(sip, sivp);
	mdb_printf("    %s %d: %p", label, v8scopeinfo_var_idx(sip, sivp),
	    namestr);

	bufp = buf;
	buflen = sizeof (buf);
	if (jsstr_print(namestr, JSSTR_QUOTED, &bufp, &buflen) == 0) {
		mdb_printf(" (%s)\n", buf);
	} else {
		mdb_printf("\n");
	}

	return (0);
}


static int do_v8context_static_slot(v8context_t *, const char *,
    uintptr_t, void *);
static int do_v8context_dynamic_slot(v8context_t *, uint_t, uintptr_t, void *);

/* ARGSUSED */
static int
dcmd_v8context(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8context_t *ctxp;

	if ((ctxp = v8context_load(addr, UM_SLEEP | UM_GC)) == NULL) {
		mdb_warn("failed to load Context\n");
		return (DCMD_ERR);
	}

	if (v8context_iter_static_slots(ctxp, do_v8context_static_slot,
	    NULL) != 0 ||
	    v8context_iter_dynamic_slots(ctxp, do_v8context_dynamic_slot,
	    NULL) != 0) {
		mdb_warn("failed to iterate context\n");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static int
do_v8context_static_slot(v8context_t *ctxp, const char *label, uintptr_t value,
    void *arg)
{
	char buf[64];
	char *bufp;
	size_t bufsz;

	mdb_printf("%s: %p", label, value);

	bufp = buf;
	bufsz = sizeof (buf);
	if (obj_jstype(value, &bufp, &bufsz, NULL) == 0) {
		mdb_printf(" (%s)\n", buf);
	} else {
		mdb_printf("\n");
	}

	return (0);
}

static int
do_v8context_dynamic_slot(v8context_t *ctxp, uint_t which, uintptr_t value,
    void *arg)
{
	char buf[16];
	(void) snprintf(buf, sizeof (buf), "    slot %d", which);
	return (do_v8context_static_slot(ctxp, buf, value, arg));
}


/* ARGSUSED */
static int
dcmd_v8type(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	char buf[64];
	char *bufp = buf;
	size_t len = sizeof (buf);

	if (obj_jstype(addr, &bufp, &len, NULL) != 0)
		return (DCMD_ERR);

	mdb_printf("0x%p: %s\n", addr, buf);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8types(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	enum_print(v8_types);
	return (DCMD_OK);
}

static int
load_current_context(uintptr_t *fpp, uintptr_t *raddrp)
{
	mdb_reg_t regfp, regip;

#ifdef __amd64
	if (mdb_getareg(1, "rbp", &regfp) != 0 ||
	    mdb_getareg(1, "rip", &regip) != 0) {
#else
#ifdef __i386
	if (mdb_getareg(1, "ebp", &regfp) != 0 ||
	    mdb_getareg(1, "eip", &regip) != 0) {
#else
#error Unrecognized microprocessor
#endif
#endif
		v8_warn("failed to load current context");
		return (-1);
	}

	if (fpp != NULL)
		*fpp = (uintptr_t)regfp;

	if (raddrp != NULL)
		*raddrp = (uintptr_t)regip;

	return (0);
}

typedef struct jsframe {
	boolean_t	jsf_showall;	/* show hidden frames and pointers */
	boolean_t	jsf_verbose;	/* show arguments and JS code */
	char		*jsf_func;	/* filter frames for named function */
	char		*jsf_prop;	/* filter arguments */
	uintptr_t	jsf_nlines;	/* lines of context (for verbose) */
	uint_t		jsf_nskipped;	/* skipped frames */
} jsframe_t;

static void
jsframe_skip(jsframe_t *jsf)
{
	jsf->jsf_nskipped++;
}

static void
jsframe_print_skipped(jsframe_t *jsf)
{
	if (jsf->jsf_nskipped == 1)
		mdb_printf("        (1 internal frame elided)\n");
	else if (jsf->jsf_nskipped > 1)
		mdb_printf("        (%d internal frames elided)\n",
		    jsf->jsf_nskipped);
	jsf->jsf_nskipped = 0;
}

static int
do_jsframe_special(uintptr_t fptr, uintptr_t raddr, jsframe_t *jsf)
{
	uint_t count;
	uintptr_t ftype;
	const char *ftypename;
	char *prop = jsf->jsf_prop;
	uintptr_t internal_frametype_addr;

	/*
	 * First see if this looks like a native frame rather than a JavaScript
	 * frame.  We check this by asking MDB to print the return address
	 * symbolically.  If that works, we assume this was NOT a V8 frame,
	 * since those are never in the symbol table.
	 */
	count = mdb_snprintf(NULL, 0, "%A", raddr);
	if (count > 1) {
		if (prop != NULL)
			return (0);

		jsframe_print_skipped(jsf);
		if (jsf->jsf_showall) {
			mdb_printf("%p %a\n", fptr, raddr);
		} else if (count <= 65) {
			mdb_printf("native: %a\n", raddr);
		} else {
			char buf[65];
			mdb_snprintf(buf, sizeof (buf), "%a", raddr);
			mdb_printf("native: %s...\n", buf);
		}
		return (0);
	}

	/*
	 * Figure out what kind of internal frame this is using the same
	 * algorithm as V8's ComputeType function.
	 */

	/*
	 * With versions of V8 < 5.1.71 (and node < v6.5.0), an ArgumentsAdaptor
	 * frame was marked by a specific SMI value at an address equivalent to
	 * the frame pointer + context offset, which is a different offset than
	 * the one used to determine the type of other internal frames. For
	 * later versions of V8, all internal (special) frames are identified
	 * by a value at the same offset, so there's no need to special case
	 * ArgumentsAdaptor frames.
	 */
	if (v8_version_current_older(5, 1, 0, 0) &&
	    mdb_vread(&ftype, sizeof (ftype), fptr + V8_OFF_FP_CONTEXT) != -1 &&
	    V8_IS_SMI(ftype) &&
	    (ftypename = enum_lookup_str(v8_frametypes, V8_SMI_VALUE(ftype),
	    NULL)) != NULL && strstr(ftypename, "ArgumentsAdaptor") != NULL) {
		if (prop != NULL)
			return (0);

		if (jsf->jsf_showall) {
			jsframe_print_skipped(jsf);
			mdb_printf("%p %a <%s>\n", fptr, raddr,
			    ftypename);
		} else {
			jsframe_skip(jsf);
		}
		return (0);
	}

	internal_frametype_addr = fptr + V8_OFF_FP_CONTEXT_OR_FRAME_TYPE;
	if (mdb_vread(&ftype, sizeof (ftype), internal_frametype_addr) != -1 &&
	    V8_IS_SMI(ftype)) {
		if (prop != NULL)
			return (0);

		ftypename = enum_lookup_str(v8_frametypes, V8_SMI_VALUE(ftype),
		    NULL);

		if (jsf->jsf_showall && ftypename != NULL) {
			jsframe_print_skipped(jsf);
			mdb_printf("%p %a <%s>\n", fptr, raddr, ftypename);
		} else {
			jsframe_skip(jsf);
		}

		return (0);
	}

	return (-1);
}

static int
do_jsframe(uintptr_t fptr, uintptr_t raddr, jsframe_t *jsf)
{
	boolean_t showall = jsf->jsf_showall;
	boolean_t verbose = jsf->jsf_verbose;
	const char *func = jsf->jsf_func;
	const char *prop = jsf->jsf_prop;
	uintptr_t nlines = jsf->jsf_nlines;

	uintptr_t funcp, funcinfop, tokpos, endpos, scriptp, lendsp, ptrp;
	uintptr_t ii, nargs;
	const char *typename;
	char *bufp;
	size_t len;
	uint8_t type;
	char buf[256];
	int lineno;

	/*
	 * Check for non-JavaScript frames first.
	 */
	if (func == NULL && do_jsframe_special(fptr, raddr, jsf) == 0)
		return (DCMD_OK);

	/*
	 * At this point we assume we're looking at a JavaScript frame.  As with
	 * native frames, fish the address out of the parent frame.
	 */
	if (mdb_vread(&funcp, sizeof (funcp),
	    fptr + V8_OFF_FP_FUNCTION) == -1) {
		v8_warn("failed to read stack at %p",
		    fptr + V8_OFF_FP_FUNCTION);
		return (DCMD_ERR);
	}

	/*
	 * Check if this thing is really a JSFunction at all. For some frames,
	 * it's a Code object, presumably indicating some internal frame.
	 */
	if (read_typebyte(&type, funcp) != 0 ||
	    (typename = enum_lookup_str(v8_types, type, NULL)) == NULL) {
		if (func != NULL || prop != NULL)
			return (DCMD_OK);

		if (showall) {
			jsframe_print_skipped(jsf);
			mdb_printf("%p %a\n", fptr, raddr);
		} else {
			jsframe_skip(jsf);
		}
		return (DCMD_OK);
	}

	if (strcmp("Code", typename) == 0) {
		if (func != NULL || prop != NULL)
			return (DCMD_OK);

		if (showall) {
			jsframe_print_skipped(jsf);
			mdb_printf("%p %a internal (Code: %p)\n",
			    fptr, raddr, funcp);
		} else {
			jsframe_skip(jsf);
		}
		return (DCMD_OK);
	}

	if (strcmp("JSFunction", typename) != 0) {
		if (func != NULL || prop != NULL)
			return (DCMD_OK);

		if (showall) {
			jsframe_print_skipped(jsf);
			mdb_printf("%p %a unknown (%s: %p)",
			    fptr, raddr, typename, funcp);
		} else {
			jsframe_skip(jsf);
		}
		return (DCMD_OK);
	}

	if (read_heap_ptr(&funcinfop, funcp, V8_OFF_JSFUNCTION_SHARED) != 0)
		return (DCMD_ERR);

	bufp = buf;
	len = sizeof (buf);
	if (jsfunc_name(funcinfop, &bufp, &len) != 0)
		return (DCMD_ERR);

	if (func != NULL && strcmp(buf, func) != 0)
		return (DCMD_OK);

	if (prop == NULL) {
		jsframe_print_skipped(jsf);
		if (showall)
			mdb_printf("%p %a ", fptr, raddr);
		else
			mdb_printf("js:     ");
		mdb_printf("%s", buf);
		if (showall)
			mdb_printf(" (JSFunction: %p)\n", funcp);
		else
			mdb_printf("\n");
	}

	if (!verbose && prop == NULL)
		return (DCMD_OK);

	if (verbose)
		jsframe_print_skipped(jsf);

	/*
	 * Although the token position is technically an SMI, we're going to
	 * byte-compare it to other SMI values so we don't want decode it here.
	 */
	if (read_heap_maybesmi(&tokpos, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION) != 0)
		return (DCMD_ERR);
	tokpos = V8_VALUE_SMI(tokpos);

	if (read_heap_ptr(&scriptp, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0)
		return (DCMD_ERR);

	if (read_heap_ptr(&ptrp, scriptp, V8_OFF_SCRIPT_NAME) != 0)
		return (DCMD_ERR);

	bufp = buf;
	len = sizeof (buf);
	(void) jsstr_print(ptrp, JSSTR_NUDE, &bufp, &len);

	if (prop != NULL && strcmp(prop, "file") == 0) {
		mdb_printf("%s\n", buf);
		return (DCMD_OK);
	}

	if (prop == NULL) {
		(void) mdb_inc_indent(10);
		mdb_printf("file: %s\n", buf);
	}

	if (read_heap_ptr(&lendsp, scriptp, V8_OFF_SCRIPT_LINE_ENDS) != 0)
		return (DCMD_ERR);

	(void) jsfunc_lineno(lendsp, tokpos, buf, sizeof (buf), &lineno);

	if (prop != NULL && strcmp(prop, "posn") == 0) {
		mdb_printf("%s\n", buf);
		return (DCMD_OK);
	}

	if (prop == NULL)
		mdb_printf("posn: %s\n", buf);

	if (read_heap_maybesmi(&nargs, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_LENGTH) == 0) {
		uintptr_t argptr;
		char arg[10];

		if (mdb_vread(&argptr, sizeof (argptr),
		    fptr + V8_OFF_FP_ARGS + nargs * sizeof (uintptr_t)) != -1 &&
		    argptr != NULL) {
			(void) snprintf(arg, sizeof (arg), "this");
			if (prop != NULL && strcmp(arg, prop) == 0) {
				mdb_printf("%p\n", argptr);
				return (DCMD_OK);
			}

			if (prop == NULL) {
				bufp = buf;
				len = sizeof (buf);
				(void) obj_jstype(argptr, &bufp, &len, NULL);

				mdb_printf("this: %p (%s)\n", argptr, buf);
			}
		}

		for (ii = 0; ii < nargs; ii++) {
			if (mdb_vread(&argptr, sizeof (argptr),
			    fptr + V8_OFF_FP_ARGS + (nargs - ii - 1) *
			    sizeof (uintptr_t)) == -1)
				continue;

			(void) snprintf(arg, sizeof (arg), "arg%" PRIuPTR,
			    ii + 1);

			if (prop != NULL) {
				if (strcmp(arg, prop) != 0)
					continue;

				mdb_printf("%p\n", argptr);
				return (DCMD_OK);
			}

			bufp = buf;
			len = sizeof (buf);
			(void) obj_jstype(argptr, &bufp, &len, NULL);

			mdb_printf("arg%d: %p (%s)\n", (ii + 1), argptr, buf);
		}
	}


	if (prop != NULL) {
		mdb_warn("unknown frame property '%s'\n", prop);
		return (DCMD_ERR);
	}

	if (nlines != 0 && read_heap_maybesmi(&endpos, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_END_POSITION) == 0) {
		jsfunc_lines(scriptp,
		    V8_SMI_VALUE(tokpos), endpos, nlines, "%5d ");
		mdb_printf("\n");
	}

	(void) mdb_dec_indent(10);

	return (DCMD_OK);
}

typedef struct findjsobjects_prop {
	struct findjsobjects_prop *fjsp_next;
	char fjsp_desc[1];
} findjsobjects_prop_t;

typedef struct findjsobjects_instance {
	uintptr_t fjsi_addr;
	struct findjsobjects_instance *fjsi_next;
} findjsobjects_instance_t;

typedef struct findjsobjects_obj {
	findjsobjects_prop_t *fjso_props;
	findjsobjects_prop_t *fjso_last;
	jspropinfo_t fjso_propinfo;
	size_t fjso_nprops;
	findjsobjects_instance_t fjso_instances;
	int fjso_ninstances;
	avl_node_t fjso_node;
	struct findjsobjects_obj *fjso_next;
	boolean_t fjso_malformed;
	char fjso_constructor[80];
} findjsobjects_obj_t;

typedef struct findjsobjects_func {
	findjsobjects_instance_t fjsf_instances;
	int fjsf_ninstances;
	avl_node_t fjsf_node;
	struct findjsobjects_func *fjsf_next;
	uintptr_t fjsf_shared;
	char fjsf_funcname[40];
	char fjsf_scriptname[80];
	char fjsf_location[20];
} findjsobjects_func_t;

typedef struct findjsobjects_stats {
	int fjss_heapobjs;
	int fjss_cached;
	int fjss_typereads;
	int fjss_jsobjs;
	int fjss_objects;
	int fjss_garbage;
	int fjss_arrays;
	int fjss_uniques;
	int fjss_funcs;
	int fjss_funcs_skipped;
	int fjss_funcs_unique;
} findjsobjects_stats_t;

typedef struct findjsobjects_reference {
	uintptr_t fjsrf_addr;
	char *fjsrf_desc;
	size_t fjsrf_index;
	struct findjsobjects_reference *fjsrf_next;
} findjsobjects_reference_t;

typedef struct findjsobjects_referent {
	avl_node_t fjsr_node;
	uintptr_t fjsr_addr;
	findjsobjects_reference_t *fjsr_head;
	findjsobjects_reference_t *fjsr_tail;
	struct findjsobjects_referent *fjsr_next;
} findjsobjects_referent_t;

typedef struct findjsobjects_state {
	uintptr_t fjs_addr;
	uintptr_t fjs_size;
	boolean_t fjs_verbose;
	boolean_t fjs_brk;
	boolean_t fjs_allobjs;
	boolean_t fjs_initialized;
	boolean_t fjs_marking;
	boolean_t fjs_referred;
	boolean_t fjs_finished;
	avl_tree_t fjs_tree;
	avl_tree_t fjs_referents;
	avl_tree_t fjs_funcinfo;
	findjsobjects_referent_t *fjs_head;
	findjsobjects_referent_t *fjs_tail;
	findjsobjects_obj_t *fjs_current;
	findjsobjects_obj_t *fjs_objects;
	findjsobjects_func_t *fjs_funcs;
	findjsobjects_stats_t fjs_stats;
} findjsobjects_state_t;

findjsobjects_obj_t *
findjsobjects_alloc(uintptr_t addr)
{
	findjsobjects_obj_t *obj;

	obj = mdb_zalloc(sizeof (findjsobjects_obj_t), UM_SLEEP);
	obj->fjso_instances.fjsi_addr = addr;
	obj->fjso_ninstances = 1;

	return (obj);
}

void
findjsobjects_free(findjsobjects_obj_t *obj)
{
	findjsobjects_prop_t *prop, *next;

	for (prop = obj->fjso_props; prop != NULL; prop = next) {
		next = prop->fjsp_next;
		mdb_free(prop, sizeof (findjsobjects_prop_t) +
		    strlen(prop->fjsp_desc));
	}

	mdb_free(obj, sizeof (findjsobjects_obj_t));
}

int
findjsobjects_cmp(findjsobjects_obj_t *lhs, findjsobjects_obj_t *rhs)
{
	findjsobjects_prop_t *lprop, *rprop;
	int rv;

	/*
	 * Don't group malformed objects with normal ones or vice versa.
	 */
	if (lhs->fjso_malformed != rhs->fjso_malformed)
		return (lhs->fjso_malformed ? -1 : 1);

	lprop = lhs->fjso_props;
	rprop = rhs->fjso_props;

	while (lprop != NULL && rprop != NULL) {
		if ((rv = strcmp(lprop->fjsp_desc, rprop->fjsp_desc)) != 0)
			return (rv > 0 ? 1 : -1);

		lprop = lprop->fjsp_next;
		rprop = rprop->fjsp_next;
	}

	if (lprop != NULL)
		return (1);

	if (rprop != NULL)
		return (-1);

	if (lhs->fjso_nprops > rhs->fjso_nprops)
		return (1);

	if (lhs->fjso_nprops < rhs->fjso_nprops)
		return (-1);

	rv = strcmp(lhs->fjso_constructor, rhs->fjso_constructor);

	return (rv < 0 ? -1 : rv > 0 ? 1 : 0);
}

int
findjsobjects_cmp_funcinfo(findjsobjects_func_t *lhs,
    findjsobjects_func_t *rhs)
{
	int diff = lhs->fjsf_shared - rhs->fjsf_shared;
	return (diff < 0 ? -1 : diff > 0 ? 1 : 0);
}

int
findjsobjects_cmp_referents(findjsobjects_referent_t *lhs,
    findjsobjects_referent_t *rhs)
{
	if (lhs->fjsr_addr < rhs->fjsr_addr)
		return (-1);

	if (lhs->fjsr_addr > rhs->fjsr_addr)
		return (1);

	return (0);
}

int
findjsobjects_cmp_ninstances(const void *l, const void *r)
{
	findjsobjects_obj_t *lhs = *((findjsobjects_obj_t **)l);
	findjsobjects_obj_t *rhs = *((findjsobjects_obj_t **)r);
	size_t lprod = lhs->fjso_ninstances * lhs->fjso_nprops;
	size_t rprod = rhs->fjso_ninstances * rhs->fjso_nprops;

	if (lprod < rprod)
		return (-1);

	if (lprod > rprod)
		return (1);

	if (lhs->fjso_ninstances < rhs->fjso_ninstances)
		return (-1);

	if (lhs->fjso_ninstances > rhs->fjso_ninstances)
		return (1);

	if (lhs->fjso_nprops < rhs->fjso_nprops)
		return (-1);

	if (lhs->fjso_nprops > rhs->fjso_nprops)
		return (1);

	return (0);
}

/*ARGSUSED*/
int
findjsobjects_prop(const char *desc, v8propvalue_t *val, void *arg)
{
	findjsobjects_state_t *fjs = arg;
	findjsobjects_obj_t *current = fjs->fjs_current;
	findjsobjects_prop_t *prop;

	if (desc == NULL)
		desc = "<unknown>";

	prop = mdb_zalloc(sizeof (findjsobjects_prop_t) +
	    strlen(desc), UM_SLEEP);

	strcpy(prop->fjsp_desc, desc);

	if (current->fjso_last != NULL) {
		current->fjso_last->fjsp_next = prop;
	} else {
		current->fjso_props = prop;
	}

	current->fjso_last = prop;
	current->fjso_nprops++;
	current->fjso_malformed =
	    val == NULL && current->fjso_nprops == 1 && desc[0] == '<';

	return (0);
}

static void
findjsobjects_constructor(findjsobjects_obj_t *obj)
{
	char *bufp = obj->fjso_constructor;
	size_t len = sizeof (obj->fjso_constructor);
	uintptr_t map, funcinfop;
	uintptr_t addr = obj->fjso_instances.fjsi_addr;
	uint8_t type;

	v8_silent++;

	if (read_heap_ptr(&map, addr, V8_OFF_HEAPOBJECT_MAP) != 0 ||
	    get_map_constructor(&addr, map) != 0)
		goto out;

	if (read_typebyte(&type, addr) != 0)
		goto out;

	if (type != V8_TYPE_JSFUNCTION)
		goto out;

	if (read_heap_ptr(&funcinfop, addr, V8_OFF_JSFUNCTION_SHARED) != 0)
		goto out;

	if (jsfunc_name(funcinfop, &bufp, &len) != 0)
		goto out;
out:
	v8_silent--;
}

static void
findjsobjects_jsfunc(findjsobjects_state_t *fjs, uintptr_t addr)
{
	findjsobjects_func_t *func, *ofunc;
	findjsobjects_instance_t *inst;
	uintptr_t funcinfo, script, name;
	avl_index_t where;
	int err;
	char *bufp;
	size_t len;

	/*
	 * This may be somewhat expensive to do for all JSFunctions, but in most
	 * core files, there aren't that many.  We could defer some of this work
	 * until the user tries to print the function ::jsfunctions, but this
	 * step is useful to do early to filter out garbage data.
	 */

	v8_silent++;
	if (read_heap_ptr(&funcinfo, addr, V8_OFF_JSFUNCTION_SHARED) != 0 ||
	    read_heap_ptr(&script, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0 ||
	    read_heap_ptr(&name, script, V8_OFF_SCRIPT_NAME) != 0) {
		fjs->fjs_stats.fjss_funcs_skipped++;
		v8_silent--;
		return;
	}

	func = mdb_zalloc(sizeof (findjsobjects_func_t), UM_SLEEP);
	func->fjsf_ninstances = 1;
	func->fjsf_instances.fjsi_addr = addr;
	func->fjsf_shared = funcinfo;

	bufp = func->fjsf_funcname;
	len = sizeof (func->fjsf_funcname);
	err = jsfunc_name(funcinfo, &bufp, &len);

	bufp = func->fjsf_scriptname;
	len = sizeof (func->fjsf_scriptname);
	err |= jsstr_print(name, JSSTR_NUDE, &bufp, &len);

	v8_silent--;
	if (err != 0) {
		fjs->fjs_stats.fjss_funcs_skipped++;
		mdb_free(func, sizeof (findjsobjects_func_t));
		return;
	}

	fjs->fjs_stats.fjss_funcs++;
	ofunc = avl_find(&fjs->fjs_funcinfo, func, &where);
	if (ofunc == NULL) {
		avl_add(&fjs->fjs_funcinfo, func);
		func->fjsf_next = fjs->fjs_funcs;
		fjs->fjs_funcs = func;
		fjs->fjs_stats.fjss_funcs_unique++;
	} else {
		inst = mdb_alloc(sizeof (findjsobjects_instance_t), UM_SLEEP);
		inst->fjsi_addr = addr;
		inst->fjsi_next = ofunc->fjsf_instances.fjsi_next;
		ofunc->fjsf_instances.fjsi_next = inst;
		ofunc->fjsf_ninstances++;
		mdb_free(func, sizeof (findjsobjects_func_t));
	}
}

int
findjsobjects_range(findjsobjects_state_t *fjs, uintptr_t addr, uintptr_t size)
{
	uintptr_t limit;
	findjsobjects_stats_t *stats = &fjs->fjs_stats;
	uint8_t type;
	int jsobject = V8_TYPE_JSOBJECT, jsarray = V8_TYPE_JSARRAY;
	int jstypedarray = V8_TYPE_JSTYPEDARRAY;
	int jsfunction = V8_TYPE_JSFUNCTION;
	caddr_t range = mdb_alloc(size, UM_SLEEP);
	uintptr_t base = addr, mapaddr;

	if (mdb_vread(range, size, addr) == -1)
		return (0);

	for (limit = addr + size; addr < limit; addr++) {
		findjsobjects_instance_t *inst;
		findjsobjects_obj_t *obj;
		avl_index_t where;

		if (V8_IS_SMI(addr))
			continue;

		if (!V8_IS_HEAPOBJECT(addr))
			continue;

		stats->fjss_heapobjs++;

		mapaddr = *((uintptr_t *)((uintptr_t)range +
		    (addr - base) + V8_OFF_HEAPOBJECT_MAP));

		if (!V8_IS_HEAPOBJECT(mapaddr))
			continue;

		mapaddr += V8_OFF_MAP_INSTANCE_ATTRIBUTES;
		stats->fjss_typereads++;

		if (mapaddr >= base && mapaddr < base + size) {
			stats->fjss_cached++;

			type = *((uint8_t *)((uintptr_t)range +
			    (mapaddr - base)));
		} else {
			if (mdb_vread(&type, sizeof (uint8_t), mapaddr) == -1)
				continue;
		}

		if (type == jsfunction) {
			findjsobjects_jsfunc(fjs, addr);
			continue;
		}

		if (type != jsobject && type != jsarray && type != jstypedarray)
			continue;

		stats->fjss_jsobjs++;

		fjs->fjs_current = findjsobjects_alloc(addr);

		if (type == jsobject || type == jstypedarray) {
			if (jsobj_properties(addr,
			    findjsobjects_prop, fjs,
			    &fjs->fjs_current->fjso_propinfo) != 0) {
				findjsobjects_free(fjs->fjs_current);
				fjs->fjs_current = NULL;
				continue;
			}

			if ((fjs->fjs_current->fjso_propinfo &
			    (JPI_MAYBE_GARBAGE)) != 0) {
				stats->fjss_garbage++;
				fjs->fjs_current->fjso_malformed = B_TRUE;
			}

			findjsobjects_constructor(fjs->fjs_current);
			stats->fjss_objects++;
		} else {
			uintptr_t ptr;
			size_t *nprops = &fjs->fjs_current->fjso_nprops;
			ssize_t len = V8_OFF_JSARRAY_LENGTH;
			ssize_t elems = V8_OFF_JSOBJECT_ELEMENTS;
			ssize_t flen = V8_OFF_FIXEDARRAY_LENGTH;
			uintptr_t nelems;
			uint8_t t;

			if (read_heap_smi(nprops, addr, len) != 0 ||
			    read_heap_ptr(&ptr, addr, elems) != 0 ||
			    !V8_IS_HEAPOBJECT(ptr) ||
			    read_typebyte(&t, ptr) != 0 ||
			    t != V8_TYPE_FIXEDARRAY ||
			    read_heap_smi(&nelems, ptr, flen) != 0 ||
			    nelems < *nprops) {
				findjsobjects_free(fjs->fjs_current);
				fjs->fjs_current = NULL;
				continue;
			}

			strcpy(fjs->fjs_current->fjso_constructor, "Array");
			stats->fjss_arrays++;
		}

		/*
		 * Now determine if we already have an object matching our
		 * properties.  If we don't, we'll add our new object; if we
		 * do we'll merely enqueue our instance.
		 */
		obj = avl_find(&fjs->fjs_tree, fjs->fjs_current, &where);

		if (obj == NULL) {
			avl_add(&fjs->fjs_tree, fjs->fjs_current);
			fjs->fjs_current->fjso_next = fjs->fjs_objects;
			fjs->fjs_objects = fjs->fjs_current;
			fjs->fjs_current = NULL;
			stats->fjss_uniques++;
			continue;
		}

		findjsobjects_free(fjs->fjs_current);
		fjs->fjs_current = NULL;

		inst = mdb_alloc(sizeof (findjsobjects_instance_t), UM_SLEEP);
		inst->fjsi_addr = addr;
		inst->fjsi_next = obj->fjso_instances.fjsi_next;
		obj->fjso_instances.fjsi_next = inst;
		obj->fjso_ninstances++;
	}

	mdb_free(range, size);

	return (0);
}

static int
findjsobjects_mapping(findjsobjects_state_t *fjs, const prmap_t *pmp,
    const char *name)
{
	if (name != NULL && !(fjs->fjs_brk && (pmp->pr_mflags & MA_BREAK)))
		return (0);

	if (fjs->fjs_addr != NULL && (fjs->fjs_addr < pmp->pr_vaddr ||
	    fjs->fjs_addr >= pmp->pr_vaddr + pmp->pr_size))
		return (0);

	return (findjsobjects_range(fjs, pmp->pr_vaddr, pmp->pr_size));
}

static void
findjsobjects_references_add(findjsobjects_state_t *fjs, v8propvalue_t *valp,
    const char *desc, size_t index)
{
	assert(valp != NULL);

	findjsobjects_referent_t search, *referent;
	findjsobjects_reference_t *reference;

	/*
	 * Searching for unboxed floating-point values is not supported.
	 */
	if (valp->v8v_isboxeddouble) {
		return;
	}

	search.fjsr_addr = valp->v8v_u.v8vu_addr;

	if ((referent = avl_find(&fjs->fjs_referents, &search, NULL)) == NULL)
		return;

	reference = mdb_zalloc(sizeof (*reference), UM_SLEEP | UM_GC);
	reference->fjsrf_addr = fjs->fjs_addr;

	if (desc != NULL) {
		reference->fjsrf_desc =
		    mdb_alloc(strlen(desc) + 1, UM_SLEEP | UM_GC);
		(void) strcpy(reference->fjsrf_desc, desc);
	} else {
		reference->fjsrf_index = index;
	}

	if (referent->fjsr_head == NULL) {
		referent->fjsr_head = reference;
	} else {
		referent->fjsr_tail->fjsrf_next = reference;
	}

	referent->fjsr_tail = reference;
}

static int
findjsobjects_references_prop(const char *desc, v8propvalue_t *val, void *arg)
{
	/*
	 * jsobj_properties will still call us if the layout of the object it's
	 * inspecting cannot be understood, but with val == NULL. In this case,
	 * there's no point in adding a reference though.
	 */
	if (val != NULL)
		findjsobjects_references_add(arg, val, desc, -1);

	return (0);
}

static void
findjsobjects_references_array(findjsobjects_state_t *fjs,
    findjsobjects_obj_t *obj)
{
	findjsobjects_instance_t *inst = &obj->fjso_instances;
	uintptr_t *elts;
	size_t i, len;
	v8propvalue_t value;

	for (; inst != NULL; inst = inst->fjsi_next) {
		uintptr_t addr = inst->fjsi_addr, ptr;

		if (read_heap_ptr(&ptr, addr, V8_OFF_JSOBJECT_ELEMENTS) != 0 ||
		    read_heap_array(ptr, &elts, &len, UM_SLEEP) != 0)
			continue;

		fjs->fjs_addr = addr;

		for (i = 0; i < len; i++) {
			jsobj_propvalue_addr(&value, elts[i]);
			findjsobjects_references_add(fjs, &value, NULL, i);
		}

		mdb_free(elts, len * sizeof (uintptr_t));
	}
}

static void
findjsobjects_referent(findjsobjects_state_t *fjs, uintptr_t addr)
{
	findjsobjects_referent_t search, *referent;

	search.fjsr_addr = addr;

	if (avl_find(&fjs->fjs_referents, &search, NULL) != NULL) {
		assert(fjs->fjs_marking);
		mdb_warn("%p is already marked; ignoring\n", addr);
		return;
	}

	referent = mdb_zalloc(sizeof (findjsobjects_referent_t), UM_SLEEP);
	referent->fjsr_addr = addr;

	avl_add(&fjs->fjs_referents, referent);

	if (fjs->fjs_tail != NULL) {
		fjs->fjs_tail->fjsr_next = referent;
	} else {
		fjs->fjs_head = referent;
	}

	fjs->fjs_tail = referent;

	if (fjs->fjs_marking)
		mdb_printf("findjsobjects: marked %p\n", addr);
}

static void
findjsobjects_references(findjsobjects_state_t *fjs)
{
	findjsobjects_reference_t *reference;
	findjsobjects_referent_t *referent;
	avl_tree_t *referents = &fjs->fjs_referents;
	findjsobjects_obj_t *obj;
	void *cookie = NULL;
	uintptr_t addr;

	fjs->fjs_referred = B_FALSE;

	v8_silent++;

	/*
	 * First traverse over all objects and arrays, looking for references
	 * to our designated referent(s).
	 */
	for (obj = fjs->fjs_objects; obj != NULL; obj = obj->fjso_next) {
		findjsobjects_instance_t *head = &obj->fjso_instances, *inst;

		if (obj->fjso_nprops != 0 && obj->fjso_props == NULL) {
			findjsobjects_references_array(fjs, obj);
			continue;
		}

		for (inst = head; inst != NULL; inst = inst->fjsi_next) {
			fjs->fjs_addr = inst->fjsi_addr;

			(void) jsobj_properties(inst->fjsi_addr,
			    findjsobjects_references_prop, fjs, NULL);
		}
	}

	v8_silent--;
	fjs->fjs_addr = NULL;

	/*
	 * Now go over our referent(s), reporting any references that we have
	 * accumulated.
	 */
	for (referent = fjs->fjs_head; referent != NULL;
	    referent = referent->fjsr_next) {
		addr = referent->fjsr_addr;

		if ((reference = referent->fjsr_head) == NULL) {
			mdb_printf("%p is not referred to by a "
			    "known object.\n", addr);
			continue;
		}

		for (; reference != NULL; reference = reference->fjsrf_next) {
			mdb_printf("%p referred to by %p",
			    addr, reference->fjsrf_addr);

			if (reference->fjsrf_desc == NULL) {
				mdb_printf("[%d]\n", reference->fjsrf_index);
			} else {
				mdb_printf(".%s\n", reference->fjsrf_desc);
			}
		}
	}

	/*
	 * Finally, destroy our referent nodes.
	 */
	while ((referent = avl_destroy_nodes(referents, &cookie)) != NULL)
		mdb_free(referent, sizeof (findjsobjects_referent_t));

	fjs->fjs_head = NULL;
	fjs->fjs_tail = NULL;
}

static findjsobjects_instance_t *
findjsobjects_instance(findjsobjects_state_t *fjs, uintptr_t addr,
    findjsobjects_instance_t **headp)
{
	findjsobjects_obj_t *obj;

	for (obj = fjs->fjs_objects; obj != NULL; obj = obj->fjso_next) {
		findjsobjects_instance_t *head = &obj->fjso_instances, *inst;

		for (inst = head; inst != NULL; inst = inst->fjsi_next) {
			if (inst->fjsi_addr == addr) {
				*headp = head;
				return (inst);
			}
		}
	}

	return (NULL);
}

/*ARGSUSED*/
static void
findjsobjects_match_all(findjsobjects_obj_t *obj, const char *ignored)
{
	mdb_printf("%p\n", obj->fjso_instances.fjsi_addr);
}

static void
findjsobjects_match_propname(findjsobjects_obj_t *obj, const char *propname)
{
	findjsobjects_prop_t *prop;

	for (prop = obj->fjso_props; prop != NULL; prop = prop->fjsp_next) {
		if (strcmp(prop->fjsp_desc, propname) == 0) {
			mdb_printf("%p\n", obj->fjso_instances.fjsi_addr);
			return;
		}
	}
}

static void
findjsobjects_match_constructor(findjsobjects_obj_t *obj,
    const char *constructor)
{
	if (strcmp(constructor, obj->fjso_constructor) == 0)
		mdb_printf("%p\n", obj->fjso_instances.fjsi_addr);
}

static void
findjsobjects_match_kind(findjsobjects_obj_t *obj, const char *propkind)
{
	jspropinfo_t p = obj->fjso_propinfo;

	if (((p & JPI_NUMERIC) != 0 && strstr(propkind, "numeric") != NULL) ||
	    ((p & JPI_DICT) != 0 && strstr(propkind, "dict") != NULL) ||
	    ((p & JPI_INOBJECT) != 0 && strstr(propkind, "inobject") != NULL) ||
	    ((p & JPI_PROPS) != 0 && strstr(propkind, "props") != NULL) ||
	    ((p & JPI_HASTRANSITIONS) != 0 &&
	    strstr(propkind, "transitions") != NULL) ||
	    ((p & JPI_HASCONTENT) != 0 &&
	    strstr(propkind, "content") != NULL) ||
	    ((p & JPI_SKIPPED) != 0 && strstr(propkind, "skipped") != NULL) ||
	    ((p & JPI_UNDEFPROPNAME) != 0 &&
	    strstr(propkind, "undefpropname") != NULL) ||
	    ((p & JPI_BADPROPS) != 0 && strstr(propkind, "badprop") != NULL) ||
	    ((p & JPI_BADLAYOUT) != 0 &&
	    strstr(propkind, "badlayout") != NULL)) {
		mdb_printf("%p\n", obj->fjso_instances.fjsi_addr);
	}
}

static int
findjsobjects_match(findjsobjects_state_t *fjs, uintptr_t addr,
    uint_t flags, void (*func)(findjsobjects_obj_t *, const char *),
    const char *match)
{
	findjsobjects_obj_t *obj;

	if (!(flags & DCMD_ADDRSPEC)) {
		for (obj = fjs->fjs_objects; obj != NULL;
		    obj = obj->fjso_next) {
			if (obj->fjso_malformed && !fjs->fjs_allobjs)
				continue;

			func(obj, match);
		}

		return (DCMD_OK);
	}

	/*
	 * First, look for the specified address among the representative
	 * objects.
	 */
	for (obj = fjs->fjs_objects; obj != NULL; obj = obj->fjso_next) {
		if (obj->fjso_instances.fjsi_addr == addr) {
			func(obj, match);
			return (DCMD_OK);
		}
	}

	/*
	 * We didn't find it among the representative objects; iterate over
	 * all objects.
	 */
	for (obj = fjs->fjs_objects; obj != NULL; obj = obj->fjso_next) {
		findjsobjects_instance_t *head = &obj->fjso_instances, *inst;

		for (inst = head; inst != NULL; inst = inst->fjsi_next) {
			if (inst->fjsi_addr == addr) {
				func(obj, match);
				return (DCMD_OK);
			}
		}
	}

	mdb_warn("%p does not correspond to a known object\n", addr);
	return (DCMD_ERR);
}

static void
findjsobjects_print(findjsobjects_obj_t *obj)
{
	int col = 19 + (sizeof (uintptr_t) * 2) + strlen("..."), len;
	uintptr_t addr = obj->fjso_instances.fjsi_addr;
	findjsobjects_prop_t *prop;

	mdb_printf("%?p %8d %8d ",
	    addr, obj->fjso_ninstances, obj->fjso_nprops);

	if (obj->fjso_constructor[0] != '\0') {
		mdb_printf("%s%s", obj->fjso_constructor,
		    obj->fjso_props != NULL ? ": " : "");
		col += strlen(obj->fjso_constructor) + 2;
	}

	for (prop = obj->fjso_props; prop != NULL; prop = prop->fjsp_next) {
		if (col + (len = strlen(prop->fjsp_desc) + 2) < 80) {
			mdb_printf("%s%s", prop->fjsp_desc,
			    prop->fjsp_next != NULL ? ", " : "");
			col += len;
		} else {
			mdb_printf("...");
			break;
		}
	}

	mdb_printf("\n", col);
}

static void
dcmd_findjsobjects_help(void)
{
	mdb_printf("%s\n\n",
"Finds all JavaScript objects in the V8 heap via brute force iteration over\n"
"all mapped anonymous memory.  (This can take up to several minutes on large\n"
"dumps.)  The output consists of representative objects, the number of\n"
"instances of that object and the number of properties on the object --\n"
"followed by the constructor and first few properties of the objects.  Once\n"
"run, subsequent calls to ::findjsobjects use cached data.  If provided an\n"
"address (and in the absence of -r, described below), ::findjsobjects treats\n"
"the address as that of a representative object, and lists all instances of\n"
"that object (that is, all objects that have a matching property signature).");

	mdb_dec_indent(2);
	mdb_printf("%<b>OPTIONS%</b>\n");
	mdb_inc_indent(2);

	mdb_printf("%s\n",
"  -b       Include the heap denoted by the brk(2) (normally excluded)\n"
"  -c cons  Display representative objects with the specified constructor\n"
"  -p prop  Display representative objects that have the specified property\n"
"  -l       List all objects that match the representative object\n"
"  -m       Mark specified object for later reference determination via -r\n"
"  -r       Find references to the specified and/or marked object(s)\n"
"  -v       Provide verbose statistics\n");
}

static findjsobjects_state_t findjsobjects_state;

static int
findjsobjects_run(findjsobjects_state_t *fjs)
{
	struct ps_prochandle *Pr;
	findjsobjects_obj_t *obj;
	findjsobjects_stats_t *stats = &fjs->fjs_stats;

	if (!fjs->fjs_initialized) {
		avl_create(&fjs->fjs_tree,
		    (int(*)(const void *, const void *))findjsobjects_cmp,
		    sizeof (findjsobjects_obj_t),
		    offsetof(findjsobjects_obj_t, fjso_node));

		avl_create(&fjs->fjs_referents,
		    (int(*)(const void *, const void *))
		    findjsobjects_cmp_referents,
		    sizeof (findjsobjects_referent_t),
		    offsetof(findjsobjects_referent_t, fjsr_node));

		avl_create(&fjs->fjs_funcinfo,
		    (int(*)(const void *, const void*))
		    findjsobjects_cmp_funcinfo,
		    sizeof (findjsobjects_func_t),
		    offsetof(findjsobjects_func_t, fjsf_node));

		fjs->fjs_initialized = B_TRUE;
	}

	if (avl_is_empty(&fjs->fjs_tree)) {
		findjsobjects_obj_t **sorted;
		int nobjs, i;
		hrtime_t start = gethrtime();

		if (mdb_get_xdata("pshandle", &Pr, sizeof (Pr)) == -1) {
			mdb_warn("couldn't read pshandle xdata");
			return (-1);
		}

		v8_silent++;

		if (Pmapping_iter(Pr,
		    (proc_map_f *)findjsobjects_mapping, fjs) != 0) {
			v8_silent--;
			return (-1);
		}

		if ((nobjs = avl_numnodes(&fjs->fjs_tree)) != 0) {
			/*
			 * We have the objects -- now sort them.
			 */
			sorted = mdb_alloc(nobjs * sizeof (void *),
			    UM_SLEEP | UM_GC);

			for (obj = fjs->fjs_objects, i = 0; obj != NULL;
			    obj = obj->fjso_next, i++) {
				sorted[i] = obj;
			}

			qsort(sorted, avl_numnodes(&fjs->fjs_tree),
			    sizeof (void *), findjsobjects_cmp_ninstances);

			for (i = 1, fjs->fjs_objects = sorted[0];
			    i < nobjs; i++)
				sorted[i - 1]->fjso_next = sorted[i];

			sorted[nobjs - 1]->fjso_next = NULL;
		}

		fjs->fjs_finished = B_TRUE;

		v8_silent--;

		if (fjs->fjs_verbose) {
			const char *f = "findjsobjects: %30s => %d\n";
			int elapsed = (int)((gethrtime() - start) / NANOSEC);

			mdb_printf(f, "elapsed time (seconds)", elapsed);
			mdb_printf(f, "heap objects", stats->fjss_heapobjs);
			mdb_printf(f, "type reads", stats->fjss_typereads);
			mdb_printf(f, "cached reads", stats->fjss_cached);
			mdb_printf(f, "JavaScript objects", stats->fjss_jsobjs);
			mdb_printf(f, "processed objects", stats->fjss_objects);
			mdb_printf(f, "possible garbage", stats->fjss_garbage);
			mdb_printf(f, "processed arrays", stats->fjss_arrays);
			mdb_printf(f, "unique objects", stats->fjss_uniques);
			mdb_printf(f, "functions found", stats->fjss_funcs);
			mdb_printf(f, "unique functions",
			    stats->fjss_funcs_unique);
			mdb_printf(f, "functions skipped",
			    stats->fjss_funcs_skipped);
		}
	}

	return (0);
}

static int
dcmd_findjsobjects(uintptr_t addr,
    uint_t flags, int argc, const mdb_arg_t *argv)
{
	findjsobjects_state_t *fjs = &findjsobjects_state;
	findjsobjects_obj_t *obj;
	boolean_t references = B_FALSE, listlike = B_FALSE;
	const char *propname = NULL;
	const char *constructor = NULL;
	const char *propkind = NULL;

	fjs->fjs_verbose = B_FALSE;
	fjs->fjs_brk = B_FALSE;
	fjs->fjs_marking = B_FALSE;
	fjs->fjs_allobjs = B_FALSE;

	if (mdb_getopts(argc, argv,
	    'a', MDB_OPT_SETBITS, B_TRUE, &fjs->fjs_allobjs,
	    'b', MDB_OPT_SETBITS, B_TRUE, &fjs->fjs_brk,
	    'c', MDB_OPT_STR, &constructor,
	    'k', MDB_OPT_STR, &propkind,
	    'l', MDB_OPT_SETBITS, B_TRUE, &listlike,
	    'm', MDB_OPT_SETBITS, B_TRUE, &fjs->fjs_marking,
	    'p', MDB_OPT_STR, &propname,
	    'r', MDB_OPT_SETBITS, B_TRUE, &references,
	    'v', MDB_OPT_SETBITS, B_TRUE, &fjs->fjs_verbose,
	    NULL) != argc)
		return (DCMD_USAGE);

	if (findjsobjects_run(fjs) != 0)
		return (DCMD_ERR);

	if (!fjs->fjs_finished) {
		mdb_warn("error: previous findjsobjects "
		    "heap scan did not complete.\n");
		return (DCMD_ERR);
	}

	if (listlike && !(flags & DCMD_ADDRSPEC)) {
		if (propname != NULL || constructor != NULL ||
		    propkind != NULL) {
			char opt = propname != NULL ? 'p' :
			    propkind != NULL ? 'k' :'c';

			mdb_warn("cannot specify -l with -%c; instead, pipe "
			    "output of ::findjsobjects -%c to "
			    "::findjsobjects -l\n", opt, opt);
			return (DCMD_ERR);
		}

		return (findjsobjects_match(fjs, addr, flags,
		    findjsobjects_match_all, NULL));
	}

	if (propname != NULL) {
		if (constructor != NULL || propkind != NULL) {
			mdb_warn("cannot specify both a property name "
			    "and a %s\n", constructor != NULL ?
			    "constructor" : "property kind");
			return (DCMD_ERR);
		}

		return (findjsobjects_match(fjs, addr, flags,
		    findjsobjects_match_propname, propname));
	}

	if (constructor != NULL) {
		if (propkind != NULL) {
			mdb_warn("cannot specify both a constructor name "
			    "and a property kind\n");
			return (DCMD_ERR);
		}

		return (findjsobjects_match(fjs, addr, flags,
		    findjsobjects_match_constructor, constructor));
	}

	if (propkind != NULL) {
		return (findjsobjects_match(fjs, addr, flags,
		    findjsobjects_match_kind, propkind));
	}

	if (references && !(flags & DCMD_ADDRSPEC) &&
	    avl_is_empty(&fjs->fjs_referents)) {
		mdb_warn("must specify or mark an object to find references\n");
		return (DCMD_ERR);
	}

	if (fjs->fjs_marking && !(flags & DCMD_ADDRSPEC)) {
		mdb_warn("must specify an object to mark\n");
		return (DCMD_ERR);
	}

	if (references && fjs->fjs_marking) {
		mdb_warn("can't both mark an object and find its references\n");
		return (DCMD_ERR);
	}

	if (flags & DCMD_ADDRSPEC) {
		findjsobjects_instance_t *inst, *head;

		/*
		 * If we've been passed an address, it's to either list like
		 * objects (-l), mark an object (-m) or find references to the
		 * specified/marked objects (-r).  (Note that the absence of
		 * any of these options implies -l.)
		 */
		inst = findjsobjects_instance(fjs, addr, &head);

		if (inst == NULL) {
			mdb_warn("%p is not a valid object\n", addr);
			return (DCMD_ERR);
		}

		if (!references && !fjs->fjs_marking) {
			for (inst = head; inst != NULL; inst = inst->fjsi_next)
				mdb_printf("%p\n", inst->fjsi_addr);

			return (DCMD_OK);
		}

		if (!listlike) {
			findjsobjects_referent(fjs, inst->fjsi_addr);
		} else {
			for (inst = head; inst != NULL; inst = inst->fjsi_next)
				findjsobjects_referent(fjs, inst->fjsi_addr);
		}
	}

	if (references)
		findjsobjects_references(fjs);

	if (references || fjs->fjs_marking)
		return (DCMD_OK);

	mdb_printf("%?s %8s %8s %s\n", "OBJECT",
	    "#OBJECTS", "#PROPS", "CONSTRUCTOR: PROPS");

	for (obj = fjs->fjs_objects; obj != NULL; obj = obj->fjso_next) {
		if (obj->fjso_malformed && !fjs->fjs_allobjs)
			continue;

		findjsobjects_print(obj);
	}

	return (DCMD_OK);
}

/*
 * Given a Node Buffer object, print out details about it.  With "-a", just
 * print the address.
 */
/* ARGSUSED */
static int
dcmd_nodebuffer(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	boolean_t opt_f = B_FALSE;
	char buf[80];
	char *bufp = buf;
	size_t len = sizeof (buf);
	uintptr_t elts, rawbuf;
	uintptr_t arraybuffer_view_buffer;
	uintptr_t arraybufferview_content_offset;

	/*
	 * The undocumented "-f" option allows users to override constructor
	 * checks.
	 */
	if (mdb_getopts(argc, argv,
	    'f', MDB_OPT_SETBITS, B_TRUE, &opt_f, NULL) != argc)
		return (DCMD_USAGE);

	if (!opt_f) {
		if (obj_jsconstructor(addr, &bufp, &len, B_FALSE) != 0)
			return (DCMD_ERR);

		if (strcmp(buf, "Buffer") != 0 &&
		    strcmp(buf, "NativeBuffer") != 0 &&
		    strcmp(buf, "Uint8Array") != 0) {
			mdb_warn("%p does not appear to be a buffer\n", addr);
			return (DCMD_ERR);
		}
	}

	if (strcmp(buf, "Buffer") == 0 ||
	    strcmp(buf, "NativeBuffer") == 0 ||
	    V8_OFF_JSARRAYBUFFER_BACKINGSTORE == -1) {
		/*
		 * This works for Buffer and NativeBuffer instances in node <
		 * 4.0 because they use elements slots to reference the backing
		 * storage. If the constructor name is not "Buffer" or
		 * "NativeBuffer" but "Uint8Array" and
		 * V8_OFF_JSARRAYBUFFER_BACKINGSTORE == -1, it means we are in
		 * the range of node versions >= 4.0 and <= 4.1 that ship
		 * with V8 4.5.x. For these versions, it also works because
		 * Buffer instances are actually typed arrays but their backing
		 * storage is an ExternalUint8Arrayelements whose address is
		 * stored in the first element's slot.
		 */
		if (read_heap_ptr(&elts, addr, V8_OFF_JSOBJECT_ELEMENTS) != 0)
			return (DCMD_ERR);

		if (obj_v8internal(elts, 0, &rawbuf) != 0)
			return (DCMD_ERR);
	} else {
		/*
		 * The buffer instance's constructor name is Uint8Array, and
		 * V8_OFF_JSARRAYBUFFER_BACKINGSTORE != -1, which means that
		 * we're dealing with a node version that ships with V8 4.6 or
		 * later. For these versions, buffer instances store their data
		 * as a typed array, but this time instead of having the backing
		 * store as an ExternalUint8Array referenced from an element
		 * slot, it can be found at two different locations:
		 *
		 * 1. As a FixedTypedArray casted as a FixedTypedArrayBase in an
		 * element slot.
		 *
		 * 2. As the "backing_store" property of the corresponding
		 * JSArrayBuffer.
		 *
		 * The second way to retrieve the backing store seems like
		 * it will be less likely to change, and is thus the one we're
		 * using.
		 */
		if (V8_OFF_JSARRAYBUFFER_BACKINGSTORE == -1 ||
		    V8_OFF_JSARRAYBUFFERVIEW_BUFFER == -1 ||
		    V8_OFF_JSARRAYBUFFERVIEW_CONTENT_OFFSET == -1)
			return (DCMD_ERR);

		if (read_heap_ptr(&arraybuffer_view_buffer, addr,
		    V8_OFF_JSARRAYBUFFERVIEW_BUFFER) != 0)
			return (DCMD_ERR);

		if (read_heap_ptr(&rawbuf, arraybuffer_view_buffer,
		    V8_OFF_JSARRAYBUFFER_BACKINGSTORE) != 0)
			return (DCMD_ERR);

		if (read_heap_smi(&arraybufferview_content_offset, addr,
		    V8_OFF_JSARRAYBUFFERVIEW_CONTENT_OFFSET) != 0)
			return (DCMD_ERR);

		rawbuf += arraybufferview_content_offset;
	}

	mdb_printf("%p\n", rawbuf);
	return (DCMD_OK);
}

static int
jsclosure_iter_var(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp, void *arg)
{
	v8context_t *ctxp = arg;
	uintptr_t namep, valp;
	size_t validx, bufsz;
	char buf[1024];
	char *bufp;
	jsobj_print_t jsop;

	bufp = buf;
	bufsz = sizeof (buf);
	(void) bsnprintf(&bufp, &bufsz, "    ");
	namep = v8scopeinfo_var_name(sip, sivp);
	if (jsstr_print(namep, JSSTR_QUOTED, &bufp, &bufsz) != 0) {
		return (-1);
	}

	validx = v8scopeinfo_var_idx(sip, sivp);
	if (v8context_var_value(ctxp, validx, &valp) != 0) {
		return (-1);
	}

	(void) bsnprintf(&bufp, &bufsz, ": ");

	bzero(&jsop, sizeof (jsop));
	jsop.jsop_depth = 1;
	jsop.jsop_bufp = &bufp;
	jsop.jsop_lenp = &bufsz;
	jsop.jsop_indent = 4;
	jsop.jsop_printaddr = B_TRUE;
	if (jsobj_print(valp, &jsop) != 0) {
		return (-1);
	}

	mdb_printf("%s\n", buf);
	return (0);
}

/* ARGSUSED */
static int
jsfunction_bound_arg(v8boundfunction_t *bfp, uint_t which, uintptr_t value,
    void *unused)
{
	char buf[80];
	size_t len = sizeof (buf);
	char *bufp;

	bufp = buf;
	(void) obj_jstype(value, &bufp, &len, NULL);

	mdb_printf("      arg%d  = %p (%s)\n", which, value, buf);
	return (0);
}

/* ARGSUSED */
static int
dcmd_jsfunction(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8function_t *fp = NULL;
	v8funcinfo_t *fip = NULL;
	v8boundfunction_t *bfp = NULL;
	mdbv8_strbuf_t *strb = NULL;
	int memflags = UM_SLEEP | UM_GC;
	int rv = DCMD_ERR;
	char buf[80];
	size_t len = sizeof (buf);
	char *bufp;

	/*
	 * Bound functions are separate from other functions.  The regular
	 * function APIs may not work on them, depending on the Node version.
	 * Handle them first.
	 * TODO the API here doesn't really allow you to distinguish between
	 * something like memory allocation failure and bad input (e.g., not a
	 * bound function).  It was written assuming you would know what type
	 * you expected something to be, but this is one of the first cases
	 * where we don't.
	 */
	if ((bfp = v8boundfunction_load(addr, memflags)) != NULL) {
		uintptr_t thisp;

		mdb_printf("bound function that wraps: %p\n",
		    v8boundfunction_target(bfp));
		thisp = v8boundfunction_this(bfp);
		bufp = buf;
		(void) obj_jstype(thisp, &bufp, &len, NULL);
		mdb_printf("with \"this\" = %p (%s)\n", thisp, buf);
		rv = v8boundfunction_iter_args(bfp, jsfunction_bound_arg, NULL);
		v8boundfunction_free(bfp);
		return (rv);
	}

	v8_warnings++;
	if ((fp = v8function_load(addr, memflags)) == NULL ||
	    (fip = v8function_funcinfo(fp, memflags)) == NULL ||
	    (strb = mdbv8_strbuf_alloc(512, memflags)) == NULL) {
		goto out;
	}

	mdbv8_strbuf_sprintf(strb, "function: ");

	if (v8funcinfo_funcname(fip, strb, MSF_ASCIIONLY) != 0) {
		goto out;
	}

	mdbv8_strbuf_sprintf(strb, "\ndefined at ");
	(void) v8funcinfo_scriptpath(fip, strb, MSF_ASCIIONLY);
	mdbv8_strbuf_sprintf(strb, " ");
	(void) v8funcinfo_definition_location(fip, strb, MSF_ASCIIONLY);
	mdb_printf("%s\n", mdbv8_strbuf_tocstr(strb));
	rv = DCMD_OK;

out:
	v8funcinfo_free(fip);
	v8function_free(fp);
	mdbv8_strbuf_free(strb);
	v8_warnings--;
	return (rv);
}

/* ARGSUSED */
static int
dcmd_jsclosure(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8function_t *funcp;
	v8context_t *ctxp;
	v8scopeinfo_t *sip;
	int memflags = UM_SLEEP | UM_GC;

	if ((funcp = v8function_load(addr, memflags)) == NULL) {
		mdb_warn("%p: failed to load JSFunction\n", addr);
		return (DCMD_ERR);
	}

	if ((ctxp = v8function_context(funcp, memflags)) == NULL) {
		mdb_warn("%p: failed to load Context for JSFunction\n", addr);
		return (DCMD_ERR);
	}

	if ((sip = v8context_scopeinfo(ctxp, memflags)) == NULL) {
		mdb_warn("%p: failed to load ScopeInfo\n", addr);
		return (DCMD_ERR);
	}

	if (v8scopeinfo_iter_vars(sip, V8SV_CONTEXTLOCALS,
	    jsclosure_iter_var, ctxp) != 0) {
		mdb_warn("%p: failed to iterate closure variables\n", addr);
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}


/* ARGSUSED */
static int
dcmd_jsconstructor(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	boolean_t opt_v = B_FALSE;
	char buf[80];
	char *bufp;
	size_t len = sizeof (buf);

	if (mdb_getopts(argc, argv, 'v', MDB_OPT_SETBITS, B_TRUE, &opt_v,
	    NULL) != argc)
		return (DCMD_USAGE);

	bufp = buf;
	if (obj_jsconstructor(addr, &bufp, &len, opt_v))
		return (DCMD_ERR);

	mdb_printf("%s\n", buf);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_jsframe(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t fptr, raddr;
	boolean_t opt_i = B_FALSE;
	jsframe_t jsf;
	int rv;

	bzero(&jsf, sizeof (jsf));
	jsf.jsf_nlines = 5;

	if (mdb_getopts(argc, argv,
	    'a', MDB_OPT_SETBITS, B_TRUE, &jsf.jsf_showall,
	    'v', MDB_OPT_SETBITS, B_TRUE, &jsf.jsf_verbose,
	    'i', MDB_OPT_SETBITS, B_TRUE, &opt_i,
	    'f', MDB_OPT_STR, &jsf.jsf_func,
	    'n', MDB_OPT_UINTPTR, &jsf.jsf_nlines,
	    'p', MDB_OPT_STR, &jsf.jsf_prop, NULL) != argc)
		return (DCMD_USAGE);

	/*
	 * As with $C, we assume we are given a *pointer* to the frame pointer
	 * for a frame, rather than the actual frame pointer for the frame of
	 * interest. This is needed to show the instruction pointer, which is
	 * actually stored with the next frame.  For debugging, this can be
	 * overridden with the "-i" option (for "immediate").
	 */
	if (opt_i) {
		rv = do_jsframe(addr, 0, &jsf);
		if (rv == 0)
			jsframe_print_skipped(&jsf);
		return (rv);
	}

	if (mdb_vread(&raddr, sizeof (raddr),
	    addr + sizeof (uintptr_t)) == -1) {
		mdb_warn("failed to read return address from %p",
		    addr + sizeof (uintptr_t));
		return (DCMD_ERR);
	}

	if (mdb_vread(&fptr, sizeof (fptr), addr) == -1) {
		mdb_warn("failed to read frame pointer from %p", addr);
		return (DCMD_ERR);
	}

	if (fptr == NULL)
		return (DCMD_OK);

	rv = do_jsframe(fptr, raddr, &jsf);
	if (rv == 0)
		jsframe_print_skipped(&jsf);
	return (rv);
}

static void
jsobj_print_propinfo(jspropinfo_t propinfo)
{
	if (propinfo == JPI_NONE)
		return;

	mdb_printf("property kind: ");
	if ((propinfo & JPI_NUMERIC) != 0)
		mdb_printf("numeric-named ");
	if ((propinfo & JPI_DICT) != 0)
		mdb_printf("dictionary ");
	if ((propinfo & JPI_INOBJECT) != 0)
		mdb_printf("in-object ");
	if ((propinfo & JPI_PROPS) != 0)
		mdb_printf("\"properties\" array ");
	mdb_printf("\n");

	if ((propinfo & (JPI_HASTRANSITIONS | JPI_HASCONTENT)) != 0) {
		mdb_printf("fallbacks: ");
		if ((propinfo & JPI_HASTRANSITIONS) != 0)
			mdb_printf("transitions ");
		if ((propinfo & JPI_HASCONTENT) != 0)
			mdb_printf("content ");
		mdb_printf("\n");
	}

	if ((propinfo & JPI_UNDEFPROPNAME) != 0)
		mdb_printf(
		    "some properties skipped due to undefined property name\n");
	if ((propinfo & JPI_SKIPPED) != 0)
		mdb_printf(
		    "some properties skipped due to unexpected layout\n");
	if ((propinfo & JPI_BADLAYOUT) != 0)
		mdb_printf("object has unexpected layout\n");
	if ((propinfo & JPI_BADPROPS) != 0)
		mdb_printf("object has invalid-looking property values\n");
}

/* ARGSUSED */
static int
dcmd_jsprint(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	char *buf, *bufp;
	size_t bufsz = 262144, len = bufsz;
	jsobj_print_t jsop;
	boolean_t opt_b = B_FALSE;
	boolean_t opt_v = B_FALSE;
	uint64_t strlen_override = 0;
	int rv, i;

	bzero(&jsop, sizeof (jsop));
	jsop.jsop_depth = 2;
	jsop.jsop_printaddr = B_FALSE;

	i = mdb_getopts(argc, argv,
	    'a', MDB_OPT_SETBITS, B_TRUE, &jsop.jsop_printaddr,
	    'b', MDB_OPT_SETBITS, B_TRUE, &opt_b,
	    'd', MDB_OPT_UINT64, &jsop.jsop_depth,
	    'N', MDB_OPT_UINT64, &strlen_override,
	    'v', MDB_OPT_SETBITS, B_TRUE, &opt_v, NULL);

	jsop.jsop_maxstrlen = (int)strlen_override;

	if (opt_b)
		jsop.jsop_baseaddr = addr;

	do {
		if (i != argc) {
			const mdb_arg_t *member = &argv[i++];

			if (member->a_type != MDB_TYPE_STRING)
				return (DCMD_USAGE);

			jsop.jsop_member = member->a_un.a_str;
		}

		for (;;) {
			if ((buf = bufp =
			    mdb_zalloc(bufsz, UM_NOSLEEP)) == NULL)
				return (DCMD_ERR);

			jsop.jsop_bufp = &bufp;
			jsop.jsop_lenp = &len;

			rv = jsobj_print(addr, &jsop);

			if (len > 0)
				break;

			mdb_free(buf, bufsz);
			bufsz <<= 1;
			len = bufsz;
		}

		if (jsop.jsop_member == NULL && rv != 0) {
			if (!jsop.jsop_descended)
				mdb_warn("%s\n", buf);

			return (DCMD_ERR);
		}

		if (jsop.jsop_member && !jsop.jsop_found) {
			if (jsop.jsop_baseaddr)
				(void) mdb_printf("%p: ", jsop.jsop_baseaddr);

			(void) mdb_printf("undefined%s",
			    i < argc ? " " : "");
		} else {
			(void) mdb_printf("%s%s", buf, i < argc &&
			    !isspace(buf[strlen(buf) - 1]) ? " " : "");
		}

		mdb_free(buf, bufsz);
		jsop.jsop_found = B_FALSE;
		jsop.jsop_baseaddr = NULL;
	} while (i < argc);

	mdb_printf("\n");

	if (opt_v)
		jsobj_print_propinfo(jsop.jsop_propinfo);

	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_jssource(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	const char *typename;
	uintptr_t nlines = 5;
	uintptr_t funcinfop, scriptp, funcnamep;
	uintptr_t tokpos, endpos;
	uint8_t type;
	char buf[256];
	char *bufp = buf;
	size_t len = sizeof (buf);

	if (mdb_getopts(argc, argv, 'n', MDB_OPT_UINTPTR, &nlines,
	    NULL) != argc)
		return (DCMD_USAGE);

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0) {
		mdb_warn("%p is not a heap object\n", addr);
		return (DCMD_ERR);
	}

	typename = enum_lookup_str(v8_types, type, "");
	if (strcmp(typename, "JSFunction") != 0) {
		mdb_warn("%p is not a JSFunction\n", addr);
		return (DCMD_ERR);
	}

	if (read_heap_ptr(&funcinfop, addr, V8_OFF_JSFUNCTION_SHARED) != 0 ||
	    read_heap_ptr(&scriptp, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0 ||
	    read_heap_ptr(&funcnamep, scriptp, V8_OFF_SCRIPT_NAME) != 0) {
		mdb_warn("%p: failed to find script for function\n", addr);
		return (DCMD_ERR);
	}

	if (read_heap_maybesmi(&tokpos, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION) != 0 ||
	    read_heap_maybesmi(&endpos, funcinfop,
	    V8_OFF_SHAREDFUNCTIONINFO_END_POSITION) != 0) {
		mdb_warn("%p: failed to find function's boundaries\n", addr);
	}

	if (jsstr_print(funcnamep, JSSTR_NUDE, &bufp, &len) == 0)
		mdb_printf("file: %s\n", buf);

	if (tokpos != endpos)
		jsfunc_lines(scriptp, tokpos, endpos, nlines, "%5d ");
	mdb_printf("\n");
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_jsfunctions(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	findjsobjects_state_t *fjs = &findjsobjects_state;
	findjsobjects_func_t *func;
	uintptr_t funcinfo;
	boolean_t showrange = B_FALSE;
	boolean_t listlike = B_FALSE;
	const char *name = NULL, *filename = NULL;
	uintptr_t instr = 0;

	if (mdb_getopts(argc, argv,
	    'l', MDB_OPT_SETBITS, B_TRUE, &listlike,
	    'x', MDB_OPT_UINTPTR, &instr,
	    'X', MDB_OPT_SETBITS, B_TRUE, &showrange,
	    'n', MDB_OPT_STR, &name,
	    's', MDB_OPT_STR, &filename,
	    NULL) != argc)
		return (DCMD_USAGE);

	if (findjsobjects_run(fjs) != 0)
		return (DCMD_ERR);

	if (listlike && !(flags & DCMD_ADDRSPEC) &&
	    (name != NULL || filename != NULL || instr != 0)) {
		mdb_warn("cannot specify -l with -n, -f, or -x\n");
		return (DCMD_ERR);
	}

	if (!fjs->fjs_finished) {
		mdb_warn("error: previous findjsobjects "
		    "heap scan did not complete.\n");
		return (DCMD_ERR);
	}

	if (flags & DCMD_ADDRSPEC) {
		listlike = B_TRUE;
	}

	if (!showrange && !listlike) {
		mdb_printf("%?s %8s %-40s %s\n", "FUNC", "#FUNCS", "NAME",
		    "FROM");
	} else if (!listlike) {
		mdb_printf("%?s %8s %?s %?s %-40s %s\n", "FUNC", "#FUNCS",
		    "START", "END", "NAME", "FROM");
	}

	for (func = fjs->fjs_funcs; func != NULL; func = func->fjsf_next) {
		uintptr_t code, ilen;

		if (listlike && (flags & DCMD_ADDRSPEC) != 0) {
			findjsobjects_instance_t *inst;

			if (addr != func->fjsf_instances.fjsi_addr) {
				continue;
			}

			for (inst = &func->fjsf_instances;
			    inst != NULL; inst = inst->fjsi_next) {
				mdb_printf("%?p\n", inst->fjsi_addr);
			}

			continue;
		}

		funcinfo = func->fjsf_shared;

		if (func->fjsf_location[0] == '\0') {
			uintptr_t tokpos, script, lends;
			ptrdiff_t tokposoff =
			    V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION;

			/*
			 * We don't want to actually decode the token position
			 * as an SMI here, so we re-encode it when we pass it to
			 * jsfunc_lineno() below.
			 */
			if (read_heap_maybesmi(&tokpos, funcinfo,
			    tokposoff) != 0 ||
			    read_heap_ptr(&script, funcinfo,
			    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0 ||
			    read_heap_ptr(&lends, script,
			    V8_OFF_SCRIPT_LINE_ENDS) != 0 ||
			    jsfunc_lineno(lends, V8_VALUE_SMI(tokpos),
			    func->fjsf_location,
			    sizeof (func->fjsf_location), NULL) != 0) {
				func->fjsf_location[0] = '\0';
			}
		}

		if (name != NULL && strstr(func->fjsf_funcname, name) == NULL)
			continue;

		if (filename != NULL &&
		    strstr(func->fjsf_scriptname, filename) == NULL)
			continue;

		code = 0;
		ilen = 0;
		if ((showrange || instr != 0) &&
		    (read_heap_ptr(&code, funcinfo,
		    V8_OFF_SHAREDFUNCTIONINFO_CODE) != 0 ||
		    read_heap_ptr(&ilen, code,
		    V8_OFF_CODE_INSTRUCTION_SIZE) != 0)) {
			code = 0;
			ilen = 0;
		}

		if ((instr != 0 && ilen != 0) &&
		    (instr < code + V8_OFF_CODE_INSTRUCTION_START ||
		    instr >= code + V8_OFF_CODE_INSTRUCTION_START + ilen))
			continue;

		if (listlike) {
			mdb_printf("%?p\n", func->fjsf_instances.fjsi_addr);
		} else if (!showrange) {
			mdb_printf("%?p %8d %-40s %s %s\n",
			    func->fjsf_instances.fjsi_addr,
			    func->fjsf_ninstances, func->fjsf_funcname,
			    func->fjsf_scriptname, func->fjsf_location);
		} else {
			uintptr_t code, ilen;

			if (read_heap_ptr(&code, funcinfo,
			    V8_OFF_SHAREDFUNCTIONINFO_CODE) != 0 ||
			    read_heap_ptr(&ilen, code,
			    V8_OFF_CODE_INSTRUCTION_SIZE) != 0) {
				mdb_printf("%?p %8d %?s %?s %-40s %s %s\n",
				    func->fjsf_instances.fjsi_addr,
				    func->fjsf_ninstances, "?", "?",
				    func->fjsf_funcname, func->fjsf_scriptname,
				    func->fjsf_location);
			} else {
				mdb_printf("%?p %8d %?p %?p %-40s %s %s\n",
				    func->fjsf_instances.fjsi_addr,
				    func->fjsf_ninstances,
				    code + V8_OFF_CODE_INSTRUCTION_START,
				    code + V8_OFF_CODE_INSTRUCTION_START + ilen,
				    func->fjsf_funcname, func->fjsf_scriptname,
				    func->fjsf_location);
			}
		}
	}

	return (DCMD_OK);
}

static void
dcmd_jsfunctions_help(void)
{
	mdb_printf("%s\n\n",
"Lists JavaScript functions, optionally filtered by a substring of the\n"
"function name or script filename or by the instruction address.  This uses\n"
"the cache created by ::findjsobjects.  If ::findjsobjects has not already\n"
"been run, this command runs it automatically without printing the output.\n"
"This can take anywhere from a second to several minutes, depending on the\n"
"size of the core dump.\n"
"\n"
"It's important to keep in mind that each time you create a function in\n"
"JavaScript (even from a function definition that has already been used),\n"
"the VM must create a new object to represent it.  For example, if your\n"
"program has a function A that returns a closure B, the VM will create new\n"
"instances of the closure function (B) each time the surrounding function (A)\n"
"is called.  To show this, the output of this command consists of one line \n"
"per function definition that appears in the JavaScript source, and the\n"
"\"#FUNCS\" column shows how many different functions were created by VM from\n"
"this definition.");

	mdb_dec_indent(2);
	mdb_printf("%<b>OPTIONS%</b>\n");
	mdb_inc_indent(2);

	mdb_printf("%s\n",
"  -l       List only closures (without other columns).  With ADDR, list\n"
"           closures for the representative function ADDR.\n"
"  -n func  List functions whose name contains this substring\n"
"  -s file  List functions that were defined in a file whose name contains\n"
"           this substring.\n"
"  -x instr List functions whose compiled instructions include this address\n"
"  -X       Show where the function's instructions are stored in memory\n");
}

/* ARGSUSED */
static int
dcmd_v8field(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8_class_t *clp;
	v8_field_t *flp;
	const char *klass, *field;
	uintptr_t offset = 0;

	/*
	 * We may be invoked with either two arguments (class and field name) or
	 * three (an offset to save).
	 */
	if (argc != 2 && argc != 3)
		return (DCMD_USAGE);

	if (argv[0].a_type != MDB_TYPE_STRING ||
	    argv[1].a_type != MDB_TYPE_STRING)
		return (DCMD_USAGE);

	klass = argv[0].a_un.a_str;
	field = argv[1].a_un.a_str;

	if (argc == 3) {
		if (argv[2].a_type != MDB_TYPE_STRING)
			return (DCMD_USAGE);

		offset = mdb_strtoull(argv[2].a_un.a_str);
	}

	for (clp = v8_classes; clp != NULL; clp = clp->v8c_next)
		if (strcmp(clp->v8c_name, klass) == 0)
			break;

	if (clp == NULL) {
		(void) mdb_printf("error: no such class: \"%s\"", klass);
		return (DCMD_ERR);
	}

	for (flp = clp->v8c_fields; flp != NULL; flp = flp->v8f_next)
		if (strcmp(field, flp->v8f_name) == 0)
			break;

	if (flp == NULL) {
		if (argc == 2) {
			mdb_printf("error: no such field in class \"%s\": "
			    "\"%s\"", klass, field);
			return (DCMD_ERR);
		}

		flp = conf_field_create(clp, field, offset);
		if (flp == NULL) {
			mdb_warn("failed to create field");
			return (DCMD_ERR);
		}
	} else if (argc == 3) {
		flp->v8f_offset = offset;
	}

	mdb_printf("%s::%s at offset 0x%x\n", klass, field, flp->v8f_offset);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8array(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8fixedarray_t *arrayp;
	uintptr_t *elts;
	size_t i, len;

	if ((arrayp = v8fixedarray_load(addr, UM_SLEEP | UM_GC)) == NULL) {
		return (DCMD_ERR);
	}

	elts = v8fixedarray_elts(arrayp);
	len = v8fixedarray_length(arrayp);

	for (i = 0; i < len; i++)
		mdb_printf("%p\n", elts[i]);

	v8fixedarray_free(arrayp);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_jsstack(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t raddr;
	jsframe_t jsf;

	bzero(&jsf, sizeof (jsf));
	jsf.jsf_nlines = 5;

	if (mdb_getopts(argc, argv,
	    'a', MDB_OPT_SETBITS, B_TRUE, &jsf.jsf_showall,
	    'v', MDB_OPT_SETBITS, B_TRUE, &jsf.jsf_verbose,
	    'f', MDB_OPT_STR, &jsf.jsf_func,
	    'n', MDB_OPT_UINTPTR, &jsf.jsf_nlines,
	    'p', MDB_OPT_STR, &jsf.jsf_prop,
	    NULL) != argc)
		return (DCMD_USAGE);

	/*
	 * The "::jsframe" walker iterates the valid frame pointers, but the
	 * "::jsframe" dcmd looks at the frame after the one it was given, so we
	 * have to explicitly examine the top frame here.
	 */
	if (!(flags & DCMD_ADDRSPEC)) {
		if (load_current_context(&addr, &raddr) != 0 ||
		    do_jsframe(addr, raddr, &jsf) != 0)
			return (DCMD_ERR);
	}

	if (mdb_pwalk_dcmd("jsframe", "jsframe", argc, argv, addr) == -1)
		return (DCMD_ERR);

	jsframe_print_skipped(&jsf);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8str(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	boolean_t opt_v = B_FALSE;
	boolean_t opt_r = B_FALSE;
	int64_t bufsz = -1;
	v8string_t *strp;
	mdbv8_strbuf_t *strb;

	if (mdb_getopts(argc, argv,
	    'v', MDB_OPT_SETBITS, B_TRUE, &opt_v,
	    'N', MDB_OPT_UINT64, &bufsz,
	    'r', MDB_OPT_SETBITS, B_TRUE, &opt_r, NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((strp = v8string_load(addr, UM_GC)) == NULL) {
		return (DCMD_ERR);
	}

	if (bufsz == -1) {
		/*
		 * The buffer size should accommodate the length of the string,
		 * plus the surrounding quotes, plus the terminator.  (If we're
		 * wrong here, the visible string will just be truncated.)
		 */
		bufsz = v8string_length(strp) + sizeof ("\"\"");
	}

	if ((strb = mdbv8_strbuf_alloc(bufsz, UM_GC)) == NULL) {
		return (DCMD_ERR);
	}

	if (v8string_write(strp, strb,
	    opt_r ? MSF_ASCIIONLY : MSF_JSON,
	    (opt_v ? JSSTR_VERBOSE : JSSTR_NONE) |
	    (opt_r ? JSSTR_NONE : JSSTR_QUOTED)) != 0)
		return (DCMD_ERR);

	mdb_printf("%s\n", mdbv8_strbuf_tocstr(strb));
	return (DCMD_OK);
}

static void
dcmd_v8load_help(void)
{
	v8_cfg_t *cfp, **cfgpp;

	mdb_printf(
	    "To traverse in-memory V8 structures, the V8 dmod requires\n"
	    "configuration that describes the layout of various V8 structures\n"
	    "in memory.  Normally, this information is pulled from metadata\n"
	    "in the target binary.  However, it's possible to use the module\n"
	    "with a binary not built with metadata by loading one of the\n"
	    "canned configurations.\n\n");

	mdb_printf("Available configurations:\n");

	(void) mdb_inc_indent(4);

	for (cfgpp = v8_cfgs; *cfgpp != NULL; cfgpp++) {
		cfp = *cfgpp;
		mdb_printf("%-10s    %s\n", cfp->v8cfg_name, cfp->v8cfg_label);
	}

	(void) mdb_dec_indent(4);
}

/* ARGSUSED */
static int
dcmd_v8load(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8_cfg_t *cfgp = NULL, **cfgpp;

	if (v8_classes != NULL) {
		mdb_warn("v8 module already configured\n");
		return (DCMD_ERR);
	}

	if (argc < 1 || argv->a_type != MDB_TYPE_STRING)
		return (DCMD_USAGE);

	for (cfgpp = v8_cfgs; *cfgpp != NULL; cfgpp++) {
		cfgp = *cfgpp;
		if (strcmp(argv->a_un.a_str, cfgp->v8cfg_name) == 0)
			break;
	}

	if (cfgp == NULL || cfgp->v8cfg_name == NULL) {
		mdb_warn("unknown configuration: \"%s\"\n", argv->a_un.a_str);
		return (DCMD_ERR);
	}

	if (autoconfigure(cfgp) == -1) {
		mdb_warn("autoconfigure failed\n");
		return (DCMD_ERR);
	}

	mdb_printf("V8 dmod configured based on %s\n", cfgp->v8cfg_name);
	return (DCMD_OK);
}

/* ARGSUSED */
static int
dcmd_v8warnings(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	v8_warnings ^= 1;
	mdb_printf("v8 warnings are now %s\n", v8_warnings ? "on" : "off");

	return (DCMD_OK);
}

static int
walk_jsframes_init(mdb_walk_state_t *wsp)
{
	if (wsp->walk_addr != NULL)
		return (WALK_NEXT);

	if (load_current_context(&wsp->walk_addr, NULL) != 0)
		return (WALK_ERR);

	return (WALK_NEXT);
}

static int
walk_jsframes_step(mdb_walk_state_t *wsp)
{
	uintptr_t addr, next;
	int rv;

	addr = wsp->walk_addr;
	rv = wsp->walk_callback(wsp->walk_addr, NULL, wsp->walk_cbdata);

	if (rv != WALK_NEXT)
		return (rv);

	if (mdb_vread(&next, sizeof (next), addr) == -1)
		return (WALK_ERR);

	if (next == NULL)
		return (WALK_DONE);

	wsp->walk_addr = next;
	return (WALK_NEXT);
}

typedef struct jsprop_walk_data {
	int jspw_nprops;
	int jspw_current;
	uintptr_t *jspw_props;
} jsprop_walk_data_t;

/*ARGSUSED*/
static int
walk_jsprop_nprops(const char *desc, v8propvalue_t *valp, void *arg)
{
	jsprop_walk_data_t *jspw = arg;

	/*
	 * Regrettably, there's really no way to make "::walk jsprop" include
	 * unboxed floating-point values, because there's no way to emit them in
	 * a way that can be read again by anything else.
	 */
	if (valp->v8v_isboxeddouble)
		return (0);

	jspw->jspw_nprops++;

	return (0);
}

/*ARGSUSED*/
static int
walk_jsprop_props(const char *desc, v8propvalue_t *valp, void *arg)
{
	jsprop_walk_data_t *jspw = arg;

	if (valp->v8v_isboxeddouble)
		return (0);

	jspw->jspw_props[jspw->jspw_current++] = valp->v8v_u.v8vu_addr;

	return (0);
}

static int
walk_jsprop_init(mdb_walk_state_t *wsp)
{
	jsprop_walk_data_t *jspw;
	uintptr_t addr;
	uint8_t type;

	if ((addr = wsp->walk_addr) == NULL) {
		mdb_warn("'jsprop' does not support global walks\n");
		return (WALK_ERR);
	}

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0 ||
	    type != V8_TYPE_JSOBJECT) {
		mdb_warn("%p is not a JSObject\n", addr);
		return (WALK_ERR);
	}

	jspw = mdb_zalloc(sizeof (jsprop_walk_data_t), UM_SLEEP | UM_GC);

	if (jsobj_properties(addr, walk_jsprop_nprops, jspw, NULL) == -1) {
		mdb_warn("couldn't iterate over properties for %p\n", addr);
		return (WALK_ERR);
	}

	jspw->jspw_props = mdb_zalloc(jspw->jspw_nprops *
	    sizeof (uintptr_t), UM_SLEEP | UM_GC);

	if (jsobj_properties(addr, walk_jsprop_props, jspw, NULL) == -1) {
		mdb_warn("couldn't iterate over properties for %p\n", addr);
		return (WALK_ERR);
	}

	jspw->jspw_current = 0;
	wsp->walk_data = jspw;

	return (WALK_NEXT);
}

static int
walk_jsprop_step(mdb_walk_state_t *wsp)
{
	jsprop_walk_data_t *jspw = wsp->walk_data;
	int rv;

	if (jspw->jspw_current >= jspw->jspw_nprops)
		return (WALK_DONE);

	if ((rv = wsp->walk_callback(jspw->jspw_props[jspw->jspw_current++],
	    NULL, wsp->walk_cbdata)) != WALK_NEXT)
		return (rv);

	return (WALK_NEXT);
}

/*
 * MDB linkage
 */

static const mdb_dcmd_t v8_mdb_dcmds[] = {
	/*
	 * Commands to inspect Node-level state
	 */
	{ "nodebuffer", ":[-a]",
		"print details about the given Node Buffer", dcmd_nodebuffer },

	/*
	 * Commands to inspect JavaScript-level state
	 */
	{ "jsclosure", ":", "print variables referenced by a closure",
		dcmd_jsclosure },
	{ "jsconstructor", ":[-v]",
		"print the constructor for a JavaScript object",
		dcmd_jsconstructor },
	{ "jsframe", ":[-aiv] [-f function] [-p property] [-n numlines]",
		"summarize a JavaScript stack frame", dcmd_jsframe },
	{ "jsfunction", ":", "print information about a JavaScript function",
		dcmd_jsfunction },
	{ "jsprint", ":[-ab] [-d depth] [member]", "print a JavaScript object",
		dcmd_jsprint },
	{ "jssource", ":[-n numlines]",
		"print the source code for a JavaScript function",
		dcmd_jssource },
	{ "jsstack", "[-av] [-f function] [-p property] [-n numlines]",
		"print a JavaScript stacktrace", dcmd_jsstack },
	{ "findjsobjects", "?[-vb] [-r | -c cons | -p prop]", "find JavaScript "
		"objects", dcmd_findjsobjects, dcmd_findjsobjects_help },
	{ "jsfunctions", "?[-X] [-s file_filter] [-n name_filter] "
	    "[-x instr_filter]", "list JavaScript functions",
	    dcmd_jsfunctions, dcmd_jsfunctions_help },

	/*
	 * Commands to inspect V8-level state
	 */
	{ "v8array", ":", "print elements of a V8 FixedArray",
		dcmd_v8array },
	{ "v8classes", NULL, "list known V8 heap object C++ classes",
		dcmd_v8classes },
	{ "v8code", ":[-d]", "print information about a V8 Code object",
		dcmd_v8code },
	{ "v8context", ":[-d]", "print information about a V8 Context object",
		dcmd_v8context },
	{ "v8field", "classname fieldname offset",
		"manually add a field to a given class", dcmd_v8field },
	{ "v8function", ":[-d]", "print JSFunction object details",
		dcmd_v8function },
	{ "v8internal", ":[fieldidx]", "print v8 object internal fields",
		dcmd_v8internal },
	{ "v8load", "version", "load canned config for a specific V8 version",
		dcmd_v8load, dcmd_v8load_help },
	{ "v8frametypes", NULL, "list known V8 frame types",
		dcmd_v8frametypes },
	{ "v8print", ":[class]", "print a V8 heap object",
		dcmd_v8print, dcmd_v8print_help },
	{ "v8str", ":[-v]", "print the contents of a V8 string",
		dcmd_v8str },
	{ "v8scopeinfo", ":", "print information about a V8 ScopeInfo object",
		dcmd_v8scopeinfo },
	{ "v8type", ":", "print the type of a V8 heap object",
		dcmd_v8type },
	{ "v8types", NULL, "list known V8 heap object types",
		dcmd_v8types },
	{ "v8warnings", NULL, "toggle V8 warnings",
		dcmd_v8warnings },

	{ NULL }
};

static const mdb_walker_t v8_mdb_walkers[] = {
	{ "jsframe", "walk V8 JavaScript stack frames",
		walk_jsframes_init, walk_jsframes_step },
	{ "jsprop", "walk property values for an object",
		walk_jsprop_init, walk_jsprop_step },
	{ NULL }
};

static mdb_modinfo_t v8_mdb = { MDB_API_VERSION, v8_mdb_dcmds, v8_mdb_walkers };

static void
configure(void)
{
	char *success;
	v8_cfg_t *cfgp = NULL;
	GElf_Sym sym;
	int major, minor, build, patch;

	if (mdb_readsym(&major, sizeof (major),
	    "_ZN2v88internal7Version6major_E") == -1 ||
	    mdb_readsym(&minor, sizeof (minor),
	    "_ZN2v88internal7Version6minor_E") == -1 ||
	    mdb_readsym(&build, sizeof (build),
	    "_ZN2v88internal7Version6build_E") == -1 ||
	    mdb_readsym(&patch, sizeof (patch),
	    "_ZN2v88internal7Version6patch_E") == -1) {
		mdb_warn("failed to determine V8 version");
		return;
	}

	v8_major = major;
	v8_minor = minor;
	v8_build = build;
	v8_patch = patch;
	mdb_printf("V8 version: %d.%d.%d.%d\n",
	    v8_major, v8_minor, v8_build, v8_patch);

	/*
	 * First look for debug metadata embedded within the binary, which may
	 * be present in recent V8 versions built with postmortem metadata.
	 */
	if (mdb_lookup_by_name("v8dbg_SmiTag", &sym) == 0) {
		cfgp = &v8_cfg_target;
		success = "Autoconfigured V8 support from target";
	} else if (v8_major == 3 && v8_minor == 1 && v8_build == 8) {
		cfgp = &v8_cfg_04;
		success = "Configured V8 support based on node v0.4";
	} else if (v8_major == 3 && v8_minor == 6 && v8_build == 6) {
		cfgp = &v8_cfg_06;
		success = "Configured V8 support based on node v0.6";
	} else {
		mdb_printf("mdb_v8: target has no debug metadata and "
		    "no existing config found\n");
		return;
	}

	if (autoconfigure(cfgp) != 0) {
		mdb_warn("failed to autoconfigure from target; "
		    "commands may have incorrect results!\n");
		return;
	}

	mdb_printf("%s\n", success);
}

static void
enable_demangling(void)
{
	const char *symname = "_ZN2v88internal7Version6major_E";
	GElf_Sym sym;
	char buf[64];

	/*
	 * Try to determine whether C++ symbol demangling has been enabled.  If
	 * not, enable it.
	 */
	if (mdb_lookup_by_name("_ZN2v88internal7Version6major_E", &sym) != 0)
		return;

	(void) mdb_snprintf(buf, sizeof (buf), "%a", sym.st_value);
	if (strstr(buf, symname) != NULL)
		(void) mdb_eval("$G");
}

const mdb_modinfo_t *
_mdb_init(void)
{
	mdb_printf("mdb_v8 version: %d.%d.%d (%s)\n", mdbv8_vers_major,
	    mdbv8_vers_minor, mdbv8_vers_micro, mdbv8_vers_tag);
	configure();
	enable_demangling();
	return (&v8_mdb);
}
