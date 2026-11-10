/* Drawing into a framebuffer without attachments, through the public renderer
 * API on the Mac's own OpenGL (CGL core contexts, no window).
 * The guest sends such a framebuffer when all its draw buffers are GL_NONE
 * (WebGL conformance: webgl-draw-buffers.html, conformance2/rendering/
 * draw-buffers.html). Apple's GL has no ARB_framebuffer_no_attachments and
 * answered the draw with GL_INVALID_FRAMEBUFFER_OPERATION, which stopped the
 * guest's whole context: Chrome drew nothing for the rest of its life.
 * The draws must be accepted, the context must keep working, and an occlusion
 * query must count every fragment of the viewport, with the depth test on
 * (there is no depth buffer, so every fragment passes). The stand-in for
 * "no attachments" must be reused while the guest switches back and forth,
 * also above 2048x2048, and freed when it has not been used for a while. */
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <OpenGL/gl3.h>
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"
#define CGL_CONTEXT_RENDERER_CALLBACKS
#include "cgl-context.h"

/* Gallium values the protocol uses (pipe/p_defines.h needs the whole tree). */
enum { TEST_SHADER_VERTEX = 0, TEST_SHADER_FRAGMENT = 1, TEST_PRIM_TRIANGLES = 4,
       TEST_BUFFER = 0, TEST_TEXTURE_2D = 2, TEST_CLEAR_COLOR0 = 1 << 2,
       TEST_FUNC_LESS = 1, TEST_QUERY_OCCLUSION_COUNTER = 0 };

static int failures;

static void check(int ok, const char *what)
{
   printf("%s: %s\n", ok ? "ok" : "FAIL", what);
   failures += !ok;
}

struct cmds {
   uint32_t dw[1024];
   unsigned n;
};

static void emit(struct cmds *c, uint32_t v)
{
   c->dw[c->n++] = v;
}

static void emit_float(struct cmds *c, float f)
{
   union { float f; uint32_t u; } v = { f };
   emit(c, v.u);
}

static void emit_shader(struct cmds *c, uint32_t handle, uint32_t type, const char *text)
{
   uint32_t bytes = strlen(text) + 1, words = (bytes + 3) / 4;
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + words));
   emit(c, handle);
   emit(c, type);
   emit(c, VIRGL_OBJ_SHADER_OFFSET_VAL(bytes));
   emit(c, 300);
   emit(c, 0);
   memset(&c->dw[c->n], 0, words * 4);
   memcpy(&c->dw[c->n], text, bytes);
   c->n += words;
   emit(c, VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER, 0, 2));
   emit(c, handle);
   emit(c, type);
}

static void emit_draw(struct cmds *c)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, VIRGL_DRAW_VBO_SIZE));
   emit(c, 0);                      /* start */
   emit(c, 3);                      /* count */
   emit(c, TEST_PRIM_TRIANGLES);
   for (int i = 0; i < VIRGL_DRAW_VBO_SIZE - 3; i++)
      emit(c, 0);
}

/* nr_cbufs colour buffers (0 = none), no depth buffer */
static void emit_framebuffer(struct cmds *c, uint32_t nr_cbufs, uint32_t surface)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0,
                      VIRGL_SET_FRAMEBUFFER_STATE_SIZE(nr_cbufs)));
   emit(c, nr_cbufs);
   emit(c, 0);
   for (uint32_t i = 0; i < nr_cbufs; i++)
      emit(c, surface);
}

static void emit_viewport(struct cmds *c, float width, float height)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, VIRGL_SET_VIEWPORT_STATE_SIZE(1)));
   emit(c, 0);                      /* start slot */
   emit_float(c, width / 2);
   emit_float(c, height / 2);
   emit_float(c, 0.5f);
   emit_float(c, width / 2);
   emit_float(c, height / 2);
   emit_float(c, 0.5f);
}

static void emit_query(struct cmds *c, uint32_t cmd, uint32_t handle)
{
   emit(c, VIRGL_CMD0(cmd, 0, 1));
   emit(c, handle);
}

static void emit_clear_red(struct cmds *c)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE));
   emit(c, TEST_CLEAR_COLOR0);
   emit_float(c, 1.0f);
   emit_float(c, 0.0f);
   emit_float(c, 0.0f);
   emit_float(c, 1.0f);
   emit(c, 0);                      /* depth (double) */
   emit(c, 0);
   emit(c, 0);                      /* stencil */
}

/* A result that is not ready when asked for is written when a later fence of
 * the context retires (QEMU makes one per guest submit). */
static void wait_for_query(volatile struct virgl_host_query_state *state)
{
   static uint32_t fence_id;
   for (int i = 0; i < 2000 && state->query_state != VIRGL_QUERY_STATE_DONE; i++) {
      virgl_renderer_create_fence(++fence_id, 1);
      virgl_renderer_poll();
      usleep(1000);
   }
}

static int submit(int ctx_id, struct cmds *c)
{
   int r = virgl_renderer_submit_cmd(c->dw, ctx_id, c->n);
   c->n = 0;
   return r;
}

/* The size of the renderer's stand-in for "no attachments", read from GL:
 * the depth renderbuffer of the framebuffer the last submit left bound (its
 * GL context is still current). A new stand-in starts at 1x1 and only grows
 * with the viewports of the draws that use it, so its size tells a reused one
 * from a new one. 0x0 when there is none. */
static void stand_in_size(GLint *width, GLint *height)
{
   GLint type = GL_NONE, name = 0;
   *width = *height = 0;
   glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
   if (type != GL_RENDERBUFFER)
      return;
   glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
   glBindRenderbuffer(GL_RENDERBUFFER, name);
   glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, width);
   glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, height);
   glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

static void check_stand_in(int width, int height, const char *what)
{
   GLint w, h;
   char text[160];
   stand_in_size(&w, &h);
   snprintf(text, sizeof(text), "%s: stand-in %dx%d (expected %dx%d)", what, w, h, width, height);
   check(w == width && h == height, text);
}

/* No attachments, viewport width x height, one draw counted by query
 * (handle), the result asked for. */
static void emit_counted_draw(struct cmds *c, float width, float height, uint32_t query)
{
   emit_framebuffer(c, 0, 0);
   emit_viewport(c, width, height);
   emit_query(c, VIRGL_CCMD_BEGIN_QUERY, query);
   emit_draw(c);
   emit_query(c, VIRGL_CCMD_END_QUERY, query);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_GET_QUERY_RESULT, 0, 2));
   emit(c, query);
   emit(c, 1);                      /* wait */
}

/* count framebuffer changes with the colour buffer and draws draws */
static int submit_colour_work(int ctx_id, struct cmds *c, int changes, int draws)
{
   int r = 0;
   for (int i = 0; i < changes || i < draws; i++) {
      if (i < changes)
         emit_framebuffer(c, 1, 6);
      if (i < draws)
         emit_draw(c);
      if (c->n > 1000 - 32)
         r |= submit(ctx_id, c);
   }
   return r | submit(ctx_id, c);
}

/* A triangle that covers the whole viewport, from gl_VertexID alone:
 * (-1,-1), (3,-1), (-1,3). */
static const char *vs_text =
   "VERT\n"
   "DCL SV[0], VERTEXID\n"
   "DCL OUT[0], POSITION\n"
   "DCL TEMP[0]\n"
   "IMM[0] UINT32 {1, 2, 0, 0}\n"
   "IMM[1] FLT32 {    4.0000,    -1.0000,     0.5000,     1.0000}\n"
   "  0: AND TEMP[0].x, SV[0].xxxx, IMM[0].xxxx\n"
   "  1: AND TEMP[0].y, SV[0].xxxx, IMM[0].yyyy\n"
   "  2: USHR TEMP[0].y, TEMP[0].yyyy, IMM[0].xxxx\n"
   "  3: U2F TEMP[0].xy, TEMP[0].xyyy\n"
   "  4: MAD OUT[0].xy, TEMP[0].xyyy, IMM[1].xxxx, IMM[1].yyyy\n"
   "  5: MOV OUT[0].zw, IMM[1].zzzw\n"
   "  6: END\n";

static const char *fs_text =
   "FRAG\n"
   "DCL OUT[0], COLOR\n"
   "IMM[0] FLT32 {    1.0000,     0.0000,     0.0000,     1.0000}\n"
   "  0: MOV OUT[0], IMM[0]\n"
   "  1: END\n";

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   if (!cgl_init_renderer_main()) {
      printf("skip: no OpenGL context on this Mac\n");
      return 0;
   }
   static int cookie;
   if (virgl_renderer_init(&cookie, 0, &cgl_renderer_callbacks)) {
      printf("FAIL: virgl_renderer_init\n");
      return 1;
   }
   check(!virgl_renderer_context_create(1, 6, "chrome"), "context");

   /* a colour buffer, to switch to and from the empty framebuffer */
   struct virgl_renderer_resource_create_args args = {
      .handle = 5, .target = TEST_TEXTURE_2D, .format = VIRGL_FORMAT_R8G8B8A8_UNORM,
      .bind = VIRGL_BIND_RENDER_TARGET, .width = 16, .height = 16, .depth = 1,
      .array_size = 1,
   };
   check(!virgl_renderer_resource_create(&args, NULL, 0), "colour buffer");
   virgl_renderer_ctx_attach_resource(1, 5);

   /* where the occlusion query results go: guest memory, no GL object */
   enum { QUERIES = 6 };
   struct virgl_host_query_state query_result[QUERIES];
   memset(query_result, 0, sizeof(query_result));
   struct iovec query_iov[QUERIES];
   for (uint32_t i = 0; i < QUERIES; i++) {
      query_iov[i].iov_base = &query_result[i];
      query_iov[i].iov_len = sizeof(query_result[i]);
      struct virgl_renderer_resource_create_args qargs = {
         .handle = 7 + i, .target = TEST_BUFFER, .format = VIRGL_FORMAT_R8_UNORM,
         .bind = VIRGL_BIND_CUSTOM, .width = sizeof(query_result[0]), .height = 1,
         .depth = 1, .array_size = 1,
      };
      check(!virgl_renderer_resource_create(&qargs, NULL, 0) &&
            !virgl_renderer_resource_attach_iov(7 + i, &query_iov[i], 1),
            "query buffer");
      virgl_renderer_ctx_attach_resource(1, 7 + i);
   }

   struct cmds c = { .n = 0 };
   emit(&c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE));
   emit(&c, 6);                     /* surface handle */
   emit(&c, 5);                     /* resource */
   emit(&c, VIRGL_FORMAT_R8G8B8A8_UNORM);
   emit(&c, 0);                     /* level */
   emit(&c, 0);                     /* layers */
   emit_shader(&c, 10, TEST_SHADER_VERTEX, vs_text);
   emit_shader(&c, 11, TEST_SHADER_FRAGMENT, fs_text);
   check(submit(1, &c) == 0, "shaders and surface");

   emit_framebuffer(&c, 0, 0);
   emit_draw(&c);
   emit_draw(&c);
   check(submit(1, &c) == 0, "draws into a framebuffer without attachments are accepted");

   emit_framebuffer(&c, 1, 6);
   emit_draw(&c);
   emit_framebuffer(&c, 0, 0);
   emit_draw(&c);
   emit_framebuffer(&c, 1, 6);
   emit_draw(&c);
   check(submit(1, &c) == 0, "switching between a colour buffer and none keeps working");

   /* Back on the colour buffer, nothing of the stand-in for "no attachments"
    * may remain: a clear covers the whole 16x16 buffer. */
   emit_clear_red(&c);
   check(submit(1, &c) == 0, "clear");
   uint32_t pixels[16 * 16] = {0};
   struct iovec iov = { pixels, sizeof(pixels) };
   struct virgl_box box = { 0, 0, 0, 16, 16, 1 };
   check(!virgl_renderer_transfer_read_iov(5, 1, 0, 16 * 4, 0, &box, 0, &iov, 1) &&
         pixels[0] == 0xff0000ff && pixels[16 * 16 - 1] == 0xff0000ff,
         "the colour buffer is cleared corner to corner");

   /* No attachments, a 64x48 viewport, depth test on (less, with writes):
    * each of two draws over the whole viewport passes 64 * 48 samples. With a
    * 1x1 stand-in or a depth test against it, they counted 1 and 0. */
   emit(&c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, VIRGL_OBJ_DSA_SIZE));
   emit(&c, 20);
   emit(&c, VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(1) | VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(1) |
            VIRGL_OBJ_DSA_S0_DEPTH_FUNC(TEST_FUNC_LESS));
   emit(&c, 0);
   emit(&c, 0);
   emit(&c, 0);
   emit(&c, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1));
   emit(&c, 20);
   for (uint32_t i = 0; i < QUERIES; i++) {
      emit(&c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_QUERY, VIRGL_OBJ_QUERY_SIZE));
      emit(&c, 30 + i);
      emit(&c, TEST_QUERY_OCCLUSION_COUNTER);
      emit(&c, 0);                  /* offset */
      emit(&c, 7 + i);              /* result buffer */
   }
   emit_framebuffer(&c, 0, 0);
   emit_viewport(&c, 64, 48);
   for (uint32_t i = 0; i < 2; i++) {
      emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 30 + i);
      emit_draw(&c);
      emit_query(&c, VIRGL_CCMD_END_QUERY, 30 + i);
   }
   for (uint32_t i = 0; i < 2; i++) {
      emit(&c, VIRGL_CMD0(VIRGL_CCMD_GET_QUERY_RESULT, 0, 2));
      emit(&c, 30 + i);
      emit(&c, 1);                  /* wait */
   }
   check(submit(1, &c) == 0, "occlusion queries without attachments are accepted");
   char what[96];
   for (uint32_t i = 0; i < 2; i++) {
      wait_for_query(&query_result[i]);
      snprintf(what, sizeof(what), "draw %u counts every sample of the 64x48 viewport: %llu", i + 1,
               (unsigned long long)query_result[i].result);
      check(query_result[i].query_state == VIRGL_QUERY_STATE_DONE &&
            query_result[i].result == 64 * 48, what);
   }

   emit_framebuffer(&c, 1, 6);
   emit_viewport(&c, 16, 16);
   emit_clear_red(&c);
   emit_draw(&c);
   check(submit(1, &c) == 0, "the colour buffer works again after the queries");

   /* A stand-in larger than 2048x2048 (a Retina-size window): it is made
    * for the draw and reused while the guest switches between "no
    * attachments" and a colour buffer: after ten round trips with smaller
    * viewports it still has the first draw's size (a new one would be 32x16). */
   emit_counted_draw(&c, 4096, 1100, 32);
   check(submit(1, &c) == 0, "a 4096x1100 draw without attachments");
   check_stand_in(4096, 1100, "made at the size of the draw");
   for (int i = 0; i < 10; i++) {
      emit_framebuffer(&c, 1, 6);
      emit_viewport(&c, 16, 16);
      emit_draw(&c);
      emit_framebuffer(&c, 0, 0);
      emit_viewport(&c, 32, 16);
      emit_draw(&c);
   }
   check(submit(1, &c) == 0, "ten switches between the colour buffer and none");
   check_stand_in(4096, 1100, "reused across switches");
   wait_for_query(&query_result[2]);
   snprintf(what, sizeof(what), "the large stand-in counts every sample of the 4096x1100 viewport: %llu",
            (unsigned long long)query_result[2].result);
   check(query_result[2].query_state == VIRGL_QUERY_STATE_DONE &&
         query_result[2].result == 4096 * 1100, what);

   /* 70 framebuffer changes without it (VREND_FB_PLACEHOLDER_IDLE = 64) free
    * it; the next draw without attachments makes a new one at its own size. */
   check(submit_colour_work(1, &c, 70, 0) == 0, "70 framebuffer changes with the colour buffer");
   emit_counted_draw(&c, 32, 16, 33);
   check(submit(1, &c) == 0, "no attachments again");
   check_stand_in(32, 16, "freed after 70 changes, a new one");
   wait_for_query(&query_result[3]);
   snprintf(what, sizeof(what), "the new stand-in counts every sample of the 32x16 viewport: %llu",
            (unsigned long long)query_result[3].result);
   check(query_result[3].query_state == VIRGL_QUERY_STATE_DONE && query_result[3].result == 32 * 16,
         what);

   /* Draws without it (VREND_FB_PLACEHOLDER_IDLE_DRAWS = 4096): 1000 keep it,
    * 4200 free it. */
   check(submit_colour_work(1, &c, 1, 1000) == 0, "1000 draws into the colour buffer");
   emit_counted_draw(&c, 16, 8, 34);
   check(submit(1, &c) == 0, "no attachments after 1000 draws");
   check_stand_in(32, 16, "kept after 1000 draws without it");
   check(submit_colour_work(1, &c, 1, 4200) == 0, "4200 draws into the colour buffer");
   emit_counted_draw(&c, 16, 8, 35);
   check(submit(1, &c) == 0, "no attachments after 4200 draws");
   check_stand_in(16, 8, "freed after 4200 draws without it, a new one");
   for (uint32_t i = 4; i < 6; i++) {
      wait_for_query(&query_result[i]);
      snprintf(what, sizeof(what), "16x8 viewport, query %u: %llu", i,
               (unsigned long long)query_result[i].result);
      check(query_result[i].query_state == VIRGL_QUERY_STATE_DONE &&
            query_result[i].result == 16 * 8, what);
   }

   emit_framebuffer(&c, 1, 6);
   check(submit(1, &c) == 0, "the colour buffer again");
   check_stand_in(0, 0, "detached from the colour buffer");

   for (uint32_t i = 0; i < QUERIES; i++) {
      virgl_renderer_ctx_detach_resource(1, 7 + i);
      virgl_renderer_resource_unref(7 + i);
   }
   virgl_renderer_ctx_detach_resource(1, 5);
   virgl_renderer_context_destroy(1);
   virgl_renderer_resource_unref(5);
   virgl_renderer_cleanup(&cookie);
   printf("%s\n", failures ? "empty framebuffer: FAILED" : "empty framebuffer: all checks passed");
   return failures != 0;
}
