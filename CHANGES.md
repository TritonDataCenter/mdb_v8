<!--
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
-->

<!--
    Copyright (c) 2015, Joyent, Inc.
-->

# mdb_v8 changelog

## Unreleased changes

* #48 want tool for running tests on multiple Node versions

## v1.1.1 (2015-10-02)

* #40: fix JSArrayBufferView's buffer offset missing
* #28 don't allow release builds that won't work on older platforms

## v1.1.0 (2015-10-02)

This version fixes support for Node v4.

* #36 Update mdb\_v8 for V8 4.6.x
* #32 fix ::findjsobjects for V8 4.5.x

## v1.0.0 (2015-09-09)

* #25 fix ia32 build
* #23 support V8 4.4.x and 4.5.x
* #20 want ::jsfunctions -l

## v0.11.1 (2015-08-20)

* #18 ::jsfunctions help text does not agree with implementation

## v0.11.0 (2015-08-20)

* #17 add information about closures and their variables

## v0.10.0 (2015-07-17)

* #10 some JSDate objects are not printed
* #9 add basic support for printing RegExps
* #8 dmod crashes on some very bad strings
* #7 some objects' properties are not printed correctly on 0.12
* #5 update license to MPL 2.0
* #6 indjsobjects could prune more garbage
* #4 document debug metadata and design constraints
* #3 JavaScript tests should be style-clean and lint-clean
* #2 import Node.js postmortem tests

## v0.9.0 (2015-07-16)

* first release from this repository, roughly equivalent to
  the version shipped in Node v0.12 and SmartOS 20150709.


## Early history of mdb_v8

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
v0.12.7 except for the license and cstyle-related changes.  The first release
from this repository was v0.9.0, which is functionally (nearly) equivalent to
the versions in Node v0.12.7 and SmartOS 2015-07-09.
