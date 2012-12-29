
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
	corelib/sched.c \
	corelib/thread.c \
	corelib/timerwheel.c \
	corelib/vprintf.c \
	$(ck_SOURCES)

noinst_LIBRARIES = libtap.a
libtap_a_CPPFLAGS = -Iinclude
libtap_a_SOURCES = thirdparty/tap/tap.c

TEST_CPPFLAGS = -Ithirdparty/tap -Ithirdparty/ck/include -Iinclude -DPHENOM_IMPL
TEST_LDADD = libphenom.a libtap.a
TESTS = tests/counter.t tests/memory.t tests/timer.t tests/printf.t \
				tests/iobasic.t
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


