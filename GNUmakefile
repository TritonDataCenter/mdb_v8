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
# MDB, so the build will not work there.  You can in principle configure most of
# the items under the CONFIGURATION sections, but you should not need to.
#


#
# CONFIGURATION FOR BUILDERS
#

# Directory for output objects
MDBV8_BUILD	 = build

# Output object name
MDBV8_SONAME	 = mdb_v8.so


#
# CONFIGURATION FOR DEVELOPERS
#

#
# List of source files that will become objects.  (These entries do not include
# the "src/" directory prefix.)
#
MDBV8_SOURCES		 = mdb_v8.c mdb_v8_cfg.c

# List of source files to run through cstyle.  This includes header files.
MDBV8_CSTYLE_SOURCES	 = $(wildcard src/*.c src/*.h)

# Compiler flags
CFLAGS			+= -Werror -Wall -Wextra -fPIC -fno-omit-frame-pointer

# XXX These should be removed.
CFLAGS			+= -Wno-unused-parameter		\
			   -Wno-missing-field-initializers	\
			   -Wno-sign-compare 

# Linker flags (including dependent libraries)
LDFLAGS			+= -lproc -lavl
SOFLAGS			 = -Wl,-soname=$(MDBV8_SONAME)

# Path to cstyle.pl tool
CSTYLE			 = tools/cstyle.pl
CSTYLE_FLAGS		+= -cCp


#
# INTERNAL DEFINITIONS
#
MDBV8_ARCH = ia32
include Makefile.arch.defs
MDBV8_ARCH = amd64
include Makefile.arch.defs


#
# DEFINITIONS USED AS RECIPES
#
MKDIRP		 = mkdir -p $@
COMPILE_ia32.c	 = $(CC) -o $@ -c $(CFLAGS) $(CPPFLAGS) $^
COMPILE_amd64.c	 = $(CC) -o $@ -c -m64 $(CFLAGS) $(CPPFLAGS) $^
MAKESO_ia32	 = $(CC) -o $@ -shared $(SOFLAGS) $(LDFLAGS) $^
MAKESO_amd64	 = $(CC) -o $@ -m64 -shared $(SOFLAGS) $(LDFLAGS) $^


#
# TARGETS
#
.PHONY: all
all: $(MDBV8_ALLTARGETS)

.PHONY: check
check:
	$(CSTYLE) $(CSTYLE_FLAGS) $(MDBV8_CSTYLE_SOURCES)

.PHONY: clean
clean:
	-rm -rf $(MDBV8_BUILD)

#
# This is a little janky, but there are two pieces here: We include
# Makefile.arch.targ twice: once to define the ia32 targets and once to define
# the amd64 targets.  Those targets also use MDBV8_ARCH in their recipes, and
# those are evaluated lazily, so we must _also_ define target-specific values
# fro MDBV8_ARCH.
#
MDBV8_ARCH=ia32
include Makefile.arch.targ
MDBV8_ARCH=amd64
include Makefile.arch.targ

$(MDBV8_DYLIB_ia32)  $(MDBV8_OBJECTS_ia32):  MDBV8_ARCH = ia32
$(MDBV8_DYLIB_amd64) $(MDBV8_OBJECTS_amd64): MDBV8_ARCH = amd64
