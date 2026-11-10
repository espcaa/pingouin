/* The sync thread's wait while the render thread runs commands (virgl-darwin-fence-wait-busy.patch):
 * a submit in progress, or one that ended less than the grace period ago, counts as busy; a waiting sync
 * thread wakes when the submit ends, or after at most FENCE_BUSY_NAP; an idle render thread does not
 * make it wait; and no wait leaves a wake-up behind that would end the next wait at once (the 2.9.1
 * flake: a submit that ended just after a 1 ms timeout left a count on the semaphore).
 * The checks follow the code's own counters and the semaphore's count, not the clock: the races are
 * forced at fixed points (FENCE_BUSY_TEST_POINT), so the result does not depend on the machine's load.
 * The only clock checks are lower bounds (a timeout is never shorter than asked). No GL. */
#define FENCE_BUSY_TEST_POINT(point) test_point(#point)
static void test_point(const char *point);
#include "vrend/vrend_renderer.c"
#include <pthread.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static mach_timebase_info_data_t tbi;
static uint64_t now_ns(void) { return mach_absolute_time() * tbi.numer / tbi.denom; }
static uint64_t ticks(uint64_t ns) { return ns * tbi.denom / tbi.numer; }
static void sleep_ns(uint64_t ns) { mach_wait_until(mach_absolute_time() + ticks(ns)); }

/* At which point of fence_wait_render the test ends the running submit ("" = never). In the grace
 * point it runs a whole submit (begin and end), as a render thread would that starts again. */
static const char *end_at = "";
static void test_point(const char *point)
{
   if (strcmp(point, end_at))
      return;
   if (!strcmp(point, "grace"))
      vrend_renderer_submit_begin();
   vrend_renderer_submit_end();
}

/* Wake-ups left on the semaphore (takes them). */
static int sem_count(void)
{
   const mach_timespec_t zero = { 0, 0 };
   int n = 0;
   while (semaphore_timedwait(fence_busy_sem, zero) == KERN_SUCCESS)
      n++;
   return n;
}

static void reset_stats(void) { memset(&fence_busy_stats, 0, sizeof(fence_busy_stats)); }

/* A real render thread: starts sleeping once the sync thread waits (or is done), then ends its
 * submit. */
static uint64_t render_sleep_ns;
static _Atomic uint64_t render_ended_at;
static atomic_bool sync_done;
static void *render(void *arg)
{
   (void)arg;
   while (!atomic_load(&fence_busy_waiting) && !atomic_load(&sync_done))
      ;
   sleep_ns(render_sleep_ns);
   atomic_store(&render_ended_at, now_ns());
   vrend_renderer_submit_end();
   return NULL;
}

int main(void)
{
   mach_timebase_info(&tbi);
   CHECK(semaphore_create(mach_task_self(), &fence_busy_sem, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
   fence_busy_on = true;
   const uint64_t grace = ticks(FENCE_BUSY_GRACE_NS);

   /* Busy: a submit in progress; just after one ended (grace); not after the grace. The times are
    * passed in, so a descheduled test thread cannot change the answer. */
   CHECK(!fence_render_busy(mach_absolute_time()));
   vrend_renderer_submit_begin();
   CHECK(fence_render_busy(mach_absolute_time() + 10 * grace));
   vrend_renderer_submit_end();
   uint64_t idle = atomic_load(&render_idle_at);
   CHECK(fence_render_busy(idle + grace / 2));
   CHECK(!fence_render_busy(idle + grace));
   CHECK(sem_count() == 0);

   /* 1. The submit ends after the flag is up, before the wait: the signal is taken, the wait does
    *    not start, nothing is left. */
   reset_stats();
   end_at = "armed";
   vrend_renderer_submit_begin();
   fence_wait_render(mach_absolute_time());
   CHECK(fence_busy_stats.late == 1 && fence_busy_stats.woken + fence_busy_stats.timed_out == 0);
   CHECK(!atomic_load(&fence_busy_waiting));
   CHECK(sem_count() == 0);

   /* 2. The submit runs on: the wait times out after FENCE_BUSY_NAP, takes its flag back; the end
    *    afterwards sends nothing. */
   reset_stats();
   end_at = "";
   vrend_renderer_submit_begin();
   uint64_t t0 = now_ns();
   fence_wait_render(mach_absolute_time());
   CHECK(now_ns() - t0 >= FENCE_BUSY_NAP_NS * 9 / 10);
   CHECK(fence_busy_stats.timed_out == 1 && fence_busy_stats.woken + fence_busy_stats.late == 0);
   vrend_renderer_submit_end();
   CHECK(sem_count() == 0);

   /* 3. The 2.9.1 flake: the submit ends just after the wait timed out, before the flag is taken
    *    back. Its signal must be taken by this wait, not left for the next one. */
   reset_stats();
   end_at = "waited";
   vrend_renderer_submit_begin();
   fence_wait_render(mach_absolute_time());
   CHECK(fence_busy_stats.late == 1 && fence_busy_stats.woken + fence_busy_stats.timed_out == 0);
   CHECK(!atomic_load(&fence_busy_waiting));
   CHECK(sem_count() == 0);

   /* 4. Grace: the render thread just stopped; the sync thread sits out the rest of the grace, and
    *    a whole submit in that window signals nothing (no flag is up). */
   reset_stats();
   end_at = "grace";
   vrend_renderer_submit_begin();
   vrend_renderer_submit_end();
   idle = atomic_load(&render_idle_at);
   fence_wait_render(idle);
   CHECK(mach_absolute_time() >= idle + grace);
   CHECK(fence_busy_stats.woken + fence_busy_stats.late + fence_busy_stats.timed_out == 0);
   CHECK(sem_count() == 0);

   /* 5. A stray count (should never happen) is dropped before the next wait, which then still
    *    waits for the end: here the full FENCE_BUSY_NAP, the submit runs on. */
   reset_stats();
   end_at = "";
   fprintf(stderr, "a stray wake-up planted on purpose (the warning below is expected):\n");
   semaphore_signal(fence_busy_sem);
   vrend_renderer_submit_begin();
   t0 = now_ns();
   fence_wait_render(mach_absolute_time());
   CHECK(now_ns() - t0 >= FENCE_BUSY_NAP_NS * 9 / 10);
   CHECK(fence_busy_stats.stray == 1 && fence_busy_stats.timed_out == 1);
   vrend_renderer_submit_end();
   CHECK(sem_count() == 0);

   /* 6. A real render thread ends a 200 us submit while the sync thread waits. Whatever the load:
    *    the wait never returns before the end unless it timed out (>= FENCE_BUSY_NAP), and leaves
    *    nothing behind. On a quiet machine every one is woken by the end; under load a run may
    *    time out, so it runs until at least one was woken (at most 200 runs). */
   reset_stats();
   render_sleep_ns = 200000;
   int runs = 0;
   while (runs < 20 || (fence_busy_stats.woken == 0 && runs < 200)) {
      runs++;
      pthread_t t;
      atomic_store(&render_ended_at, 0);
      atomic_store(&sync_done, false);
      vrend_renderer_submit_begin();
      t0 = now_ns();
      uint64_t timed_out = fence_busy_stats.timed_out;
      CHECK(pthread_create(&t, NULL, render, NULL) == 0);
      fence_wait_render(mach_absolute_time());
      uint64_t woke = now_ns();
      atomic_store(&sync_done, true);
      uint64_t ended = atomic_load(&render_ended_at);
      CHECK((ended && ended <= woke) ||
            (fence_busy_stats.timed_out == timed_out + 1 && woke - t0 >= FENCE_BUSY_NAP_NS * 9 / 10));
      pthread_join(t, NULL);
      CHECK(sem_count() == 0);
   }
   CHECK(fence_busy_stats.woken + fence_busy_stats.late + fence_busy_stats.timed_out == (uint64_t)runs);
   CHECK(fence_busy_stats.stray == 0);
   fprintf(stderr, "200 us submits: %d runs, %llu woken at the end, %llu end seen late, %llu timed out\n",
           runs, (unsigned long long)fence_busy_stats.woken, (unsigned long long)fence_busy_stats.late,
           (unsigned long long)fence_busy_stats.timed_out);
   CHECK(fence_busy_stats.woken >= 1);

   fprintf(stderr, "PASS: fence waits follow the render thread's submits\n");
   return 0;
}
