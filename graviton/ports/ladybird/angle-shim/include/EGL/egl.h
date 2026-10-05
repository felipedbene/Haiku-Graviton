#ifndef DEBEOS_EGL_EGL_H
#define DEBEOS_EGL_EGL_H

// DeBeOS/Haiku arm64 EGL header — part of the DeBeOS null GL/EGL shim that
// completes Track C's angle-shim. Haiku ships no EGL implementation; these are
// the Khronos EGL 1.5 type/constant/entry-point declarations the Compositor's
// OpenGLContext.cpp references. The matching entry points are no-device null
// implementations in gles_null_backend.cpp: eglGetPlatformDisplay/eglInitialize
// report no display, so a GPU WebGL context never comes up and the page renders
// through the Skia CPU-raster path (which is how all page compositing works on
// DeBeOS regardless). Values are the real Khronos registry values.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EGLAPI
#    define EGLAPI
#endif
#ifndef EGLAPIENTRY
#    define EGLAPIENTRY
#endif
#define EGLAPIENTRYP EGLAPIENTRY*

typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLClientBuffer;
typedef void* EGLImage;
typedef void* EGLNativeDisplayType;

typedef void (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038

#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_IMAGE ((EGLImage)0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)

#define EGL_BACK_BUFFER 0x3084

// Config attribute names (used only inside the WebGL path, which is compiled
// out on DeBeOS; declared for completeness and a future real backend).
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_TEXTURE_2D 0x305F
#define EGL_TEXTURE_RGBA 0x305E
#define EGL_TEXTURE_TARGET 0x3081
#define EGL_TEXTURE_FORMAT 0x3080

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, EGLint const* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config);
EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, EGLint const* attrib_list);
EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void);
EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(char const* procname);
EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
EGLAPI EGLImage EGLAPIENTRY eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, EGLAttrib const* attrib_list);
EGLAPI EGLBoolean EGLAPIENTRY eglDestroyImage(EGLDisplay dpy, EGLImage image);
EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform, void* native_display, EGLAttrib const* attrib_list);
EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, EGLint const* attrib_list);

#ifdef __cplusplus
}
#endif

#endif
