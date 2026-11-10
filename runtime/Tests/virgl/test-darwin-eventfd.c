/* The macOS eventfd stand-in (virgl-darwin-thread-sync.patch): one fd that
 * a writer can signal (also through a dup, as the Venus render server gets
 * it) and a reader can poll and drain, like Linux's eventfd. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "virgl_util.h"

static int readable(int fd)
{
   struct pollfd p = { .fd = fd, .events = POLLIN };
   return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", #c); return 1; } } while (0)

int main(void)
{
   CHECK(has_eventfd());
   int fd = create_eventfd(0);
   CHECK(fd >= 0);
   CHECK(!readable(fd));

   CHECK(write_eventfd(fd, 1) == 0);
   CHECK(readable(fd));
   flush_eventfd(fd);
   CHECK(!readable(fd));

   /* A dup (or an fd passed to the render server) signals the same fd. */
   int other = dup(fd);
   CHECK(other >= 0);
   for (int i = 0; i < 100000; i++)
      CHECK(write_eventfd(other, 1) == 0); /* a full FIFO still counts */
   CHECK(readable(fd));
   flush_eventfd(fd);
   CHECK(!readable(fd));

   int initial = create_eventfd(1);
   CHECK(initial >= 0 && readable(initial));

   close(other);
   close(fd);
   close(initial);
   puts("darwin eventfd: ok");
   return 0;
}
