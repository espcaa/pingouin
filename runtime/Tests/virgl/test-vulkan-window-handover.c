/* GL work that crosses vrend's GL contexts must be seen by the other context
 * (virgl-darwin-context-switch-flush.patch, #499). vrend_blitter.c is compiled
 * into this test for its static functions; vrend_sync_make_current comes from
 * libvirgl.a. On the Mac's OpenGL (the GPU: Apple's software renderer cannot
 * sample an IOSurface RECTANGLE texture), three shared CGL contexts as QEMU
 * makes them (main, the guest's, the blitter's), no window, no Venus.
 *  1. Blitter -> guest (Vulkan windows without a copy, ADR 0047): three window
 *     buffers in turn (a swapchain), each an IOSurface of 4096-texel rows as
 *     vkr makes them, written by the CPU with the frame's number; the real
 *     vrend_blitter_restride re-strides it into the window's 2D texture, and
 *     the guest's very next draw must show that frame (before the patch: the
 *     buffer's previous frame, every time). PINGOUIN_TEST_BIND=once binds each
 *     window texture once and never again (vrend binds a sampler view only
 *     when its slot changed), else before every draw.
 *  2. Guest -> blitter (vrend_renderer_blit_int's GL fallback goes the same
 *     way): the guest's context draws the frame into a RECTANGLE texture,
 *     the blitter re-strides that, and the copy must hold the frame just drawn.
 * PINGOUIN_TEST_BENCH=1 (not a build-time run; take the bench lock): the CPU
 * time of the re-stride and of its two context switches at 1816x870 and
 * 3840x2160, with up to 16 more re-strides queued in the blitter before the
 * switch back. The switch back must stay a GPU wait (its time must not grow
 * with the blitter's queued work), and the guest's draw must still show the
 * frame just re-strided. */
#include "vrend/vrend_blitter.c"
#include "cgl-context.h"
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>

#define BUFFERS 3
#define FRAMES 300
#define ROW 4096u /* texels a row, as vkr's VKR_IOSURFACE_ROW_PIXELS */

static int failures;

static void check(int ok, const char *what)
{
   printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
   failures += !ok;
}

static CFNumberRef number(int64_t v)
{
   return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &v);
}

static IOSurfaceRef make_surface(unsigned rows)
{
   const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
                          kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
   const void *values[] = { number(ROW), number(rows), number(4), number(ROW * 4), number('BGRA') };
   CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
   for (int i = 0; i < 5; i++)
      CFRelease(values[i]);
   IOSurfaceRef s = IOSurfaceCreate(props);
   CFRelease(props);
   return s;
}

static int test_make_current(virgl_gl_context ctx)
{
   return cgl_make_current(ctx) ? 0 : -1;
}

static const struct vrend_if_cbs test_cbs = { .make_current = test_make_current };

typedef int (*tex_image_iosurface_fn)(void *, GLenum, GLenum, GLsizei, GLsizei, GLenum, GLenum,
                                      IOSurfaceRef, GLuint);
static tex_image_iosurface_fn bind_iosurface;

/* frame number and position in every texel; as a little-endian word of RGBA bytes */
static uint32_t texel(unsigned f, unsigned x, unsigned y)
{
   return (f & 0xffffu) | (((x * 7u + y * 13u) & 255u) << 16) | 0xff000000u;
}

static const char fullscreen_vs[] =
   "#version 410\n"
   "void main() {\n"
   "   gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1), 0.0, 1.0);\n"
   "}\n";
/* the guest's draw that samples the window (Hyprland's) */
static const char sample_fs[] =
   "#version 410\n"
   "uniform sampler2D tex;\n"
   "out vec4 color;\n"
   "void main() { color = texelFetch(tex, ivec2(gl_FragCoord.xy), 0); }\n";
/* the guest's draw that writes a frame (case 2) */
static const char frame_fs[] =
   "#version 410\n"
   "uniform uint frame;\n"
   "out vec4 color;\n"
   "void main() {\n"
   "   uvec2 p = uvec2(gl_FragCoord.xy);\n"
   "   uint v = (frame & 0xffffu) | (((p.x * 7u + p.y * 13u) & 255u) << 16) | 0xff000000u;\n"
   "   color = vec4(uvec4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24)) / 255.0;\n"
   "}\n";

static GLuint build(const char *fs)
{
   GLuint p = glCreateProgram();
   const char *src[2] = { fullscreen_vs, fs };
   const GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(type[i]);
      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glAttachShader(p, s);
      glDeleteShader(s);
   }
   glBindFragDataLocation(p, 0, "color");
   glLinkProgram(p);
   GLint ok = 0;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   return ok ? p : 0;
}

static GLuint texture_2d(GLenum target, unsigned w, unsigned h)
{
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(target, t);
   glTexImage2D(target, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST); /* one level: complete */
   glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glBindTexture(target, 0);
   return t;
}

static GLuint framebuffer(GLenum target, GLuint tex)
{
   GLuint fb;
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, tex, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   return fb;
}

/* rows 0, h/2 and h-1 of fb; counts a frame as right, the previous frame, or other */
struct tally { unsigned right, previous, other; };

static void read_rows(GLuint fb, unsigned w, unsigned h, unsigned f, uint32_t *got, struct tally *t)
{
   const unsigned ys[3] = { 0, h / 2, h - 1 };
   unsigned wrong = 0, prev = 0;
   glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
   for (int r = 0; r < 3; r++)
      glReadPixels(0, ys[r], w, 1, GL_RGBA, GL_UNSIGNED_BYTE, got + (size_t)r * w);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
   for (int r = 0; r < 3; r++)
      for (unsigned x = 0; x < w; x++) {
         const uint32_t v = got[(size_t)r * w + x];
         wrong += v != texel(f, x, ys[r]);
         prev += v == texel(f - BUFFERS, x, ys[r]) || v == texel(f - 1, x, ys[r]);
      }
   t->right += !wrong;
   t->previous += wrong && prev == 3 * w;
   t->other += wrong && prev != 3 * w;
}

/* The guest's side of case 1 and the bench: window buffers (vrend_resource_iosurface_bind,
 * the 2D textures) and the draw that samples them. */
struct window {
   unsigned w, h, stride;
   IOSurfaceRef surf[BUFFERS];
   uint32_t *base[BUFFERS];
   GLuint rect[BUFFERS], tex[BUFFERS], out, fb, prog, vao;
};

static bool window_init(struct window *win, void *guest, unsigned w, unsigned h)
{
   win->w = w;
   win->h = h;
   win->stride = (w * 4 + 255) & ~255u; /* Mesa's PRIME rows, bytes */
   const unsigned rows = (win->stride * h + ROW * 4 - 1) / (ROW * 4);
   bool ok = true;
   for (int i = 0; i < BUFFERS; i++) {
      win->surf[i] = make_surface(rows);
      if (!win->surf[i])
         return false;
      IOSurfaceLock(win->surf[i], 0, NULL);
      win->base[i] = IOSurfaceGetBaseAddress(win->surf[i]);
      IOSurfaceUnlock(win->surf[i], 0, NULL);
      glGenTextures(1, &win->rect[i]);
      glBindTexture(GL_TEXTURE_RECTANGLE, win->rect[i]);
      ok &= !bind_iosurface(guest, GL_TEXTURE_RECTANGLE, GL_RGBA8, ROW, rows, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, win->surf[i], 0);
      glBindTexture(GL_TEXTURE_RECTANGLE, 0);
      win->tex[i] = texture_2d(GL_TEXTURE_2D, w, h);
   }
   win->out = texture_2d(GL_TEXTURE_2D, w, h);
   win->fb = framebuffer(GL_TEXTURE_2D, win->out);
   win->prog = build(sample_fs);
   glGenVertexArrays(1, &win->vao);
   return ok && win->prog;
}

/* the guest (Venus, finished before the compositor got it) wrote frame f into buffer i:
 * BGRA in memory, so the word GL reads back as RGBA bytes is texel() */
static void window_write(struct window *win, int i, unsigned f)
{
   for (unsigned y = 0; y < win->h; y++) {
      uint32_t *row = win->base[i] + (size_t)y * (win->stride / 4);
      for (unsigned x = 0; x < win->w; x++) {
         const uint32_t v = texel(f, x, y);
         row[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
      }
   }
}

/* the rest of the compositor's command buffer: the draw that samples the window, and the
 * submit's fence (vrend_renderer_create_fence: glFenceSync and glFlush) */
static void window_draw(struct window *win, int i, int unit)
{
   glUseProgram(win->prog);
   if (unit) {
      glUniform1i(glGetUniformLocation(win->prog, "tex"), unit);
   } else {
      glUniform1i(glGetUniformLocation(win->prog, "tex"), 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, win->tex[i]);
   }
   glBindFramebuffer(GL_FRAMEBUFFER, win->fb);
   glViewport(0, 0, win->w, win->h);
   glBindVertexArray(win->vao);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
   glFlush();
   glDeleteSync(fence);
}

static void window_free(struct window *win)
{
   glDeleteTextures(BUFFERS, win->rect);
   glDeleteTextures(BUFFERS, win->tex);
   glDeleteTextures(1, &win->out);
   glDeleteFramebuffers(1, &win->fb);
   glDeleteProgram(win->prog);
   glDeleteVertexArrays(1, &win->vao);
   for (int i = 0; i < BUFFERS; i++)
      CFRelease(win->surf[i]);
}

static void case_blitter_to_guest(void *guest, bool bind_once)
{
   struct window win;
   const unsigned w = 1816, h = 870; /* a Zed window on the reporter's Mac (#499) */
   cgl_make_current(guest);
   check(window_init(&win, guest, w, h), "case 1: the window buffers bind as RECTANGLE textures");
   if (failures)
      return;
   if (bind_once) {
      for (int k = 0; k < BUFFERS; k++) {
         glActiveTexture(GL_TEXTURE1 + k);
         glBindTexture(GL_TEXTURE_2D, win.tex[k]);
      }
      glActiveTexture(GL_TEXTURE0);
   }
   struct tally t = { 0 };
   unsigned restrided = 0;
   uint32_t *got = calloc((size_t)w * 3, 4);
   for (unsigned f = 1; f <= FRAMES; f++) {
      const int i = f % BUFFERS;
      window_write(&win, i, f);
      /* the draw that samples the window re-strides it first (vrend_resource_heap_refresh) */
      restrided += vrend_blitter_restride(guest, win.tex[i], w, h, win.rect[i], ROW, 0,
                                          win.stride / 4, 0);
      window_draw(&win, i, bind_once ? 1 + i : 0);
      if (f > BUFFERS) /* every buffer written once first */
         read_rows(win.fb, w, h, f, got, &t);
      else
         glFinish();
   }
   char what[240];
   snprintf(what, sizeof(what), "case 1: the blitter's re-stride ran for all %u frames", FRAMES);
   check(restrided == FRAMES, what);
   snprintf(what, sizeof(what),
            "case 1 (blitter -> guest, bind %s): the guest's next draw shows the frame just "
            "re-strided: %u of %u (an older frame: %u, other: %u)",
            bind_once ? "once" : "each draw", t.right, FRAMES - BUFFERS, t.previous, t.other);
   check(t.right == FRAMES - BUFFERS, what);
   free(got);
   window_free(&win);
}

static void case_guest_to_blitter(void *guest)
{
   const unsigned w = 1816, h = 870;
   cgl_make_current(guest);
   GLuint src = texture_2d(GL_TEXTURE_RECTANGLE, w, h);
   GLuint src_fb = framebuffer(GL_TEXTURE_RECTANGLE, src);
   GLuint copy = texture_2d(GL_TEXTURE_2D, w, h);
   GLuint copy_fb = framebuffer(GL_TEXTURE_2D, copy);
   GLuint prog = build(frame_fs), vao;
   glGenVertexArrays(1, &vao);
   check(prog != 0, "case 2: the guest's frame program builds");
   if (!prog)
      return;
   struct tally t = { 0 };
   unsigned restrided = 0;
   uint32_t *got = calloc((size_t)w * 3, 4);
   for (unsigned f = 1; f <= FRAMES; f++) {
      /* the guest's context draws frame f (a GL app's frame) ... */
      glUseProgram(prog);
      glUniform1ui(glGetUniformLocation(prog, "frame"), f);
      glBindFramebuffer(GL_FRAMEBUFFER, src_fb);
      glViewport(0, 0, w, h);
      glBindVertexArray(vao);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      /* ... and the blitter's context reads it right away (rows of w texels, 4-byte RGBA) */
      restrided += vrend_blitter_restride(guest, copy, w, h, src, w, 0, w, 0);
      if (f > 1)
         read_rows(copy_fb, w, h, f, got, &t);
   }
   char what[240];
   snprintf(what, sizeof(what), "case 2: the blitter's re-stride ran for all %u frames", FRAMES);
   check(restrided == FRAMES, what);
   snprintf(what, sizeof(what),
            "case 2 (guest -> blitter): the blitter's copy holds the frame the guest just drew: "
            "%u of %u (an older frame: %u, other: %u)", t.right, FRAMES - 1, t.previous, t.other);
   check(t.right == FRAMES - 1, what);
   free(got);
   glDeleteTextures(1, &src);
   glDeleteTextures(1, &copy);
   glDeleteFramebuffers(1, &src_fb);
   glDeleteFramebuffers(1, &copy_fb);
   glDeleteProgram(prog);
   glDeleteVertexArrays(1, &vao);
}

static mach_timebase_info_data_t timebase;

static double ms_since(uint64_t t0)
{
   return (double)(mach_absolute_time() - t0) * timebase.numer / timebase.denom / 1e6;
}

static int by_value(const void *a, const void *b)
{
   const double x = *(const double *)a, y = *(const double *)b;
   return (x > y) - (x < y);
}

static void stats(const char *what, double *v, unsigned n)
{
   qsort(v, n, sizeof(*v), by_value);
   printf("  %-26s median %.3f ms, p95 %.3f, max %.3f\n", what, v[n / 2], v[n * 95 / 100], v[n - 1]);
}

/* Bench (PINGOUIN_TEST_BENCH=1): vrend_blitter_restride in its parts, timed on the CPU; `load`
 * more re-strides into a scratch texture are queued in the blitter before the switch back. */
static void bench(void *guest, unsigned w, unsigned h, unsigned load)
{
   enum { N = 120 };
   struct vrend_blitter_ctx *b = &vrend_blit_ctx;
   struct window win;
   cgl_make_current(guest);
   if (!window_init(&win, guest, w, h)) {
      check(0, "bench: the window buffers bind as RECTANGLE textures");
      return;
   }
   GLuint scratch = texture_2d(GL_TEXTURE_2D, w, h);
   double into[N], back[N], call[N];
   struct tally t = { 0 };
   unsigned lag = 0;
   uint32_t *got = calloc((size_t)w * 3, 4);
   for (unsigned f = 1; f <= N + BUFFERS; f++) {
      const int i = f % BUFFERS;
      window_write(&win, i, f);
      const uint64_t t0 = mach_absolute_time();
      vrend_renderer_init_blit_ctx(b); /* the switch into the blitter */
      const double t_into = ms_since(t0);
      for (unsigned l = 0; l < load; l++)
         restride_draw(b, scratch, w, h, win.rect[i], ROW, 0, win.stride / 4, 0);
      restride_draw(b, win.tex[i], w, h, win.rect[i], ROW, 0, win.stride / 4, 0);
      GLsync probe = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      const uint64_t t1 = mach_absolute_time();
      vrend_sync_make_current(guest); /* the switch back */
      const double t_back = ms_since(t1), t_call = ms_since(t0);
      window_draw(&win, i, 0);
      if (f <= BUFFERS) {
         glFinish();
         glDeleteSync(probe);
         continue;
      }
      read_rows(win.fb, w, h, f, got, &t);
      GLint status = GL_SIGNALED;
      glGetSynciv(probe, GL_SYNC_STATUS, 1, NULL, &status);
      lag += status != GL_SIGNALED;
      glDeleteSync(probe);
      into[f - BUFFERS - 1] = t_into;
      back[f - BUFFERS - 1] = t_back;
      call[f - BUFFERS - 1] = t_call;
   }
   printf("BENCH %ux%u, %u more re-strides queued: right %u of %u (older %u, other %u), "
          "blitter fence unsignalled after the guest's readback %u\n",
          w, h, load, t.right, N, t.previous, t.other, lag);
   stats("into the blitter", into, N);
   stats("back to the guest", back, N);
   stats("whole re-stride call (CPU)", call, N);
   check(t.right == N, "bench: the guest's draw shows the frame just re-strided");
   free(got);
   glDeleteTextures(1, &scratch);
   window_free(&win);
}

int main(void)
{
   void *main_ctx = cgl_core_context(NULL);
   void *guest = main_ctx ? cgl_core_context(main_ctx) : NULL;
   void *blitter = main_ctx ? cgl_core_context(main_ctx) : NULL;
   if (!guest || !blitter) {
      printf("SKIP: no CGL core profile context\n");
      return 0;
   }
   void *lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
   bind_iosurface = lib ? (tex_image_iosurface_fn)dlsym(lib, "CGLTexImageIOSurface2D") : NULL;
   check(bind_iosurface != NULL, "CGLTexImageIOSurface2D found");
   if (failures)
      return failures;

   /* the blitter's context, as vrend_renderer_init_blit_ctx leaves it */
   vrend_clicbs = &test_cbs;
   struct vrend_blitter_ctx *b = &vrend_blit_ctx;
   cgl_make_current(blitter);
   b->gl_context = blitter;
   b->initialised = true;
   blit_set_glsl_version(b, 410);
   glGenVertexArrays(1, &b->vaoid);
   check(restride_init(b), "the re-stride shader builds in the blitter's context");
   glFlush();
   if (failures)
      return failures;

   const char *bench_env = getenv("PINGOUIN_TEST_BENCH");
   if (bench_env && !strcmp(bench_env, "1")) {
      mach_timebase_info(&timebase);
      const unsigned sizes[2][2] = { { 1816, 870 }, { 3840, 2160 } };
      const unsigned loads[] = { 0, 4, 8, 16 };
      for (int s = 0; s < 2; s++)
         for (unsigned l = 0; l < sizeof(loads) / sizeof(loads[0]); l++)
            bench(guest, sizes[s][0], sizes[s][1], loads[l]);
      return failures;
   }
   const char *bind_env = getenv("PINGOUIN_TEST_BIND");
   case_blitter_to_guest(guest, bind_env && !strcmp(bind_env, "once"));
   case_guest_to_blitter(guest);
   return failures;
}
