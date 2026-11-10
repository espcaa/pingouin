/* GL stand-in for test-query-result: a dylib whose __interpose section replaces
 * glGetQueryObjectuiv and glGetQueryObjectui64v (also where libepoxy looks them up with
 * dlsym), so the test can
 *  - make the GL refuse a query's result while it answers that the result is available,
 *    as a GL that refuses calls of a context can (#547): the call goes to the GL for a
 *    name that is no query (GL_INVALID_OPERATION), nothing is written;
 *  - answer "not available yet" (0) for the next polls, as for a query the GPU has not
 *    finished.
 * Test-only; never shipped. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>

static int refuse_result;
static unsigned long refused;
static long pending_polls;

void query_result_refuse(int on)
{
   refuse_result = on;
}

unsigned long query_result_refused(void)
{
   return refused;
}

/* the next n "available?" questions get 0; -1: all of them until set again */
void query_result_pending(long n)
{
   pending_polls = n;
}

long query_result_pending_left(void)
{
   return pending_polls;
}

static void stand_in_GetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params)
{
   if (pending_polls && pname == GL_QUERY_RESULT_AVAILABLE) {
      if (pending_polls > 0)
         pending_polls--;
      *params = 0;
      return;
   }
   if (refuse_result && pname == GL_QUERY_RESULT) {
      refused++;
      id = 0x7fffffff;
   }
   glGetQueryObjectuiv(id, pname, params);
}

static void stand_in_GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params)
{
   if (refuse_result && pname == GL_QUERY_RESULT) {
      refused++;
      id = 0x7fffffff;
   }
   glGetQueryObjectui64v(id, pname, params);
}

__attribute__((used)) static const struct {
   const void *replacement;
   const void *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
   { (const void *)stand_in_GetQueryObjectuiv, (const void *)glGetQueryObjectuiv },
   { (const void *)stand_in_GetQueryObjectui64v, (const void *)glGetQueryObjectui64v },
};
