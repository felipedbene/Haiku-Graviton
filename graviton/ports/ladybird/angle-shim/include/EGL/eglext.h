#ifndef DEBEOS_EGL_EGLEXT_H
#define DEBEOS_EGL_EGLEXT_H

// DeBeOS/Haiku arm64 EGL extension header (DeBeOS null GL/EGL shim). Declares
// the EGL extension symbols the Compositor's OpenGLContext.cpp references.
// EGL_NO_CONFIG_KHR is the only one used outside the (DeBeOS-disabled) WebGL
// path -- it is the default value of OpenGLContext::Impl::config. The rest are
// used only inside USE_VULKAN_DMABUF_IMAGES / AK_OS_MACOS blocks, which are not
// compiled on DeBeOS; they are declared so a future real backend builds.

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EGL_NO_CONFIG_KHR ((EGLConfig)0)

// EGL_KHR_image / EGL_EXT_image_dma_buf_import (used only under USE_VULKAN_DMABUF_IMAGES).
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444

typedef EGLBoolean (EGLAPIENTRYP PFNEGLQUERYDMABUFFORMATSEXTPROC)(EGLDisplay dpy, EGLint max_formats, EGLint* formats, EGLint* num_formats);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLQUERYDMABUFMODIFIERSEXTPROC)(EGLDisplay dpy, EGLint format, EGLint max_modifiers, uint64_t* modifiers, EGLBoolean* external_only, EGLint* num_modifiers);

#ifdef __cplusplus
}
#endif

#endif
