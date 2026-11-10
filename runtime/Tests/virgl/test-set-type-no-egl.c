/* A compositor's dma-buf import on macOS OpenGL (virgl-set-type-without-egl.patch),
 * through the public renderer API on Apple's software renderer (no GPU, no Venus).
 *
 * PIPE_RESOURCE_SET_TYPE types a resource the guest made as a blob, when its
 * compositor imports a client's dma-buf. macOS OpenGL (CGL, no EGL) cannot import
 * the memory; that used to return EINVAL, and the decode error ended the whole
 * context (Hyprland: black desktop for good). Now:
 *  - a blob whose memory cannot be read gets a blank texture, the context goes on
 *    and later commands still run, with one log line;
 *  - an unknown resource id still ends the context (a guest bug, not an import);
 *  - bad sizes are still refused by the normal resource checks.
 * The Metal heap copy itself (Venus memory) needs a Venus device and the GPU: the VM
 * tests cover it (tracks: gpu-robust.md, "Black desktop root cause"). */
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include "soft-gl.h"
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"

static CGLContextObj main_ctx;
static int failures;
static int blank_lines;

static void count_log(enum virgl_log_level_flags level, const char *message, void *data)
{
   (void)level;
   (void)data;
   blank_lines += strstr(message, "it stays blank, the context goes on") != NULL;
   fputs(message, stderr);
}

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

static virgl_renderer_gl_context create_gl_context(void *cookie, int scanout,
                                                   struct virgl_renderer_gl_ctx_param *param)
{
   (void)cookie;
   (void)scanout;
   (void)param;
   return soft_gl_context(main_ctx);
}

static void destroy_gl_context(void *cookie, virgl_renderer_gl_context ctx)
{
   (void)cookie;
   CGLDestroyContext(ctx);
}

static int make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx)
{
   (void)cookie;
   (void)scanout;
   return CGLSetCurrentContext(ctx) ? -1 : 0;
}

static struct virgl_renderer_callbacks callbacks = {
   .version = 1,
   .write_fence = write_fence,
   .create_gl_context = create_gl_context,
   .destroy_gl_context = destroy_gl_context,
   .make_current = make_current,
};

/* SET_TYPE for one plane, as guest Mesa sends it for an EGL dma-buf import. */
static int set_type(int ctx_id, uint32_t res, uint32_t width, uint32_t height, uint32_t stride)
{
   uint32_t dw[1 + VIRGL_PIPE_RES_SET_TYPE_SIZE(1)] = {
      VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0, VIRGL_PIPE_RES_SET_TYPE_SIZE(1)),
      res, VIRGL_FORMAT_B8G8R8X8_UNORM, VIRGL_BIND_SAMPLER_VIEW, width, height,
      0 /* usage */, 0xffffffff, 0x00ffffff /* DRM_FORMAT_MOD_INVALID */, stride, 0,
   };
   return virgl_renderer_submit_cmd(dw, ctx_id, 1 + VIRGL_PIPE_RES_SET_TYPE_SIZE(1));
}

/* A guest-memory blob attached to a context, untyped like a dma-buf's resource. */
static void make_blob(int ctx_id, uint32_t handle, struct iovec *iov)
{
   struct virgl_renderer_resource_create_blob_args args = {
      .res_handle = handle,
      .ctx_id = ctx_id,
      .blob_mem = VIRGL_RENDERER_BLOB_MEM_GUEST,
      .size = iov->iov_len,
      .iovecs = iov,
      .num_iovs = 1,
   };
   check(!virgl_renderer_resource_create_blob(&args), "guest blob made");
   virgl_renderer_ctx_attach_resource(ctx_id, handle);
}

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   main_ctx = soft_gl_context(NULL);
   if (!main_ctx || CGLSetCurrentContext(main_ctx)) {
      printf("skip: no OpenGL context on this Mac\n");
      return 0;
   }
   soft_gl_require();
   virgl_set_log_callback(count_log, NULL, NULL);
   static int cookie;
   if (virgl_renderer_init(&cookie, 0, &callbacks)) {
      printf("FAIL: virgl_renderer_init\n");
      return 1;
   }
   check(!virgl_renderer_context_create(1, 10, "compositor") &&
         !virgl_renderer_context_create(2, 5, "buggy") &&
         !virgl_renderer_context_create(3, 7, "too-big"), "three contexts");

   struct iovec iov = { calloc(1, 64 * 1024), 64 * 1024 };
   make_blob(1, 10, &iov);
   check(set_type(1, 10, 64, 64, 256) == 0,
         "a dma-buf import CGL cannot share gets a blank texture (no error)");
   check(set_type(1, 10, 64, 64, 256) == 0,
         "the compositor's context goes on: its next command runs");
   check(blank_lines == 1, "one log line says the resource stays blank");

   check(set_type(2, 99, 64, 64, 256) != 0, "an unknown resource still ends that context");
   check(set_type(2, 99, 64, 64, 256) != 0, "...and its later commands are refused");

   struct iovec iov3 = { calloc(1, 4096), 4096 };
   make_blob(3, 30, &iov3);
   check(set_type(3, 30, 1u << 20, 64, 256) != 0, "a width past the GL limit is refused");

   check(set_type(1, 10, 64, 64, 256) == 0, "the compositor is not hit by the others");

   virgl_renderer_context_destroy(1);
   virgl_renderer_context_destroy(2);
   virgl_renderer_context_destroy(3);
   virgl_renderer_resource_unref(10);
   virgl_renderer_resource_unref(30);
   virgl_renderer_cleanup(&cookie);
   free(iov.iov_base);
   free(iov3.iov_base);
   printf("%s\n", failures ? "FAILED" : "all ok");
   return failures != 0;
}
