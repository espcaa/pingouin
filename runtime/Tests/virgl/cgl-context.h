/* CGL core profile contexts for the runtime tests: the Mac's own OpenGL
 * (4.1 core), no window. CGL is looked up with dlsym, so tests built with the
 * renderer's flags (epoxy headers, no OpenGL framework on the link line) can
 * use it as well as tests that link OpenGL.
 *
 * Tests that drive the renderer through its public API define
 * CGL_CONTEXT_RENDERER_CALLBACKS before including this file and get
 * cgl_renderer_callbacks: every renderer context shares with cgl_main_ctx. */
#ifndef CGL_CONTEXT_H
#define CGL_CONTEXT_H

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static struct {
   int (*choose)(const int *, void **, int *);
   int (*create)(void *, void *, void **);
   int (*current)(void *);
   int (*destroy)(void *);
   int (*release_pixel_format)(void *);
} cgl;

static bool cgl_load(void)
{
   if (cgl.choose)
      return true;
   void *lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
   if (!lib)
      return false;
   cgl.choose = (int (*)(const int *, void **, int *))dlsym(lib, "CGLChoosePixelFormat");
   cgl.create = (int (*)(void *, void *, void **))dlsym(lib, "CGLCreateContext");
   cgl.current = (int (*)(void *))dlsym(lib, "CGLSetCurrentContext");
   cgl.destroy = (int (*)(void *))dlsym(lib, "CGLDestroyContext");
   cgl.release_pixel_format = (int (*)(void *))dlsym(lib, "CGLReleasePixelFormat");
   return cgl.choose && cgl.create && cgl.current && cgl.destroy && cgl.release_pixel_format;
}

/* A core profile context (3.2 core and later: 4.1 on the Mac) that shares
 * objects with share (may be NULL), or NULL. With PINGOUIN_TEST_SOFTWARE_GL=1
 * it is on Apple's software renderer, not the GPU (STANDARDS section 14). */
static void *cgl_core_context(void *share)
{
   const char *soft = getenv("PINGOUIN_TEST_SOFTWARE_GL");
   const int attrs[] = {99 /* kCGLPFAOpenGLProfile */, 0x3200 /* 3.2 core and later */,
                        soft && *soft == '1' ? 70 /* kCGLPFARendererID */ : 0,
                        0x00020400 /* kCGLRendererGenericFloatID */, 0};
   void *pix = NULL, *ctx = NULL;
   int n = 0;
   if (!cgl_load() || cgl.choose(attrs, &pix, &n) || !pix)
      return NULL;
   if (cgl.create(pix, share, &ctx))
      ctx = NULL;
   cgl.release_pixel_format(pix);
   return ctx;
}

static bool cgl_make_current(void *ctx)
{
   return cgl_load() && !cgl.current(ctx);
}

/* A current core profile context for tests that compile shaders (inline: tests that
 * make their own contexts do not use it). */
static inline bool cgl_init_current(void)
{
   void *ctx = cgl_core_context(NULL);
   return ctx && cgl_make_current(ctx);
}

#ifdef CGL_CONTEXT_RENDERER_CALLBACKS
static void *cgl_main_ctx;

static void cgl_write_fence(void *cookie, uint32_t fence)
{
   (void)cookie;
   (void)fence;
}

static virgl_renderer_gl_context cgl_create_gl_context(void *cookie, int scanout,
                                                       struct virgl_renderer_gl_ctx_param *param)
{
   (void)cookie;
   (void)scanout;
   return cgl_core_context(param->shared ? cgl_main_ctx : NULL);
}

static void cgl_destroy_gl_context(void *cookie, virgl_renderer_gl_context ctx)
{
   (void)cookie;
   cgl.destroy(ctx);
}

static int cgl_renderer_make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx)
{
   (void)cookie;
   (void)scanout;
   return cgl_make_current(ctx) ? 0 : -1;
}

static struct virgl_renderer_callbacks cgl_renderer_callbacks = {
   .version = 1,
   .write_fence = cgl_write_fence,
   .create_gl_context = cgl_create_gl_context,
   .destroy_gl_context = cgl_destroy_gl_context,
   .make_current = cgl_renderer_make_current,
};

/* The main context the renderer's contexts share with, made current. */
static bool cgl_init_renderer_main(void)
{
   cgl_main_ctx = cgl_core_context(NULL);
   return cgl_main_ctx && cgl_make_current(cgl_main_ctx);
}
#endif

#endif
