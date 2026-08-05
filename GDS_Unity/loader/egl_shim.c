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

/* SDL2 entry points, resolved once via dlopen/dlsym. */
static struct {
  void *h;
  int (*Init)(unsigned);
  const char *(*GetError)(void);
  const char *(*GetCurrentVideoDriver)(void);
  SDL_Window *(*CreateWindow)(const char *, int, int, int, int, unsigned);
  void (*DestroyWindow)(SDL_Window *);
  int (*GL_SetAttribute)(int, int);
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
  S.CreateWindow      = (SDL_Window *(*)(const char *, int, int, int, int, unsigned))sym(h, "SDL_CreateWindow");
  S.DestroyWindow     = (void (*)(SDL_Window *))sym(h, "SDL_DestroyWindow");
  S.GL_SetAttribute   = (int (*)(int, int))sym(h, "SDL_GL_SetAttribute");
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
  return sdl_ok();
}

/* ---- state ---- */
static int g_screen_w = 640, g_screen_h = 480;
static int g_es_major = 2, g_alpha_size = 8, g_depth_size = 24, g_stencil_size = 8;
static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_context_id = 1;
static int g_did_init = 0;

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
  int id;
} _egl_context;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local int has_real_gl = 0;

int egl_shim_screen_w(void) { return g_screen_w; }
int egl_shim_screen_h(void) { return g_screen_h; }

/* ---- window + share-root context creation (must run on the main thread) ---- */
void egl_shim_create_window(void) {
  if (g_did_init) return;
  g_did_init = 1;
  if (!sdl_load()) { printf("[egl] SDL2 unavailable: %s\n", S.GetError ? S.GetError() : "?"); return; }
  /* SDL must be initialized before any window/context call, or SDL_CreateWindow
   * can't set up the video driver / load the EGL/GL library ("Can't load EGL/GL
   * library on window creation").  This is why the first on-device run had
   * "SDL video driver = (null)" and no GL context. */
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

  /* try a few buffer formats, like terraria: {alpha,depth,stencil} */
  static const int fmts[][3] = { {8,24,8}, {0,24,8}, {8,16,0}, {0,16,0}, {0,0,0} };
  for (size_t f = 0; f < sizeof fmts / sizeof fmts[0] && !egl_share_root; f++) {
    S.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    S.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    S.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    S.GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    S.GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    S.GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    S.GL_SetAttribute(SDL_GL_ALPHA_SIZE, fmts[f][0]);
    S.GL_SetAttribute(SDL_GL_DEPTH_SIZE, fmts[f][1]);
    S.GL_SetAttribute(SDL_GL_STENCIL_SIZE, fmts[f][2]);
    S.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    egl_window = S.CreateWindow("Game Dev Story", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, width, height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!egl_window) { printf("[egl] fmt %zu (a%d d%d s%d) failed\n",
                              f, fmts[f][0], fmts[f][1], fmts[f][2]); continue; }
    egl_share_root = S.GL_CreateContext(egl_window);
    if (!egl_share_root) {
      printf("[egl] ctx fmt %zu (a%d d%d s%d) failed: %s\n",
             f, fmts[f][0], fmts[f][1], fmts[f][2], S.GetError());
      S.DestroyWindow(egl_window); egl_window = NULL;
    }
    if (egl_share_root) { g_alpha_size = fmts[f][0]; g_depth_size = fmts[f][1]; g_stencil_size = fmts[f][2]; }
  }
    if (!egl_share_root) { printf("[egl] no GL context created\n"); return; }
    printf("[egl] window=%p context=%p (SDL_CreateWindow may have logged a surface warning)\n",
           (void *)egl_window, (void *)egl_share_root);

    if (S.GL_GetDrawableSize) S.GL_GetDrawableSize(egl_window, &g_screen_w, &g_screen_h);
  if (g_screen_w <= 0) g_screen_w = width;
  if (g_screen_h <= 0) g_screen_h = height;
  if (S.GL_SetSwapInterval) S.GL_SetSwapInterval(1);

  /* log the real GL identity (proves the Mali driver is behind the context) */
  void *gs = S.GL_GetProcAddress ? S.GL_GetProcAddress("glGetString") : NULL;
  if (gs) {
    const unsigned char *(*g)(unsigned) = (const unsigned char *(*)(unsigned))gs;
    printf("[egl] GL_VENDOR=%s\n", g(0x1F00));   /* GL_VENDOR */
    printf("[egl] GL_RENDERER=%s\n", g(0x1F01)); /* GL_RENDERER */
    printf("[egl] GL_VERSION=%s\n", g(0x1F02));  /* GL_VERSION */
  }
  printf("[egl] window %dx%d context ready (ES%d)\n", g_screen_w, g_screen_h, g_es_major);

  /* release from the bootstrap thread; the game binds when it makes current */
  if (S.GL_MakeCurrent) S.GL_MakeCurrent(egl_window, NULL);
}

int egl_shim_ensure_current(void) {
  if (!egl_window || !egl_share_root) return 0;
  if (has_real_gl) return 1;
  if (current_context && current_context->sdl_context) {
    if (S.GL_MakeCurrent(egl_window, current_context->sdl_context) == 0)
      { has_real_gl = 1; return 1; }
  }
  if (S.GL_MakeCurrent(egl_window, egl_share_root) == 0) { has_real_gl = 1; return 1; }
  return 0;
}

void egl_shim_swap(void) { if (egl_window && S.GL_SwapWindow) S.GL_SwapWindow(egl_window); }

/* ---- EGL API ---- */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id; g_n_eglGetDisplay++; return (EGLDisplay)&g_did_init;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
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
  (void)dpy; (void)attrib_list;
  g_n_eglChooseConfig++;
  if (configs && config_size > 0) configs[0] = (EGLConfig)&g_did_init;
  if (num_config) *num_config = 1;
  return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  g_n_eglCreateWindowSurface++;
  return (EGLSurface)&g_did_init; /* the real window lives in SDL; surface is symbolic */
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  return (EGLSurface)&g_did_init;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c) return EGL_NO_CONTEXT;
  if (!egl_window || !S.GL_CreateContext) { free(c); return EGL_NO_CONTEXT; }
  pthread_mutex_lock(&egl_ctx_mutex);
  S.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  S.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_es_major);
  S.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
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
  (void)dpy; (void)read;
  g_n_eglMakeCurrent++;
  _egl_context *context = (_egl_context *)ctx;
  if (!context || !draw || !S.GL_MakeCurrent) {
    current_context = NULL; has_real_gl = 0;
    if (egl_window) S.GL_MakeCurrent(egl_window, NULL);
    return EGL_TRUE;
  }
  current_context = context;
  if (!egl_window) return EGL_TRUE;
  if (S.GL_MakeCurrent(egl_window, context->sdl_context) == 0) {
    has_real_gl = 1;
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
  if (has_real_gl && egl_window && S.GL_SwapWindow) {
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
    if (g_n_eglSwapBuffers == 1) egl_show_counts("SwapBuffers(skipped)");
  }
  return EGL_TRUE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) { (void)dpy; (void)surface; return EGL_TRUE; }

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) { if (context->sdl_context && S.GL_DeleteContext) S.GL_DeleteContext(context->sdl_context); free(context); }
  return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (!value) return EGL_TRUE;
  if (attribute == 0x3057) *value = g_screen_w;      /* EGL_WIDTH */
  else if (attribute == 0x3056) *value = g_screen_h; /* EGL_HEIGHT */
  else *value = 0;
  return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 24 + g_alpha_size; break;   /* EGL_BUFFER_SIZE */
  case 0x3021: *value = g_alpha_size; break;        /* EGL_ALPHA_SIZE */
  case 0x3022: *value = 8; break;                   /* EGL_BLUE_SIZE */
  case 0x3023: *value = 8; break;                   /* EGL_GREEN_SIZE */
  case 0x3024: *value = 8; break;                   /* EGL_RED_SIZE */
  case 0x3025: *value = g_depth_size; break;
  case 0x3026: *value = g_stencil_size; break;
  case 0x3027: *value = 0x3038; break;              /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;                   /* EGL_CONFIG_ID */
  case 0x3033: *value = 0x0005; break;              /* EGL_SURFACE_TYPE = WINDOW|PBUFFER */
  case 0x3040: *value = 0x04; break;                /* EGL_RENDERABLE_TYPE = ES2 */
  case 0x3042: *value = 0x04; break;
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
  return NULL;
}

EGLBoolean eglBindAPI(unsigned api) { (void)api; return EGL_TRUE; }

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
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
  (void)dpy; if (S.GL_SetSwapInterval) S.GL_SetSwapInterval(interval); return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void) { return (EGLContext)current_context; }

EGLSurface eglGetCurrentSurface(EGLint readdraw) { (void)readdraw; return (EGLSurface)&g_did_init; }

EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint v) {
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
