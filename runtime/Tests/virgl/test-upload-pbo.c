/* Texture uploads through the pixel unpack buffer (virgl-transfer-upload-pbo.patch),
 * through the public renderer API on the Mac's own OpenGL (CGL core contexts, no
 * window; PINGOUIN_TEST_SOFTWARE_GL=1 for Apple's software renderer).
 * A frozen screen in screenshot mode uploads a whole window on every pointer move,
 * from guest memory in many pieces: those uploads now go through a buffer the
 * pieces are gathered into. Every upload must give the same pixels as before:
 * whole textures and boxes inside them, row strides wider than the box, 1 and 4
 * bytes per pixel, odd widths, guest memory in one piece and in many pieces of odd
 * sizes, a box at an offset in the guest's memory, a small upload (below the size
 * that takes the buffer), a texture written again (the buffer is reused), and
 * rectangle textures. Each case is read back and compared with the guest's bytes;
 * all of it runs twice, with the buffer and with PINGOUIN_VIRGL_UPLOAD_PBO=0 (the old
 * path), as two processes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/wait.h>
#include "virglrenderer.h"
#include "virgl_hw.h"
#define CGL_CONTEXT_RENDERER_CALLBACKS
#include "cgl-context.h"

enum { TEST_TEXTURE_2D = 2, TEST_TEXTURE_RECT = 5 };

static int failures;

static void check(int ok, const char *what)
{
   printf("%s: %s\n", ok ? "ok" : "FAIL", what);
   failures += !ok;
}

static uint8_t pattern(uint32_t i, uint32_t seed)
{
   uint32_t v = i * 2654435761u + seed * 40503u;
   return (uint8_t)(v >> 13);
}

/* Guest memory of SIZE bytes in PIECES pieces of uneven sizes (one big buffer
 * cut up, so the bytes are known), as QEMU hands a resource's pages over. */
struct guest_mem {
   uint8_t *bytes;
   size_t size;
   struct iovec iov[64];
   unsigned n;
};

static void guest_mem_make(struct guest_mem *m, size_t size, unsigned pieces, uint32_t seed)
{
   m->bytes = malloc(size);
   m->size = size;
   for (size_t i = 0; i < size; i++)
      m->bytes[i] = pattern((uint32_t)i, seed);
   m->n = 0;
   size_t off = 0;
   for (unsigned p = 0; p < pieces && off < size; p++) {
      size_t left = size - off, len;
      if (p == pieces - 1)
         len = left;
      else {
         len = size / pieces + (p % 3 == 0 ? 4093 : p % 3 == 1 ? 17 : 0);
         if (len > left)
            len = left;
      }
      m->iov[m->n].iov_base = m->bytes + off;
      m->iov[m->n].iov_len = len;
      m->n++;
      off += len;
   }
}

/* One case: a texture of W x H (BPP bytes a pixel, FORMAT), guest memory with
 * rows of STRIDE bytes in PIECES pieces, the box (X, Y, BW, BH) uploaded from
 * OFFSET; then the whole texture is read back and every pixel compared: the box
 * from the guest's bytes, the rest from the texture's previous content. */
static void one_case(const char *what, uint32_t target, uint32_t format, uint32_t bpp,
                     uint32_t w, uint32_t h, uint32_t stride, unsigned pieces, uint32_t x,
                     uint32_t y, uint32_t bw, uint32_t bh, uint32_t offset, uint32_t seed)
{
   static uint32_t handle = 100;
   struct guest_mem mem;
   size_t size = (size_t)offset + (size_t)stride * h;
   uint8_t *before = calloc((size_t)w * h, bpp), *after = calloc((size_t)w * h, bpp);
   char line[256];

   handle++;
   struct virgl_renderer_resource_create_args args = {
      .handle = handle, .target = target, .format = format,
      .bind = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, .width = w, .height = h,
      .depth = 1, .array_size = 1,
   };
   if (virgl_renderer_resource_create(&args, NULL, 0)) {
      snprintf(line, sizeof(line), "%s: texture made", what);
      check(0, line);
      return;
   }

   /* the texture's first content: the whole of it, written tightly from one piece */
   struct guest_mem first;
   guest_mem_make(&first, (size_t)w * h * bpp, 1, seed + 7);
   struct virgl_box all = { 0, 0, 0, w, h, 1 };
   virgl_renderer_transfer_write_iov(handle, 0, 0, w * bpp, 0, &all, 0, first.iov, first.n);
   memcpy(before, first.bytes, (size_t)w * h * bpp);

   guest_mem_make(&mem, size, pieces, seed);
   struct virgl_box box = { x, y, 0, bw, bh, 1 };
   int r = virgl_renderer_transfer_write_iov(handle, 0, 0, stride, 0, &box,
                                             offset + (uint64_t)y * stride + (uint64_t)x * bpp,
                                             mem.iov, mem.n);

   struct iovec back = { after, (size_t)w * h * bpp };
   virgl_renderer_transfer_read_iov(handle, 0, 0, w * bpp, 0, &all, 0, &back, 1);

   uint32_t bad = 0;
   for (uint32_t row = 0; row < h; row++)
      for (uint32_t col = 0; col < w; col++) {
         const uint8_t *want;
         if (col >= x && col < x + bw && row >= y && row < y + bh)
            want = mem.bytes + offset + (size_t)row * stride + (size_t)col * bpp;
         else
            want = before + ((size_t)row * w + col) * bpp;
         bad += memcmp(after + ((size_t)row * w + col) * bpp, want, bpp) != 0;
      }
   snprintf(line, sizeof(line), "%s: %ux%u, %u bytes a pixel, stride %u, %u pieces, box "
            "%u,%u %ux%u at offset %u: %u pixels wrong", what, w, h, bpp, stride, mem.n,
            x, y, bw, bh, offset, bad);
   check(r == 0 && bad == 0, line);

   virgl_renderer_resource_unref(handle);
   free(mem.bytes);
   free(first.bytes);
   free(before);
   free(after);
}

static int run_cases(void)
{
   if (!cgl_init_renderer_main()) {
      printf("skip: no OpenGL context on this Mac\n");
      return 0;
   }
   static int cookie;
   if (virgl_renderer_init(&cookie, 0, &cgl_renderer_callbacks)) {
      printf("FAIL: virgl_renderer_init\n");
      return 1;
   }
   const uint32_t BGRA = VIRGL_FORMAT_B8G8R8A8_UNORM, R8 = VIRGL_FORMAT_R8_UNORM;

   one_case("window, whole", TEST_TEXTURE_2D, BGRA, 4, 735, 460, 735 * 4, 37, 0, 0, 735, 460,
            0, 1);
   one_case("window, one piece", TEST_TEXTURE_2D, BGRA, 4, 735, 460, 735 * 4, 1, 0, 0, 735,
            460, 0, 2);
   one_case("window, wide rows", TEST_TEXTURE_2D, BGRA, 4, 600, 300, 640 * 4, 23, 0, 0, 600,
            300, 0, 3);
   one_case("damage box", TEST_TEXTURE_2D, BGRA, 4, 800, 500, 800 * 4, 29, 13, 7, 517, 211,
            0, 4);
   one_case("damage box, one piece", TEST_TEXTURE_2D, BGRA, 4, 800, 500, 800 * 4, 1, 101, 59,
            333, 300, 0, 5);
   one_case("box at an offset", TEST_TEXTURE_2D, BGRA, 4, 512, 384, 512 * 4, 11, 3, 5, 500,
            370, 4096, 6);
   one_case("glyphs, odd width", TEST_TEXTURE_2D, R8, 1, 1001, 333, 1003, 19, 0, 0, 1001, 333,
            0, 7);
   one_case("small upload", TEST_TEXTURE_2D, BGRA, 4, 64, 64, 64 * 4, 3, 0, 0, 64, 64, 0, 8);
   one_case("rectangle texture", TEST_TEXTURE_RECT, BGRA, 4, 700, 200, 700 * 4, 13, 0, 0, 700,
            200, 0, 9);
   /* the same texture again and again: the buffer's storage is renewed each time */
   for (uint32_t i = 0; i < 4; i++)
      one_case("again", TEST_TEXTURE_2D, BGRA, 4, 640, 400, 640 * 4, 31, 0, 0, 640, 400, 0,
               10 + i);

   virgl_renderer_cleanup(&cookie);
   return failures != 0;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc > 1 && !strcmp(argv[1], "child"))
      return run_cases();

   /* both paths, each in its own process (the switch is read once) */
   int bad = 0;
   for (int pbo = 1; pbo >= 0; pbo--) {
      printf("== uploads %s the pixel buffer\n", pbo ? "through" : "without");
      pid_t pid = fork();
      if (pid == 0) {
         if (!pbo)
            setenv("PINGOUIN_VIRGL_UPLOAD_PBO", "0", 1);
         execl(argv[0], argv[0], "child", (char *)NULL);
         _exit(127);
      }
      int st = 0;
      waitpid(pid, &st, 0);
      bad |= !WIFEXITED(st) || WEXITSTATUS(st) != 0;
   }
   printf("%s\n", bad ? "upload: FAILED" : "upload: all checks passed");
   return bad;
}
