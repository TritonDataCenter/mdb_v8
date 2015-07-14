# mdb_v8: postmortem debugging for Node.js and other V8-based programs

This repository contains the canonical source for mdb\_v8, an
[mdb](http://illumos.org/man/1/mdb) debugger module ("dmod") for debugging both
live processes and core dumps of programs using Google's
[V8 JavaScript engine](https://developers.google.com/v8/), and particularly
[Node.js](https://nodejs.org/).


# History of mdb_v8

The canonical source for mdb\_v8 has moved between several repositories.  To
avoid confusion between different versions, here's the history:

* 2011-12-16: mdb\_v8 is initially [integrated](https://github.com/joyent/illumos-joyent/commit/48f2bcac10415ae79c34b6e8d8870a135b57a6c9)
  into the [SmartOS](https://smartos.org/) distribution of the
  [illumos](https://www.illumos.org/) operating system.
* 2013-07-17: mdb\_v8 is copied into the Node repository so that the dmod can
  be built into Node binary.  At this point, the canonical copy lives in the
  Node repo, but SmartOS continues to deliver its own copy.  The #master version
  of both copies is identical except for the license, and subsequent changes are
  applied to both versions.
* 2015-07-14: mdb\_v8 is copied into this repository to allow for its own
  documentation and test suite and to streamline development.  The copies in
  both Node and SmartOS are considered downstream.  Changes should be made to
  this copy first, then propagated into these copies.

The initial commit in this repository represents the copy of mdb\_v8 in illumos
at the time of the commit, which is equivalent to the copy of mdb\_v8 in Node
v0.12.7 except for the license and cstyle-related changes.


# See also

[Postmortem Debugging in Dynamic
Environments](https://queue.acm.org/detail.cfm?id=2039361) outlines the
history and motivation for postmortem debugging and the challenges underlying
postmortem debugging in higher-level languages.
