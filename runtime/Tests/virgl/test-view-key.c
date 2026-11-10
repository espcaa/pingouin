/* Sampler view slots and the shader key (virgl-legacy-vertex-cache.patch), without a GL
 * context: the renderer source is built in and the few GL calls of a view bind are stubbed.
 * Since a draw selects its shaders only when something marked them dirty, every change of
 * what vrend_fill_shader_key() reads from a bound view must set shader_dirty:
 *  - a texture buffer view with a swizzle the shader applies (L8 -> RRR1) replaced by one
 *    without (RGBA8), by no view, by a 2D view, or dropped by set_num_sampler_views;
 *  - a compute slot also sets cs_shader_dirty;
 *  - views without key bits swapped (2D for 2D, RGBA8 buffer for none) do not.
 *  - with a fragment shader bound that declares slot 0 a 2D_ARRAY sampler, a plain 2D view
 *    coming or going there does (virgl-shader-array-sampler-2d-view.patch), and its key
 *    bit is set exactly then;
 *  - a second view of slot 0's texture with another swizzle coming to slot 1 does (each
 *    then takes its swizzle in the shader: virgl-view-swizzle-shared-texture.patch), and
 *    both slots' views get the key's swizzle lowering.
 * And vrend_view_key_bits() agrees with vrend_fill_shader_key(): two views give the same
 * key exactly when they give the same bits (so a new key input from views cannot be
 * missed silently).
 * The pixel case for this sits not in test-vertex-binds: on Apple's software renderer,
 * linking any program while a texture buffer is bound crashes in glBufferData (also
 * without these caches). */
#include "vrend/vrend_renderer.c"

static int failures;

static void check(int ok, const char *what)
{
   printf("%s: %s\n", ok ? "ok" : "FAIL", what);
   failures += !ok;
}

static void GLAPIENTRY nop_bind_texture(GLenum target, GLuint id)
{
   (void)target;
   (void)id;
}

static void GLAPIENTRY nop_tex_parameteri(GLenum target, GLenum name, GLint value)
{
   (void)target;
   (void)name;
   (void)value;
}

static void GLAPIENTRY nop_tex_buffer(GLenum target, GLenum format, GLuint id)
{
   (void)target;
   (void)format;
   (void)id;
}

enum { V_L8 = 1, V_A8, V_RGBA8, V_2D, V_2D_B, V_RECT, V_COUNT };

static struct vrend_resource buffer = { .target = GL_TEXTURE_BUFFER, .gl_id = 7, .tbo_tex_id = 8,
                                        .storage_bits = VREND_STORAGE_GL_BUFFER };
static struct vrend_texture texture = { .base = { .target = GL_TEXTURE_2D, .gl_id = 9,
                                                  .storage_bits = VREND_STORAGE_GL_TEXTURE } };
static struct vrend_sampler_view views[V_COUNT];

static void make_views(struct vrend_sub_context *sub)
{
   static const struct { int id; enum virgl_formats format; bool buffer, rect; } v[] = {
      { V_L8, VIRGL_FORMAT_L8_UNORM, true, false },
      { V_A8, VIRGL_FORMAT_A8_UNORM, true, false },
      { V_RGBA8, VIRGL_FORMAT_R8G8B8A8_UNORM, true, false },
      { V_2D, VIRGL_FORMAT_R8G8B8A8_UNORM, false, false },
      { V_2D_B, VIRGL_FORMAT_L8_UNORM, false, false },
      { V_RECT, VIRGL_FORMAT_R8G8B8A8_UNORM, false, true },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(v); i++) {
      struct vrend_sampler_view *view = &views[v[i].id];
      pipe_reference_init(&view->reference, 1);   /* the test's own: never freed */
      view->format = v[i].format;
      view->texture = v[i].buffer ? &buffer : &texture.base;
      view->target = view->texture->target;
      /* buffers: not the texture's id (no GL state); 2D views are the texture itself, as
       * without GL texture views (their swizzle is the texture's: stubbed) */
      view->gl_id = v[i].buffer || v[i].rect ? 100 + v[i].id : texture.base.gl_id;
      view->emulated_rect = v[i].rect;
      /* 2D views: RGBA as they come, the L8 one RRR1 */
      const GLint rgba[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
      const GLint rrr1[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
      memcpy(view->gl_swizzle, v[i].id == V_2D_B ? rrr1 : rgba, sizeof(rgba));
      vrend_object_insert(sub->object_hash, view, v[i].id, VIRGL_OBJECT_SAMPLER_VIEW);
   }
}

/* bind view (0 = none) to slot 0 of stage, starting from a clean sub context */
static bool dirty_after(struct vrend_context *ctx, uint32_t stage, int from, int to,
                        bool *cs_dirty)
{
   vrend_set_single_sampler_view(ctx, stage, 0, from);
   ctx->sub->shader_dirty = false;
   ctx->sub->cs_shader_dirty = false;
   vrend_set_single_sampler_view(ctx, stage, 0, to);
   if (cs_dirty)
      *cs_dirty = ctx->sub->cs_shader_dirty;
   return ctx->sub->shader_dirty;
}

static void key_for(struct vrend_sub_context *sub, struct vrend_sampler_view *view,
                    struct vrend_shader_key *key)
{
   struct vrend_shader_selector fragment = { .type = PIPE_SHADER_FRAGMENT };
   struct vrend_sampler_view *old = sub->views[PIPE_SHADER_FRAGMENT].views[0];
   sub->views[PIPE_SHADER_FRAGMENT].views[0] = view;
   sub->views[PIPE_SHADER_FRAGMENT].max_num_views = 1;
   memset(key, 0, sizeof(*key));
   vrend_fill_shader_key(sub, &fragment, key);
   sub->views[PIPE_SHADER_FRAGMENT].views[0] = old;
}

int main(void)
{
   struct vrend_context ctx = { 0 };
   struct vrend_sub_context sub = { .parent = &ctx };
   char text[160];

   epoxy_glBindTexture = nop_bind_texture;
   epoxy_glTexParameteri = nop_tex_parameteri;
   epoxy_glTexBuffer = nop_tex_buffer;
   ctx.sub = &sub;
   ctx.shader_cfg.glsl_version = 410;
   vrend_state.use_core_profile = true;
   sub.object_hash = vrend_object_init_ctx_table();
   make_views(&sub);

   static const struct { uint32_t stage; int from, to; bool dirty; const char *what; } t[] = {
      { PIPE_SHADER_FRAGMENT, 0, V_L8, true, "no view -> L8 buffer (RRR1 in the shader)" },
      { PIPE_SHADER_FRAGMENT, V_L8, V_RGBA8, true, "L8 buffer -> RGBA8 buffer" },
      { PIPE_SHADER_FRAGMENT, V_L8, 0, true, "L8 buffer -> no view" },
      { PIPE_SHADER_FRAGMENT, V_L8, V_2D, true, "L8 buffer -> 2D texture" },
      { PIPE_SHADER_FRAGMENT, V_L8, V_A8, true, "L8 buffer -> A8 buffer (another swizzle)" },
      { PIPE_SHADER_FRAGMENT, V_2D, V_RECT, true, "2D texture -> emulated rectangle" },
      { PIPE_SHADER_FRAGMENT, V_RECT, 0, true, "emulated rectangle -> no view" },
      { PIPE_SHADER_FRAGMENT, V_2D, V_2D_B, false, "2D texture -> another 2D texture" },
      { PIPE_SHADER_FRAGMENT, 0, V_2D, false, "no view -> 2D texture" },
      { PIPE_SHADER_VERTEX, V_L8, V_2D, true, "vertex stage: L8 buffer -> 2D texture" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(t); i++) {
      bool dirty = dirty_after(&ctx, t[i].stage, t[i].from, t[i].to, NULL);
      snprintf(text, sizeof(text), "%s: %s", t[i].what,
               t[i].dirty ? "shaders marked dirty" : "nothing to select again");
      check(dirty == t[i].dirty, text);
   }
   bool cs_dirty = false;
   check(dirty_after(&ctx, PIPE_SHADER_COMPUTE, V_L8, 0, &cs_dirty) && cs_dirty,
         "compute stage: L8 buffer -> no view marks the compute shader dirty");

   /* set_num_sampler_views drops the slots past the new count */
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 0, V_2D);
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 1, V_L8);
   vrend_set_num_sampler_views(&ctx, PIPE_SHADER_FRAGMENT, 0, 2);
   sub.shader_dirty = false;
   vrend_set_num_sampler_views(&ctx, PIPE_SHADER_FRAGMENT, 0, 1);
   check(sub.shader_dirty, "set_num_sampler_views drops an L8 buffer slot: shaders marked dirty");
   vrend_set_num_sampler_views(&ctx, PIPE_SHADER_FRAGMENT, 0, 0);

   /* a fragment shader that declares slot 0 a 2D_ARRAY sampler: a 2D view there is a key
    * bit (sampled as 2D), so it coming or going selects the shaders again */
   struct vrend_shader_selector arrays = { .type = PIPE_SHADER_FRAGMENT };
   arrays.sinfo.samplers_2d_array_mask = 1;
   sub.shaders[PIPE_SHADER_FRAGMENT] = &arrays;
   static const struct { int from, to; bool dirty; const char *what; } a[] = {
      { 0, V_2D, true, "2D_ARRAY sampler: no view -> 2D texture" },
      { V_2D, V_2D_B, false, "2D_ARRAY sampler: 2D texture -> another 2D texture" },
      { V_2D, 0, true, "2D_ARRAY sampler: 2D texture -> no view" },
      { V_2D, V_RECT, true, "2D_ARRAY sampler: 2D texture -> emulated rectangle" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(a); i++) {
      bool dirty = dirty_after(&ctx, PIPE_SHADER_FRAGMENT, a[i].from, a[i].to, NULL);
      snprintf(text, sizeof(text), "%s: %s", a[i].what,
               a[i].dirty ? "shaders marked dirty" : "nothing to select again");
      check(dirty == a[i].dirty, text);
   }
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 0, 0);
   {
      struct vrend_shader_key k2d, knone, kbuf;
      struct vrend_sampler_view *old = sub.views[PIPE_SHADER_FRAGMENT].views[0];
      sub.views[PIPE_SHADER_FRAGMENT].max_num_views = 1;
      sub.views[PIPE_SHADER_FRAGMENT].views[0] = &views[V_2D];
      memset(&k2d, 0, sizeof(k2d));
      vrend_fill_shader_key(&sub, &arrays, &k2d);
      sub.views[PIPE_SHADER_FRAGMENT].views[0] = NULL;
      memset(&knone, 0, sizeof(knone));
      vrend_fill_shader_key(&sub, &arrays, &knone);
      sub.views[PIPE_SHADER_FRAGMENT].views[0] = &views[V_RGBA8];
      memset(&kbuf, 0, sizeof(kbuf));
      vrend_fill_shader_key(&sub, &arrays, &kbuf);
      sub.views[PIPE_SHADER_FRAGMENT].views[0] = old;
      check(k2d.sampler_views_2d_for_array_mask == 1 && !knone.sampler_views_2d_for_array_mask &&
            !kbuf.sampler_views_2d_for_array_mask,
            "2D_ARRAY sampler: the key bit is set for a 2D view only");
   }
   sub.shaders[PIPE_SHADER_FRAGMENT] = NULL;

   /* two views of one texture with different swizzles: GL keeps one swizzle per texture,
    * so both are applied in the shader, and the second coming or going selects again */
   vrend_set_num_sampler_views(&ctx, PIPE_SHADER_FRAGMENT, 0, 2);
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 0, V_2D);
   sub.shader_dirty = false;
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 1, V_2D_B);
   check(sub.shader_dirty, "a second view of slot 0's texture with another swizzle: shaders marked dirty");
   {
      struct vrend_shader_selector plain = { .type = PIPE_SHADER_FRAGMENT };
      struct vrend_shader_key k;
      memset(&k, 0, sizeof(k));
      vrend_fill_shader_key(&sub, &plain, &k);
      check(vrend_shader_sampler_views_mask_get(k.sampler_views_lower_swizzle_mask, 0) &&
            vrend_shader_sampler_views_mask_get(k.sampler_views_lower_swizzle_mask, 1) &&
            k.tex_swizzle[1] == (0 | 0 << 3 | 0 << 6 | PIPE_SWIZZLE_1 << 9) &&
            k.tex_swizzle[0] == (0 | 1 << 3 | 2 << 6 | 3 << 9),
            "two views of one texture: both swizzles in the shader");
   }
   sub.shader_dirty = false;
   vrend_set_single_sampler_view(&ctx, PIPE_SHADER_FRAGMENT, 1, 0);
   check(sub.shader_dirty, "the second view goes: shaders marked dirty");
   vrend_set_num_sampler_views(&ctx, PIPE_SHADER_FRAGMENT, 0, 0);

   /* vrend_view_key_bits() and vrend_fill_shader_key() agree on every pair */
   struct vrend_sampler_view *all[V_COUNT] = { NULL };
   for (int i = 1; i < V_COUNT; i++)
      all[i] = &views[i];
   bool agree = true;
   for (int a = 0; a < V_COUNT; a++) {
      for (int b = 0; b < V_COUNT; b++) {
         struct vrend_shader_key ka, kb;
         key_for(&sub, all[a], &ka);
         key_for(&sub, all[b], &kb);
         bool same_key = !memcmp(&ka, &kb, sizeof(ka));
         bool same_bits = vrend_view_key_bits(all[a]) == vrend_view_key_bits(all[b]);
         if (same_key != same_bits) {
            printf("views %d and %d: same key %d, same key bits %d\n", a, b, same_key, same_bits);
            agree = false;
         }
      }
   }
   check(agree, "the key bits of a view change exactly when its part of the shader key does");

   printf("%s\n", failures ? "view key: FAILED" : "view key: all checks passed");
   return failures != 0;
}
