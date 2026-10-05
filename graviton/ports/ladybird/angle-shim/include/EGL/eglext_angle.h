#ifndef DEBEOS_EGL_EGLEXT_ANGLE_H
#define DEBEOS_EGL_EGLEXT_ANGLE_H

// DeBeOS/Haiku arm64 ANGLE-private EGL extension header (DeBeOS null GL/EGL
// shim). These ANGLE enums are not in the Khronos EGL registry. On DeBeOS the
// WebGL/ANGLE path is compiled out (ENABLE_WEBGL is undefined -- Haiku has no
// GL device), so none of these are referenced in a DeBeOS build; they are given
// distinct dummy values SOLELY to satisfy the compiler should a future real
// ANGLE backend enable that path. The no-device null backend never acts on any
// of them and no behavior branches on their value.

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- ANGLE platform/display enums (unused by the no-device null backend) ---
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE 0x348F
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

// --- ANGLE context-creation attributes (unused by the no-device null backend) ---
#define EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE 0x33AC
#define EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE 0x3453
#define EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE 0x3451
#define EGL_EXTENSIONS_ENABLED_ANGLE 0x345F

// --- ANGLE IOSurface/texture enums (macOS-only path; unused on DeBeOS) ---
#define EGL_BIND_TO_TEXTURE_TARGET_ANGLE 0x348D
#define EGL_TEXTURE_RECTANGLE_ANGLE 0x345B
#define EGL_IOSURFACE_PLANE_ANGLE 0x345C
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#define EGL_TEXTURE_TYPE_ANGLE 0x345E
#define EGL_IOSURFACE_ANGLE 0x3454

// eglWaitUntilWorkScheduledANGLE: macOS-only in OpenGLContext.cpp (unused on
// DeBeOS). Declared + null-implemented for a future real backend.
EGLAPI void EGLAPIENTRY eglWaitUntilWorkScheduledANGLE(EGLDisplay dpy);

#ifdef __cplusplus
}
#endif

#endif
