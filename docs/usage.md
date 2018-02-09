<!--
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
-->

<!--
    Copyright (c) 2017, Joyent, Inc.
-->

# Postmortem debugging with mdb_v8

Quick links:

* [Supported environments](#supported-environments)
* [MDB Prerequisites](#mdb-prerequisites)
* [Tutorial](#tutorial)
* [Tips for using the postmortem technique](#tips-for-using-the-postmortem-technique)
* [Building programs for debugging](#building-programs-for-debugging)
* [Node-specific MDB command reference](#node-specific-mdb-command-reference)

Postmortem debugging is the process of debugging a program from a snapshot of
its internal state, usually after the program has crashed.  This state is
usually in the form of a *core file*.  On Unix-like systems, core files are
generated automatically for most C and C++ programs when they crash.  You can
configure Node.js to save a core file when your Node program would otherwise
crash by using the `--abort-on-uncaught-exception` flag when running Node.  You
can also save a core file of any *running* program using the gcore(1) program.
This allows you to use the postmortem debugging tools to analyze programs that
haven't actually crashed (e.g., to examine memory usage).

The postmortem approach has several major advantages over other techniques:

* For crashes, core files provide enormously more information than the stack
  trace that normally accompanies an uncaught exception.  Because of this, the
  problem usually does not need to be reproducible in order to debug it.
* When a program has not crashed but is instead behaving wrongly (e.g.,
  a web server returning 500 errors) and you think that restarting it may
  address the problem, saving a core file first can enable you to restore
  service (by restarting your program) and still debug the problem later (by
  analyzing the core file).  In general, saving a core file of a running but
  misbehaving broken allows you to patch things up now and root-cause the
  problem later.
* Since you can copy core files across machines and analyze them later, you can
  afford to run sophisticated, time-consuming analysis that may not be possible
  on a production system while the program is still running.  For example, when
  it's not reasonable in production to enable V8 flags to profile heap objects,
  it's still possible to save a core file of a production service and then
  analyze the distribution of objects after the fact.
* Postmortem debugging works for cases where other methods don't, including when
  your Node program has entered an infinite loop or when V8 itself is broken.

Operating systems have had rich support for postmortem debugging of native
programs (i.e., C programs, including the OS kernel itself) for many years.
There are several challenges associated with supporting dynamic environments
like Node.js, and we're not aware of any other major environments that provide
such support.

Node binaries since version 0.8 have contained debugging metadata that can be
processed by debuggers to read V8's in-memory structures.  This allows debuggers
to print out JavaScript-level stack traces and dump out JavaScript objects from
the core file.


## Supported environments

This document describes mdb\_v8, a debugger module for the [Modular Debugger
(MDB)](http://illumos.org/man/mdb) that's shipped on
[illumos](http://illumos.org)-based operating systems like
[SmartOS](http://smartos.org).

mdb\_v8 is the only program (that we know of) that uses the debug information
contained in Node binaries.  It also ships with metadata for Node 0.6 and 0.4,
so it's capable of debugging core files from Node 0.4 through Node 4.0.

While MDB itself only runs on illumos, it's capable of understanding core files
from GNU/Linux systems as well.

In short, in order to use these tools, you'll want:

* a core file from any Node program from version v0.4 through version v4,
  created on either an illumos or GNU/Linux system
* access to an illumos system on which to run mdb
* a copy of mdb\_v8.so, the debugger module itself

You can get access to an illumos system by:

* free: running [SmartOS](http://smartos.org) in a VM on your laptop
* easiest: storing your core file in [Joyent's Manta
  service](https://apidocs.joyent.com/manta/) and logging into a transient
  Manta container with [mlogin(1)](https://apidocs.joyent.com/manta/mlogin.html)
* alternatively: running SmartOS in the [Joyent Public
  Cloud](https://www.joyent.com/public-cloud)

## MDB prerequisites

This document is not a complete guide to using MDB.  For that, see the [MDB man
page](http://illumos.org/man/mdb) and the [MDB
guide](http://illumos.org/books/mdb/preface.html).   Be aware that MDB is also
used as a kernel and low-level C debugger, so much of the material may be too
low-level for Node.

Here's the minimum you should know to use MDB for Node.js:

* While MDB only runs on illumos-based systems, but it can be used to read core
  files from GNU/Linux systems.
* You can run MDB on live processes using `mdb -p PID`.  This stops the process
  while MDB is attached.
* You can also run MDB on core files using `mdb /path/to/core/file`.  You can
  create a core file for a Node program by either running `node` with
  `--abort-on-uncaught-exception` (which will cause the program to save a core
  file if an uncaught exception is thrown) or by running `gcore PID` (to save a
  core file of the currently running process PID).
* After launching MDB, you'll want to load the Node-specific debugger module.
  On SmartOS systems, you can just type `::load /path/to/mdb_v8.so`.

To get the latest copy of the mdb\_v8.so file, see the [README](../README.md) in
this repo.


## Tutorial

The easiest way to get started with mdb\_v8 is to play around with a simple
example.  Follow along on your own.  When you're done, you should have the
pieces to get started with your own Node programs.

Let's take a look at a simple Node server in a file called loop.js:

    /*
     * This script demonstrates a simple stack trace involving an anonymous
     * function, then goes into an infinite loop so you can catch it with the
     * debugger.  One caveat: we grab a stack trace at the start to force V8 to
     * compute line number information for this script.  If we didn't have this,
     * it wouldn't be a huge deal, but debugger would only print out position-
     * in-file information instead of line numbers.
     */
    new Error().stack;
    
    function main(arg) { func1(); }
    function func1() { func2(); }
    
    function func2()
    {
            (function () {
                    for (;;)
                            ;
            })();
    }
    
    main({ 'hello': 'world' });

Run this program in the background:

    $ node loop.js &
    [1] 24719
    $ 

Now attach to this process with MDB:

    $ mdb -p 24719
    Loading modules: [ ld.so.1 libumem.so.1 libc.so.1 ]
    > 

When you attach MDB to a process like this, it stops the process and gives you
a prompt. `::status` is a good place to start (most MDB commands start with
"::"):

    > ::status
    debugging PID 24719 (32-bit)
    file: /home/dap/node-v0.10.36-sunos-x86/bin/node
    threading model: native threads
    status: stopped by debugger

Don't worry if you don't know what all that means. MDB is telling you that it's
attached to pid 24719, which is a 32-bit process, and it's currently stopped.
In order to get any of the Node-specific debugger commands, you have to load
the V8 debugger module:

    > ::load v8
    V8 version: 3.14.5.9
    Autoconfigured V8 support from target
    C++ symbol demangling enabled

This will load the copy of mdb\_v8.so that's shipped with your version of
SmartOS.  You may want to use a newer copy of this binary from the mdb\_v8
github project.  You can load a newer binary using:

    > ::load /path/to/mdb_v8.so
    V8 version: 3.14.5.9
    Autoconfigured V8 support from target
    C++ symbol demangling enabled

Either way, now you can get a combined JavaScript/C stack trace with the
`jsstack` command:

    > ::jsstack
    js:     <anonymous> (as <anon>)
    js:     func2
    js:     func1
    js:     main
    js:     <anonymous> (as <anon>)
            (1 internal frame elided)
    js:     <anonymous> (as Module._compile)
    js:     <anonymous> (as Module._extensions..js)
    js:     <anonymous> (as Module.load)
    js:     <anonymous> (as Module._load)
    js:     <anonymous> (as Module.runMain)
    js:     startup
    js:     <anonymous> (as <anon>)
            (1 internal frame elided)
            (1 internal frame elided)
    native: _ZN2v88internalL6InvokeEbNS0_6HandleINS0_10JSFunctionEEENS1_INS0...
    native: v8::internal::Execution::Call+0xd0
    native: v8::Function::Call+0x15c
    native: node::Load+0x152
    native: node::Start+0x182
    native: main+0x1b
    native: _start+0x83

The top frame is our anonymous function.  That was called by "func2", which was
called by "func1", which was called by the JavaScript function "main".  After
that you see the JavaScript and C frames that are part of Node itself.

You can get more information about the whole stack, including source code for
each function on the stack, using `::jsstack -v`.  Try that yourself.  If you
want to hide the code (but still get the other "-v" output), add "-n 0":

    > ::jsstack -vn0
    js:     <anonymous> (as <anon>)
              file: /home/dap/loop.js
              posn: line 16
    js:     func2
              file: /home/dap/loop.js
              posn: line 14
              this: a95096c1 (<unknown>)
    js:     func1
              file: /home/dap/loop.js
              posn: line 12
              this: a95096c1 (<unknown>)
    js:     main
              file: /home/dap/loop.js
              posn: line 11
              this: a95096c1 (<unknown>)
              arg1: be784209 (JSObject: Object)
    js:     <anonymous> (as <anon>)
              file: /home/dap/loop.js
              posn: line 1
              this: be77eab5 (JSObject: Object)
              arg1: be77eab5 (JSObject: Object)
              arg2: be77fe41 (JSFunction)
              arg3: be77ea39 (JSObject: Module)
              arg4: be77d739 (ConsString)
              arg5: be7802f9 (SeqAsciiString)
            (1 internal frame elided)
    js:     <anonymous> (as Module._compile)
              file: module.js
              posn: line 374
              this: be77ea39 (JSObject: Module)
              arg1: be77fad5 (SeqAsciiString)
              arg2: be77d739 (ConsString)
    js:     <anonymous> (as Module._extensions..js)
              file: module.js
              posn: line 472
              this: be749599 (JSObject: Object)
              arg1: be77ea39 (JSObject: Module)
              arg2: be77d739 (ConsString)
    js:     <anonymous> (as Module.load)
              file: module.js
              posn: line 346
              this: be77ea39 (JSObject: Module)
              arg1: be77d739 (ConsString)
    js:     <anonymous> (as Module._load)
              file: module.js
              posn: line 275
              this: be7492c9 (JSFunction)
              arg1: be74525d (ConsString)
              arg2: a9508081 (Oddball: "null")
              arg3: a95080a1 (Oddball: "true")
    js:     <anonymous> (as Module.runMain)
              file: module.js
              posn: line 495
              this: be7492c9 (JSFunction)
    js:     startup
              file: node.js
              posn: line 30
              this: a95096c1 (<unknown>)
    js:     <anonymous> (as <anon>)
              file: node.js
              posn: line 27
              this: a95096c1 (<unknown>)
              arg1: be71e9a9 (JSObject: process)
            (1 internal frame elided)
            (1 internal frame elided)
    native: _ZN2v88internalL6InvokeEbNS0_6HandleINS0_10JSFunctionEEENS1_INS0...
    native: v8::internal::Execution::Call+0xd0
    native: v8::Function::Call+0x15c
    native: node::Load+0x152
    native: node::Start+0x182
    native: main+0x1b
    native: _start+0x83

Notice that this shows us the file and line number where each function is
defined.  It also shows us the arguments to each function in the form of
memory addresses.  **These addresses are just identifiers; you don't have to
actually know anything about pointers to use them for debugging JavaScript.**

Let's take the argument to the JavaScript "main" function.  From the output
above, the address for that object is `be784209`.  We can print that object:

    > be784209::jsprint
    {
        "hello": "world",
    }

Success!  Note that if we didn't know why this program had entered an infinite
loop, we would be able to learn from this information both what function the
program got stuck in and what arguments were passed to that function.


### Finding JavaScript objects

The next thing you may want to do is get a summary of all JavaScript objects in
your program.  The `findjsobjects` command does this by scanning the entire
JavaScript heap and classifying all the objects that it finds based on their
properties.  This may take a minute, and then it prints out a summary of what
it found:

    > ::findjsobjects
      OBJECT #OBJECTS   #PROPS CONSTRUCTOR: PROPS
    a9509b31        1        0 JSON
    a952d229        1        0 Module
    a951a321        1        0 <anonymous> (as d)
    a95154a9        1        0 MathConstructor
    a95254e1        1        0 Buffer
    be71f289        1        0 <anonymous> (as <anon>)
    be715901       35        0 Array
    81b099e5      328        0 Object
    be711295        1        1 Arguments: length
    be749571        1        1 Object: ...
    be7494cd        1        1 Object: /home/dap/loop.js
    be780789        1        2 Error: arguments, type
    ...
    be7590c5        1      178 Object: O_NOCTTY, ELOOP, ENOLINK, ENOTTY, ...
    be71a2f5       27        7 Array
    81b0e209       20       12 PropertyDescriptor: value_, hasValue_, ...
    be7837cd       12      930 Array
    be781c45       30      540 Array
    >

Each row in the output describes a group of objects that have the same set of
properties.  The first column in each row is the address of a *representative*
object for that group.  You can print that object with `jsprint`.  For
example, this one appears to be used internally by Node:

    > be7494cd::jsprint    
    {
        "/home/dap/loop.js": {
            "id": ".",
            "exports": [...],
            "parent": null,
            "filename": "/home/dap/loop.js",
            "loaded": false,
            "children": [...],
            "paths": [...],
        },
    }

Like `util.inspect`, `jsprint` only descends a few levels and prints "..."
after that.  You can have it descend deeper with the "-d" option.

Once you've run `findjsobjects` once, it keeps track of what it already found
so that you can quickly look for specific kinds of objects. You usually search
by a property name or constructor. For example, if I wanted to find my "hello
world" object (and I didn't know it was one of the arguments above), I could
find it with:

    > ::findjsobjects -p hello
    be7841cd

MDB also supports pipelines for debugger commands, so you can find and print
the object in one step:

    > ::findjsobjects -p hello | ::jsprint
    {
        "hello": "world",
    }

#### Dealing with garbage

The mechanism that `findjsobjects` uses means that it also tends to pick up
"garbage" objects that used to exist in your program, but which are no longer
valid. In my process, I can see some of these when I try to look for Node's
global "process" object, which I know has a versions property:

    > ::findjsobjects -p versions | ::jsprint
    {
        "version": 11,
        "moduleLoadList": 0,
        "versions": "title",
        "arch": 786563,
        "platform": <unknown JavaScript object type "AccessorInfo">,
        "argv": "version",
        "execArgv": 262401,
        "env": 0,
        "pid": "moduleLoadList",
        "features": 1048961,
    }

You can recognize garbage objects because you may see warnings when MDB prints
them out (e.g., "unknown JavaScript object type") and the properties look very
strange. In this case, we know the "versions" property is supposed to be an
object, not the string "title".  These garbage objects are annoying, but
they're generally easily distinguished from valid objects.

#### Representative objects

We said that `findjsobjects` prints out the *representative* object from each
group of objects.  Since you might have tens of thousands of instances of the
same thing, it doesn't print all those by default.  You have to pipe to
"findjsobjects" again to get that.  If you wanted to actually find the global
"process" object and ignore the garbage, the reliable way to do that is:

    > ::findjsobjects -p versions | ::findjsobjects | ::jsprint
    ...
    {
        "version": "v0.10.36",
        "moduleLoadList": [
            "Binding evals",
            "Binding natives",
            "NativeModule events",
            "NativeModule buffer",
            "Binding buffer",
    ...
        "versions": {
            "http_parser": "1.0",
            "node": "0.10.36",
            "v8": "3.14.5.9",
            "ares": "1.9.0-DEV",
            "uv": "0.10.30",
            "zlib": "1.2.8",
            "modules": "11",
            "openssl": "1.0.1l",
        },
        "arch": "ia32",
        "argv": [
            "node",
            "/home/dap/loop.js",
        ],
        "execArgv": [],
        "env": {},
        "pid": 24719,
    ...
    }

The output above is clipped for brevity.  There were a few garbage objects and
the "process" object itself has a lot of stuff in it.

### Where to go from here

This example demonstrated a bunch of the basics around using MDB to inspect
program state. The best way to get familiar with these commands is to use them
on your own programs, ideally to debug real problems. But even if you don't
have a real problem, try creating a core file of a production service and
poking around. You might be surprised at what you find.

## Tips for using the postmortem technique

Debugging actual issues by inspecting state is very different than both
console.log debugging and tracing execution with breakpoints. It's much more
powerful in some ways, since it shows you much more information and doesn't
disrupt the running program. When debugging problems this way, the approach
mirrors the scientific method:

* Form a hypothesis about what happened.
* Inspect the state of the core file to confirm or disprove the hypothesis.
* If proved, you're done. If not, refine the hypothesis and repeat.

A typical example for a crash might look something like this:

1. The Node program dies with "TypeError: Cannot read property 'foo' of
   undefined".  This tells you that the code was something like
   `someObject.foo`, but `someObject` was undefined.  The stack trace should
   tell you the line of code with the problem, but suppose you don't know why
   `someObject` would have been `undefined`.
2. Inspect the code to see where `someObject` comes from.  Either something
   *set* it to `undefined`, or something else *didn't* set it to a
   non-`undefined` value.  Look at the surrounding code and try to understand
   why it assumed that it would be defined.  Take a guess: suppose function
   setSomeObject normally sets it, and your hypothesis is that setSomeObject
   never ran.  Try to prove or disprove that based on the other evidence in the
   core file.  If that's not it, make other hypotheses and try to prove or
   disprove those.

This is necessarily vague, since every problem is unique.  It can be
challenging to learn to think about problems this way and to develop creative
ways to prove or disprove hypotheses. Sometimes, you get lucky: if you think
some socket got closed, you can find the socket object, inspect its state, and
test that hypothesis. Sometimes, it's not so easy: if you think a particular
function may have been called that shouldn't have been, it may not be easy to
confirm that. But be creative: maybe the function doesn't set any variables,
but does it instantiate any objects, even temporary ones? You may be able to
find them with `findjsobjects`.

One nice thing about this approach is that by the end, you can be confident in
your root-cause because you've got lots of data to back it up. Often, the data
may point to a very specific combination of events (e.g., this kind of request
happening concurrently with that other kind of request) that may help you
reproduce the problem in development, so you can be sure that your fix works.

## Building programs for debugging

You can help yourself by including state in your program that only exists to
help debug it. For example, the vasync module provides familiar control-flow
functions (like pipeline and waterfall), with the added benefit that the state
is stored in a JavaScript object (rather than local variables in a closure).
The upshot is that when your Node program hangs while running a waterfall of 10
functions, it's easy to use `findjsobjects` to find out exactly which function
didn't complete.

Other tips:

* Save a record of all outstanding operations. Keep them in an object indexed
  by request id.  Then you can see what was going on during the core file.
* Save a timestamp when you start each operation, particularly outgoing
  requests. This helps you determine whether a request has been hung (i.e., the
  external service is hung).
* Keep timestamps and counters for interesting events, like the last time a
  client successfully connected to the server or made a request or received an
  error or became disconnected or tried to reconnect.

## Node-specific MDB command reference

There are three groups of commands:

* Commands intended for JavaScript developers are prefixed with "js".
* Node.js-specific commands are prefixed with "node".
* Commands intended for developers debugging V8 internals are prefixed with
  "v8".  These are not documented here.

### findjsobjects

    [ addr ]::findjsobjects [-vb] [-r | -c cons | -p prop]

With no arguments, finds all JavaScript objects in the V8 heap via brute force
iteration over all mapped anonymous memory.  (This can take up to several
minutes on large dumps.) The output consists of representative objects, the
number of instances of that object and the number of properties on the object
-- followed by the constructor and first few properties of the objects.  Once
run, subsequent calls to findjsobjects use cached data.

If provided an address (and in the absence of -r, described below),
findjsobjects treats the address as that of a representative object, and
lists all instances of that object (that is, all objects that have a matching
property signature).

With -p or -c, representative objects are filtered by a property name or
constructor, respectively.  The output consists of only the representative
objects.

Option summary:

    -b       Include the heap denoted by the brk(2) (normally excluded)
    -c cons  Display representative objects with the specified constructor
    -p prop  Display representative objects that have the specified property
    -l       List all objects that match the representative object
    -m       Mark specified object for later reference determination via -r
    -r       Find references to the specified and/or marked object(s)
    -v       Provide verbose statistics

### jsclosure

    addr::jsclosure

Given a function instance identified by the address `addr`, print out the
closure variables that are visible to that function.  You may find `addr` from
the stack, from the output of the `jsfunctions` command, or as a property of
some other object. See the FAQ entry entitled ["How to find the address of a
function instance?"](#how-to-find-the-address-of-a-function-instance) for more
details on how to find that address.

As an example, here's some example code from the file `events.js` in Node v0.10:

```javascript
EventEmitter.prototype.once = function once(type, listener) {
  if (typeof listener !== 'function')
    throw TypeError('listener must be a function');

  var fired = false;

  function g() {
    this.removeListener(type, g);

    if (!fired) {
      fired = true;
      listener.apply(this, arguments);
    }
  }

  g.listener = listener;
  this.on(type, g);

  return this;
};
```

Each time the `once()` function is invoked, a new closure is created for the
`g()` function.  Each _closure_ has references to different values of variables
like `type` and `listener`.  The `jsclosure` command prints these out:

    > 8270a879::jsclosure
        "listener": 8270aa59: function <anonymous> (as <anon>)
        "type": a7015511: "error"
        "fired": b47080c1: false
        "g": 8270a879: function g

Of course, you can use other commands to inspect these values further.

See also: `jsfunction`

### jsarray

    addr::jsarray [-i]

Given an address `addr` of an instance of the JavaScript "Array" class, print
the contents of the array.  Each element of the array is printed on its own
line.  With no options, the result can be piped to `::jsprint` to print each
element.

For example, take this snippet:

    var x = [ 'one', 'two', 'three' ];

With `::jsarray`, this prints three pointers, one for each string:

    > 9d780985::jsarray
    8bc27629
    8bc27639
    8bc27649

We can pipe this to `::jsprint` to see the strings:

    > 9d780985::jsarray | ::jsprint
    "one"
    "two"
    "three"

With "-i", each value is prefixed with its integer index in the array:

    > 9d780985::jsarray -i
    0 8bc27629
    1 8bc27639
    2 8bc27649

Note that JavaScript arrays can contain holes, where there is no element for a
particular index (not even "undefined").  These appear when an element was
removed using the `delete` keyword or when an array was initialized with a
specific length, but not all of the elements were filled in.

When `::jsarray` encounters a hole, the hole is printed out using a special
value that `::jsprint` shows indicates a hole.  For example, take this snippet:

    var x = new Array(4);
    x[0] = 'one';
    x[1] = 'two';
    x[2] = 'three';
    delete (x[1]);

This produces an array with two holes: one at index 1 (because it was deleted)
and one at index 3 (because it was never initialized, but the length was
initialized to include this index).  If you print this out with `console.log`,
you'd see:

    [ 'one', , 'three',  ]

(Note the extra commas where there are holes.)

`::jsarray` prints the holes, which may look like ordinary values:

    > 81780a0d::jsarray
    aba27661
    b4d080c1
    aba27681
    b4d080c1

but are recognized by `::jsprint` as special:

    > 81780a0d::jsarray | ::jsprint
    "one"
    hole
    "three"
    hole

The default behavior around the printing of hole values may change in the
future.

See also: `walk jselement`

### jsconstructor

    addr::jsconstructor [-v]

Given an object identified by `addr`, print the name of the JavaScript function
that was used as the object's constructor.  With -v, provides the constructor
function's underlying V8 heap object address for use with `v8function`.

See also: `jsfunction`

### jsframe

    addr::jsframe [-aiv] [-f function] [-p property] [-n numlines]

Given the address `addr` of a stack frame pointer, print details about the
stack frame *pointed to* by that address.  The format is the same as for
`jsstack`.  With "-i", print the stack frame *at* `addr` instead of the one
that `addr` points to, but note that since return addresses are stored in the
previous frame, the function name cannot be printed when using "-i".

The "-a", "-v", "-f", "-p", and "-n" arguments are exactly the same as for
`jsstack`.


### jsfunction

    addr::jsfunction

Given a function instance identified by the address `addr`, print out
information about the function.  For most functions, this includes the
function's name and where it was defined:

    > b17812f5::jsfunction
    function: afunc
    defined at /home/dap/mdb_v8/bound.js line 5

For functions created with JavaScript's `Function.bind`, this command instead
prints out information about the _target_ function (the one that was wrapped
with `bind`):

    > a8781389::jsfunction
    bound function that wraps: a87812cd
    with "this" = a878135d (JSObject: Object)
          arg0  = 80527875 (SeqAsciiString)

You can use `jsfunction` again on the target function (e.g., `a87812cd` in this
case) to see information about the underlying function.

See also:

- `jsclosure` for printing out closure variables for this function.
- `jssource` for printing the source code for this function.
- `jsprint` for printing the values for "this" and the arguments for bound
   functions.


### jsfunctions

    ::jsfunctions [-X] [-s file_filter] [-n name_filter] [-x instr_filter]
    [ADDR]::jsfunctions -l

Lists JavaScript functions, optionally filtered by a substring of the
function name or script filename or by the instruction address.  This uses
the cache created by `findjsobjects`.  If `findjsobjects` has not already
been run, this command runs it automatically without printing the output.
This can take anywhere from a second to several minutes, depending on the
size of the core dump.

It's important to keep in mind that each time you create a function in
JavaScript (even from a function definition that has already been used),
the VM must create a new object to represent it.  For example, if your
program has a function A that returns a closure B, the VM will create new
instances of the closure function (B) each time the surrounding function (A)
is called.  To show this, the output of this command consists of one line 
per function definition that appears in the JavaScript source, and the
"#FUNCS" column shows how many different functions were created by VM from
this definition.

The "-s", "-n", and "-x" flags allow you to filter by script filename, function
name, or instruction addresses.  The "-x" flag is useful for mapping stack
traces collected with other tools (e.g., libumem debugging tools) back to
JavaScript functions.

The "-l" option allows you to list the closures themselves (without the extra
columns), as with `findjsobjects`.  You can also use this mode with a
representative closure address to enumerate all closures for the same function.

Option summary:

    -l       List only closures (without other columns).  With ADDR, list
             closures for the representative function ADDR.
    -s file  List functions that were defined in a file whose name contains
             this substring.
    -n func  List functions whose name contains this substring
    -x instr List functions whose compiled instructions include this address
    -X       Show where the function's instructions are stored in memory


### jsprint

    addr::jsprint [-ab] [-d depth] [member]

Given a JavaScript value identified by `addr`, print it out.  Primitive types
like booleans, null, undefined, and small integers are printed with their exact
value.  Objects and arrays are printed in a JSON-like format, but it's
important to realize that it's not JSON.  Particularly:

* Very long strings may be truncated.
* Objects and arrays are only traversed to a depth of `depth`, which defaults
  to two.  After that you'll see '...'.
* Arrays may include hidden `hole` values that V8 uses to distinguish between
  elements that have been set to `undefined` vs. elements that have never been
  set.
* Function instances are printed with a summary that describes them by name,
  but isn't valid JSON or JavaScript.  For example:

      > 9bf36465::jsprint
      function <anonymous> (as Socket.write)

  See the FAQ entry entitled ["How to find the address of a function instance
  ?"](#how-to-find-the-address-of-a-function-instance) for more details on how
  to find functions' addresses.

If `member` is specified, then only the `member` property of the object is
printed.  This is particularly useful in pipelines where you've used
`findjsobjects` to find all objects with a given property and then print that
property of each one.  For example, to find all objects with a "method"
property and print out "method" from each one:

    > ::findjsobjects -p method | ::findjsobjects -l | ::jsprint method
    ...
    "GET"
    "POST"
    "POST"
    ...

With "-a", the address of each object and sub-object is printed inline.  This
is useful for walking through a complex structure, where you might print the
top-level, find a subobject that you're interested in, and then print that, and
so on.  For example, here's an HTTP request printed with "-a":

    > 9bf6e5d9::jsprint -a
    9bf6e5d9: {
        "_readableState": 9bf6e8ad: {
            "highWaterMark": 8000: 16384,
            "buffer": 9bf6bb55: [...],
    ...

Since the default depth is two, we can't see what `buffer` really is, but we
can take the address of the `\_readableState` and print that out:

    > 9bf6e8ad::jsprint -a  
    9bf6e8ad: {
        "highWaterMark": 8000: 16384,
        "buffer": 9bf6bb55: [],
        "length": 0: 0,
    ...

and we can follow a chain of addresses like this indefinitely.

With "-b", the address of the *base* object is printed inline before each
object and subobject.  This looks like "-a", except that the address that's
printed is the address of the object passed in as `addr` rather than the
address of each value.  This is useful when used in a pipeline along with
`member` (see above) and you want to find an object that has a particular
value.  You can use "-b", pipe the output to `grep`, and you'll wind up with
the address of the object that had the value you're looking for (rather than
the address of the value you're looking for, which is what you'd get with
"-a").  For example:

    > ::findjsobjects -p method | ::findjsobjects -l | ::jsprint -b method ! grep GET
    b3710b15: "GET"
    a05dfe15: "GET"
    9bf6e5d9: "GET"
    be7e330d: "GET"
    b371bfd5: "GET"
    a0c6d129: "GET"
    b37ef619: "GET"
    b376d12d: "GET"
    a0c67ff5: "GET"

    > a0c67ff5::jsprint -d1
    {
        "method": "GET",
        "domain": null,
        "_events": [...],
        ...
    }


### jssource

    addr::jssource [-n numlines]

Given a JavaScript function instance identified by the address `addr`, print
the source code for that function with `numlines` of surrounding context. See
the FAQ entry entitled ["How to find the address of a function instance
?"](#how-to-find-the-address-of-a-function-instance) for more details on how
to find the function's address.

`numlines` defaults to 5.  You can combine this with `jsfunctions` to find the
source code for any JavaScript function in your program, including functions
built into Node.js:

    > ::jsfunctions -s url.js -n Url.parseHost
        FUNC   #FUNCS NAME                                     FROM
    9bf2aa25        1 <anonymous> (as Url.parseHost)           url.js position 21893

    > 9bf2aa25::jssource -n 0
    file: url.js

      680 Url.prototype.parseHost = function() {
      681   var host = this.host;
      682   var port = portPattern.exec(host);
      683   if (port) {
      684     port = port[0];
      685     if (port !== ':') {
      686       this.port = port.substr(1);
      687     }
      688     host = host.substr(0, host.length - port.length);
      689   }
      690   if (host) this.hostname = host;
      691 };


### jsstack

    ::jsstack [-av] [-f function] [-p property] [-n numlines]

Print a stacktrace for the current program that includes both JavaScript frames
and native frames (i.e., C and C++ frames).  Frames are annotated with whether
they're JavaScript or native, and by default internal frames are elided with a
message indicating so:

    js:     <anonymous> (as doput)
    js:     listOnTimeout
            (1 internal frame elided)
    native: _ZN2v88internalL6InvokeEbNS0_6HandleINS0_10JSFunctionEEENS1_INS0...
    native: v8::internal::Execution::Call+0xc9
    native: v8::Function::Call+0x10b

With "-v", print arguments and source code for each JavaScript function on the
stack.  With "-n", limit the source code to `numlines` of context.  If
`numlines` is 0, then the source code is left out, but the arguments are still
shown:

    js:     listOnTimeout
              file: timers.js
              posn: line 79
              this: a6f09305 (JSObject: Timer)

With "-f", show only frames for function `function`.  With "-p", show only
`property` for matching frames, where `property` may be "file", "posn", "this",
"arg1", and so on.

With "-a", show all information about hidden frames, the frame pointer for each
frame, and other native objects for each frame (e.g., JSFunction addresses).

### walk jselement

    addr::walk jselement

Given a JavaScript array identified by `addr`, enumerates the elements of the
array.  This behaves very similarly to `::jsarray`.  See the notes about holes
under `::jsarray`.

For example, the array created by this code:

    var x = [ 'one', 'two', 'three' ]

is enumerated in the debugger like this:

    > 9d780985::walk jselement
    0x8bc27629
    0x8bc27639
    0x8bc27649

The results can be piped to `::jsprint` to print the actual values:

    > 9d780985::walk jselement | ::jsprint
    "one"
    "two"
    "three"

See also: `jsarray`.

### walk jsframe

    ::walk jsframe

Enumerates the frame pointers of the stack.  The `jsstack` command is nearly
equivalent to `::walk jsframe | ::jsframe`, which enumerates the frame pointers
and then prints each one with the `jsframe` command.


### walk jsprop

    addr::walk jsprop

Given a JavaScript object identified by `addr`, enumerates the values for
properties contained in the object.  This is useful for collection objects to
print the values inside the collection.

**Note:** On some versions of Node, due to V8's optimization of unboxing
doubles, properties whose values are represented internally as double-precision
floating-point numbers may not be included in the output of "::walk jsprop".
They have no representation that can be distinguished from other values.  Values
likely to be affected by this are non-integer numbers and integers larger than
2147483648.  This currently only affects 64-bit versions of Node v4 and later.


### nodebuffer

    addr::nodebuffer

Given a Node.js Buffer object, print the address of the memory that stores the
contents of the buffer.  This memory is usually allocated from the heap, as
with malloc(3C).

### V8 internal commands

These command are intended primarily for developers working on V8 internals or
the debugger module itself, not developers debugging Node programs.  They're
only documented here as a jumping off point for developers interested in
learning more about V8's internal details.

Printing the debugger module's configuration:

* v8classes: print the names of all known C++ classes that make up the V8 heap
* v8frametypes: print the names and identifiers for all known V8 frame types
* v8types: print the names and identifiers for all known heap object types

Walking V8 structures:

* v8array: given a V8 FixedArray, print the elements of the array
* v8code: print details about a V8 Code object (including disassembly)
* v8context: print information about a V8 Context object
* v8function: print details about a V8 function object (including
  disassembly). See the FAQ entry entitled ["How to find the address of a
  function instance?"](#how-to-find-the-address-of-a-function-instance) for
  more details on that.
* v8internal: fetch internal fields from heap objects
* v8load: manually load configuration for Node v0.4 or v0.6
* v8print: print a C++ object that's part of V8's heap
* v8scopeinfo: print information about a V8 ScopeInfo object
* v8str: print the contents of a V8 string (optionally show details of structure)
* v8type: print the V8 type of a heap object

Modifying configuration:

* v8field: define a C++ field in a C++ class so that v8print will print it
* v8warnings: toggle warnings


### Frequently asked questions

#### Why does MDB report the wrong line number for my function?

The line numbers reported by MDB describe where the function was defined, not
where the function called the next function in the stack.

#### Why does MDB report the position instead of the line number for my function?

In some cases, instead of seeing a line number, you may see a position number,
as in "position 13546". MDB can't always tell what line number a position
corresponds to. It can only tell when V8 computed this before the core file was
created. In this case, the function would have been defined position 13546 in
the file. In "vim", you can find this with :goto 13546. Beware that Node
prepends about 50 characters to all files before passing them to V8, so these
position numbers appear to be off by about that much.

#### I keep seeing `hole` in ::jsprint output.  What's up with that?

In ::jsprint output, you may see values called "hole". You can think of "hole"
as undefined. You usually see it in arrays where the array itself hasn't been
fully populated.

JavaScript requires that V8 be able to distinguish between array elements that
have not been assigned a value and those that have been assigned "undefined".
V8 uses the special "hole" value for this internally.

#### How to find the address of a function instance?

The address of a function instance is the address of a `JSFunction` object,
not the address of the stack frame used to store the data associated to a
specific call of that function, or the address of the function's code.

Getting the address of a function instance is useful e.g to get access to its
closure with the `::jsclosure` command.

##### From the output of `::jsfunctions`

`::jsfunctions`' output displays the appropriate address on its first column:

```
> ::jsfunctions
    FUNC   #FUNCS NAME                                     FROM
fe843a01        1 <anonymous> (as Module.requireRepl)      module.js position 14781
```

`fe843a01` is a valid `JSFunction` instance's address:

```
> fe843a01::jsclosure
    "NativeModule": 9094c625: function NativeModule
    "runInThisContext": fe8394b9: function <anonymous> (as <anon>)
    "runInNewContext": fe839591: function <anonymous> (as <anon>)
    "assert": 909349dd: function ok
    "hasOwnProperty": 90934b21: function hasOwnProperty
    "Module": 90947c95: function Module
    "modulePaths": 90934cc1: [
        9093dbd5: "/usr/node_modules",
        9093dbe9: "/usr/vm/node_modules",
        aac2e149: "/opt/smartdc/agents/lib/node_modules/vm-agent/node/lib/node",
    ]
    "path": 90947e0d: {
        "sep": 85b16189: "/",
        "delimiter": 85b19f61: ":",
    }
    "debug": fe8437a5: function <anonymous> (as Module._debug)
    "statPath": 90934995: function statPath
    "packageMainCache": 90934cd1: {
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/bunyan/node_modules/dtrace-provider": aac2e259: "./dtrace-provider.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/bunyan": aac2e2c9: "./lib/bunyan.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/restify-clients/node_modules/restify-errors/node_modules/verror/node_modules/extsprintf": 9c44fe81: "./lib/extsprintf.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/vmadm": aac8e8b9: "lib/index.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/vasync/node_modules/verror": aac63181: "./lib/verror.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/restify-clients/node_modules/once": 9c43f311: "once.js",
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/restify-clients/node_modules/backoff": fe808091: undefined,
        "/opt/smartdc/agents/lib/node_modules/vm-agent/node_modules/r
    "readPackage": 90934afd: function readPackage
    "tryPackage": 90934971: function tryPackage
    "tryFile": 90934929: function tryFile
    "tryExtensions": 9093494d: function tryExtensions
    "resolvedArgv": fe808091: undefined
    "stripBOM": 90934a25: function stripBOM
>
```

##### As a property of some other object

To find the `JSFunction` instance's address of a function that is the value of
some object's property, use the `-a` command line switch of the `::jsprint`
command.

For instance, let's display the latest function of the functions that are set
as a property named `callback` on some objects:

```
> ::findjsobjects -p callback | ::findjsobjects | ::jsprint ! tail -1
function <anonymous> (as <anon>)
```

By default, no address is displayed. Now let's use the `-a` command line
switch of the `::jsprint` command:


```
> ::findjsobjects -p callback | ::findjsobjects | ::jsprint -a callback ! tail -1
86030809: function <anonymous> (as <anon>)
>
```

The address on the left is the `JSFunction` instance's address we're looking
for:

```
> 86030809::jsclosure
    "state": 9c2baf5d: {
        "highWaterMark": 8000: 16384,
        "buffer": 9c2bd3c5: [...],
        "length": 0: 0,
        "pipes": 9c2b102d: [...],
        "pipesCount": 2: 1,
        "flowing": fe8080b1: true,
        "ended": fe8080c1: false,
        "endEmitted": fe8080c1: false,
        "reading": fe8080b1: true,
        "calledRead": fe8080b1: true,
        "sync": fe8080c1: false,
        "needReadable": fe8080b1: true,
        "emittedReadable": fe8080c1: false,
        "readableListening": fe8080b1: true,
        "objectMode": fe8080c1: false,
        "defaultEncoding": 85b1528d: "utf8",
        "ranOut": fe8080b1: true,
        "awaitDrain": 0: 0,
        "readingMore": fe8080c1: false,
        "decoder": fe808081: null,
        "encoding": fe808081: null,
    }
// Output truncated
```

##### From the output of `::jsstack`

To get the address of a `JSFunction` instance that corresponds to a given
stack frame, use the `-a` command line switch for the `::jsstack` command as
follows:

```
> ::jsstack -a
80475c8 libc.so.1`_lwp_kill+0x15
80475e8 libc.so.1`raise+0x2b
8047638 libc.so.1`abort+0x10e
        (1 internal frame elided)
80476e8 v8::internal::Isolate::DoThrow+0x2bb
8047708 v8::internal::Isolate::Throw+0x1f
8047748 v8::internal::Runtime_Throw+0x3e
8047764 0xaa70a376 internal (Code: aa70a301)
8047778 0xbf964e8c fail (JSFunction: 909371e1)
80477a4 0xaa74b63e ok (JSFunction: 909349dd)
80477c4 0xbf964c85 _handleLoadErr (JSFunction: 86c488a5)
80477f4 0x9274eec9 _onVmLoad (JSFunction: 86c48965)
8047814 0xaa70e501 <ArgumentsAdaptorFrame>
8047838 0x9274e989 <anonymous> (as <anon>) (JSFunction: 86c48abd)
8047868 0x92730a7d _childCloseHandler (JSFunction: 86c49e39)
80478a0 0xbc07143f <anonymous> (as EventEmitter.emit) (JSFunction: fe83a385)
80478bc 0xaa70e501 <ArgumentsAdaptorFrame>
80478e0 0x927a552f maybeClose (JSFunction: 90939b7d)
80478fc 0xbc079eb1 <anonymous> (as <anon>) (JSFunction: 86c49bc1)
8047914 0xaa70e501 <ArgumentsAdaptorFrame>
8047948 0xbc071403 <anonymous> (as EventEmitter.emit) (JSFunction: fe83a385)
8047964 0xaa70e501 <ArgumentsAdaptorFrame>
8047980 0xbc079e10 <anonymous> (as <anon>) (JSFunction: 8693ff69)
804799c 0xaa721899 <InternalFrame>
80479d8 0xaa71308a <EntryFrame>
8047a58 _ZN2v88internalL6InvokeEbNS0_6HandleINS0_10JSFunctionEEENS1_INS0_6ObjectEEEiPS5_Pb+0x101
8047a98 v8::internal::Execution::Call+0xc9
8047af8 v8::Function::Call+0x10b
8047b68 node::MakeCallback+0x5e
8047bc8 node::MakeCallback+0x66
8047c18 node::HandleWrap::OnClose+0x8f
8047c58 uv_run+0x154
8047cb8 node::Start+0x16d
8047cd8 main+0x1b
8047cfc _start+0x83
```

In the output above, let's consider the call to the `_onVmLoad` function:

```
80477f4 0x9274eec9 _onVmLoad (JSFunction: 86c48965)
```

We can find three different addresses:

1. The address of the stack frame on the left: `80477f4`.
2. The address of the function's code second on the left: `0x9274eec9`.
3. The address of the `JSFunction` instance for this function call on the
right: `86c48965`.

The third address on the right is the one to use with `::jsclosure`:

```
> 86c48965::jsclosure
    "cb": 86c48859: function <anonymous> (as next)
    "stash": 86c48671: {}
    "startLoad": 86c48959: 1454543447282
    "_handleLoadErr": 86c488a5: function _handleLoadErr
>
```

##### From `::jsclosure`'s output

`::jsclosure` itself can display closure variables that are function
instances:

```
> 86c48965::jsclosure
    "cb": 86c48859: function <anonymous> (as next)
    "stash": 86c48671: {}
    "startLoad": 86c48959: 1454543447282
    "_handleLoadErr": 86c488a5: function _handleLoadErr
>
```

In the output above, `_handleLoadErr` is a function, and the `JSFunction`
instance's address is the only address displayed by default by `::jscolosure`:

```
> 86c488a5::jsclosure
    "cb": 86c48859: function <anonymous> (as next)
    "stash": 86c48671: {}
    "startLoad": 86c48959: 1454543447282
    "_handleLoadErr": 86c488a5: function _handleLoadErr
>
```
### Other MDB commands useful for Node programmers

Getting help:

* `::help`: get help on any command
* `::dcmds`: list all commands
* `::walkers`: list all walkers
* `::status`: print basic information about the program being debugged

Working with memory:

* `::dump`: dump a region of memory
* `::findleaks`: find native leaks (requires libumem)
* `::ugrep`: scan all memory for a given byte sequence (e.g., an address)
* `::umastat`: print native memory allocator statistics (requires libumem)
* `::whatis`: print out what a given native object appears to be
