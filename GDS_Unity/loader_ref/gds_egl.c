/* gds_egl.c -- EGL/GLES table for the GDS loader (reference interface).
 * The device (ArkOS/R36S, Mali-G31) ships libEGL.so/libGLESv2.so and SDL2.
 * Our loader dlopens them RTLD_GLOBAL (see glibc_shims kv_egl_dlopen), so this
 * table resolves the real egl/gl entry points via dlsym(RTLD_DEFAULT) exactly
 * like the reference's raw-EGL path.  gds_egl_init() calls egl_shim_create_window
 * to create the real SDL window + GLES2 context BEFORE Unity's graphics init. */
#define _GNU_SOURCE
#include <stdio.h>
#include "musl_compat.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "nx_elf.h"
#include "gds.h"
#include "egl_shim.h"

/* the loader's window/context creation (egl_shim.c, dlopen-based SDL) */
void egl_shim_create_window(void);
int  egl_shim_ensure_current(void);
void *egl_shim_gl_proc(const char *name);

static nx_import tab[512];
static size_t tab_n;

/* EGL entry points our egl_shim provides directly (they proxy to the real
 * Mali EGL through eglGetProcAddress / dlsym). */
extern EGLDisplay eglGetDisplay(EGLNativeDisplayType);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *,
                                  EGLint, EGLint *);
extern EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig,
                                         EGLNativeWindowType, const EGLint *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
extern EGLBoolean eglTerminate(EGLDisplay);
extern EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
extern EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
extern EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
extern EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
extern EGLint eglGetError(void);
extern void *eglGetProcAddress(const char *);
extern EGLBoolean eglBindAPI(unsigned);
extern const char *eglQueryString(EGLDisplay, EGLint);
extern EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
extern EGLContext eglGetCurrentContext(void);
extern EGLSurface eglGetCurrentSurface(EGLint);
extern EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface, EGLint, EGLint);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);

static void add(const char *name, void *fn) {
    if (!fn) return;
    tab[tab_n++] = (nx_import){ name, fn };
}

void gds_egl_init(void) {
    /* create the real window + GLES2 context first (proven on this device) */
    egl_shim_create_window();
    egl_shim_ensure_current();
    tab_n = 0;
    add("eglGetDisplay", (void *)eglGetDisplay);
    add("eglInitialize", (void *)eglInitialize);
    add("eglTerminate",  (void *)eglTerminate);
    add("eglChooseConfig", (void *)eglChooseConfig);
    add("eglCreateWindowSurface", (void *)eglCreateWindowSurface);
    add("eglCreatePbufferSurface", (void *)eglCreatePbufferSurface);
    add("eglCreateContext", (void *)eglCreateContext);
    add("eglMakeCurrent", (void *)eglMakeCurrent);
    add("eglSwapBuffers", (void *)eglSwapBuffers);
    add("eglDestroySurface", (void *)eglDestroySurface);
    add("eglDestroyContext", (void *)eglDestroyContext);
    add("eglQuerySurface", (void *)eglQuerySurface);
    add("eglGetConfigAttrib", (void *)eglGetConfigAttrib);
    add("eglGetError", (void *)eglGetError);
    add("eglGetProcAddress", (void *)eglGetProcAddress);
    add("eglBindAPI", (void *)eglBindAPI);
    add("eglQueryString", (void *)eglQueryString);
    add("eglSwapInterval", (void *)eglSwapInterval);
    add("eglGetCurrentContext", (void *)eglGetCurrentContext);
    add("eglGetCurrentSurface", (void *)eglGetCurrentSurface);
    add("eglSurfaceAttrib", (void *)eglSurfaceAttrib);
    /* passthrough: any real EGL symbol from the Mali driver */
    static const char *extra[] = {
        "eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative",
        "eglGetConfigs", "eglCreateSyncKHR", "eglDestroySyncKHR",
        "eglClientWaitSyncKHR", "eglGetSyncAttribKHR", "eglSwapInterval",
        "eglPresentationTimeANDROID", "eglDupNativeFenceFDANDROID",
    };
    for (size_t i = 0; i < sizeof extra / sizeof *extra; i++) {
        void *f = dlsym(RTLD_DEFAULT, extra[i]);
        if (f) add(extra[i], f);
    }
    fprintf(stderr, "[egl] table: %zu entries\n", tab_n);
}

const nx_import *gds_egl_table(size_t *n) { *n = tab_n; return tab; }

void *gds_egl_sym(const char *name) {
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].addr;
    return gds_gl_sym(name);
}

void *gds_gl_sym(const char *name) {
    void *f = dlsym(RTLD_DEFAULT, name);
    if (f) return f;
    return eglGetProcAddress(name);   /* egl_shim proxies to SDL_GL_GetProcAddress */
}
