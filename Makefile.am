
lib_LIBRARIES = libphenom.a

ck_SOURCES = \
	thirdparty/ck/src/ck_bag.c \
	thirdparty/ck/src/ck_barrier_centralized.c \
	thirdparty/ck/src/ck_barrier_combining.c \
	thirdparty/ck/src/ck_barrier_dissemination.c \
	thirdparty/ck/src/ck_barrier_mcs.c \
	thirdparty/ck/src/ck_barrier_tournament.c \
	thirdparty/ck/src/ck_epoch.c \
	thirdparty/ck/src/ck_hp.c \
	thirdparty/ck/src/ck_hs.c \
	thirdparty/ck/src/ck_ht.c

libphenom_a_CPPFLAGS = \
	-Ithirdparty/ck/include -Iinclude \
	-DPHENOM_IMPL=1
libphenom_a_CFLAGS = @IRONMANCFLAGS@
libphenom_a_SOURCES = \
	corelib/counter.c \
	corelib/dtoa.c \
	corelib/error.c \
	corelib/log.c \
	corelib/memory.c \
	corelib/pingfd.c \
	corelib/pipe2.c \
	corelib/pprintf.c \
	corelib/nbio.c \
	corelib/job.c \
	corelib/thread.c \
	corelib/timerwheel.c \
	corelib/vprintf.c \
	corelib/streams/make.c \
	corelib/streams/read.c \
	corelib/streams/write.c \
	corelib/streams/fd.c \
	corelib/streams/temp.c \
	$(ck_SOURCES)

# some automakes put include/phenom in here, which causes conflicts
# with system includes so we turn off default includes by default.
DEFAULT_INCLUDES =

noinst_LIBRARIES = libtap.a
libtap_a_CPPFLAGS = -Iinclude
libtap_a_SOURCES = thirdparty/tap/tap.c

TEST_CPPFLAGS = -Ithirdparty/tap -Ithirdparty/ck/include -Iinclude -DPHENOM_IMPL
TEST_LDADD = libphenom.a libtap.a
TESTS = tests/counter.t tests/memory.t tests/timer.t tests/printf.t \
				tests/iobasic.t tests/stream.t tests/tpool.t \
				tests/bench/iopipes.t
noinst_PROGRAMS = $(TESTS)

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: build-tests

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

tests_stream_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_stream_t_LDADD = $(TEST_LDADD)

tests_tpool_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_tpool_t_LDADD = $(TEST_LDADD)

tests_bench_iopipes_t_CPPFLAGS = $(TEST_CPPFLAGS)
tests_bench_iopipes_t_LDADD = $(TEST_LDADD)
