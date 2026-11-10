/* The sync thread's fallback (virgl-thread-sync-fallback.patch): when the
 * thread cannot start, fences must be polled, and the poll fd says so.
 * No GL: the context callbacks are stubs and no fence is ever made. */
#include "vrend/vrend_renderer.c"

static int contexts, destroyed;
static int fake_context;

static virgl_gl_context make_none(UNUSED int scanout, UNUSED struct virgl_gl_ctx_param *p)
{
   return NULL;
}
static virgl_gl_context make_one(UNUSED int scanout, UNUSED struct virgl_gl_ctx_param *p)
{
   contexts++;
   return (virgl_gl_context)&fake_context;
}
static void destroy_one(UNUSED virgl_gl_context ctx)
{
   destroyed++;
}
static int make_current(UNUSED virgl_gl_context ctx)
{
   return 0;
}

static struct vrend_if_cbs cbs = {
   .destroy_gl_context_surfaceless = destroy_one,
   .make_current_surfaceless = make_current,
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", #c); return 1; } } while (0)

static void start(void)
{
   vrend_state.eventfd = -1;
   vrend_state.use_async_fence_cb = true;
   vrend_renderer_use_threaded_sync();
}

int main(void)
{
   vrend_clicbs = &cbs;
   list_inithead(&vrend_state.fence_list);
   list_inithead(&vrend_state.fence_wait_list);

   /* No shared GL context for the thread. */
   cbs.create_gl_context_surfaceless = make_none;
   start();
   CHECK(!vrend_state.sync_thread);
   CHECK(!vrend_state.use_async_fence_cb);
   CHECK(vrend_renderer_get_poll_fd() < 0);

   /* No eventfd (the FIFO cannot be made in TMPDIR). */
   cbs.create_gl_context_surfaceless = make_one;
   setenv("TMPDIR", "/nonexistent/pingouin-test", 1);
   start();
   CHECK(!vrend_state.sync_thread);
   CHECK(!vrend_state.use_async_fence_cb);
   CHECK(vrend_renderer_get_poll_fd() < 0);
   CHECK(destroyed == contexts);

   /* Everything there: the thread runs and keeps the async path. */
   unsetenv("TMPDIR");
   start();
   CHECK(vrend_state.sync_thread);
   CHECK(vrend_state.use_async_fence_cb);
   CHECK(vrend_renderer_get_poll_fd() >= 0);
   vrend_free_sync_thread();
   CHECK(destroyed == contexts);

   puts("thread sync fallback: ok");
   return 0;
}
