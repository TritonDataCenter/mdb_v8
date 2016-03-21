<!--
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
-->

<!--
    Copyright (c) 2015, Joyent, Inc.
-->

# mdb_v8: postmortem debugging for Node.js

This repository contains the canonical source for mdb\_v8, an
[mdb](http://illumos.org/man/1/mdb) debugger module ("dmod") for debugging both
live processes and core dumps of programs using Google's [V8 JavaScript
engine](https://developers.google.com/v8/), and particularly
[Node.js](https://nodejs.org/).  This module fully supports Node versions 5, 4,
0.12, and 0.10.  Basic functionality (stack traces, printing out objects, and
using `findjsobjects`) should also work on Node versions 0.8, 0.6, and 0.4, but
those versions are not regularly tested.

Downstream versions of mdb\_v8 exist in both Node.js and SmartOS.  See
[CHANGES.md](CHANGES.md) for details.


## Using mdb_v8

**For information about using these tools, see the [usage
guide](docs/usage.md).**


## Building from source

You can build mdb\_v8 by cloning this repository and running `make`.  It will
only build and run on illumos-based systems.  See the [usage
guide](docs/usage.md) for details on system support.


## Binary downloads

Binaries for mdb\_v8 can be found at
https://us-east.manta.joyent.com/Joyent_Dev/public/mdb_v8.  If you have the
[Manta command-line tools](https://www.npmjs.com/package/manta) installed, you
can list the latest binaries with:

    $ mfind -t o $(mget -q /Joyent_Dev/public/mdb_v8/latest)
    /Joyent_Dev/public/mdb_v8/v1.1.1/mdb_v8_amd64.so
    /Joyent_Dev/public/mdb_v8/v1.1.1/mdb_v8_ia32.so

You can fetch a specific binary like this (in this case, the 32-bit version
1.1.1 binary):

    $ mget -O /Joyent_Dev/public/mdb_v8/v1.1.1/mdb_v8_ia32.so

or using curl:

    $ curl -O https://us-east.manta.joyent.com/Joyent_Dev/public/mdb_v8/v1.1.1/mdb_v8_ia32.so

This one-liner will get you the latest 32-bit binary:

    $ mget -O $(mget -q /Joyent_Dev/public/mdb_v8/latest)/mdb_v8_ia32.so


## Design goals

An important design constraint on this tool is that it should not rely on
assistance from the JavaScript runtime environment (i.e., V8) to debug Node.js
programs.  This is for many reasons:

* In production, it's extremely valuable to be able to save a core file and then
  restart the program (or let it keep running, but undisturbed by a debugger).
  This allows you to restore service quickly, but still debug the problem later.
* There are many important failure modes where support from the runtime is not
  available, including crashes in the VM itself, crashes in native libraries and
  add-ons, and cases where the threads that could provide that support are
  stuck, as in a tight loop (or blocked on other threads that are looping).
* By not requiring runtime support, it's possible to [stop the program at very
  specific points of execution (using other tools), save a core file, and then
  set the program running again with minimal
  disruption](https://www.joyent.com/blog/stopping-a-broken-program-in-its-tracks).
  With tools like DTrace, you can stop the program at points that the VM can't
  know about, like when a thread is taken off-CPU.
* Many issues span both native code and JavaScript code (e.g., native memory
  leaks induced by JavaScript calls), where it's useful to have both native and
  JavaScript state available.

In short, there are many kinds of problems that cannot be debugged with a
debugger that relies on the running process to help debug itself.  The ACM Queue
article [Postmortem Debugging in Dynamic
Environments](https://queue.acm.org/detail.cfm?id=2039361) outlines the history
and motivation for postmortem debugging and the challenges underlying postmortem
debugging in higher-level languages.


## Implementation notes

We built this tool on mdb for two reasons:

* mdb provides a rich plugin interface through which dmods (debugger modules)
  can define their own walkers and commands.  These commands can function in a
  pipeline, sending and receiving output to and from other commands.  These
  commands aren't just macros -- they're documented, have options similar to
  Unix command-line tools, they can build up their own data structures, and so
  on.  Plugins run in the address space of the debugger, not the program being
  debugged.
* mdb abstracts the notion of a _target_, so the same dmod can be used to debug
  both live processes and core files.  mdb\_v8 uses mdb's built in facilities
  for safely listing symbols, reading memory from the core file, emitting
  output, and so on, without knowing how to do any of that itself.

In order to provide postmortem support, mdb\_v8 has to grok a number of internal
implementation details of the V8 VM.  Some algorithms, like property iteration,
are (regrettably) duplicated inside mdb\_v8.  But many pieces, particularly
related to the structure of V8 internal fields, are dynamically configured based
on the process being debugged.  It works like this:

* As part of the V8 build process, a C++ file is
  [generated](https://github.com/v8/v8/blob/master/tools/gen-postmortem-metadata.py)
  that defines a number of global `int`s that describe the class hierarchy that
  V8 uses to represent Heap objects.  The class names, their inheritance
  hierarchy, and their field names are described by the names of these
  constants, and the values describe offsets of fields inside class instances.
  This C++ file is linked into the final V8 binary.

  You can think of the debug metadata as debug information similar to DWARF or
  CTF, but it's considerably lighter-weight than DWARF and much easier to
  interpret.  Besides that, because of the way V8 defines heap classes,
  traditional DWARF or CTF would not have been sufficient to encode the
  information we needed because many of the relevant classes and nearly all of
  the class members are totally synthetic at compile-time and not present at all
  in the final V8 binary.

  Because of this approach (rather than, say, attempting to parse the C++ header
  files that describe up the V8 heap), the values of these constants are always
  correct for the program being debugged, whether it's 32-bit, 64-bit, or has
  any compile-time configuration options altered that would affect these
  structures.
* When mdb\_v8 starts up, it reads the values of these symbols from the program
  being debugged and uses that information to traverse V8 heap structures.

An ideal solution would avoid duplicating any VM knowledge in the debugger
module.  There are two obvious approaches for doing that:

1. In addition to encoding heap structure in the binary at build-time, encode
   algorithmic pieces as well.  This could use a mechanism similar to the
   [DTrace ustack
   helper](http://dtrace.org/blogs/dap/2013/11/20/understanding-dtrace-ustack-helpers/),
   which allows VMs to encode deep internal details in a way that even the
   kernel can safely use, even in delicate kernel contexts.  To get to this
   point would require figuring out all the kinds of information a debugger
   might need and figuring out how the VM could encode it in production binaries
   (i.e., efficiently) for execution by an arbitrary debugger.
2. Alternatively, VMs could provide their own standalone postmortem debugging
   tools that could reconstituting a program's state from a core file and then
   providing a normal debugging interface.  Those debuggers wouldn't necessarily
   help with issues that span both native and JavaScript code.

Both of these approach require considerable first-class support from the VM (and
team, who would have to maintain these implementations), which does not seem to
exist for the case of V8 (or any other VM we know of).  The existing approach
requires minimal maintenance because much of the metadata is created through
the same mechanisms in the build process that define the classes and fields
themselves.


## Contributing

See the [Developer's Notes](docs/development.md) for details.


## License

With the exception of the "cstyle.pl" tool, all components in this repo are
licensed under the MPL 2.0.  "cstyle.pl" is licensed under the CDDL.  (Various
pieces in this repo were historically released under other open source licenses
as well.)
