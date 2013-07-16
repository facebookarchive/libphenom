# Phenom

Phenom is an eventing framework for building high performance and high
scalability systems in C

## System Requirements

Phenom is known to compile and pass its test suite on:

 * Linux systems with `epoll`
 * OS X and BSDish systems that have the `kqueue(2)` facility, including
   FreeBSD 9.1 and OpenBSD 5.2
 * Illumos and Solaris style systems that have `port_create(3C)`.

## Facilities

 * Memory management with counters - record how much of which kinds
   of memory that your application is using.
 * Work items - decompose your application into portions of work
   and let the phenom scheduler manage getting them done
 * streaming I/O with buffers
 * Handy data structures

## Goals

 * Balance ease of use with performance
 * Aim to be neutral wrt. your choice of threaded or event-based dispatch
   and work well with both.
 * Where possible, avoid contention points in our implementation so as to
   avoid limiting scalability with the number of cores in the system.

## Docs

Clone this repo and run `php mkdoc.php` and open `docs/phenom.html` in
your browser to have the searchable docs at your fingertips.
