/* Vulkan windows without a copy (virgl-vulkan-windows-iosurface.patch, ADR 0047):
 * the re-stride that turns an IOSurface of fixed-width rows (the Vulkan window
 * buffer's memory) into the window's 2D texture, on the Mac's OpenGL (core
 * profile, no window, no Venus). vrend_blitter.c is compiled into this test for
 * its static functions: the real shader and the real draw.
 *  - 4-byte BGRA-ordered and RGBA-ordered pixels and 8-byte half-float pixels
 *    come out bit-exact (half floats: zeros of both signs, denormals, infinity),
 *    with an offset and a stride that cross the IOSurface's rows;
 *  - the layout check refuses what would read past the IOSurface or overflow. */
#include "vrend/vrend_blitter.c"
#include "cgl-context.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

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

/* rows of `row` BGRA texels, as vkr makes them (narrower here, so images cross rows) */
static IOSurfaceRef make_surface(unsigned row, unsigned rows)
{
   const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
                          kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
   const void *values[] = { number(row), number(rows), number(4), number(row * 4),
                            number('BGRA') };
   CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
   for (int i = 0; i < 5; i++)
      CFRelease(values[i]);
   IOSurfaceRef s = IOSurfaceCreate(props);
   CFRelease(props);
   return s;
}

typedef int (*tex_image_iosurface_fn)(void *, GLenum, GLenum, GLsizei, GLsizei, GLenum, GLenum,
                                      IOSurfaceRef, GLuint);

static uint16_t specials[] = { 0x0000, 0x8000, 0x0001, 0x03ff, 0x8001, 0x0400, 0x3c00,
                               0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x3555, 0x1234 };

static void run_case(struct vrend_blitter_ctx *b, void *ctx, tex_image_iosurface_fn bind,
                     const char *name, int mode, unsigned w, unsigned h, uint32_t offset)
{
   const unsigned bpp = mode == 2 ? 8 : 4, row = 64;
   const uint32_t stride = ((w * bpp + 255) / 256) * 256 + 256; /* padded rows, as Mesa's PRIME */
   const unsigned rows = (offset + stride * h) / (row * 4) + 2;
   IOSurfaceRef s = make_surface(row, rows);
   if (!s) {
      check(0, "IOSurfaceCreate");
      return;
   }
   IOSurfaceLock(s, 0, NULL);
   uint8_t *base = IOSurfaceGetBaseAddress(s);
   const size_t size = IOSurfaceGetBytesPerRow(s) * rows;
   for (size_t i = 0; i < size; i++)
      base[i] = (uint8_t)(i * 131 + 7);
   if (mode == 2) {
      for (unsigned y = 0; y < h; y++)
         for (unsigned x = 0; x < w * 4; x++) {
            uint16_t v = (y * w * 4 + x) < sizeof(specials) / 2 ? specials[y * w * 4 + x]
                                                                : (uint16_t)((x * 2654435761u + y) >> 9);
            if (((v >> 10) & 31) == 31)
               v &= 0xfc00; /* NaN payloads are not compared: infinities only */
            memcpy(base + offset + (size_t)y * stride + x * 2, &v, 2);
         }
   }
   IOSurfaceUnlock(s, 0, NULL);

   GLuint rect, dst;
   glGenTextures(1, &rect);
   glBindTexture(GL_TEXTURE_RECTANGLE, rect);
   int err = bind(ctx, GL_TEXTURE_RECTANGLE, GL_RGBA8, row, rows, GL_BGRA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, s, 0);
   glGenTextures(1, &dst);
   glBindTexture(GL_TEXTURE_2D, dst);
   glTexImage2D(GL_TEXTURE_2D, 0, mode == 2 ? GL_RGBA16F : GL_RGBA8, w, h, 0,
                mode == 2 ? GL_RGBA : GL_BGRA, mode == 2 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
   bool drawn = !err && restride_draw(b, dst, w, h, rect, row, offset / 4, stride / 4, mode);

   uint8_t *got = calloc((size_t)w * h, bpp);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, b->restride_fb);
   glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
   if (mode == 0)
      glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, got);
   else if (mode == 1)
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, got);
   else
      glReadPixels(0, 0, w, h, GL_RGBA, GL_HALF_FLOAT, got);
   glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

   /* every format's bytes as they are in the guest's memory */
   unsigned wrong = 0;
   for (unsigned y = 0; y < h; y++)
      wrong += memcmp(got + (size_t)y * w * bpp, base + offset + (size_t)y * stride, (size_t)w * bpp) != 0;
   char what[160];
   snprintf(what, sizeof(what), "%s %ux%u, offset %u, stride %u: %s", name, w, h, offset, stride,
            err ? "CGL refused the IOSurface" : !drawn ? "not drawn" : wrong ? "rows differ" : "bit-exact");
   check(!err && drawn && !wrong, what);
   free(got);
   glDeleteTextures(1, &rect);
   glDeleteTextures(1, &dst);
   CFRelease(s);
}

int main(void)
{
   /* layout: inside, past the end, overflow, odd sizes */
   check(vrend_iosurface_layout_ok(0, 14848, 1816, 870, 8, 4096, 834), "layout: a 1816x870 FP16 window fits");
   check(!vrend_iosurface_layout_ok(0, 14848, 1816, 870, 8, 4096, 788), "layout: rows past the IOSurface refused");
   check(vrend_iosurface_layout_ok(16384 * 834 - 4, 4, 1, 1, 4, 4096, 834), "layout: the last texel itself is inside");
   check(!vrend_iosurface_layout_ok(16384 * 834, 4, 1, 1, 4, 4096, 834), "layout: one texel past the end refused");
   check(!vrend_iosurface_layout_ok(2, 256, 16, 4, 4, 64, 64), "layout: an offset that is not a whole texel refused");
   check(!vrend_iosurface_layout_ok(0, 60, 16, 4, 4, 64, 64), "layout: a stride shorter than a row refused");
   check(!vrend_iosurface_layout_ok(0, 256, 16, 4, 3, 64, 64), "layout: 3-byte pixels refused");
   check(!vrend_iosurface_layout_ok(UINT64_MAX - 3, 256, 16, 4, 4, 64, 64), "layout: an offset that wraps refused");
   check(!vrend_iosurface_layout_ok(0, 1ull << 62, 1, 9, 4, 4096, 16384), "layout: rows that wrap refused");
   check(!vrend_iosurface_layout_ok(0, 256, 16, 0, 4, 64, 64), "layout: no rows refused");
   check(!vrend_iosurface_layout_ok(0, 1ull << 40, 1, 3, 4, 4096, 16384), "layout: a stride past the memory refused");
   check(!vrend_iosurface_layout_ok(0, 4, 1, 1, 4, 65536, 65536), "layout: more texels than 32-bit numbers refused");

   if (!cgl_init_current()) {
      printf("SKIP: no CGL core profile context\n");
      return failures;
   }
   void *lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
   tex_image_iosurface_fn bind = (tex_image_iosurface_fn)dlsym(lib, "CGLTexImageIOSurface2D");
   void *(*current)(void) = (void *(*)(void))dlsym(lib, "CGLGetCurrentContext");
   struct vrend_blitter_ctx *b = &vrend_blit_ctx;
   blit_set_glsl_version(b, 410);
   glGenVertexArrays(1, &b->vaoid);
   check(bind && current && restride_init(b), "the re-stride shader builds on the core profile");
   if (failures)
      return failures;
   run_case(b, current(), bind, "BGRA", 0, 37, 19, 0);
   run_case(b, current(), bind, "BGRA", 0, 64, 5, 4096 + 12);
   run_case(b, current(), bind, "RGBA", 1, 37, 19, 256);
   run_case(b, current(), bind, "half-float RGBA", 2, 23, 11, 0);
   run_case(b, current(), bind, "half-float RGBA", 2, 64, 3, 512 + 8);
   return failures;
}
