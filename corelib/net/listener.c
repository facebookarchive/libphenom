/*
 * Copyright 2013-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "phenom/listener.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/dns.h"
#include "phenom/printf.h"
#ifdef HAVE_SYSCTLBYNAME
#include <sys/sysctl.h>
#endif

static ph_memtype_def_t defs[] = {
  { "socket", "listener", sizeof(ph_listener_t), PH_MEM_FLAGS_ZERO },
};
static struct {
  ph_memtype_t listener;
} mt;
static void accept_dispatch(ph_job_t *j, ph_iomask_t why, void *data);
static void listener_dtor(ph_job_t *job);

static struct ph_job_def listener_template = {
  accept_dispatch,
  PH_MEMTYPE_INVALID,
  listener_dtor
};

static void do_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.listener);
  listener_template.memtype = mt.listener;
}
PH_LIBRARY_INIT(do_init, 0)

static void listener_dtor(ph_job_t *job)
{
  ph_listener_t *lstn = (ph_listener_t*)job;

  ph_unused_parameter(lstn);
}

static void accept_dispatch(ph_job_t *j, ph_iomask_t why, void *data)
{
  ph_socket_t fd;
  ph_listener_t *lstn = data;
  ph_sockaddr_t addr;
  socklen_t alen = sizeof(addr.sa);
  ph_sock_t *sock;

  ph_unused_parameter(why);

#ifdef HAVE_ACCEPT4
  {
    int a4flags = 0;

    if (lstn->flags & PH_SOCK_CLOEXEC) {
      a4flags |= SOCK_CLOEXEC;
    }
    if (lstn->flags & PH_SOCK_NONBLOCK) {
      a4flags |= SOCK_NONBLOCK;
    }

    fd = accept4(j->fd, &addr.sa.sa, &alen, a4flags);
  }
#else
  fd = accept(j->fd, &addr.sa.sa, &alen);
#endif

  if (fd == -1) {
    goto done;
  }

#ifndef HAVE_ACCEPT4
  if (lstn->flags & PH_SOCK_CLOEXEC) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  if (lstn->flags & PH_SOCK_NONBLOCK) {
    ph_socket_set_nonblock(fd, true);
  }
#endif

  addr.family = addr.sa.sa.sa_family;
  sock = ph_sock_new_from_socket(fd, &lstn->addr, &addr);
  if (!sock) {
    close(fd);
    goto done;
  }

  sock->job.emitter_affinity = ck_pr_faa_32(&lstn->emitter_affinity, 1);
  lstn->acceptor(lstn, sock);

done:
  if (lstn->enabled) {
    ph_job_set_nbio(j, PH_IOMASK_READ, NULL);
  }
}

ph_listener_t *ph_listener_new(const char *name,
    ph_listener_accept_func acceptor)
{
  ph_listener_t *lstn;

  lstn = (ph_listener_t*)ph_job_alloc(&listener_template);
  if (!lstn) {
    return NULL;
  }

  lstn->job.fd = -1;
  lstn->acceptor = acceptor;
  lstn->flags = PH_SOCK_CLOEXEC|PH_SOCK_NONBLOCK;
  lstn->job.data = lstn;
  ph_snprintf(lstn->name, sizeof(lstn->name), "%s", name);

  ph_listener_set_backlog(lstn, 0);
  return lstn;
}

ph_socket_t ph_listener_get_fd(ph_listener_t *lstn)
{
  return lstn->job.fd;
}

ph_result_t ph_listener_bind(ph_listener_t *lstn, const ph_sockaddr_t *addr)
{
  int err;

  if (lstn->job.fd == -1) {
    int on = 1;

    lstn->job.fd = ph_socket_for_addr(addr, SOCK_STREAM, lstn->flags);
    if (lstn->job.fd == -1) {
      return PH_ERR;
    }
#ifdef SO_REUSEADDR
    setsockopt(lstn->job.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
#ifdef SO_REUSEPORT
    setsockopt(lstn->job.fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
  }
  if (bind(lstn->job.fd, &addr->sa.sa, ph_sockaddr_socklen(addr)) == 0) {
    return PH_OK;
  }
  err = errno;
  if (err == EADDRINUSE && addr->family == AF_UNIX) {
    unlink(addr->sa.nix.sun_path);
    if (bind(lstn->job.fd, &addr->sa.sa, ph_sockaddr_socklen(addr)) == 0) {
      return PH_OK;
    }
    errno = err;
  }
  return PH_ERR;
}

void ph_listener_set_backlog(ph_listener_t *lstn, int backlog)
{
  if (backlog > 0) {
    lstn->backlog = backlog;
    return;
  }

#ifdef SOMAXCONN
  lstn->backlog = SOMAXCONN;
#else
  lstn->backlog = 128;
#endif
#ifdef __linux__
  {
    int fd = open("/proc/sys/net/core/somaxconn", O_RDONLY);
    if (fd >= 0) {
      char buf[32];
      int x;

      x = read(fd, buf, sizeof(buf));
      if (x > 0) {
        lstn->backlog = strtol(buf, 0, 10);
      }
      close(fd);
    }
  }
#elif defined(HAVE_SYSCTLBYNAME)
  {
    int lim = 0;
    size_t limlen = sizeof(lim);
    if (sysctlbyname("kern.ipc.somaxconn", &lim, &limlen, 0, 0) == 0) {
      lstn->backlog = lim;
    }
  }
#endif
}

void ph_listener_enable(ph_listener_t *lstn, bool enable)
{
  if (lstn->enabled == enable) {
    return;
  }

  if (!enable) {
    ph_job_set_nbio(&lstn->job, 0, NULL);
    lstn->enabled = enable;
    return;
  }

  if (!lstn->listening) {
    if (listen(lstn->job.fd, lstn->backlog)) {
      ph_panic("failed to listen() on %s: `Pe%d", lstn->name, errno);
    }
    lstn->listening = true;
  }

  lstn->enabled = enable;
  ph_job_set_nbio(&lstn->job, PH_IOMASK_READ, NULL);
}


/* vim:ts=2:sw=2:et:
 */

