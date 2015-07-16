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
MDBV8_SOURCES	 = mdb_v8.c mdb_v8_cfg.c

# List of source files to run through cstyle.  This includes header files.
MDBV8_CSTYLE_SOURCES = $(wildcard src/*.c src/*.h)

# Compiler flags
CFLAGS		+= -fPIC
CFLAGS		+= -Werror -Wall -Wextra -fno-omit-frame-pointer

# XXX These should be removed.
CFLAGS		+= -Wno-unused-parameter		\
		   -Wno-missing-field-initializers	\
		   -Wno-sign-compare 

# Linker flags (including dependent libraries)
LDFLAGS		+= -lproc -lavl
SOFLAGS		 = -Wl,-soname=$(MDBV8_SONAME)

# Path to cstyle.pl tool
CSTYLE		 = tools/cstyle.pl
CSTYLE_FLAGS	+= -cCp


#
# INTERNAL DEFINITIONS
#
MDBV8_BUILD32	 = $(MDBV8_BUILD)/ia32
MDBV8_BUILD64	 = $(MDBV8_BUILD)/amd64
MDBV8_DYLIB32	 = $(MDBV8_BUILD32)/$(MDBV8_SONAME)
MDBV8_DYLIB64	 = $(MDBV8_BUILD64)/$(MDBV8_SONAME)
MDBV8_OBJECTS32	 = $(MDBV8_SOURCES:%.c=$(MDBV8_BUILD32)/%.o)
MDBV8_OBJECTS64	 = $(MDBV8_SOURCES:%.c=$(MDBV8_BUILD64)/%.o)


#
# DEFINITIONS USED AS RECIPES
#
MKDIRP		 = mkdir -p $@
COMPILE.c	 = $(CC) -o $@ -c $(CFLAGS) $(CPPFLAGS) $^
COMPILE64.c	 = $(CC) -o $@ -c -m64 $(CFLAGS) $(CPPFLAGS) $^
MAKESO		 = $(CC) -o $@ -shared $(SOFLAGS) $(LDFLAGS) $^
MAKESO64	 = $(CC) -o $@ -m64 -shared $(SOFLAGS) $(LDFLAGS) $^


#
# TARGETS
#
.PHONY: all
all: $(MDBV8_DYLIB32) $(MDBV8_DYLIB64)

.PHONY: check
check:
	$(CSTYLE) $(CSTYLE_FLAGS) $(MDBV8_CSTYLE_SOURCES)

.PHONY: clean
clean:
	-rm -rf $(MDBV8_BUILD)

$(MDBV8_DYLIB32): $(MDBV8_OBJECTS32)
	$(MAKESO)

$(MDBV8_DYLIB64): $(MDBV8_OBJECTS64)
	$(MAKESO64)

$(MDBV8_OBJECTS32): $(MDBV8_BUILD32)/%.o: src/%.c | $(MDBV8_BUILD32)
	$(COMPILE.c)

$(MDBV8_OBJECTS64): $(MDBV8_BUILD64)/%.o: src/%.c | $(MDBV8_BUILD64)
	$(COMPILE64.c)

$(MDBV8_BUILD32):
	$(MKDIRP)

$(MDBV8_BUILD64):
	$(MKDIRP)
