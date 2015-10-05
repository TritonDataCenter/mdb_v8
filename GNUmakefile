#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

#
# Copyright (c) 2015, Joyent, Inc.
#

#
# Makefile for the V8 MDB dmod.  This build produces 32-bit and 64-bit binaries
# for mdb_v8 in the "build" directory.
#
# Targets:
#
#     all	builds the mdb_v8.so shared objects
#
#     check	run style checker on source files
#
#     clean	removes all generated files
#
# This Makefile has been designed to work out of the box (without manual or
# automatic configuration) on illumos systems.  Other systems do not support
# MDB, so the build will not work there.  In principle, you can change most
# of the items under the CONFIGURATION sections, but you should not need to.
#

#
# CONFIGURATION FOR BUILDERS
#

# Directory for output objects
MDBV8_BUILD	 = build
# Output object name
MDBV8_SONAME	 = mdb_v8.so
# Tag binaries with "dev" or "release".  This should be a valid C string.
MDBV8_VERS_TAG	 = "dev"


#
# CONFIGURATION FOR DEVELOPERS
#

#
# List of source files that will become objects.  (These entries do not include
# the "src/" directory prefix.)
#
MDBV8_SOURCES		 = mdb_v8.c mdb_v8_cfg.c mdb_v8_context.c
MDBV8_GENSOURCES	 = mdb_v8_version.c

# List of source files to run through cstyle.  This includes header files.
MDBV8_CSTYLE_SOURCES	 = $(wildcard src/*.c src/*.h)

# Compiler flags
CFLAGS			+= -Werror -Wall -Wextra -fPIC -fno-omit-frame-pointer
CPPFLAGS		+= -DMDBV8_VERS_TAG='$(MDBV8_VERS_TAG)'
# XXX These should be removed.
CFLAGS			+= -Wno-unused-parameter		\
			   -Wno-missing-field-initializers	\
			   -Wno-sign-compare 
# This is necessary for including avl_impl.h.
CFLAGS			+= -Wno-unknown-pragmas

# Linker flags (including dependent libraries)
LDFLAGS			+= -lproc
SOFLAGS			 = -Wl,-soname=$(MDBV8_SONAME)

# Path to cstyle.pl tool
CSTYLE			 = tools/cstyle.pl
CSTYLE_FLAGS		+= -cCp

# Path to catest tool
CATEST			 = tools/catest

# JavaScript source files (used in test code)
JS_FILES		 = $(wildcard test/standalone/*.js)
JSL_FILES_NODE		 = $(JS_FILES)
JSSTYLE_FILES		 = $(JS_FILES)
JSL_CONF_NODE		 = tools/jsl.node.conf


#
# INTERNAL DEFINITIONS
#
MDBV8_ARCH = ia32
include Makefile.arch.defs
MDBV8_ARCH = amd64
include Makefile.arch.defs

LIBAVL_SUBMODULE	 = deps/illumos-libavl
CPPFLAGS		+= -I$(LIBAVL_SUBMODULE)/include

$(LIBAVL_amd64):	CFLAGS_ARCH += -m64
$(MDBV8_TARGETS_amd64):	CFLAGS	+= -m64
$(MDBV8_TARGETS_amd64):	SOFLAGS	+= -m64

$(LIBAVL_ia32):		CFLAGS_ARCH += -m32
$(MDBV8_TARGETS_ia32):	CFLAGS += -m32
$(MDBV8_TARGETS_ia32):	SOFLAGS += -m32

#
# DEFINITIONS USED AS RECIPES
#
MKDIRP		 = mkdir -p $@
COMPILE.c	 = $(CC) -o $@ -c $(CFLAGS) $(CPPFLAGS) $^
MAKESO	 	 = $(CC) -o $@ -shared $(SOFLAGS) $(LDFLAGS) $^
GITDESCRIBE	 = $(shell git describe --all --long --dirty | \
    awk -F'-g' '{print $$NF}')

#
# TARGETS
#
.PHONY: all
all: $(MDBV8_ALLTARGETS)

check: check-cstyle

.PHONY: check-cstyle
check-cstyle: 
	$(CSTYLE) $(CSTYLE_FLAGS) $(MDBV8_CSTYLE_SOURCES)

CLEAN_FILES += $(MDBV8_BUILD)

.PHONY: test
test: $(MDBV8_ALLTARGETS)
	$(CATEST) -a

.PHONY: prepush
prepush: check test

.PHONY: release
release: MDBV8_VERS_TAG := "release, from $(GITDESCRIBE)"
release: clean $(MDBV8_ALLTARGETS)

#
# Makefile.arch.targ is parametrized by MDBV8_ARCH.  It defines a group of
# the current value of MDBV8_ARCH.  When we include it the first time, it
# defines targets for the 32-bit object files and shared object file.  The
# second time, it defines targets for the 64-bit object files and shared object
# file.  This avoids duplicating Makefile snippets, though admittedly today
# these snippets are short enough that it hardly makes much difference.
#
MDBV8_ARCH=ia32
include Makefile.arch.targ
MDBV8_ARCH=amd64
include Makefile.arch.targ

#
# mdb_v8_version.c is generated based on the "version" file.  The version number
# is baked into the binary itself (so the dmod can report its version) and also
# used in the publish target.
#
$(MDBV8_BUILD)/mdb_v8_version.c: version
	tools/mkversion < $^ > $@

#
# Include common Joyent Makefile for JavaScript "check" targets.
#
include Makefile.targ
