/* The sampler limit the renderer reports to the guest must not exceed the
 * host's for any shader stage. The guest's Mesa takes
 * caps.v2.max_texture_samplers as every stage's limit
 * (GL_MAX_TEXTURE_IMAGE_UNITS). It was always 32; the Mac has 16 per stage,
 * so a guest program with more samplers (dEQP-GLES3 uses the reported limit)
 * failed to link on the Mac and stopped the guest context.
 * Public renderer API on the Mac's own OpenGL (CGL core context, no window). */
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include "virglrenderer.h"
#include "virgl_hw.h"
#define CGL_CONTEXT_RENDERER_CALLBACKS
#include "cgl-context.h"

int main(void)
{
   if (!cgl_init_renderer_main()) {
      printf("skip: no OpenGL context on this Mac\n");
      return 0;
   }
   /* every stage the Mac's core profile has (no compute in GL 4.1) */
   static const struct {
      GLenum limit;
      const char *stage;
   } stages[] = {
      {GL_MAX_TEXTURE_IMAGE_UNITS, "fragment"},
      {GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, "vertex"},
      {GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS, "geometry"},
      {GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS, "tessellation control"},
      {GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS, "tessellation evaluation"},
   };
   GLint host[sizeof(stages) / sizeof(stages[0])];
   for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
      host[i] = 0;
      glGetIntegerv(stages[i].limit, &host[i]);
   }

   static int cookie;
   if (virgl_renderer_init(&cookie, 0, &cgl_renderer_callbacks)) {
      printf("FAIL: virgl_renderer_init\n");
      return 1;
   }
   uint32_t max_ver = 0, max_size = 0;
   virgl_renderer_get_cap_set(2 /* VIRTIO_GPU_CAPSET_VIRGL2 */, &max_ver, &max_size);
   union virgl_caps *caps = calloc(1, max_size > sizeof(*caps) ? max_size : sizeof(*caps));
   virgl_renderer_fill_caps(2 /* VIRTIO_GPU_CAPSET_VIRGL2 */, max_ver, caps);
   uint32_t reported = caps->v2.max_texture_samplers;
   virgl_renderer_cleanup(&cookie);
   free(caps);

   int failures = reported < 16;
   printf("%s: samplers per stage: reported %u (GLES 3.0 needs 16)\n",
          reported >= 16 ? "ok" : "FAIL", reported);
   for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
      int ok = host[i] <= 0 || (int)reported <= host[i];
      printf("%s: %s stage: host %d\n", ok ? "ok" : "FAIL", stages[i].stage, host[i]);
      failures += !ok;
   }
   printf("%s\n", failures ? "sampler limit: FAILED" : "sampler limit: all checks passed");
   return failures != 0;
}
