# libPhenom

libPhenom is an eventing framework for building high performance and high
scalability systems in C

## System Requirements

libPhenom is known to compile and pass its test suite on:

 * Linux systems with `epoll`
 * OS X
 * Illumos and Solaris style systems that have `port_create(3C)`.

libPhenom has been known to compile and pass its test suite on these
systems, but they have not been tried in a little while, so may require
a little bit of TLC:

 * BSDish systems that have the `kqueue(2)` facility, including
   FreeBSD 9.1 and OpenBSD 5.2

libPhenom depends on:

 * `pkg-config` to find dependencies.
 * [c-ares](http://c-ares.haxx.se) for DNS resolution.
 * [Concurrency Kit](http://concurrencykit.org/) for its excellent
   concurrency primitives and key data structures.  We include CK
   with phenom.
 * **OpenSSL** is required; this should be provided by your OS.
   You will need to install `openssl-devel` or `libssl-dev` or a similarly
   named package.
 * **autoconf**, **automake** and **libtool** are required to build libPhenom

libPhenom works best if built with GCC version 4.3 or later, but should
be able to build with any C99 compiler.

[![Build Status](https://travis-ci.org/facebook/libphenom.png)](https://travis-ci.org/facebook/libphenom)

## Facilities

 * Memory management with counters - record how much of which kinds
   of memory your application is using.
 * Jobs - decompose your application into portions of work
   and let the phenom scheduler manage getting them done
 * streaming I/O with buffers
 * Handy data structures (hash tables, lists, queues)
 * Variant data type to enable serialization and deserialization of
   JSON
 * A printf implementation with registerable object formatting

## Goals

 * Balance ease of use with performance
 * Aim to be neutral wrt. your choice of threaded or event-based dispatch
   and work well with both.
 * Where possible, avoid contention points in our implementation so as to
   avoid limiting scalability with the number of cores in the system.

## How to use these docs

If you're reading these on http://facebook.github.io/libphenom, simply start
typing and the search box will suggest topics.  You may select topics from the
`Topics` menu or browse the header files via the `Headers` menu.

## Getting it

You can obtain the sources from https://github.com/facebook/libphenom:

```bash
$ git clone https://github.com/facebook/libphenom.git
```

or [grab a snapshot of master](https://github.com/facebook/libphenom/archive/master.zip)



## Build

```bash
$ ./autogen.sh
$ ./configure
$ make
$ make check
$ sudo make install
```

## Quick Start for using the library

You'll want to set up the main loop using something like this:

```c
#include "phenom/defs.h"
#include "phenom/configuration.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"

int main(int argc, char **argv)
{
  // Must be called prior to calling any other phenom functions
  ph_library_init();
  // Optional config file for tuning internals
  ph_config_load_config_file("/path/to/my/config.json");
  // Enable the non-blocking IO manager
  ph_nbio_init(0);

  // Do stuff here to register client/server stuff.
  // This enables a very simple request/response console
  // that allows you to run diagnostic commands:
  // `echo memory | nc -UC /tmp/phenom-debug-console`
  // (on BSD systems, use `nc -Uc`!)
  // The code behind this is in
  // https://github.com/facebook/libphenom/blob/master/corelib/debug_console.c
  ph_debug_console_start("/tmp/phenom-debug-console");

  // Run
  ph_sched_run();

  return 0;
}
```

And compile it using something like this: (you need optimization enabled)

```bash
$ gcc -O2 main.c `pkg-config libphenom --cflags --libs`
```

(this will link against `libphenom` and `libcares`).

A more detailed example can be found at https://github.com/facebook/libphenom/blob/master/examples/echo.c

Want more inspiration?  Take a look at the code in the test suite.

## Status

We're still hacking and evolving this library, so there may be some rough
edges.  We're very open to feedback; check out the Contributing section
below.

## Getting Help

We've started an IRC channel for realtime conversations; join us in
[#phenom @ freenode](irc://#phenom@chat.freenode.net/)

## Contributing

If you're thinking of hacking on libPhenom we'd love to hear from you!
Feel free to use the Github issue tracker and pull requests to discuss and
submit code changes.

We (Facebook) have to ask for a "Contributor License Agreement" from someone
who sends in a patch or code that we want to include in the codebase.  This is
a legal requirement; a similar situation applies to Apache and other ASF
projects.

If we ask you to fill out a CLA we'll direct you to [our online CLA
page](https://developers.facebook.com/opensource/cla) where you can complete it
easily.  We use the same form as the Apache CLA so that friction is minimal.

## License

libPhenom is made available under the terms of the Apache License 2.0.  See the
LICENSE file that accompanies this distribution for the full text of the
license.

