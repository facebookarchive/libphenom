
lib_LTLIBRARIES = libphenom.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libphenom.pc

libphenom_la_CPPFLAGS = \
	-Ithirdparty/ck/include -Iinclude -I$(top_srcdir)\
	-DPHENOM_IMPL=1
libphenom_la_CFLAGS = @IRONMANCFLAGS@
libphenom_la_SOURCES = \
	corelib/init.c \
	corelib/buf.c \
	corelib/config.c \
	corelib/counter.c \
	corelib/debug_console.c \
	corelib/dns/addrinfo.c \
	corelib/dtoa.c \
	corelib/error.c \
	corelib/hook.c \
	corelib/log.c \
	corelib/memory.c \
	corelib/openssl/bio_stream.c \
	corelib/openssl/bio_bufq.c \
	corelib/openssl/init.c \
	corelib/openssl/ssl_stream.c \
	corelib/pingfd.c \
	corelib/pipe2.c \
	corelib/pprintf.c \
	corelib/nbio/common.c \
	corelib/nbio/epoll.c \
	corelib/nbio/kqueue.c \
	corelib/nbio/portfs.c \
	corelib/job.c \
	corelib/string.c \
	corelib/net/listener.c \
	corelib/net/sockaddr.c \
	corelib/net/socket.c \
	corelib/thread.c \
	corelib/timerwheel.c \
	corelib/vprintf.c \
	corelib/variant/variant.c \
	corelib/variant/json-dump.c \
	corelib/variant/json-load.c \
	corelib/variant/pack.c \
	corelib/variant/path.c \
	corelib/hash/murmur.c \
	corelib/hash/table.c \
	corelib/streams/copy.c \
	corelib/streams/make.c \
	corelib/streams/read.c \
	corelib/streams/write.c \
	corelib/streams/fd.c \
	corelib/streams/string.c \
	corelib/streams/temp.c

if GIMLI
gimli_libphenom_la_LDFLAGS = -module
gimli_libphenom_la_CPPFLAGS = \
	-Iinclude -I$(top_srcdir)\
	-DPHENOM_IMPL=1
gimli_libphenom_la_CFLAGS = @IRONMANCFLAGS@
gimli_libphenom_la_LIBADD = libphenom.la
gimli_libphenom_la_SOURCES = \
	corelib/gimli/boot.c \
	corelib/gimli/counter.c \
	corelib/gimli/log.c \
	corelib/gimli/thread.c \
	corelib/gimli/sockaddr.c
lib_LTLIBRARIES += gimli_libphenom.la
endif

# some automakes put include/phenom in here, which causes conflicts
# with system includes so we turn off default includes by default.
DEFAULT_INCLUDES =

noinst_LIBRARIES = libtap.a
libtap_a_CPPFLAGS = -Iinclude
libtap_a_SOURCES = thirdparty/tap/tap.c

TEST_CPPFLAGS = -Ithirdparty/tap -Iinclude -DPHENOM_IMPL
TEST_LDADD = libphenom.la libtap.a
# don't drive me mad when I'm tab completing
TEST_SUITE_LOG = tests/suite.log
TESTS = tests/counter.t tests/memory.t tests/timer.t tests/printf.t \
				tests/iobasic.t tests/stream.t tests/tpool.t \
				tests/string.t \
				tests/hashtable.t \
				tests/sockaddr.t \
				tests/dns.t \
				tests/variant.t \
				tests/buf.t \
				tests/bench/iopipes.t
noinst_PROGRAMS = $(TESTS) $(EXAMPLES)

EXAMPLES = examples/echo examples/sclient

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: build-tests docs clean-docs

all-local: docs check-lint
clean-local: clean-docs
install-exec-local: install-phenom-hdrs

PYTHON=@PYTHON@
export PYTHON

check-lint:
	./thirdparty/clint.sh

install-phenom-hdrs:
	$(mkdir_p) "$(DESTDIR)$(includedir)/phenom"
	$(INSTALL_HEADER) include/phenom/*.h $(DESTDIR)$(includedir)/phenom

clean-docs:
	-rm docs/declmap.js

docs: docs/declmap.js

update-www: docs
	PRETTY_VIA_PYTHON=1 @PHP@ mkdoc.php
	test -d ../phenom-pages
	cp docs/* ../phenom-pages/

docs/declmap.js: mkdoc.php include/phenom/*.h README.markdown
	@PHP@ mkdoc.php

examples_echo_CPPFLAGS = $(TEST_CPPFLAGS)
examples_echo_LDADD = $(TEST_LDADD)

examples_sclient_CPPFLAGS = $(TEST_CPPFLAGS)
examples_sclient_LDADD = $(TEST_LDADD)

tests_counter_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_counter_t_LDADD = $(TEST_LDADD)

tests_memory_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_memory_t_LDADD = $(TEST_LDADD)

tests_timer_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_timer_t_LDADD = $(TEST_LDADD)

tests_printf_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_printf_t_LDADD = $(TEST_LDADD)

tests_iobasic_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_iobasic_t_LDADD = $(TEST_LDADD)

tests_hashtable_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_hashtable_t_LDADD = $(TEST_LDADD)

tests_sockaddr_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_sockaddr_t_LDADD = $(TEST_LDADD)

tests_buf_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_buf_t_LDADD = $(TEST_LDADD)

tests_dns_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_dns_t_LDADD = $(TEST_LDADD)

tests_stream_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_stream_t_LDADD = $(TEST_LDADD)

tests_string_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_string_t_LDADD = $(TEST_LDADD)

tests_tpool_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_tpool_t_LDADD = $(TEST_LDADD)

tests_variant_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_variant_t_LDADD = $(TEST_LDADD)

if HAVE_LIBEVENT
LIBEVENT=-levent
endif
tests_bench_iopipes_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_bench_iopipes_t_LDADD = $(TEST_LDADD) $(LIBEVENT)

if HAVE_CLANG
# See http://blog.alexrp.com/2013/09/26/clangs-static-analyzer-and-automake/
analyze_srcs = $(filter %.c, $(libphenom_la_SOURCES))
analyze_plists = $(analyze_srcs:%.c=%.plist)
MOSTLYCLEANFILES = $(analyze_plists)

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $<
	@$(COMPILE) --analyze $(libphenom_la_CPPFLAGS) $< -o $@

clang-analyze: $(analyze_plists)
clang-clean:
	-rm -f $(analyze_plists)
else
clang-analyze:
clang-clean:
endif
.PHONY: clang-analyze clang-clean
