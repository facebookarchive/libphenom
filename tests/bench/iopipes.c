#include "phenom/job.h"
#include "phenom/thread.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/counter.h"
#include <sysexits.h>
#include <sys/socket.h>
#include <sys/resource.h>

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

static struct ph_nbio_stats stats;

int num_socks = 100;
int num_active = 1;
int time_duration = 1000;
int io_threads = 0;

ph_job_t *events = NULL;
int *write_ends = NULL;

static void deadline_reached(ph_job_t *job, ph_iomask_t why, void *data)
{
  unused_parameter(job);
  unused_parameter(why);
  unused_parameter(data);

  gettimeofday(&end_time, NULL);
  ph_nbio_stat(&stats);
  ph_sched_stop();
}

static void consume_data(ph_job_t *job, ph_iomask_t why, void *data)
{
  char buf[10];
  ptrdiff_t off;

  off = job - events;

  unused_parameter(why);
  unused_parameter(data);

  ignore_result(read(job->fd, buf, sizeof(buf)));
  ignore_result(write(write_ends[off], "y", 1));

  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

int main(int argc, char **argv)
{
  int c;
  int i;
  struct rlimit rl;

  io_threads = ph_num_cores();
  while ((c = getopt(argc, argv, "n:a:c:t:")) != -1) {
    switch (c) {
      case 'n':
        num_socks = atoi(optarg);
        break;
      case 'a':
        num_active = atoi(optarg);
        break;
      case 'c':
        io_threads = atoi(optarg);
        break;
      case 't':
        time_duration = atoi(optarg) * 1000;
        break;
      default:
        fprintf(stderr, "Illegal arguments %c\n", c);
        exit(EX_USAGE);
    }
  }

  ph_nbio_init(io_threads);

  events = calloc(num_socks, sizeof(*events));
  write_ends = calloc(num_socks, sizeof(*write_ends));

  rl.rlim_cur = rl.rlim_max = num_socks * 2 + 64;
  if (setrlimit(RLIMIT_NOFILE, &rl)) {
    perror("setrlimit");
    exit(EX_OSERR);
  }

  for (i = 0; i < num_socks; i++) {
    int pair[2];

    ph_job_init(&events[i]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
      perror("socketpair");
      exit(EX_OSERR);
    }

    write_ends[i] = pair[1];
    events[i].fd = pair[0];
    ph_socket_set_nonblock(pair[0], true);
    ph_socket_set_nonblock(pair[1], true);

    events[i].callback = consume_data;
    ph_job_set_nbio(&events[i], PH_IOMASK_READ, 0);

    ignore_result(write(write_ends[i], "x", 1));
  }

  ph_job_init(&deadline);
  deadline.callback = deadline_reached;
  ph_job_set_timer_in_ms(&deadline, time_duration);

  ph_log(PH_LOG_ERR, "Created %d events, using %d cores\n",
      num_socks, io_threads);

  gettimeofday(&start_time, NULL);
  ph_sched_run();

  {
    double duration;
    double rate;
    char cbuf[64];

    ph_log(PH_LOG_ERR, "%" PRIi64 " timer ticks\n", stats.timer_ticks);
    timersub(&end_time, &start_time, &elapsed_time);
    duration = elapsed_time.tv_sec + (elapsed_time.tv_usec/1000000.0f);
    rate = stats.num_dispatched / duration;

    ph_log(PH_LOG_ERR, "Over %.3fs, fired %s events/s",
        duration,
        commaprint((uint64_t)rate, cbuf, sizeof(cbuf))
    );
  }

  return EX_OK;
}

/* vim:ts=2:sw=2:et:
 */

