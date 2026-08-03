/* egl_shim.h -- EGL/GLES + ANativeWindow shim backed by the real SDL2 on the
 * device (glibc build).  Modeled on terraria-nextos's src/egl_shim.c, but SDL2
 * is reached through dlopen/dlsym instead of being linked, so the zig build
 * needs no SDL2 dev headers (ArkOS ships SDL2 at runtime).
 *
 * GDS's libunity.so imports exactly this surface (verified via nm on the
 * committed .so):
 *   egl*           (20+ symbols: eglGetDisplay..eglSwapBuffers)
 *   ANativeWindow* (fromSurface/getWidth/getHeight/setBuffersGeometry/acquire/release)
 *   ALooper*, ASensor*   (can be no-op)
 * Unity resolves its GL entry points through eglGetProcAddress, so making that
 * return real SDL_GL_GetProcAddress pointers (which reach the Mali driver that
 * the loader dlopens RTLD_GLOBAL) is what lets nativeRecreateGfxState succeed.
 */
#ifndef EGL_SHIM_H
#define EGL_SHIM_H

#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef unsigned EGLBoolean;
typedef int32_t EGLint;

#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_SUCCESS 0x3000
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)

#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

/* ---- window / context lifecycle (called by the loader before graphics init) */
void egl_shim_create_window(void);
int egl_shim_ensure_current(void);
int egl_shim_screen_w(void);
int egl_shim_screen_h(void);
void egl_shim_swap(void);

/* ---- route table: return the shim function for a surface symbol, else NULL */
void *kv_egl_route(const char *name);

/* ---- EGL API (exported non-static so the route table can reference them) */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglTerminate(EGLDisplay dpy);
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attrib_list);
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list);
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx);
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
                           EGLint attribute, EGLint *value);
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                              EGLint attribute, EGLint *value);
EGLint eglGetError(void);
void *eglGetProcAddress(const char *procname);
EGLBoolean eglBindAPI(unsigned api);
const char *eglQueryString(EGLDisplay dpy, EGLint name);
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval);
EGLContext eglGetCurrentContext(void);
EGLSurface eglGetCurrentSurface(EGLint readdraw);
EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint v);

#endif /* EGL_SHIM_H */
