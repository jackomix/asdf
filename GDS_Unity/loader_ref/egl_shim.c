/* egl_shim.c -- real GLES2 window/context for the GDS glibc loader.
 *
 * GDS's libunity.so runs Android's Unity engine.  Its graphics init
 * (nativeRecreateGfxState) asks the EGL surface layer for a display/config/
 * context/surface and then resolves every GL entry point through
 * eglGetProcAddress.  Terraria-nextos proved the recipe on this exact R36S:
 * create a real SDL2 window + GLES2 context (SDL backs it with the Mali driver
 * that the loader dlopens RTLD_GLOBAL), and answer the engine's EGL/ANativeWindow
 * calls from that real context.
 *
 * We follow that recipe but reach SDL2 through dlopen/dlsym so the zig
 * cross-build needs no SDL2 headers.  Each fake EGL context owns a real SDL GL
 * context sharing the bootstrap ("share root") context; eglMakeCurrent binds it
 * and eglSwapBuffers presents it.
 */
#include "egl_shim.h"
#include "musl_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

/* ---- SDL2 constants (SDL2/SDL_video.h + SDL_events.h values) ---- */
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_WINDOW_OPENGL 0x00000002u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_GL_RED_SIZE 0x0001
#define SDL_GL_GREEN_SIZE 0x0002
#define SDL_GL_BLUE_SIZE 0x0003
#define SDL_GL_ALPHA_SIZE 0x0004
#define SDL_GL_DOUBLEBUFFER 0x0005
#define SDL_GL_DEPTH_SIZE 0x0007
#define SDL_GL_STENCIL_SIZE 0x0008
#define SDL_GL_CONTEXT_MAJOR_VERSION 0x0010
#define SDL_GL_CONTEXT_MINOR_VERSION 0x0011
#define SDL_GL_CONTEXT_PROFILE_MASK 0x0013
#define SDL_GL_SHARE_WITH_CURRENT_CONTEXT 0x0015
#define SDL_GL_CONTEXT_PROFILE_ES 0x0004

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct { int format; int w; int h; int refresh_rate; void *driverdata; } SDL_DisplayMode;
/* SDL_Event is a 56-byte union; we only drain and drop events. */
typedef union { unsigned char pad[56]; } SDL_Event;

/* SDL2 entry points, resolved once via dlopen/dlsym. */
static struct {
  void *h;
  int (*Init)(unsigned);
  const char *(*GetError)(void);
  const char *(*GetCurrentVideoDriver)(void);
  int (*SetHint)(const char *, const char *);
  SDL_Window *(*CreateWindow)(const char *, int, int, int, int, unsigned);
  void (*DestroyWindow)(SDL_Window *);
  int (*GL_SetAttribute)(int, int);
  int (*GL_GetAttribute)(int, int *);
  SDL_GLContext (*GL_CreateContext)(SDL_Window *);
  void (*GL_DeleteContext)(SDL_GLContext);
  int (*GL_MakeCurrent)(SDL_Window *, SDL_GLContext);
  SDL_GLContext (*GL_GetCurrentContext)(void);
  void *(*GL_GetProcAddress)(const char *);
  int (*GL_SetSwapInterval)(int);
  void (*GL_SwapWindow)(SDL_Window *);
  void (*GL_GetDrawableSize)(SDL_Window *, int *, int *);
  int (*GetDesktopDisplayMode)(int, SDL_DisplayMode *);
  unsigned (*WasInit)(unsigned);
  int (*PollEvent)(SDL_Event *);
} S;

static int sdl_ok(void) { return S.Init && S.CreateWindow && S.GL_CreateContext && S.GL_MakeCurrent; }

static void *sym(void *h, const char *name) {
  void *p = dlsym(h, name);
  if (!p && h) p = dlsym(RTLD_DEFAULT, name);
  return p;
}

static int sdl_load(void) {
  if (sdl_ok()) return 1;
  const char *names[] = { "libSDL2-2.0.so.0", "libSDL2.so.0", "libSDL2.so", 0 };
  for (int i = 0; names[i]; i++) {
    S.h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (S.h) break;
  }
  void *h = S.h ? S.h : RTLD_DEFAULT;
  S.Init              = (int (*)(unsigned))sym(h, "SDL_Init");
  S.GetError          = (const char *(*)(void))sym(h, "SDL_GetError");
  S.GetCurrentVideoDriver = (const char *(*)(void))sym(h, "SDL_GetCurrentVideoDriver");
  S.SetHint           = (int (*)(const char *, const char *))sym(h, "SDL_SetHint");
  S.CreateWindow      = (SDL_Window *(*)(const char *, int, int, int, int, unsigned))sym(h, "SDL_CreateWindow");
  S.DestroyWindow     = (void (*)(SDL_Window *))sym(h, "SDL_DestroyWindow");
  S.GL_SetAttribute   = (int (*)(int, int))sym(h, "SDL_GL_SetAttribute");
  S.GL_GetAttribute   = (int (*)(int, int *))sym(h, "SDL_GL_GetAttribute");
  S.GL_CreateContext  = (SDL_GLContext (*)(SDL_Window *))sym(h, "SDL_GL_CreateContext");
  S.GL_DeleteContext  = (void (*)(SDL_GLContext))sym(h, "SDL_GL_DeleteContext");
  S.GL_MakeCurrent    = (int (*)(SDL_Window *, SDL_GLContext))sym(h, "SDL_GL_MakeCurrent");
  S.GL_GetCurrentContext = (SDL_GLContext (*)(void))sym(h, "SDL_GL_GetCurrentContext");
  S.GL_GetProcAddress = (void *(*)(const char *))sym(h, "SDL_GL_GetProcAddress");
  S.GL_SetSwapInterval= (int (*)(int))sym(h, "SDL_GL_SetSwapInterval");
  S.GL_SwapWindow     = (void (*)(SDL_Window *))sym(h, "SDL_GL_SwapWindow");
  S.GL_GetDrawableSize= (void (*)(SDL_Window *, int *, int *))sym(h, "SDL_GL_GetDrawableSize");
  S.GetDesktopDisplayMode = (int (*)(int, SDL_DisplayMode *))sym(h, "SDL_GetDesktopDisplayMode");
  S.WasInit           = (unsigned (*)(unsigned))sym(h, "SDL_WasInit");
  S.PollEvent         = (int (*)(SDL_Event *))sym(h, "SDL_PollEvent");
  return sdl_ok();
}

/* ---- state ---- */
static int g_screen_w = 640, g_screen_h = 480;
static int g_es_major = 2, g_alpha_size = 8, g_depth_size = 24, g_stencil_size = 8;
/* REPORTED depth/stencil (eglGetConfigAttrib answers) stay at the unity
 * minimum-spec floor even when the device only negotiated a shallower real
 * window: Unity just records the framebuffer expectation, and qemu proves
 * d24/s8 answers pass its minimum-spec test while real d0 does not. */
static int g_report_depth = 24, g_report_stencil = 8;
static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_context_id = 1;
static int g_did_init = 0;
static char g_fallback_config_token;

/* == 0.68: Horizon Chase egl_config_contract + real-EGL capture, VERBATIM ==
 * Unity 2022 (ours: 2022.3.62f2; Horizon: 2022.3.33f2 lineage) rejects a
 * fabricated EGLConfig with "Unable to find a configuration matching minimum
 * spec!" then tgkill(SIGTRAP) -- exactly our device death. The shipped
 * Horizon (and Cuphead-lineage) fix: capture SDL's real Mali EGL display/
 * window-surface right after the share root is created, pick a REAL EGLConfig
 * from the driver for the exact RGBA8888 contract Unity tests, hand that
 * config to Unity's eglChooseConfig, and let eglGetConfigAttrib delegate to
 * the real driver. Fake data never satisfies the matcher; real data always
 * does. The contract helpers below are hc_* copied unchanged so the
 * selection predicate is byte-identical to the proven port. */

typedef struct {
  int red, green, blue, alpha;
  int depth, stencil, samples;
  int renderable, surfaces, native_visual_type;
} hc_egl_config_properties;

enum {
  HC_EGL_BUFFER_SIZE = 0x3020, HC_EGL_ALPHA_SIZE = 0x3021,
  HC_EGL_BLUE_SIZE = 0x3022, HC_EGL_GREEN_SIZE = 0x3023,
  HC_EGL_RED_SIZE = 0x3024, HC_EGL_DEPTH_SIZE = 0x3025,
  HC_EGL_STENCIL_SIZE = 0x3026, HC_EGL_NATIVE_VISUAL_TYPE = 0x302f,
  HC_EGL_SAMPLES = 0x3031, HC_EGL_SAMPLE_BUFFERS = 0x3032,
  HC_EGL_SURFACE_TYPE = 0x3033, HC_EGL_NONE = 0x3038,
  HC_EGL_COLOR_BUFFER_TYPE = 0x303f, HC_EGL_RENDERABLE_TYPE = 0x3040,
  HC_EGL_CONFORMANT = 0x3042, HC_EGL_COVERAGE_SAMPLES_NV = 0x30e1,
  HC_EGL_DEPTH_ENCODING_NV = 0x30e2,
  HC_EGL_DEPTH_ENCODING_NONLINEAR_NV = 0x30e3,
  HC_EGL_RGB_BUFFER = 0x308e,
  HC_EGL_PBUFFER_BIT = 0x0001, HC_EGL_WINDOW_BIT = 0x0004,
  HC_EGL_OPENGL_ES2_BIT = 0x0004, HC_EGL_OPENGL_ES3_BIT_KHR = 0x0040,
  HC_EGL_RGBA_CHANNEL_BITS = 8, HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY = 17
};

static size_t hc_egl_build_window_config_attributes(
    int es_major, int depth, int stencil, int *attributes, size_t capacity) {
  const int renderable =
      es_major >= 3 ? HC_EGL_OPENGL_ES3_BIT_KHR : HC_EGL_OPENGL_ES2_BIT;
  const int values[HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY] = {
      HC_EGL_RED_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_GREEN_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_BLUE_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_ALPHA_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_DEPTH_SIZE, depth,
      HC_EGL_STENCIL_SIZE, stencil,
      HC_EGL_SURFACE_TYPE, HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT,
      HC_EGL_RENDERABLE_TYPE, renderable,
      HC_EGL_NONE
  };
  if (!attributes || capacity < HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY)
    return 0;
  memcpy(attributes, values, sizeof values);
  return HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY;
}

static int hc_egl_attribute_value(
    const int *attributes, int attribute, int fallback) {
  if (!attributes)
    return fallback;
  for (size_t index = 0; index + 1 < 128; index += 2) {
    if (attributes[index] == HC_EGL_NONE)
      break;
    if (attributes[index] == attribute)
      return attributes[index + 1];
  }
  return fallback;
}

static int hc_egl_config_meets_unity(
    const hc_egl_config_properties *candidate,
    const hc_egl_config_properties *required) {
  if (!candidate || !required)
    return 0;
  return candidate->native_visual_type != 0x108 &&
         candidate->red == required->red &&
         candidate->green == required->green &&
         candidate->blue == required->blue &&
         candidate->alpha == required->alpha &&
         candidate->depth >= required->depth &&
         candidate->stencil >= required->stencil &&
         candidate->samples >= required->samples &&
         (candidate->renderable & required->renderable) ==
             required->renderable &&
         (candidate->surfaces & required->surfaces) == required->surfaces;
}

/* Real-EGL entry points captured from the Mali driver that SDL already
 * loaded.  NB: our loader's own dynamic symbols share these API names (the
 * loader exports eglChooseConfig etc. so libunity's imports bind), so
 * dlsym(RTLD_DEFAULT,...) would resolve back into THIS shim.  The reference
 * ports can use RTLD_DEFAULT because their shim exports egl_shim_* names.
 * We therefore dlopen the driver by name and use the explicit handle; the
 * launcher's SDL_VIDEO_EGL_DRIVER points at the same lib SDL opened. */
static void *(*r_eglGetCurrentDisplay)(void);
static unsigned (*r_eglChooseConfig)(void *, const int *, void **, int, int *);
static unsigned (*r_eglGetConfigAttrib)(void *, void *, int, int *);
static void *(*r_eglGetCurrentSurface)(int);
static void *(*r_eglCreatePbufferSurface)(void *, void *, const int *);
static unsigned (*r_eglMakeCurrent)(void *, void *, void *, void *);
static void *(*r_eglCreateContext)(void *, void *, void *, const int *);
static void *g_real_dpy = NULL;    /* SDL's real EGLDisplay */
static void *g_real_cfg = NULL;    /* real EGLConfig meeting Unity's RGBA8888 contract */
static void *g_win_surf = NULL;    /* real EGLSurface of SDL's window */
static void *g_pbuf = NULL;        /* real 16x16 pbuffer for Unity worker contexts */
static void *g_real_egl_hdl = NULL;
static int g_nullgl;   /* tentative; the NullGL section below owns the init */

static void gds_capture_real_egl(void) {
  if (g_nullgl) return;
  const char *drv = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *names[] = { (drv && *drv) ? drv : "libEGL.so",
                          "libEGL.so.1", "libEGL.so", 0 };
  for (int i = 0; names[i] && !g_real_egl_hdl; i++) {
    g_real_egl_hdl = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
    if (!g_real_egl_hdl)
      g_real_egl_hdl = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
  }
  fprintf(stderr, "[egl] real EGL handle=%p (env SDL_VIDEO_EGL_DRIVER=%s)\n",
          g_real_egl_hdl, drv ? drv : "(null)");
  if (!g_real_egl_hdl) return;
#define CAP(field, sym) \
  field = (void *)dlsym(g_real_egl_hdl, sym); \
  fprintf(stderr, "[egl]   %s = %p\n", sym, (void *)field)
  CAP(r_eglGetCurrentDisplay, "eglGetCurrentDisplay");
  CAP(r_eglChooseConfig, "eglChooseConfig");
  CAP(r_eglGetConfigAttrib, "eglGetConfigAttrib");
  CAP(r_eglGetCurrentSurface, "eglGetCurrentSurface");
  CAP(r_eglCreatePbufferSurface, "eglCreatePbufferSurface");
  CAP(r_eglMakeCurrent, "eglMakeCurrent");
  CAP(r_eglCreateContext, "eglCreateContext");
#undef CAP
  if (r_eglGetCurrentDisplay) g_real_dpy = r_eglGetCurrentDisplay();
  if (r_eglGetCurrentSurface) g_win_surf = r_eglGetCurrentSurface(0x3059 /*EGL_DRAW*/);
  fprintf(stderr, "[egl] real dpy=%p window surface=%p\n", g_real_dpy, g_win_surf);
  if (!(g_real_dpy && r_eglChooseConfig && r_eglGetConfigAttrib)) {
    fprintf(stderr, "[egl] no real EGLDisplay -- hardcoded attr answers stay\n");
    return;
  }

  int attrs[HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY];
  hc_egl_build_window_config_attributes(
      g_es_major, g_depth_size, g_stencil_size, attrs,
      sizeof attrs / sizeof attrs[0]);
  void *cfgs[64];
  int n = 0;
  if (!r_eglChooseConfig(g_real_dpy, attrs, cfgs, 64, &n) || n <= 0) {
    fprintf(stderr, "[egl] real choose n=%d -- hardcoded attr answers stay\n", n);
    return;
  }
  hc_egl_config_properties required = {
      .red = HC_EGL_RGBA_CHANNEL_BITS,
      .green = HC_EGL_RGBA_CHANNEL_BITS,
      .blue = HC_EGL_RGBA_CHANNEL_BITS,
      .alpha = HC_EGL_RGBA_CHANNEL_BITS,
      .depth = g_depth_size,
      .stencil = g_stencil_size,
      .samples = 0,
      .renderable = g_es_major >= 3
                        ? HC_EGL_OPENGL_ES3_BIT_KHR
                        : HC_EGL_OPENGL_ES2_BIT,
      .surfaces = HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT,
      .native_visual_type = 0
  };
  hc_egl_config_properties selected = {0};
  int checked = n < 64 ? n : 64;
  for (int i = 0; i < checked; i++) {
    hc_egl_config_properties candidate = {0};
#define HC_QUERY_CONFIG(field, attribute)                                  \
    do {                                                                   \
      r_eglGetConfigAttrib(g_real_dpy, cfgs[i], (attribute), &candidate.field); \
    } while (0)
    HC_QUERY_CONFIG(red, HC_EGL_RED_SIZE);
    HC_QUERY_CONFIG(green, HC_EGL_GREEN_SIZE);
    HC_QUERY_CONFIG(blue, HC_EGL_BLUE_SIZE);
    HC_QUERY_CONFIG(alpha, HC_EGL_ALPHA_SIZE);
    HC_QUERY_CONFIG(depth, HC_EGL_DEPTH_SIZE);
    HC_QUERY_CONFIG(stencil, HC_EGL_STENCIL_SIZE);
    HC_QUERY_CONFIG(samples, HC_EGL_SAMPLES);
    HC_QUERY_CONFIG(renderable, HC_EGL_RENDERABLE_TYPE);
    HC_QUERY_CONFIG(surfaces, HC_EGL_SURFACE_TYPE);
    HC_QUERY_CONFIG(native_visual_type, HC_EGL_NATIVE_VISUAL_TYPE);
#undef HC_QUERY_CONFIG
    if (hc_egl_config_meets_unity(&candidate, &required)) {
      g_real_cfg = cfgs[i];
      selected = candidate;
      break;
    }
  }
  if (!g_real_cfg) {
    fprintf(stderr,
            "[egl] none of the %d real EGLConfigs is RGBA8888; keeping the "
            "logical SDL contract\n", n);
    return;
  }
  fprintf(stderr,
          "[egl] REAL Unity EGLConfig selected rgba=%d/%d/%d/%d depth=%d "
          "stencil=%d samples=%d renderable=0x%x candidates=%d\n",
          selected.red, selected.green, selected.blue, selected.alpha,
          selected.depth, selected.stencil, selected.samples,
          selected.renderable, n);
  /* real pbuffer for Unity's shared worker contexts (verbatime Horizon) */
  if (r_eglCreatePbufferSurface) {
    static const int pb[] = { 0x3057, 16, 0x3056, 16, HC_EGL_NONE };
    g_pbuf = r_eglCreatePbufferSurface(g_real_dpy, g_real_cfg, pb);
    fprintf(stderr, "[egl] real worker pbuffer = %p\n", g_pbuf);
  }
}

/* 0.61: always-on call trace (stderr; swap/getproc are hot -> capped). */
static int g_trc_cap = 400;
static int g_trc_n = 0;
static void TR(const char *msg) {
  if (g_trc_n >= g_trc_cap) return;
  g_trc_n++;
  fprintf(stderr, "[egl] %s\n", msg);
}


/* ---- per-call counters (diagnostics: is Unity actually driving our EGL?) ---- */
static volatile int g_n_eglGetDisplay, g_n_eglInitialize, g_n_eglChooseConfig;
static volatile int g_n_eglCreateContext, g_n_eglMakeCurrent, g_n_eglSwapBuffers;
static volatile int g_n_eglCreateWindowSurface, g_n_eglGetProcAddress;
static void egl_show_counts(const char *why) {
  printf("[egl] CALLS %s: GetDisplay=%d Initialize=%d ChooseConfig=%d "
         "CreateContext=%d MakeCurrent=%d SwapBuffers=%d CreateWindowSurface=%d GetProcAddress=%d\n",
         why, g_n_eglGetDisplay, g_n_eglInitialize, g_n_eglChooseConfig,
         g_n_eglCreateContext, g_n_eglMakeCurrent, g_n_eglSwapBuffers,
         g_n_eglCreateWindowSurface, g_n_eglGetProcAddress);
}

typedef struct {
  SDL_GLContext sdl_context;
  EGLSurface current_draw;   /* exact handles handed to eglMakeCurrent */
  EGLSurface current_read;
  int is_pbuffer;
  int id;
} _egl_context;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

/* ---- NullGL: fake GL backdrop for headless iteration (qemu/CI) -------------
 * Engages ONLY when the real SDL/Mali bootstrap failed (auto) or when forced
 * with GDS_NULLGL=2; GDS_NULLGL=0 disables it entirely.  When engaged,
 * eglCreateContext/eglMakeCurrent/eglSwapBuffers succeed symbolically and
 * eglGetProcAddress hands out deterministic fake GL entry points whose answers
 * mimic the R36S's Mali-G31 ES2 context, so Unity's logic can run to the next
 * real bug without a GPU.  The device happy path (real SDL window + context)
 * is untouched: g_nullgl stays 0 there. */
static int g_nullgl = 0;       /* engaged (tentative decl lives above) */
static int g_nullgl_cfg = -1;  /* parsed env: 0=off 1=auto 2=force */
static int nullgl_mode(void) {
  if (g_nullgl_cfg < 0) {
    const char *e = getenv("GDS_NULLGL");
    g_nullgl_cfg = e ? atoi(e) : 1;
  }
  return g_nullgl_cfg;
}
static void nullgl_engage(const char *why) {
  if (g_nullgl || nullgl_mode() == 0) return;
  g_nullgl = 1;
  fprintf(stderr, "[egl] *** NullGL fallback engaged (%s) -- no real GPU; "
                  "GL calls become deterministic no-ops ***\n", why);
}

static unsigned nullgl_next_id = 1;
static void nullgl_gen(int n, unsigned *ids) {
  if (!ids) return;
  for (int i = 0; i < n && i < 0x10000; i++) ids[i] = nullgl_next_id++;
}
static unsigned nullgl_create_one(void) { return nullgl_next_id++; }

static const unsigned char *nullgl_GetString(unsigned name) {
  switch (name) {
  case 0x1F00: return (const unsigned char *)"ARM";
  case 0x1F01: return (const unsigned char *)"Mali-G31";
  case 0x1F02: return (const unsigned char *)"OpenGL ES 2.0 v1.NullGL";
  case 0x8B8C: return (const unsigned char *)"OpenGL ES GLSL ES 1.00";
  case 0x1F03: return (const unsigned char *)
      "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil "
      "GL_EXT_texture_format_BGRA8888 GL_OES_texture_npot";
  default:     return (const unsigned char *)"";
  }
}
static unsigned nullgl_GetError(void) { return 0; } /* GL_NO_ERROR */

static void nullgl_GetIntegerv(unsigned pname, int *p) {
  if (!p) return;
  switch (pname) {
  case 0x0D33: p[0] = 8192; break;                       /* MAX_TEXTURE_SIZE */
  case 0x0D3A: p[0] = 8192; p[1] = 8192; break;          /* MAX_VIEWPORT_DIMS */
  case 0x80A9: p[0] = 8192; break;                       /* MAX_CUBE_MAP_TEXTURE_SIZE */
  case 0x84E8: p[0] = 8192; break;                       /* MAX_RENDERBUFFER_SIZE */
  case 0x8872: p[0] = 8; break;                          /* MAX_TEXTURE_IMAGE_UNITS */
  case 0x8B4D: p[0] = 8; break;                          /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
  case 0x8DFB: p[0] = 4; break;                          /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
  case 0x8869: p[0] = 16; break;                         /* MAX_VERTEX_ATTRIBS */
  case 0x8DFD: p[0] = 128; break;                        /* MAX_VERTEX_UNIFORM_VECTORS */
  case 0x8DFC: p[0] = 64; break;                         /* MAX_FRAGMENT_UNIFORM_VECTORS */
  case 0x8DF9: p[0] = 15; break;                         /* MAX_VARYING_VECTORS */
  case 0x821D: p[0] = 0; break;                          /* NUM_EXTENSIONS */
  case 0x8B8D: p[0] = 200; break;                        /* MAX_TEXTURE_MAX_ANISOTROPY? (harmless) */
  case 0x0B71: p[0] = 1; break;                          /* SUBPIXEL_BITS */
  case 0x0C50: p[0] = 1; p[1] = 4064; break;             /* ALIASED_LINE_WIDTH_RANGE (ints) */
  default:     p[0] = 0; break;
  }
}
static void nullgl_GetFloatv(unsigned pname, float *p) {
  if (!p) return;
  switch (pname) {
  case 0x84FF: p[0] = 16.0f; break;                      /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
  case 0x0C50: p[0] = 1.0f; p[1] = 4064.0f; break;       /* ALIASED_LINE_WIDTH_RANGE */
  case 0x0C52: p[0] = 1.0f; p[1] = 4064.0f; break;       /* ALIASED_POINT_SIZE_RANGE */
  case 0x846D: p[0] = 255.0f; p[1] = 255.0f; break;      /* DEPTH_RANGE */
  case 0x0D3A: p[0] = 8192.0f; p[1] = 8192.0f; break;
  default:     p[0] = 0.0f; break;
  }
}
static void nullgl_GetBooleanv(unsigned pname, unsigned char *p) {
  if (!p) return;
  (void)pname;
  p[0] = 0; /* GL_FALSE */
}
static unsigned nullgl_CheckFramebufferStatus(unsigned target) {
  (void)target; return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}
static void nullgl_GetShaderiv(unsigned shader, unsigned pname, int *p) {
  (void)shader; if (!p) return;
  switch (pname) {
  case 0x8B81: p[0] = 1; break;  /* COMPILE_STATUS ok */
  case 0x8B84: p[0] = 0; break;  /* INFO_LOG_LENGTH */
  case 0x8B80: p[0] = 0; break;  /* DELETE_STATUS */
  case 0x8B89: p[0] = 0x8B31; break; /* SHADER_TYPE = FRAGMENT (arbitrary) */
  default:     p[0] = 0; break;
  }
}
static void nullgl_GetProgramiv(unsigned program, unsigned pname, int *p) {
  (void)program; if (!p) return;
  switch (pname) {
  case 0x8B82: p[0] = 1; break;  /* LINK_STATUS ok */
  case 0x8B83: p[0] = 1; break;  /* VALIDATE_STATUS ok */
  case 0x8B84: p[0] = 0; break;  /* INFO_LOG_LENGTH */
  case 0x8B80: p[0] = 0; break;  /* DELETE_STATUS */
  case 0x8B86: p[0] = 0; break;  /* ACTIVE_UNIFORMS */
  case 0x8B87: p[0] = 0; break;  /* ACTIVE_ATTRIBUTES */
  case 0x8B8A: p[0] = 0; break;  /* ATTACHED_SHADERS */
  default:     p[0] = 0; break;
  }
}
static void nullgl_InfoLog(unsigned obj, int bufsize, int *length, char *log) {
  (void)obj;
  if (length) *length = 0;
  if (log && bufsize > 0) log[0] = 0;
}
static void nullgl_ReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *pix) {
  (void)x; (void)y; (void)fmt; (void)type;
  if (!pix || w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
  memset(pix, 0, (size_t)w * (size_t)h * 4); /* worst-case RGBA8 footprint */
}
static void nullgl_GetActiveAny(unsigned prog, unsigned idx, int bufsize, int *length,
                                int *size, unsigned *type, char *name) {
  (void)prog; (void)idx; (void)size;
  if (length) *length = 0;
  if (type) *type = 0;
  if (name && bufsize > 0) name[0] = 0;
}
static void nullgl_GetShaderPrecisionFormat(unsigned st, unsigned pt, int *range, int *precision) {
  (void)st; (void)pt;
  if (range) { range[0] = 127; range[1] = 127; }
  if (precision) *precision = 23;
}
static int nullgl_false0(void) { return 0; }
static long nullgl_noop(void) { return 0; } /* generic: args ignored, returns 0/NULL */

void *nullgl_gl_proc(const char *n) {
  static const struct { const char *n; void *f; } t[] = {
    {"glGetString", nullgl_GetString}, {"glGetStringi", nullgl_noop},
    {"glGetError", nullgl_GetError},
    {"glGetIntegerv", nullgl_GetIntegerv}, {"glGetFloatv", nullgl_GetFloatv},
    {"glGetBooleanv", nullgl_GetBooleanv},
    {"glCheckFramebufferStatus", nullgl_CheckFramebufferStatus},
    {"glGetShaderiv", nullgl_GetShaderiv}, {"glGetProgramiv", nullgl_GetProgramiv},
    {"glGetShaderInfoLog", nullgl_InfoLog}, {"glGetProgramInfoLog", nullgl_InfoLog},
    {"glGenTextures", nullgl_gen}, {"glGenBuffers", nullgl_gen},
    {"glGenFramebuffers", nullgl_gen}, {"glGenRenderbuffers", nullgl_gen},
    {"glGenVertexArraysOES", nullgl_gen}, {"glGenVertexArrays", nullgl_gen},
    {"glCreateShader", nullgl_create_one}, {"glCreateProgram", nullgl_create_one},
    {"glReadPixels", nullgl_ReadPixels},
    {"glGetActiveUniform", nullgl_GetActiveAny}, {"glGetActiveAttrib", nullgl_GetActiveAny},
    {"glGetShaderPrecisionFormat", nullgl_GetShaderPrecisionFormat},
    {"glGetUniformLocation", nullgl_false0}, {"glGetAttribLocation", nullgl_false0},
    {"glIsEnabled", nullgl_false0}, {"glIsTexture", nullgl_false0},
    {"glIsBuffer", nullgl_false0}, {"glIsProgram", nullgl_false0},
    {"glIsShader", nullgl_false0}, {"glIsFramebuffer", nullgl_false0},
    {"glIsRenderbuffer", nullgl_false0}, {"glIsVertexArray", nullgl_false0},
    {"glIsVertexArrayOES", nullgl_false0},
    {"glUnmapBuffer", nullgl_false0}, {"glUnmapBufferOES", nullgl_false0},
    {"glGetVertexAttribPointerv", nullgl_noop},
    {"glMapBuffer", nullgl_noop}, {"glMapBufferOES", nullgl_noop},
    {"glMapBufferRange", nullgl_noop}, {"glMapBufferRangeEXT", nullgl_noop},
    {0, 0}
  };
  for (int i = 0; t[i].n; i++) if (strcmp(t[i].n, n) == 0) return t[i].f;
  return nullgl_noop; /* every other entry point: benign no-op */
}

int egl_shim_screen_w(void) { return g_screen_w; }
int egl_shim_screen_h(void) { return g_screen_h; }

/* Real GL answers captured at bootstrap (1 when available); the JNI GLES20
 * bridge reads these so Kairosoft's SupportsDepth24-style checks see the same
 * extensions the real context has, on any thread. */
static char g_gl_str_cache[5][3072]; /* vendor renderer version sl extensions */
static int g_gl_str_cached = 0;
const char *gds_gl_string_for_jni(unsigned name) {
  /* Horizon routes JNI GL queries at the LIVE context whenever the calling
   * thread has one; the bootstrap cache is just the no-context fallback.
   * (Unity parses real worker-context strings for capability decisions.) */
  if (has_real_gl && S.GL_GetProcAddress) {
    const unsigned char *(*g)(unsigned) =
      (const unsigned char *(*)(unsigned))S.GL_GetProcAddress("glGetString");
    if (g) {
      const unsigned char *s = g(name);
      if (s) return (const char *)s;
    }
  }
  if (g_gl_str_cached) {
    switch (name) {
    case 0x1F00: return g_gl_str_cache[0];
    case 0x1F01: return g_gl_str_cache[1];
    case 0x1F02: return g_gl_str_cache[2];
    case 0x8B8C: return g_gl_str_cache[3];
    case 0x1F03: return g_gl_str_cache[4];
    default: return "";
    }
  }
  return (const char *)nullgl_GetString(name);
}

/* ---- window + share-root context creation (must run on the main thread) ---- */
void egl_shim_create_window(void) {
  if (g_did_init) return;
  g_did_init = 1;
  if (nullgl_mode() == 2) { nullgl_engage("GDS_NULLGL=2 (forced)"); return; }
  if (!sdl_load()) {
    printf("[egl] SDL2 unavailable: %s\n", S.GetError ? S.GetError() : "?");
    nullgl_engage("SDL2 unavailable");
    return;
  }
  /* SDL must be initialized before any window/context call, or SDL_CreateWindow
   * can't set up the video driver / load the EGL/GL library ("Can't load EGL/GL
   * library on window creation").  This is why the first on-device run had
   * "SDL video driver = (null)" and no GL context. */
  /* Horizon Chase lesson: force the GLES driver BEFORE init, or this SDL+
   * Mali/KMSDRM pair hands back an "OpenGL ES-CM 1.1" fixed-pipeline context
   * and Unity can never build an ES2+ device ("Graphics device is null"). */
  if (S.SetHint) {
    S.SetHint("SDL_OPENGL_ES_DRIVER", "1");
    S.SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
  }
  if (S.Init && !S.WasInit(SDL_INIT_VIDEO)) {
    int r = S.Init(SDL_INIT_VIDEO);
    printf("[egl] SDL_Init(VIDEO) = %s (%d)\n", r == 0 ? "ok" : "FAILED", r);
    if (r != 0) printf("[egl] SDL_Init error: %s\n", S.GetError ? S.GetError() : "?");
    /* audio is optional for the first render milestone */
    if (S.WasInit) { int ar = S.Init(SDL_INIT_AUDIO); (void)ar; }
  }
  if (S.GetCurrentVideoDriver)
    printf("[egl] SDL video driver = %s\n", S.GetCurrentVideoDriver());

  /* pick window size from the desktop mode, else 640x480 */
  int width = 0, height = 0;
  if (S.GetDesktopDisplayMode) {
    SDL_DisplayMode mode; memset(&mode, 0, sizeof mode);
    if (S.GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
      width = mode.w; height = mode.h;
    }
  }
  if (width <= 0) width = 640;
  if (height <= 0) height = 480;

  /* Version x format negotiation, proven on this exact device stack by the
   * Horizon Chase port: alpha is ALWAYS 8 (that Unity device enumeration does
   * an exact RGBA8888 compare); try ES 3 then ES 2 per window, and reject any
   * context that doesn't report an "OpenGL ES" identity.
   * Depth variants before the d0 fallback: the R36S KMSDRM accepted ONLY a
   * d0 SDL window for us (device 0.66 log), so try depth-without-stencil
   * permutations before giving up on real depth. */
  static const int fmts[][2] = { {24,8}, {24,0}, {16,0}, {16,8}, {0,0} };
  int versions[2] = {3, 2};
  { const char *force = getenv("GDS_GLES_MAJOR");   /* HC_GLES_MAJOR analog */
    if (force && (force[0] == '2' || force[0] == '3')) {
      versions[0] = force[0] - '0'; versions[1] = 0;
    } }
  int nversions = versions[1] ? 2 : 1;
  for (size_t f = 0; f < sizeof fmts / sizeof fmts[0] && !egl_share_root; f++) {
    for (size_t vv = 0; vv < (size_t)nversions && !egl_share_root; vv++) {
      S.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      S.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, versions[vv]);
      S.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      S.GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      S.GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      S.GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      S.GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
      S.GL_SetAttribute(SDL_GL_DEPTH_SIZE, fmts[f][0]);
      S.GL_SetAttribute(SDL_GL_STENCIL_SIZE, fmts[f][1]);
      S.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      if (!egl_window) {
        egl_window = S.CreateWindow("Game Dev Story", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, width, height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (!egl_window) {
          printf("[egl] fmt %zu (a8 d%d s%d) window failed\n", f, fmts[f][0], fmts[f][1]);
          break;    /* next format: new window attempt */
        }
      }
      egl_share_root = S.GL_CreateContext(egl_window);
      if (!egl_share_root) {
        printf("[egl] ctx ES%d fmt %zu (a8 d%d s%d) failed: %s\n",
               versions[vv], f, fmts[f][0], fmts[f][1], S.GetError());
        continue;
      }
      /* Identity check: must be GLES, not desktop GL.  Accept "OpenGL ES-CM"
       * (the R36S Mali/KMSDRM share-root string) EXACTLY like the shipped,
       * working Horizon Chase port does -- its ctx_is_gles() is just
       * strstr(version, "OpenGL ES").  This share root is only SDL's bond
       * between Unity's worker contexts; the game renders through its own
       * SDL-created contexts, never through this one.  Rejecting ES-CM here
       * (<=0.64) is what forced the pointless NullGL fallback on a device
       * whose GL stack another Unity port drives at 60fps. */
      {
        void *gs = S.GL_GetProcAddress ? S.GL_GetProcAddress("glGetString") : NULL;
        const char *ver = NULL;
        if (gs) {
          const unsigned char *(*g)(unsigned) = (const unsigned char *(*)(unsigned))gs;
          const unsigned char *s = g(0x1F02);
          ver = s ? (const char *)s : NULL;
        }
        if (ver && !strstr(ver, "OpenGL ES")) {
          printf("[egl] ctx ES%d rejected (desktop GL), GL_VERSION='%s'\n",
                 versions[vv], ver);
          S.GL_DeleteContext(egl_share_root); egl_share_root = NULL;
          continue;
        }
        printf("[egl] accepted ES%d ctx, GL_VERSION=%s\n", versions[vv],
               ver ? ver : "(unknown)");
      }
      g_es_major = versions[vv];
      /* read back the REALLY negotiated buffer configuration */
      if (S.GL_GetAttribute) {
        int v;
        if (S.GL_GetAttribute(SDL_GL_ALPHA_SIZE, &v) == 0)   g_alpha_size = v;
        if (S.GL_GetAttribute(SDL_GL_DEPTH_SIZE, &v) == 0)   g_depth_size = v;
        if (S.GL_GetAttribute(SDL_GL_STENCIL_SIZE, &v) == 0) g_stencil_size = v;
      } else {
        g_alpha_size = 8; g_depth_size = fmts[f][0]; g_stencil_size = fmts[f][1];
      }
      /* Unity 2022's config minimum-spec test rejects depth-poor configs;
       * a d0-negotiated window still renders a 2D game, so report the
       * floor upward and log the gap loudly. */
      g_report_depth = g_depth_size; g_report_stencil = g_stencil_size;
      if (g_report_depth < 16) {
        printf("[egl] real window is d%d/s%d -- REPORTING d24 to Unity (minimum-spec floor)\n",
               g_depth_size, g_stencil_size);
        g_report_depth = 24;
      }
      if (g_report_stencil < 8) g_report_stencil = 8;
    }
    if (!egl_share_root && egl_window) { S.DestroyWindow(egl_window); egl_window = NULL; }
  }
    if (!egl_share_root) {
      printf("[egl] no GL context created\n");
      nullgl_engage("SDL GL context creation failed");
      return;
    }
    printf("[egl] window=%p context=%p (SDL_CreateWindow may have logged a surface warning)\n",
           (void *)egl_window, (void *)egl_share_root);
    printf("[egl] negotiated a%d d%d s%d ES%d (report d%d s%d)\n", g_alpha_size, g_depth_size, g_stencil_size, g_es_major, g_report_depth, g_report_stencil);

    /* 0.68: capture SDL's real Mali EGL objects NOW (share root still current
     * on this thread) and pick the real EGLConfig Unity's matcher will accept.
     * Before this, our fabricated config token let qemu pass (NullGL answers)
     * but the real driver rejected it -> minimum-spec abort on device. */
    gds_capture_real_egl();

    /* KMSDRM settles the drawable a few frames after the context appears;
     * Horizon polls events and waits for the size to match (30 x 10ms)
     * before letting the game at the window. */
  if (S.PollEvent && S.GL_GetDrawableSize) {
    int dw = 0, dh = 0;
    for (int i = 0; i < 30; i++) {
      SDL_Event ev;
      while (S.PollEvent(&ev)) {}
      S.GL_GetDrawableSize(egl_window, &dw, &dh);
      if (dw > 0 && dh > 0) break;
      struct timespec ts = { 0, 10000000 };
      nanosleep(&ts, NULL);
    }
    if (dw > 0 && dh > 0) { g_screen_w = dw; g_screen_h = dh; }
  }
  if (S.GL_GetDrawableSize && (g_screen_w <= 0 || g_screen_h <= 0))
    S.GL_GetDrawableSize(egl_window, &g_screen_w, &g_screen_h);
  if (g_screen_w <= 0) g_screen_w = width;
  if (g_screen_h <= 0) g_screen_h = height;
  if (S.GL_SetSwapInterval) S.GL_SetSwapInterval(1);

  /* log the real GL identity (proves the Mali driver is behind the context)
   * and cache the strings: android/opengl/GLES20.glGetString goes through our
   * JNI bridge on threads where no context is current, so the real driver
   * answer has to be captured here while the root context is bound. */
  void *gs = S.GL_GetProcAddress ? S.GL_GetProcAddress("glGetString") : NULL;
  if (gs) {
    const unsigned char *(*g)(unsigned) = (const unsigned char *(*)(unsigned))gs;
    static const struct { unsigned name; const char *tag; } rows[5] = {
      { 0x1F00, "GL_VENDOR" }, { 0x1F01, "GL_RENDERER" }, { 0x1F02, "GL_VERSION" },
      { 0x8B8C, "GL_SL" },     { 0x1F03, "GL_EXTENSIONS" },
    };
    for (int i = 0; i < 5; i++) {
      const unsigned char *s = g(rows[i].name);
      snprintf(g_gl_str_cache[i], sizeof g_gl_str_cache[i], "%s",
               s ? (const char *)s : "");
      if (i < 3)
        printf("[egl] %s=%s\n", rows[i].tag, g_gl_str_cache[i]);
      else
        printf("[egl] %s: %u bytes\n", rows[i].tag,
               (unsigned)strlen(g_gl_str_cache[i]));
    }
    g_gl_str_cached = 1;
  }
  printf("[egl] window %dx%d context ready (ES%d)\n", g_screen_w, g_screen_h, g_es_major);

  /* release from the bootstrap thread; the game binds when it makes current */
  if (S.GL_MakeCurrent) S.GL_MakeCurrent(egl_window, NULL);
}

int egl_shim_ensure_current(void) {
  if (g_nullgl) return 1;
  if (!egl_window || !egl_share_root) return 0;
  if (has_real_gl) return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (ctx && ctx->sdl_context && S.GL_MakeCurrent(egl_window, ctx->sdl_context) == 0) {
    has_real_gl = 1; current_context = ctx; return 1;
  }
  if (S.GL_MakeCurrent(egl_window, egl_share_root) == 0) { has_real_gl = 1; return 1; }
  return 0;
}

void egl_shim_swap(void) { if (egl_window && S.GL_SwapWindow) S.GL_SwapWindow(egl_window); }

/* ---- EGL API ---- */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  { char b[96]; snprintf(b,sizeof b,"eglGetDisplay(%p)",(void*)display_id); TR(b); }
  (void)display_id; g_n_eglGetDisplay++; return (EGLDisplay)&g_did_init;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  TR("eglInitialize");
  (void)dpy; if (major) *major = 1; if (minor) *minor = 4;
  g_n_eglInitialize++; if (g_n_eglInitialize == 1) egl_show_counts("after Initialize");
  return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
  (void)dpy;
  if (egl_share_root && S.GL_DeleteContext) S.GL_DeleteContext(egl_share_root);
  egl_share_root = NULL;
  if (egl_window && S.DestroyWindow) S.DestroyWindow(egl_window);
  egl_window = NULL;
  return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                           EGLint config_size, EGLint *num_config) {
  { char b[1024], *q=b; q+=snprintf(q,sizeof(b)-(q-b),"eglChooseConfig want:"); if(attrib_list) for(int i=0;attrib_list[i]!=0x3038&&i<40;i+=2) q+=snprintf(q,(b+sizeof b)-q," %x=%x",attrib_list[i],attrib_list[i+1]); void *ra=__builtin_return_address(0); extern const char *gds_mod_at(const void *, void **); void *base=0; const char *mod=gds_mod_at(ra,&base); q+=snprintf(q,(b+sizeof b)-q,"  (from %s+%lx)", mod?mod:"?", (unsigned long)ra-(unsigned long)base); TR(b); }
  (void)dpy;
  g_n_eglChooseConfig++;
  { /* Horizon: one-shot dump of Unity's request (their fprintf block) */
    static int request_logged;
    if (!request_logged) {
      request_logged = 1;
      fprintf(stderr,
              "[egl] Unity EGLConfig request rgba=%d/%d/%d/%d depth=%d "
              "stencil=%d samples=%d renderable=0x%x surfaces=0x%x "
              "real_cfg=%p\n",
              hc_egl_attribute_value(attrib_list, HC_EGL_RED_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_GREEN_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_BLUE_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_ALPHA_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_DEPTH_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_STENCIL_SIZE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_SAMPLES, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_RENDERABLE_TYPE, -1),
              hc_egl_attribute_value(attrib_list, HC_EGL_SURFACE_TYPE, -1),
              g_real_cfg);
    }
  }
  if (configs && config_size > 0)
    configs[0] = g_real_cfg
                     ? (EGLConfig)g_real_cfg
                     : (EGLConfig)&g_fallback_config_token;
  if (num_config) *num_config = 1;
  return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                  const EGLint *attrib_list) {
  TR("eglCreateWindowSurface");
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  g_n_eglCreateWindowSurface++;
  EGLSurface s = (EGLSurface)strdup("window");   /* unique token (Horizon) */
  printf("[egl] eglCreateWindowSurface -> %p\n", (void *)s);
  return s; /* the real window lives in SDL; surface is symbolic */
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
  TR("eglCreatePbufferSurface");
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");  /* unique token (Horizon) */
  printf("[egl] eglCreatePbufferSurface -> %p\n", (void *)s);
  return s;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list) {
  { char b[128]; int v=-1; if(attrib_list) for(int i=0; attrib_list[i]!=0x3038; i+=2) if(attrib_list[i]==0x3098) v=attrib_list[i+1]; snprintf(b,sizeof b,"eglCreateContext client_version=%d share=%p",v,(void*)share_context); TR(b); }
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c) return EGL_NO_CONTEXT;
  if (!egl_window || !S.GL_CreateContext) {
    if (g_nullgl) {
      c->sdl_context = NULL;
      c->id = next_context_id++;
      g_n_eglCreateContext++;
      printf("[egl] eglCreateContext -> %p [ctx_id=%d, NullGL]\n", (void *)c, c->id);
      return (EGLContext)c;
    }
    free(c); return EGL_NO_CONTEXT;
  }
  pthread_mutex_lock(&egl_ctx_mutex);
  /* Honor the caller's requested client version (0x3098) when it's a real ES
   * major; Unity asks for 2, workers may ask for 3 -- don't force the wrong
   * one.  Buffer sizes mirror the negotiated share-root window. */
  int want_major = -1;
  if (attrib_list)
    for (int i = 0; attrib_list[i] != 0x3038 && i < 32; i += 2)
      if (attrib_list[i] == 0x3098) want_major = attrib_list[i + 1];
  S.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  S.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, want_major >= 2 && want_major <= 3 ? want_major : g_es_major);
  S.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  /* Horizon's egl_set_ctx_attrs resets the full RGBA8888 contract for every
   * context -- SDL's gl_config is global state, so stale values from any
   * interim user could otherwise leak into Unity's workers. */
  S.GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  S.GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  S.GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  S.GL_SetAttribute(SDL_GL_ALPHA_SIZE, g_alpha_size);
  S.GL_SetAttribute(SDL_GL_DEPTH_SIZE, g_depth_size);
  S.GL_SetAttribute(SDL_GL_STENCIL_SIZE, g_stencil_size);
  S.GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root) S.GL_MakeCurrent(egl_window, egl_share_root);
  c->sdl_context = S.GL_CreateContext(egl_window);
  S.GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  S.GL_MakeCurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_ctx_mutex);
  if (!c->sdl_context) { free(c); return EGL_NO_CONTEXT; }
  c->id = next_context_id++;
  g_n_eglCreateContext++;
  printf("[egl] eglCreateContext -> %p [ctx_id=%d]\n", c, c->id);
  return (EGLContext)c;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  { char b[96]; snprintf(b,sizeof b,"eglMakeCurrent draw=%p ctx=%p",(void*)draw,(void*)ctx); TR(b); }
  (void)dpy; (void)read;
  g_n_eglMakeCurrent++;
  _egl_context *context = (_egl_context *)ctx;
  if (!context || !draw) {
    /* Horizon UNBIND: clears current only; last_context survives so
     * egl_shim_ensure_current can rebind the same context. */
    current_context = NULL; has_real_gl = 0;
    if (egl_window && S.GL_MakeCurrent) S.GL_MakeCurrent(egl_window, NULL);
    return EGL_TRUE;
  }
  /* 0.68 (Horizon surface identity): Android EGL returns the exact surface
   * handles bound by eglMakeCurrent from eglGetCurrentSurface; Unity compares
   * them by handle before Present.  Both our surfaces used to be the same
   * token and GetCurrentSurface returned a third -- identity never held. */
  context->is_pbuffer = (((char *)draw)[0] == 'w') ? 0 : 1;
  context->current_draw = draw;
  context->current_read = read;
  current_context = context;
  last_context = context;
  if (g_nullgl) return EGL_TRUE;
  if (!egl_window || !S.GL_MakeCurrent) return EGL_TRUE;
  if (S.GL_MakeCurrent(egl_window, context->sdl_context) == 0) {
    has_real_gl = 1;
    /* first-bind identity: what the DRIVER gives Unity's worker contexts.
     * The share root's GL_VERSION only describes the bootstrap ctx. */
    static int bind_ver_n = 0;
    if (bind_ver_n < 4 && S.GL_GetProcAddress) {
      const unsigned char *(*g)(unsigned) =
        (const unsigned char *(*)(unsigned))S.GL_GetProcAddress("glGetString");
      if (g) {
        const unsigned char *s = g(0x1F02);
        printf("[egl] worker ctx id=%d bound, GL_VERSION='%s'\n",
               context->id, s ? (const char *)s : "(null)");
        bind_ver_n++;
      }
    }
  } else {
    has_real_gl = 0;
    printf("[egl] eglMakeCurrent failed: %s\n", S.GetError ? S.GetError() : "?");
  }
  if (g_n_eglMakeCurrent <= 4) egl_show_counts("MakeCurrent");
  return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  g_n_eglSwapBuffers++;
  static int swn = 0;
  /* Horizon: only the window-bound context presents; worker (pbuffer) swaps
   * are silent no-ops so uploads don't fight the front buffer. */
  if (has_real_gl && egl_window && S.GL_SwapWindow &&
      current_context && !current_context->is_pbuffer) {
    S.GL_SwapWindow(egl_window);
    if (g_n_eglSwapBuffers <= 3) {
      egl_show_counts("SwapBuffers(real)");
      if (S.GetError && ++swn <= 8) {
        const char *e = S.GetError();
        if (e && *e) printf("[egl] SDL_GL_SwapWindow error: %s\n", e);
      }
    }
  } else {
    if (++swn <= 8) printf("[egl] SwapBuffers SKIPPED (no real GL / window)\n");
    if (g_n_eglSwapBuffers == 1)
      printf("[egl] SwapBuffers skipped details: has_real_gl=%d window=%p ctx=%p pbuffer=%d\n",
             has_real_gl, (void *)egl_window, (void *)current_context,
             current_context ? current_context->is_pbuffer : -1);
  }
  return EGL_TRUE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);   /* strdup'd token */
  return EGL_TRUE;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) { if (context->sdl_context && S.GL_DeleteContext) S.GL_DeleteContext(context->sdl_context); free(context); }
  return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  { char b[96]; snprintf(b,sizeof b,"eglQuerySurface attr=0x%x",(int)attribute); TR(b); }
  (void)dpy; (void)surface;
  if (!value) return EGL_TRUE;
  if (attribute == 0x3057) *value = g_screen_w;      /* EGL_WIDTH */
  else if (attribute == 0x3056) *value = g_screen_h; /* EGL_HEIGHT */
  else *value = 0;
  return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
  { char b[160]; void *ra=__builtin_return_address(0); extern const char *gds_mod_at(const void *, void **); void *base=0; const char *mod=gds_mod_at(ra,&base); snprintf(b,sizeof b,"eglGetConfigAttrib attr=0x%x from %s+%lx",(int)attribute, mod?mod:"?", (unsigned long)ra-(unsigned long)base); TR(b); }
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  /* 0.68: the REAL driver config is the authority for every attribute
   * (Horizon GetConfigAttrib delegation, verbatim).  Unity 2022's minimum-
   * spec matcher reads the full attribute matrix; fabricated answers fail
   * some field the reference ports never had to identify because the real
   * driver answers instead.  Hardcoded switch below remains the qemu/NullGL
   * and no-real-EGL fallback. */
  if (g_real_dpy && g_real_cfg && r_eglGetConfigAttrib &&
      r_eglGetConfigAttrib(g_real_dpy, g_real_cfg, attribute, value))
    return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 24 + g_alpha_size; break;   /* EGL_BUFFER_SIZE */
  case 0x3021: *value = g_alpha_size; break;        /* EGL_ALPHA_SIZE */
  case 0x3022: *value = 8; break;                   /* EGL_BLUE_SIZE */
  case 0x3023: *value = 8; break;                   /* EGL_GREEN_SIZE */
  case 0x3024: *value = 8; break;                   /* EGL_RED_SIZE */
  case 0x3025: *value = g_report_depth; break;
  case 0x3026: *value = g_report_stencil; break;
  case 0x3027: *value = 0x3038; break;              /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;                   /* EGL_CONFIG_ID */
  case 0x3033: *value = 0x0005; break;              /* EGL_SURFACE_TYPE = WINDOW|PBUFFER */
  /* RENDERABLE_TYPE/CONFORMANT: Unity 2022 does a strict bit-subset test
   * against the requested bits (evidence: device 0.66 log -- it asks
   * eglChooseConfig 3040=4 then rejects a config answering 0x40 alone with
   * "Unable to find a configuration matching minimum spec!").  Answer
   * ES3-only-when-ES3 like Horizon and this Unity rejects the config.
   * ES3 hardware is ES2-renderable, so report BOTH bits once ES3 was
   * negotiated: 0x40 (ES3) | 0x04 (ES2) = 0x44. */
  case 0x3040: *value = g_es_major >= 3 ? 0x44 : 0x04; break;
  case 0x3042: *value = g_es_major >= 3 ? 0x44 : 0x04; break;
  /* EGL_DEPTH_ENCODING_NV: a real EGL without the extension returns
   * EGL_BAD_ATTRIBUTE here and Unity's helper substitutes 0x30E3 (NONE).
   * We used to answer 0 -- Unity's config descriptor then recorded
   * "nonlinear NV depth" ([desc+0x38]=0) which a later pure-CPU sanity check
   * rejects, producing "Graphics device is null" with no further EGL calls.
   * Evidence: local qemu run (run3.log) + libunity disasm 0x62aaf0-0x62ab30. */
  case 0x30e2: *value = 0x30e3; break;              /* EGL_DEPTH_ENCODING_NONE_NV */
  case 0x3039: *value = 0x308E; break;              /* EGL_COLOR_BUFFER_TYPE = RGB */
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint eglGetError(void) { return EGL_SUCCESS; }

void *eglGetProcAddress(const char *procname) {
  void *ptr = S.GL_GetProcAddress ? S.GL_GetProcAddress(procname) : NULL;
  if (ptr) return ptr;
  /* GLES may suffix extension names with OES; try the stripped name. */
  size_t len = procname ? strlen(procname) : 0;
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof stripped) {
      memcpy(stripped, procname, len - 3); stripped[len - 3] = 0;
      if (S.GL_GetProcAddress) { ptr = S.GL_GetProcAddress(stripped); if (ptr) return ptr; }
    }
  }
  if (g_nullgl && procname) return nullgl_gl_proc(procname);
  return NULL;
}

EGLBoolean eglBindAPI(unsigned api) { { char b[64]; snprintf(b,sizeof b,"eglBindAPI(0x%x)",api); TR(b); } (void)api; return EGL_TRUE; }

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
  { char b[96]; snprintf(b,sizeof b,"eglQueryString(0x%x)",(int)name); TR(b); }
  (void)dpy;
  switch (name) {
  case 0x3053: return "GDS";             /* EGL_VENDOR */
  case 0x3054: return "1.4 GDS";         /* EGL_VERSION */
  case 0x3055: return "";                /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";       /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  { char b[64]; snprintf(b,sizeof b,"eglSwapInterval(%d)",(int)interval); TR(b); }
  (void)dpy; if (S.GL_SetSwapInterval) S.GL_SetSwapInterval(interval); return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void) { return (EGLContext)current_context; }

/* Horizon: return the exact handle bound by eglMakeCurrent (Unity's
 * pre-Present identity check). */
EGLSurface eglGetCurrentSurface(EGLint readdraw) {
  if (!current_context) return EGL_NO_SURFACE;
  return readdraw == 0x305A /* EGL_READ */
             ? current_context->current_read
             : current_context->current_draw;
}

EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint v) {
  { char b[96]; snprintf(b,sizeof b,"eglSurfaceAttrib a=0x%x v=0x%x",(int)a,(int)v); TR(b); }
  (void)dpy; (void)s; (void)a; (void)v; return EGL_TRUE;
}

/* ---- ANativeWindow shims (libunity calls these on the surface it was handed) ---- */
static int g_anw_w = 640, g_anw_h = 480;
void *ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface; return (void *)&g_anw_w;
}
int32_t ANativeWindow_setBuffersGeometry(void *win, int32_t w, int32_t h, int32_t fmt) {
  (void)win; (void)fmt; if (w > 0) g_anw_w = w; if (h > 0) g_anw_h = h; return 0;
}
int32_t ANativeWindow_getWidth(void *win) { (void)win; return egl_shim_screen_w() > 0 ? egl_shim_screen_w() : g_anw_w; }
int32_t ANativeWindow_getHeight(void *win) { (void)win; return egl_shim_screen_h() > 0 ? egl_shim_screen_h() : g_anw_h; }
int32_t ANativeWindow_getFormat(void *win) { (void)win; return 1; }
void ANativeWindow_acquire(void *win) { (void)win; }
void ANativeWindow_release(void *win) { (void)win; }

/* ---- NDK no-op stubs (ALooper / ASensorManager / ASensor) ----
 * libunity imports these but never uses them on the graphics path; provide
 * them so relocations resolve instead of slotting NULL. */
void *ALooper_forThread(void) { return NULL; }
void *ALooper_prepare(int opts) { (void)opts; return NULL; }
void *ALooper_acquire(void *l) { (void)l; return NULL; }
void ALooper_release(void *l) { (void)l; }
int ALooper_pollOnce(int timeout, int *out, void **events, void **data) {
  (void)timeout; (void)out; (void)events; (void)data; return 0;
}
int ALooper_wake(void *l) { (void)l; return 0; }
void *ASensorManager_getInstance(void) { return NULL; }
void *ASensorManager_createEventQueue(void *m, void *l, void *h, void *d) { (void)m;(void)l;(void)h;(void)d; return NULL; }
int ASensorManager_destroyEventQueue(void *m, void *q) { (void)m;(void)q; return 0; }
void *ASensorManager_getSensorList(void *m, int *n) { (void)m; if (n) *n = 0; return NULL; }
void *ASensorManager_getDefaultSensor(void *m, int t, int f) { (void)m;(void)t;(void)f; return NULL; }
int ASensorManager_getSensorByTypeHandle(void) { return 0; }
int ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
void *ASensorEventQueue_getEvents(void *q, void *e, int n) { (void)q;(void)e;(void)n; return NULL; }
int ASensorEventQueue_enableSensor(void *q, void *s, int rate) { (void)q;(void)s;(void)rate; return 0; }
int ASensorEventQueue_disableSensor(void *q, void *s) { (void)q;(void)s; return 0; }
int ASensorEventQueue_setEventRate(void *q, void *s, long rate) { (void)q;(void)s;(void)rate; return 0; }
int ASensor_getType(void *s) { (void)s; return 0; }
int ASensor_getResolution(void *s) { (void)s; return 0; }
int ASensor_getMinDelay(void *s) { (void)s; return 0; }
const char *ASensor_getName(void *s) { (void)s; return ""; }
const char *ASensor_getVendor(void *s) { (void)s; return ""; }

/* ---- route table: which surface symbols libunity imports, and where they go ---- */
void *kv_egl_route(const char *name) {
  static const struct { const char *n; void *f; } m[] = {
    {"eglGetDisplay", eglGetDisplay}, {"eglInitialize", eglInitialize},
    {"eglTerminate", eglTerminate}, {"eglChooseConfig", eglChooseConfig},
    {"eglCreateWindowSurface", eglCreateWindowSurface},
    {"eglCreatePbufferSurface", eglCreatePbufferSurface},
    {"eglCreateContext", eglCreateContext}, {"eglMakeCurrent", eglMakeCurrent},
    {"eglSwapBuffers", eglSwapBuffers}, {"eglDestroySurface", eglDestroySurface},
    {"eglDestroyContext", eglDestroyContext}, {"eglQuerySurface", eglQuerySurface},
    {"eglGetConfigAttrib", eglGetConfigAttrib}, {"eglGetError", eglGetError},
    {"eglGetProcAddress", eglGetProcAddress}, {"eglBindAPI", eglBindAPI},
    {"eglQueryString", eglQueryString}, {"eglSwapInterval", eglSwapInterval},
    {"eglGetCurrentContext", eglGetCurrentContext},
    {"eglGetCurrentSurface", eglGetCurrentSurface},
    {"eglGetCurrentDisplay", eglGetDisplay},
    {"eglSurfaceAttrib", eglSurfaceAttrib},
    {"ANativeWindow_fromSurface", ANativeWindow_fromSurface},
    {"ANativeWindow_setBuffersGeometry", ANativeWindow_setBuffersGeometry},
    {"ANativeWindow_getWidth", ANativeWindow_getWidth},
    {"ANativeWindow_getHeight", ANativeWindow_getHeight},
    {"ANativeWindow_getFormat", ANativeWindow_getFormat},
    {"ANativeWindow_acquire", ANativeWindow_acquire},
    {"ANativeWindow_release", ANativeWindow_release},
    {"ALooper_forThread", ALooper_forThread}, {"ALooper_prepare", ALooper_prepare},
    {"ALooper_acquire", ALooper_acquire}, {"ALooper_release", ALooper_release},
    {"ALooper_pollOnce", ALooper_pollOnce}, {"ALooper_wake", ALooper_wake},
    {"ASensorManager_getInstance", ASensorManager_getInstance},
    {"ASensorManager_createEventQueue", ASensorManager_createEventQueue},
    {"ASensorManager_destroyEventQueue", ASensorManager_destroyEventQueue},
    {"ASensorManager_getSensorList", ASensorManager_getSensorList},
    {"ASensorManager_getDefaultSensor", ASensorManager_getDefaultSensor},
    {"ASensorEventQueue_hasEvents", ASensorEventQueue_hasEvents},
    {"ASensorEventQueue_getEvents", ASensorEventQueue_getEvents},
    {"ASensorEventQueue_enableSensor", ASensorEventQueue_enableSensor},
    {"ASensorEventQueue_disableSensor", ASensorEventQueue_disableSensor},
    {"ASensorEventQueue_setEventRate", ASensorEventQueue_setEventRate},
    {"ASensor_getType", ASensor_getType}, {"ASensor_getResolution", ASensor_getResolution},
    {"ASensor_getMinDelay", ASensor_getMinDelay}, {"ASensor_getName", ASensor_getName},
    {"ASensor_getVendor", ASensor_getVendor},
    {0, 0}
  };
  for (int i = 0; m[i].n; i++) if (strcmp(m[i].n, name) == 0) return m[i].f;
  return NULL;
}
