#
# Copyright (c) 2015, Joyent, Inc.
#

#
# Makefile for the V8 MDB dmod.  This build produces "mdb_v8.so" in the "build"
# directory.
#
# Targets:
#
#     all	builds the mdb_v8.so shared object
#
#     clean	removes all generated files
#

#
# CONFIGURATION
#

# Directory for output objects
MDBV8_BUILD	 = build

# List of sources files (not including "src/" directory prefix)
MDBV8_SOURCES	 =	\
		    mdb_v8.c		\
		    mdb_v8_cfg.c

# Compiler flags
CFLAGS		+= -Werror -Wall -Wextra -fno-omit-frame-pointer

# XXX These should be removed.
CFLAGS		+= -Wno-unused-parameter		\
		   -Wno-missing-field-initializers	\
		   -Wno-sign-compare 

# Linker flags (including dependent libraries)
LDFLAGS		+= -lproc -lavl

# Output object name
MDBV8_DYLIB	 = $(MDBV8_BUILD)/mdb_v8.so


#
# DEFINITIONS
#
MDBV8_OBJECTS	 = $(MDBV8_SOURCES:%.c=$(MDBV8_BUILD)/%.o)


#
# TARGETS
#
.PHONY: all
all: $(MDBV8_DYLIB)

.PHONY: clean
clean:
	-rm -rf $(MDBV8_BUILD)

$(MDBV8_DYLIB): $(MDBV8_OBJECTS)
	$(CC) -shared -o $@ -Wl,-soname=mdb_v8.so $(LDFLAGS) $^

$(MDBV8_OBJECTS): $(MDBV8_BUILD)/%.o: src/%.c | $(MDBV8_BUILD)
	$(CC) -o $@ -c -fPIC $(CPPFLAGS) $(CFLAGS) $^

$(MDBV8_BUILD):
	mkdir -p $@
