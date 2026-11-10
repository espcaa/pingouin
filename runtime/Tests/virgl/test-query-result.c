/* Query results the GL does not answer (virgl-query-result-unanswered.patch), through the
 * public renderer API on Apple's software renderer (soft-gl.h).
 * vrend read a query's state and result into stack variables the GL writes only when it
 * answers, then copied them into the guest's result buffer. A guest can make the GL
 * refuse (GET_QUERY_RESULT for a query never begun, or still running); so can a GL that
 * refuses calls of a context (#547). Before the fix the guest got host stack bytes as its
 * result, or "not ready" for ever (the guest's Mesa waits until the query is done).
 *  - never begun (4-byte and 8-byte results), or still running: done, result 0. The
 *    stack is filled with 'X' or 0 first; on this toolchain vrend's own calls leave 0
 *    where the state goes, so before the fix the guest waited for ever;
 *  - one log line for all of them;
 *  - available, but the result refused (query-result-gl.c, linked in): done, result 0
 *    (before the fix: what the stack held, 9 here);
 *  - the GL answering: the result is the GL's (every sample of a 16x16 draw), also for
 *    a query that was not answered before;
 *  - a query the GL has not finished ("not available" for the first polls) still waits,
 *    then gets the GL's answer: only an unanswered query reads as done with 0;
 *  - a query still waiting when its context is lost: done with 0. vrend took it off its
 *    waiting list without writing it, so the guest waited for ever. */
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "soft-gl.h"
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"

/* Gallium values the protocol uses (pipe/p_defines.h needs the whole tree). */
enum { TEST_SHADER_VERTEX = 0, TEST_SHADER_FRAGMENT = 1, TEST_PRIM_TRIANGLES = 4,
       TEST_BUFFER = 0, TEST_TEXTURE_2D = 2,
       TEST_QUERY_OCCLUSION_COUNTER = 0, TEST_QUERY_TIME_ELAPSED = 4 };
/* what the guest's buffer holds before the host answers */
#define GUEST_MARK 0xa5a5a5a5a5a5a5a5ull

void query_result_refuse(int on);
unsigned long query_result_refused(void);
void query_result_pending(long n);
long query_result_pending_left(void);

static CGLContextObj main_ctx;
static int failures, unanswered_lines, lost_lines;

static void count_log(enum virgl_log_level_flags level, const char *message, void *data)
{
   (void)level;
   (void)data;
   unanswered_lines += strstr(message, "query result: the GL did not answer (GL query ") != NULL;
   lost_lines += strstr(message, "context error reported 2 ") != NULL;
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

static void emit_query(struct cmds *c, uint32_t cmd, uint32_t handle)
{
   emit(c, VIRGL_CMD0(cmd, 0, 1));
   emit(c, handle);
}

static void emit_create_query(struct cmds *c, uint32_t handle, uint32_t type, uint32_t res)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_QUERY, VIRGL_OBJ_QUERY_SIZE));
   emit(c, handle);
   emit(c, type);
   emit(c, 0);                      /* offset */
   emit(c, res);                    /* result buffer */
}

static void emit_get_result(struct cmds *c, uint32_t handle)
{
   emit(c, VIRGL_CMD0(VIRGL_CCMD_GET_QUERY_RESULT, 0, VIRGL_QUERY_RESULT_SIZE));
   emit(c, handle);
   emit(c, 1);                      /* wait */
}

/* surface on resource 5, framebuffer, 16x16 viewport, blend, rasterizer, shaders */
static void emit_setup(struct cmds *c)
{
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
      "IMM[0] FLT32 {    0.0000,     1.0000,     0.0000,     1.0000}\n"
      "  0: MOV OUT[0], IMM[0]\n"
      "  1: END\n";
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE));
   emit(c, 6);
   emit(c, 5);
   emit(c, VIRGL_FORMAT_R8G8B8A8_UNORM);
   emit(c, 0);
   emit(c, 0);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)));
   emit(c, 1);
   emit(c, 0);
   emit(c, 6);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, VIRGL_SET_VIEWPORT_STATE_SIZE(1)));
   emit(c, 0);
   emit_float(c, 8);
   emit_float(c, 8);
   emit_float(c, 0.5f);
   emit_float(c, 8);
   emit_float(c, 8);
   emit_float(c, 0.5f);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, VIRGL_OBJ_BLEND_SIZE));
   emit(c, 40);
   emit(c, 0);
   emit(c, 0);
   for (int i = 0; i < VIRGL_MAX_COLOR_BUFS; i++)
      emit(c, VIRGL_OBJ_BLEND_S2_RT_COLORMASK(0xf));
   emit(c, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1));
   emit(c, 40);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, VIRGL_OBJ_RS_SIZE));
   emit(c, 41);
   emit(c, VIRGL_OBJ_RS_S0_DEPTH_CLIP(1));
   for (int i = 0; i < VIRGL_OBJ_RS_SIZE - 2; i++)
      emit(c, 0);
   emit(c, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1));
   emit(c, 41);
   emit_shader(c, 10, TEST_SHADER_VERTEX, vs_text);
   emit_shader(c, 11, TEST_SHADER_FRAGMENT, fs_text);
}

static int submit(int ctx_id, struct cmds *c)
{
   int r = virgl_renderer_submit_cmd(c->dw, ctx_id, c->n);
   c->n = 0;
   return r;
}

/* Fill the stack the renderer's calls will use next with one byte value: an unwritten
 * local then holds it, so a result made of it shows that it was never set. */
__attribute__((noinline)) static void fill_stack(int byte)
{
   char junk[256 * 1024];
   memset(junk, byte, sizeof(junk));
   __asm__ volatile("" : : "r"(junk) : "memory");
}

__attribute__((noinline)) static int submit_on_filled_stack(struct cmds *c, int byte)
{
   fill_stack(byte);
   return submit(1, c);
}

enum { QUERIES = 4 };
static struct virgl_host_query_state results[QUERIES];

static void guest_waits(int i)
{
   results[i].query_state = VIRGL_QUERY_STATE_WAIT_HOST;
   results[i].result_size = 0;
   results[i].result = GUEST_MARK;
}

static void wait_for(volatile struct virgl_host_query_state *state, int polls)
{
   static uint32_t fence_id;
   for (int n = 0; n < polls && state->query_state != VIRGL_QUERY_STATE_DONE; n++) {
      virgl_renderer_create_fence(++fence_id, 1);
      virgl_renderer_poll();
      usleep(1000);
   }
}

static void wait_for_query(int i)
{
   wait_for(&results[i], 2000);
}

/* GET_QUERY_RESULT for query 30 + i on a stack filled with byte: done, result 0 */
static void unanswered_row(struct cmds *c, int i, int byte, const char *what)
{
   char text[200];
   guest_waits(i);
   emit_get_result(c, 30 + i);
   int r = submit_on_filled_stack(c, byte);
   snprintf(text, sizeof(text), "%s, stack %s: the submit runs", what, byte ? "'X'" : "0");
   check(r == 0, text);
   snprintf(text, sizeof(text), "%s, stack %s: done (state %u), not a wait for ever", what,
            byte ? "'X'" : "0", results[i].query_state);
   check(results[i].query_state == VIRGL_QUERY_STATE_DONE, text);
   snprintf(text, sizeof(text), "%s, stack %s: result 0 (0x%llx)", what,
            byte ? "'X'" : "0", (unsigned long long)results[i].result);
   check(results[i].result == 0, text);
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
   check(!virgl_renderer_context_create(1, 6, "queries"), "context");
   struct virgl_renderer_resource_create_args args = {
      .handle = 5, .target = TEST_TEXTURE_2D, .format = VIRGL_FORMAT_R8G8B8A8_UNORM,
      .bind = VIRGL_BIND_RENDER_TARGET, .width = 16, .height = 16, .depth = 1,
      .array_size = 1,
   };
   check(!virgl_renderer_resource_create(&args, NULL, 0), "colour buffer");
   virgl_renderer_ctx_attach_resource(1, 5);

   /* where the query results go: guest memory, as in QEMU */
   struct iovec iov[QUERIES];
   for (int i = 0; i < QUERIES; i++) {
      iov[i].iov_base = &results[i];
      iov[i].iov_len = sizeof(results[i]);
      struct virgl_renderer_resource_create_args qargs = {
         .handle = 7 + i, .target = TEST_BUFFER, .format = VIRGL_FORMAT_R8_UNORM,
         .bind = VIRGL_BIND_CUSTOM, .width = sizeof(results[i]), .height = 1,
         .depth = 1, .array_size = 1,
      };
      check(!virgl_renderer_resource_create(&qargs, NULL, 0) &&
            !virgl_renderer_resource_attach_iov(7 + i, &iov[i], 1), "query buffer");
      virgl_renderer_ctx_attach_resource(1, 7 + i);
   }

   struct cmds c = { .n = 0 };
   emit_setup(&c);
   emit_create_query(&c, 30, TEST_QUERY_OCCLUSION_COUNTER, 7);
   emit_create_query(&c, 31, TEST_QUERY_OCCLUSION_COUNTER, 8);
   emit_create_query(&c, 32, TEST_QUERY_TIME_ELAPSED, 9);
   emit_create_query(&c, 33, TEST_QUERY_OCCLUSION_COUNTER, 10);
   check(submit(1, &c) == 0, "setup and queries");

   /* The GL refuses: a query never begun (4 and 8 bytes), one still running. */
   unanswered_lines = 0;
   unanswered_row(&c, 0, 'X', "occlusion query never begun");
   unanswered_row(&c, 1, 0, "occlusion query never begun");
   unanswered_row(&c, 2, 'X', "time query never begun (8 bytes)");
   emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 33);
   check(submit(1, &c) == 0, "begin a query");
   unanswered_row(&c, 3, 'X', "occlusion query still running");
   emit_query(&c, VIRGL_CCMD_END_QUERY, 33);
   check(submit(1, &c) == 0, "end it");
   char text[120];
   snprintf(text, sizeof(text), "one log line for all of them (%d)", unanswered_lines);
   check(unanswered_lines == 1, text);

   /* Available, but the GL refuses the result itself (4 and 8 bytes). */
   for (int i = 1; i <= 2; i++) {
      emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 30 + i);
      emit_draw(&c);
      emit_query(&c, VIRGL_CCMD_END_QUERY, 30 + i);
      check(submit(1, &c) == 0, "a draw inside a query");
      unsigned long refused = query_result_refused();
      guest_waits(i);
      emit_get_result(&c, 30 + i);
      query_result_refuse(1);
      int r = submit(1, &c);
      wait_for_query(i);
      query_result_refuse(0);
      snprintf(text, sizeof(text), "%s result refused: done with 0, not stack bytes "
               "(state %u, 0x%llx)", i == 1 ? "4-byte" : "8-byte", results[i].query_state,
               (unsigned long long)results[i].result);
      check(r == 0 && query_result_refused() > refused &&
            results[i].query_state == VIRGL_QUERY_STATE_DONE && results[i].result == 0, text);
   }

   /* The GL answers: every sample of the 16x16 draw, also for query 30 (not answered
    * before) and the one that was still running. */
   for (int i = 0; i < QUERIES; i += 3) {
      guest_waits(i);
      emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 30 + i);
      emit_draw(&c);
      emit_query(&c, VIRGL_CCMD_END_QUERY, 30 + i);
      emit_get_result(&c, 30 + i);
      check(submit(1, &c) == 0, "a draw inside a query");
      wait_for_query(i);
      snprintf(text, sizeof(text), "query %d answered: done with the GL's 256 samples (%llu)",
               30 + i, (unsigned long long)results[i].result);
      check(results[i].query_state == VIRGL_QUERY_STATE_DONE && results[i].result == 256, text);
   }

   /* Not finished yet: the GL answers "not available" for the first 3 polls. The query
    * must wait (a fix that made it done with 0 here would fail), then get the 256. */
   guest_waits(0);
   emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 30);
   emit_draw(&c);
   emit_query(&c, VIRGL_CCMD_END_QUERY, 30);
   emit_get_result(&c, 30);
   query_result_pending(3);
   check(submit(1, &c) == 0, "a draw inside a query");
   snprintf(text, sizeof(text), "query not finished yet: still waiting after the submit "
            "(state %u, 0x%llx)", results[0].query_state, (unsigned long long)results[0].result);
   check(results[0].query_state == VIRGL_QUERY_STATE_WAIT_HOST && results[0].result == GUEST_MARK,
         text);
   wait_for_query(0);
   snprintf(text, sizeof(text), "query not finished yet: done with the GL's 256 samples once "
            "it is (state %u, %llu, %ld polls left)", results[0].query_state,
            (unsigned long long)results[0].result, query_result_pending_left());
   check(results[0].query_state == VIRGL_QUERY_STATE_DONE && results[0].result == 256 &&
         query_result_pending_left() == 0, text);
   query_result_pending(0);

   /* Context 2 has a query the GL has not finished when its command stream breaks (the
    * context is lost). vrend then takes it off its waiting list: it must be done with 0,
    * or the guest's Mesa waits for it for ever. */
   static struct virgl_host_query_state lost;
   struct iovec lost_iov = { .iov_base = &lost, .iov_len = sizeof(lost) };
   struct virgl_renderer_resource_create_args largs = {
      .handle = 20, .target = TEST_BUFFER, .format = VIRGL_FORMAT_R8_UNORM,
      .bind = VIRGL_BIND_CUSTOM, .width = sizeof(lost), .height = 1, .depth = 1,
      .array_size = 1,
   };
   check(!virgl_renderer_context_create(2, 4, "lost") &&
         !virgl_renderer_resource_create(&largs, NULL, 0) &&
         !virgl_renderer_resource_attach_iov(20, &lost_iov, 1), "a second context");
   virgl_renderer_ctx_attach_resource(2, 20);
   lost.query_state = VIRGL_QUERY_STATE_WAIT_HOST;
   lost.result = GUEST_MARK;
   emit_create_query(&c, 50, TEST_QUERY_OCCLUSION_COUNTER, 20);
   emit_query(&c, VIRGL_CCMD_BEGIN_QUERY, 50);
   emit_query(&c, VIRGL_CCMD_END_QUERY, 50);
   emit_get_result(&c, 50);
   query_result_pending(-1);
   check(submit(2, &c) == 0 && lost.query_state == VIRGL_QUERY_STATE_WAIT_HOST,
         "a query of context 2 waits for the GL");
   emit(&c, VIRGL_CMD0(VIRGL_CCMD_BEGIN_QUERY, 0, 9));   /* 9 words announced, none sent */
   submit(2, &c);
   snprintf(text, sizeof(text), "context 2 is lost (%d log lines)", lost_lines);
   check(lost_lines > 0, text);
   wait_for(&lost, 200);
   snprintf(text, sizeof(text), "its query: done with 0, not a wait for ever (state %u, 0x%llx)",
            lost.query_state, (unsigned long long)lost.result);
   check(lost.query_state == VIRGL_QUERY_STATE_DONE && lost.result == 0, text);
   query_result_pending(0);
   virgl_renderer_ctx_detach_resource(2, 20);
   virgl_renderer_context_destroy(2);
   virgl_renderer_resource_unref(20);

   for (int i = 0; i < QUERIES; i++) {
      virgl_renderer_ctx_detach_resource(1, 7 + i);
      virgl_renderer_resource_unref(7 + i);
   }
   virgl_renderer_ctx_detach_resource(1, 5);
   virgl_renderer_context_destroy(1);
   virgl_renderer_resource_unref(5);
   virgl_renderer_cleanup(&cookie);
   printf("%s\n", failures ? "query result: FAILED" : "query result: all checks passed");
   return failures != 0;
}
