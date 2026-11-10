/* Venus memory in the guest GPU memory budget goes with the storage
 * (virgl-venus-memory-budget.patch), through the public renderer API.
 *
 * The guest's kernel sends DETACH when an app closes its GEM handle, but a dma-buf fd
 * or a mapping it keeps holds the blob, and so the host storage. The bytes must stay
 * charged until the blob itself is freed (virgl_renderer_resource_unref), or a loop of
 * "make a blob, keep the fd, close the handle" takes the Mac's memory past the budget.
 *
 * Venus shm blobs (blob id 0, mappable) need no Vulkan device: run-regressions.py links
 * a stub libvulkan that only lets the Venus context start, so no driver and no GPU is
 * used. Venus runs in this process (the render server as a thread, as in QEMU on
 * macOS); a build with a forked render server has no Venus here and the test says so. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "virglrenderer.h"

#define MB (1u << 20)
#define VENUS_CAPSET 4
#define CTX 1

static int failures;

static void check(int ok, const char *what)
{
   printf("%s: %s\n", ok ? "ok" : "FAIL", what);
   failures += !ok;
}

static void write_fence(void *cookie, uint32_t fence)
{
   (void)cookie;
   (void)fence;
}

static void write_context_fence(void *cookie, uint32_t ctx_id, uint32_t ring_idx,
                                uint64_t fence_id)
{
   (void)cookie;
   (void)ctx_id;
   (void)ring_idx;
   (void)fence_id;
}

static struct virgl_renderer_callbacks callbacks = {
   .version = 3,
   .write_fence = write_fence,
   .write_context_fence = write_context_fence,
};

/* A Venus shm blob of the given size, attached to the context like the guest's kernel
 * does; 0 when it was made. */
static int make_blob(uint32_t handle, uint64_t size)
{
   struct virgl_renderer_resource_create_blob_args args = {
      .res_handle = handle,
      .ctx_id = CTX,
      .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
      .blob_flags = VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE,
      .blob_id = 0,
      .size = size,
   };
   int ret = virgl_renderer_resource_create_blob(&args);
   if (!ret)
      virgl_renderer_ctx_attach_resource(CTX, handle);
   return ret;
}

int main(void)
{
#ifndef ENABLE_RENDER_SERVER_WORKER_THREAD
   printf("skip: this build forks the render server (no in-process Venus to test)\n");
   return 0;
#else
   setvbuf(stdout, NULL, _IOLBF, 0);
   setenv("PINGOUIN_GPU_MEMORY_MB", "64", 1);
   int ret = virgl_renderer_init(NULL, VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL |
                                 VIRGL_RENDERER_RENDER_SERVER | VIRGL_RENDERER_THREAD_SYNC |
                                 VIRGL_RENDERER_ASYNC_FENCE_CB, &callbacks);
   if (ret) {
      printf("FAIL: renderer with Venus did not start (%d)\n", ret);
      return 1;
   }
   ret = virgl_renderer_context_create_with_flags(CTX, VENUS_CAPSET, 4, "test");
   if (ret) {
      printf("FAIL: Venus context not made (%d)\n", ret);
      return 1;
   }

   check(make_blob(1, 40 * MB) == 0, "a 40 MB blob fits a 64 MB budget");
   check(make_blob(2, 40 * MB) != 0, "a second 40 MB blob is refused");

   /* The app closes its handle and keeps a dma-buf fd: DETACH, no unref. The
    * blob's storage lives on, so its bytes stay charged. */
   virgl_renderer_ctx_detach_resource(CTX, 1);
   check(make_blob(3, 40 * MB) != 0,
         "after DETACH the kept blob still counts: another 40 MB is refused");

   /* The blob itself is freed: now the bytes come back. */
   virgl_renderer_resource_unref(1);
   check(make_blob(4, 40 * MB) == 0, "after the blob is freed 40 MB fit again");
   virgl_renderer_resource_unref(4);

   /* A loop of make, keep, close never gets past the budget. */
   int made = 0;
   for (uint32_t h = 10; h < 26; h++) {
      if (make_blob(h, 8 * MB) == 0) {
         made++;
         virgl_renderer_ctx_detach_resource(CTX, h);
      }
   }
   check(made <= 8, "make 8 MB + DETACH + keep, 16 times: at most 8 made (64 MB)");
   for (uint32_t h = 10; h < 26; h++)
      virgl_renderer_resource_unref(h);
   check(make_blob(30, 48 * MB) == 0, "all freed: 48 MB fit again");
   virgl_renderer_resource_unref(30);

   /* no virgl_renderer_cleanup: joining the in-process render server thread waits
    * for a socket it has not closed yet (QEMU never cleans up either) */
   virgl_renderer_context_destroy(CTX);
   printf("%s\n", failures ? "FAILED" : "all ok");
   return failures != 0;
#endif
}
