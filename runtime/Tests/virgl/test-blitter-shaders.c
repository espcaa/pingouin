/* The renderer's own blit shaders must compile on the Mac's core profile.
 * They were "#version 130", which Apple's OpenGL refuses: every QEMU start
 * logged the failure and blits that need a shader (format conversion,
 * swizzle, multisample resolve to another format, depth) drew nothing.
 * Builds the blitter's real shaders (vrend_blitter.c is compiled into this
 * test for its static functions) in a CGL core profile context, no window. */
#include "vrend/vrend_blitter.c"
#include "cgl-context.h"

static int link_with_vs(const char *name, GLuint vs, GLuint fs)
{
   if (!fs) {
      printf("FAIL: %s does not compile (log above)\n", name);
      return 1;
   }
   GLuint prog = glCreateProgram();
   GLint ok = 0;
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glLinkProgram(prog);
   glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   glDeleteProgram(prog);
   glDeleteShader(fs);
   if (!ok) {
      printf("FAIL: %s does not link\n", name);
      return 1;
   }
   printf("PASS: %s\n", name);
   return 0;
}

int main(void)
{
   if (!cgl_init_current()) {
      puts("SKIP: no OpenGL context here");
      return 0;
   }
   struct vrend_blitter_ctx blit = {0};
   blit.use_gles = !epoxy_is_desktop_gl();
   blit_set_glsl_version(&blit, vrend_renderer_get_glsl_version());
   printf("blit shaders: %s", blit.glsl_header);

   blit.vs = blit_shader_build_and_check(&blit, GL_VERTEX_SHADER, VS_PASSTHROUGH_GL);
   if (!blit.vs) {
      puts("FAIL: blit vertex shader does not compile");
      return 1;
   }
   puts("PASS: blit vertex shader");

   static const struct {
      const char *name;
      enum tgsi_texture_type target;
      int samples;
   } targets[] = {
      {"1D", TGSI_TEXTURE_1D, 0},
      {"2D", TGSI_TEXTURE_2D, 0},
      {"RECT", TGSI_TEXTURE_RECT, 0},
      {"3D", TGSI_TEXTURE_3D, 0},
      {"CUBE", TGSI_TEXTURE_CUBE, 0},
      {"1D_ARRAY", TGSI_TEXTURE_1D_ARRAY, 0},
      {"2D_ARRAY", TGSI_TEXTURE_2D_ARRAY, 0},
      {"CUBE_ARRAY", TGSI_TEXTURE_CUBE_ARRAY, 0},
      {"2D_MSAA", TGSI_TEXTURE_2D_MSAA, 4},
      {"2D_ARRAY_MSAA", TGSI_TEXTURE_2D_ARRAY_MSAA, 4},
   };
   static const struct {
      const char *name;
      enum tgsi_return_type ret;
   } types[] = {
      {"float", TGSI_RETURN_TYPE_UNORM},
      {"uint", TGSI_RETURN_TYPE_UINT},
      {"int", TGSI_RETURN_TYPE_SINT},
   };
   static const enum pipe_swizzle bgra[4] = {
      PIPE_SWIZZLE_Z, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_X, PIPE_SWIZZLE_W,
   };
   int failed = 0;
   char name[128];
   for (unsigned t = 0; t < ARRAY_SIZE(targets); t++) {
      for (unsigned r = 0; r < ARRAY_SIZE(types); r++) {
         /* integer multisample sources are resolved by taking sample 0 */
         int samples = targets[t].samples && types[r].ret != TGSI_RETURN_TYPE_UNORM ?
                       1 : targets[t].samples;
         snprintf(name, sizeof(name), "color %s %s", targets[t].name, types[r].name);
         failed |= link_with_vs(name, blit.vs,
                                blit_build_frag_tex_col(&blit, targets[t].target, types[r].ret,
                                                        NULL, samples, 0));
      }
      snprintf(name, sizeof(name), "color %s swizzled, sRGB decode and encode", targets[t].name);
      failed |= link_with_vs(name, blit.vs,
                             blit_build_frag_tex_col(&blit, targets[t].target,
                                                     TGSI_RETURN_TYPE_UNORM, bgra,
                                                     targets[t].samples,
                                                     BLIT_MANUAL_SRGB_DECODE |
                                                     BLIT_MANUAL_SRGB_ENCODE));
   }
   /* Depth blits at the host's GLSL version and at 1.50, where cube map arrays
    * still need their extension line. */
   static const struct {
      const char *name;
      enum tgsi_texture_type target;
   } depth_targets[] = {
      {"2D", TGSI_TEXTURE_2D}, {"RECT", TGSI_TEXTURE_RECT}, {"CUBE", TGSI_TEXTURE_CUBE},
      {"2D_ARRAY", TGSI_TEXTURE_2D_ARRAY}, {"CUBE_ARRAY", TGSI_TEXTURE_CUBE_ARRAY},
   };
   const int depth_versions[] = { vrend_renderer_get_glsl_version(), 150 };
   for (unsigned v = 0; v < ARRAY_SIZE(depth_versions); v++) {
      blit_set_glsl_version(&blit, depth_versions[v]);
      GLuint vs = blit_shader_build_and_check(&blit, GL_VERTEX_SHADER, VS_PASSTHROUGH_GL);
      for (unsigned t = 0; t < ARRAY_SIZE(depth_targets); t++) {
         snprintf(name, sizeof(name), "depth %s (GLSL %d)", depth_targets[t].name, blit.glsl_ver);
         failed |= link_with_vs(name, vs, blit_build_frag_depth(&blit, depth_targets[t].target, false));
      }
      snprintf(name, sizeof(name), "depth 2D_MSAA (GLSL %d)", blit.glsl_ver);
      failed |= link_with_vs(name, vs, blit_build_frag_depth(&blit, TGSI_TEXTURE_2D_MSAA, true));
      snprintf(name, sizeof(name), "depth 2D_ARRAY_MSAA (GLSL %d)", blit.glsl_ver);
      failed |= link_with_vs(name, vs, blit_build_frag_depth(&blit, TGSI_TEXTURE_2D_ARRAY_MSAA, true));
      glDeleteShader(vs);
   }
   glDeleteShader(blit.vs);
   return failed;
}
