/*
Copyright 2005-2013 Rich Felker
Copyright 2013 Facebook, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "phenom/sysutil.h"
#include "phenom/stream.h"

// Generate a non-crypto secure "random" number
// This just needs to be sufficiently different
// that we can avoid a collision in most cases;
// we drive it in a loop with 100 attempts
static uint64_t noddy_rando(char *nametemplate)
{
  uint64_t base;
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_REALTIME)
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  base = ts.tv_nsec;

#elif defined(__MACH__)
  base = mach_absolute_time();
#else
# error implement me
#endif
  return base * 65537 ^ (uintptr_t)&base / 16 + (uintptr_t)nametemplate;
}

static void make_name(char *nametemplate, int len)
{
  uint64_t r = noddy_rando(nametemplate);
  int i;

  for (i = 0; i < len; i++, r >>= 5) {
    nametemplate[i] = 'A'+(r&15)+(r&16)*2;
  }
}

int ph_mkostemp(char *nametemplate, int flags)
{
  return ph_mkostemps(nametemplate, 0, flags);
}

int ph_mkostemps(char *nametemplate, int suffixlen, int flags)
{
  int len = strlen(nametemplate);
  char *base = nametemplate + (len - suffixlen) - 1;
  int tlen = 0;
  int fd, retries = 100;

  while (*base == 'X' && base > nametemplate) {
    tlen++;
    base--;
  }

  if (tlen == 0) {
    errno = EINVAL;
    return -1;
  }
  base++;

  while (retries--) {
    make_name(base, tlen);
    fd = open(nametemplate, flags|O_RDWR|O_CREAT|O_EXCL, 0600);
    if (fd >= 0) {
      return fd;
    }
    if (errno != EEXIST) {
      return -1;
    }
  }
  return -1;
}

/* vim:ts=2:sw=2:et:
 */

