#include "phenom/job.h"
#include "phenom/thread.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/counter.h"
#include <sysexits.h>
#include <sys/socket.h>
#include <sys/resource.h>

#ifdef HAVE_LIBEVENT
# include <event.h>
#endif

static char *commaprint(uint64_t n, char *retbuf, uint32_t size)
{
  char *p = retbuf + size - 1;
  int i = 0;

  *p = '\0';
  do {
    if (i % 3 == 0 && i != 0) {
      *--p = ',';
    }
    *--p = '0' + n % 10;
    n /= 10;
    i++;
  } while (n != 0);

  return p;
}


struct timeval start_time, end_time, elapsed_time;
static ph_job_t deadline;

static struct ph_nbio_stats stats = {0, 0, 0, 0};

int num_socks = 100;
int time_duration = 1000;
int io_threads = 0;
int use_libevent = 0;

ph_job_t *events = NULL;
#ifdef HAVE_LIBEVENT
struct event *levents = NULL;
#endif
int *write_ends = NULL;

#ifdef HAVE_LIBEVENT
static void lev_read(int fd, short which, void *arg)
{
  ph_job_t *job = arg;
  char buf[10];
  ptrdiff_t off;

  off = job - events;

  ph_unused_parameter(which);

  ph_ignore_result(read(fd, buf, sizeof(buf)));
  ph_ignore_result(write(write_ends[off], "y", 1));

  event_add(&levents[off], NULL);

  stats.num_dispatched++;
}
#endif

static void deadline_reached(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  gettimeofday(&end_time, NULL);
  ph_nbio_stat(&stats);
  ph_sched_stop();
}

static void consume_data(ph_job_t *job, ph_iomask_t why, void *data)
{
  char buf[10];
  ptrdiff_t off;

  off = job - events;

  ph_unused_parameter(why);
  ph_unused_parameter(data);

  ph_ignore_result(read(job->fd, buf, sizeof(buf)));
  ph_ignore_result(write(write_ends[off], "y", 1));

  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

int main(int argc, char **argv)
{
  int c;
  int i;
  struct rlimit rl;

  io_threads = 0;
  while ((c = getopt(argc, argv, "n:a:c:t:e")) != -1) {
    switch (c) {
      case 'n':
        num_socks = atoi(optarg);
        break;
      case 'c':
        io_threads = atoi(optarg);
        break;
      case 't':
        time_duration = atoi(optarg) * 1000;
        break;
      case 'e':
        use_libevent = 1;
#ifdef HAVE_LIBEVENT
        event_init();
#endif
        break;
      default:
        fprintf(stderr,
            "-n NUMBER   specify number of sockets (default %d)\n",
            num_socks);
        fprintf(stderr,
            "-c NUMBER   specify IO sched concurrency level (default: auto)\n"
        );
        fprintf(stderr,
            "-t NUMBER   specify duration of test in seconds "
            "(default %ds)\n", time_duration/1000);
        fprintf(stderr,
            "-e          Use libevent instead of libphenom\n");
        exit(EX_USAGE);
    }
  }

  if (use_libevent) {
    io_threads = 1;
  }

  ph_library_init();
  ph_log_level_set(PH_LOG_INFO);
  ph_nbio_init(io_threads);
  ph_nbio_stat(&stats);
  io_threads = stats.num_threads;

  events = calloc(num_socks, sizeof(*events));
  write_ends = calloc(num_socks, sizeof(*write_ends));
#ifdef HAVE_LIBEVENT
  levents = calloc(num_socks, sizeof(*levents));
#endif

  rl.rlim_cur = rl.rlim_max = (num_socks * 2) + (io_threads * 2) + 64;
  if (setrlimit(RLIMIT_NOFILE, &rl)) {
    perror("setrlimit");
    // Don't exit: valgrind causes this to fail and terminating
    // here stops us from collecting anything useful
  }

  for (i = 0; i < num_socks; i++) {
    int pair[2];

    ph_job_init(&events[i]);
    events[i].emitter_affinity = i;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
      perror("socketpair");
      exit(EX_OSERR);
    }

    write_ends[i] = pair[1];
    events[i].fd = pair[0];
    ph_socket_set_nonblock(pair[0], true);
    ph_socket_set_nonblock(pair[1], true);

    events[i].callback = consume_data;

    if (use_libevent) {
#ifdef HAVE_LIBEVENT
      event_set(&levents[i], events[i].fd, EV_READ, lev_read, &events[i]);
      event_add(&levents[i], NULL);
#endif
    } else {
      ph_job_set_nbio(&events[i], PH_IOMASK_READ, 0);
    }

    ph_ignore_result(write(write_ends[i], "x", 1));
  }

  if (use_libevent) {
#ifdef HAVE_LIBEVENT
    struct timeval dead = { time_duration / 1000, 0 };
    event_loopexit(&dead);

    ph_log(PH_LOG_INFO, "Using libevent\n");
#else
    ph_panic("No libevent support");
#endif
  } else {
    ph_job_init(&deadline);
    deadline.callback = deadline_reached;
    ph_job_set_timer_in_ms(&deadline, time_duration);
  }

  ph_log(PH_LOG_INFO, "Created %d events, using %d threads\n",
      num_socks, io_threads);

  gettimeofday(&start_time, NULL);
  if (use_libevent) {
#ifdef HAVE_LIBEVENT
    event_loop(0);
    gettimeofday(&end_time, NULL);
#endif
  } else {
    ph_sched_run();
  }

  {
    double duration;
    double rate;
    char cbuf[64];
    char *logname;

    ph_log(PH_LOG_INFO, "%" PRIi64 " timer ticks\n", stats.timer_ticks);
    timersub(&end_time, &start_time, &elapsed_time);
    duration = elapsed_time.tv_sec + (elapsed_time.tv_usec/1000000.0f);
    rate = stats.num_dispatched / duration;

    ph_log(PH_LOG_INFO, "Over %.3fs, fired %s events/s",
        duration,
        commaprint((uint64_t)rate, cbuf, sizeof(cbuf))
    );

    // To support automated data collection by run-pipes.php
    logname = getenv("APPEND_FILE");
    if (logname && logname[0]) {
      ph_stream_t *s = ph_stm_file_open(logname,
                          O_WRONLY|O_CREAT|O_APPEND, 0666);
      if (s) {
        ph_stm_printf(s, "%s,%d,%d,%f\n",
            use_libevent ? "libevent" : "libphenom",
            num_socks,
            io_threads,
            rate);
        ph_stm_flush(s);
        ph_stm_close(s);
      }
    }

    ph_log(PH_LOG_INFO, "Over %.3fs, fired %s events/core/s",
        duration,
        commaprint((uint64_t)(rate/io_threads), cbuf, sizeof(cbuf))
    );
  }

  free(events);
  free(write_ends);
#ifdef HAVE_LIBEVENT
  free(levents);
#endif

#ifdef __APPLE__
  // OS/X "mysteriously" spins CPU cycles somewhere in the kernel after we've
  // exit'd to close out descriptors.  Let folks know that that is what is
  // going on (very noticeable above 100k events).  We could close them here,
  // but it doesn't make it go faster and it is just more code to run.
  ph_log(PH_LOG_INFO, "Kernel may spin closing descriptors. Enjoy!");
#endif

  return EX_OK;
}

/* vim:ts=2:sw=2:et:
 */

