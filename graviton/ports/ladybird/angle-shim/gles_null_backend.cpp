// SPDX-License-Identifier: BSD-2-Clause
//
// gles_null_backend.cpp -- DeBeOS/Haiku null GLES2/GLES3 backend.
//
// DeBeOS headless arm64 ships no GL, GLES or EGL implementation (no Mesa
// libGLESv2, no libEGL, no llvmpipe GLES path). Upstream Ladybird resolves the
// GL entry points its generated Compositor code calls against ANGLE's
// libGLESv2; with no ANGLE and no system GLES, those symbols are unresolved at
// the Compositor link.
//
// This file provides every GLES2/GLES3 (+ ANGLE-private) entry point that the
// generated LibCompositing/WebGL/GLFunctions.cpp references, as a NULL backend:
// a correct implementation for a platform with no GL device. The EGL side
// (gles_null_backend reports no display, see the EGL block below) makes WebGL
// context creation fail cleanly, so these GL entry points are never reached at
// runtime -- they exist to let the Compositor link honestly. A normal page,
// which uses no WebGL, renders through the CPU raster (Skia) path unaffected.
//
// Real hardware/software-rasterised WebGL on DeBeOS requires a genuine
// GLES2/GLES3 + EGL backend (an ANGLE port, or a Mesa GLES/EGL build for Haiku
// arm64). That is a separate, larger effort; this backend is the honest
// no-device behaviour until it lands.

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
extern "C" {

void glActiveTexture(GLenum) {  }
void glAttachShader(GLuint, GLuint) {  }
void glBeginQuery(GLenum, GLuint) {  }
void glBeginTransformFeedback(GLenum) {  }
void glBindAttribLocation(GLuint, GLuint, const GLchar *) {  }
void glBindBuffer(GLenum, GLuint) {  }
void glBindBufferBase(GLenum, GLuint, GLuint) {  }
void glBindBufferRange(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) {  }
void glBindFramebuffer(GLenum, GLuint) {  }
void glBindRenderbuffer(GLenum, GLuint) {  }
void glBindSampler(GLuint, GLuint) {  }
void glBindTexture(GLenum, GLuint) {  }
void glBindTransformFeedback(GLenum, GLuint) {  }
void glBindVertexArray(GLuint) {  }
void glBindVertexArrayOES(GLuint) {  }
void glBlendColor(GLfloat, GLfloat, GLfloat, GLfloat) {  }
void glBlendEquation(GLenum) {  }
void glBlendEquationSeparate(GLenum, GLenum) {  }
void glBlendFunc(GLenum, GLenum) {  }
void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum) {  }
void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {  }
void glBufferData(GLenum, GLsizeiptr, const void *, GLenum) {  }
void glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void *) {  }
GLenum glCheckFramebufferStatus(GLenum) { return 0; }
void glClear(GLbitfield) {  }
void glClearBufferfi(GLenum, GLint, GLfloat, GLint) {  }
void glClearBufferfv(GLenum, GLint, const GLfloat *) {  }
void glClearBufferiv(GLenum, GLint, const GLint *) {  }
void glClearBufferuiv(GLenum, GLint, const GLuint *) {  }
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {  }
void glClearDepthf(GLfloat) {  }
void glClearStencil(GLint) {  }
GLenum glClientWaitSync(GLsync, GLbitfield, GLuint64) { return 0; }
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean) {  }
void glCompileShader(GLuint) {  }
void glCompressedTexImage2DRobustANGLE(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, GLsizei, const void *) {  }
void glCompressedTexImage3DRobustANGLE(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, GLsizei, const void *) {  }
void glCompressedTexSubImage2DRobustANGLE(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, GLsizei, const void *) {  }
void glCompressedTexSubImage3DRobustANGLE(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, GLsizei, const void *) {  }
void glCopyBufferSubData(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr) {  }
void glCopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) {  }
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {  }
GLuint glCreateProgram(void) { return 0; }
GLuint glCreateShader(GLenum) { return 0; }
void glCullFace(GLenum) {  }
void glDeleteBuffers(GLsizei, const GLuint *) {  }
void glDeleteFramebuffers(GLsizei, const GLuint *) {  }
void glDeleteProgram(GLuint) {  }
void glDeleteQueries(GLsizei, const GLuint *) {  }
void glDeleteRenderbuffers(GLsizei, const GLuint *) {  }
void glDeleteSamplers(GLsizei, const GLuint *) {  }
void glDeleteShader(GLuint) {  }
void glDeleteSync(GLsync) {  }
void glDeleteTextures(GLsizei, const GLuint *) {  }
void glDeleteTransformFeedbacks(GLsizei, const GLuint *) {  }
void glDeleteVertexArrays(GLsizei, const GLuint *) {  }
void glDeleteVertexArraysOES(GLsizei, const GLuint *) {  }
void glDepthFunc(GLenum) {  }
void glDepthMask(GLboolean) {  }
void glDepthRangef(GLfloat, GLfloat) {  }
void glDetachShader(GLuint, GLuint) {  }
void glDisable(GLenum) {  }
void glDisableVertexAttribArray(GLuint) {  }
void glDrawArrays(GLenum, GLint, GLsizei) {  }
void glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei) {  }
void glDrawArraysInstancedANGLE(GLenum, GLint, GLsizei, GLsizei) {  }
void glDrawBuffers(GLsizei, const GLenum *) {  }
void glDrawBuffersEXT(GLsizei, const GLenum *) {  }
void glDrawElements(GLenum, GLsizei, GLenum, const void *) {  }
void glDrawElementsInstanced(GLenum, GLsizei, GLenum, const void *, GLsizei) {  }
void glDrawElementsInstancedANGLE(GLenum, GLsizei, GLenum, const void *, GLsizei) {  }
void glDrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *) {  }
void glEnable(GLenum) {  }
void glEnableVertexAttribArray(GLuint) {  }
void glEndQuery(GLenum) {  }
void glEndTransformFeedback(void) {  }
GLsync glFenceSync(GLenum, GLbitfield) { return nullptr; }
void glFinish(void) {  }
void glFlush(void) {  }
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {  }
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {  }
void glFramebufferTextureLayer(GLenum, GLenum, GLuint, GLint, GLint) {  }
void glFrontFace(GLenum) {  }
void glGenBuffers(GLsizei, GLuint*) {  }
void glGenFramebuffers(GLsizei, GLuint*) {  }
void glGenQueries(GLsizei, GLuint*) {  }
void glGenRenderbuffers(GLsizei, GLuint*) {  }
void glGenSamplers(GLsizei, GLuint*) {  }
void glGenTextures(GLsizei, GLuint*) {  }
void glGenTransformFeedbacks(GLsizei, GLuint*) {  }
void glGenVertexArrays(GLsizei, GLuint*) {  }
void glGenVertexArraysOES(GLsizei, GLuint*) {  }
void glGenerateMipmap(GLenum) {  }
void glGetActiveAttrib(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*) {  }
void glGetActiveUniform(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*) {  }
void glGetActiveUniformBlockName(GLuint, GLuint, GLsizei, GLsizei*, GLchar*) {  }
void glGetActiveUniformBlockivRobustANGLE(GLuint, GLuint, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetActiveUniformsiv(GLuint, GLsizei, const GLuint *, GLenum, GLint*) {  }
GLint glGetAttribLocation(GLuint, const GLchar *) { return 0; }
void glGetBooleanvRobustANGLE(GLenum, GLsizei, GLsizei*, GLboolean*) {  }
void glGetBufferParameterivRobustANGLE(GLenum, GLenum, GLsizei, GLsizei*, GLint*) {  }
GLenum glGetError(void) { return 0; }
void glGetFloatvRobustANGLE(GLenum, GLsizei, GLsizei*, GLfloat*) {  }
void glGetInteger64vRobustANGLE(GLenum, GLsizei, GLsizei*, GLint64*) {  }
void glGetIntegervRobustANGLE(GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetInternalformativRobustANGLE(GLenum, GLenum, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {  }
void glGetProgramiv(GLuint, GLenum, GLint*) {  }
void glGetProgramivRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetQueryObjectuivRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, GLuint*) {  }
void glGetRenderbufferParameterivRobustANGLE(GLenum, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {  }
void glGetShaderPrecisionFormat(GLenum, GLenum, GLint*, GLint*) {  }
void glGetShaderSource(GLuint, GLsizei, GLsizei*, GLchar*) {  }
void glGetShaderiv(GLuint, GLenum, GLint*) {  }
void glGetShaderivRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, GLint*) {  }
const GLubyte * glGetString(GLenum) { return reinterpret_cast<const GLubyte*>(""); }
void glGetSynciv(GLsync, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glGetTexParameterfvRobustANGLE(GLenum, GLenum, GLsizei, GLsizei*, GLfloat*) {  }
void glGetTexParameterivRobustANGLE(GLenum, GLenum, GLsizei, GLsizei*, GLint*) {  }
GLuint glGetUniformBlockIndex(GLuint, const GLchar *) { return 0; }
void glGetUniformIndices(GLuint, GLsizei, const GLchar *const*, GLuint*) {  }
GLint glGetUniformLocation(GLuint, const GLchar *) { return 0; }
void glGetVertexAttribPointervRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, void**) {  }
void glGetVertexAttribfvRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, GLfloat*) {  }
void glGetVertexAttribivRobustANGLE(GLuint, GLenum, GLsizei, GLsizei*, GLint*) {  }
void glHint(GLenum, GLenum) {  }
void glInvalidateFramebuffer(GLenum, GLsizei, const GLenum *) {  }
void glInvalidateSubFramebuffer(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei) {  }
GLboolean glIsBuffer(GLuint) { return 0; }
GLboolean glIsEnabled(GLenum) { return 0; }
GLboolean glIsFramebuffer(GLuint) { return 0; }
GLboolean glIsProgram(GLuint) { return 0; }
GLboolean glIsRenderbuffer(GLuint) { return 0; }
GLboolean glIsShader(GLuint) { return 0; }
GLboolean glIsTexture(GLuint) { return 0; }
GLboolean glIsVertexArray(GLuint) { return 0; }
GLboolean glIsVertexArrayOES(GLuint) { return 0; }
void glLineWidth(GLfloat) {  }
void glLinkProgram(GLuint) {  }
void* glMapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) { return nullptr; }
void glPauseTransformFeedback(void) {  }
void glPixelStorei(GLenum, GLint) {  }
void glPolygonOffset(GLfloat, GLfloat) {  }
void glReadBuffer(GLenum) {  }
void glReadPixelsRobustANGLE(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, GLsizei*, GLsizei*, GLsizei*, void*) {  }
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {  }
void glRenderbufferStorageMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {  }
void glRequestExtensionANGLE(const GLchar *) {  }
void glResumeTransformFeedback(void) {  }
void glSampleCoverage(GLfloat, GLboolean) {  }
void glSamplerParameterf(GLuint, GLenum, GLfloat) {  }
void glSamplerParameteri(GLuint, GLenum, GLint) {  }
void glScissor(GLint, GLint, GLsizei, GLsizei) {  }
void glShaderSource(GLuint, GLsizei, const GLchar *const*, const GLint *) {  }
void glStencilFunc(GLenum, GLint, GLuint) {  }
void glStencilFuncSeparate(GLenum, GLenum, GLint, GLuint) {  }
void glStencilMask(GLuint) {  }
void glStencilMaskSeparate(GLenum, GLuint) {  }
void glStencilOp(GLenum, GLenum, GLenum) {  }
void glStencilOpSeparate(GLenum, GLenum, GLenum, GLenum) {  }
void glTexImage2DRobustANGLE(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *) {  }
void glTexImage3DRobustANGLE(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *) {  }
void glTexParameterf(GLenum, GLenum, GLfloat) {  }
void glTexParameteri(GLenum, GLenum, GLint) {  }
void glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {  }
void glTexStorage3D(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei) {  }
void glTexSubImage2DRobustANGLE(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *) {  }
void glTexSubImage3DRobustANGLE(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *) {  }
void glTransformFeedbackVaryings(GLuint, GLsizei, const GLchar *const*, GLenum) {  }
void glUniform1f(GLint, GLfloat) {  }
void glUniform1fv(GLint, GLsizei, const GLfloat *) {  }
void glUniform1i(GLint, GLint) {  }
void glUniform1iv(GLint, GLsizei, const GLint *) {  }
void glUniform1ui(GLint, GLuint) {  }
void glUniform1uiv(GLint, GLsizei, const GLuint *) {  }
void glUniform2f(GLint, GLfloat, GLfloat) {  }
void glUniform2fv(GLint, GLsizei, const GLfloat *) {  }
void glUniform2i(GLint, GLint, GLint) {  }
void glUniform2iv(GLint, GLsizei, const GLint *) {  }
void glUniform2ui(GLint, GLuint, GLuint) {  }
void glUniform2uiv(GLint, GLsizei, const GLuint *) {  }
void glUniform3f(GLint, GLfloat, GLfloat, GLfloat) {  }
void glUniform3fv(GLint, GLsizei, const GLfloat *) {  }
void glUniform3i(GLint, GLint, GLint, GLint) {  }
void glUniform3iv(GLint, GLsizei, const GLint *) {  }
void glUniform3ui(GLint, GLuint, GLuint, GLuint) {  }
void glUniform3uiv(GLint, GLsizei, const GLuint *) {  }
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat) {  }
void glUniform4fv(GLint, GLsizei, const GLfloat *) {  }
void glUniform4i(GLint, GLint, GLint, GLint, GLint) {  }
void glUniform4iv(GLint, GLsizei, const GLint *) {  }
void glUniform4ui(GLint, GLuint, GLuint, GLuint, GLuint) {  }
void glUniform4uiv(GLint, GLsizei, const GLuint *) {  }
void glUniformBlockBinding(GLuint, GLuint, GLuint) {  }
void glUniformMatrix2fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix2x3fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix2x4fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix3fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix3x2fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix3x4fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix4x2fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
void glUniformMatrix4x3fv(GLint, GLsizei, GLboolean, const GLfloat *) {  }
GLboolean glUnmapBuffer(GLenum) { return 0; }
void glUseProgram(GLuint) {  }
void glValidateProgram(GLuint) {  }
void glVertexAttrib1f(GLuint, GLfloat) {  }
void glVertexAttrib1fv(GLuint, const GLfloat *) {  }
void glVertexAttrib2f(GLuint, GLfloat, GLfloat) {  }
void glVertexAttrib2fv(GLuint, const GLfloat *) {  }
void glVertexAttrib3f(GLuint, GLfloat, GLfloat, GLfloat) {  }
void glVertexAttrib3fv(GLuint, const GLfloat *) {  }
void glVertexAttrib4f(GLuint, GLfloat, GLfloat, GLfloat, GLfloat) {  }
void glVertexAttrib4fv(GLuint, const GLfloat *) {  }
void glVertexAttribDivisor(GLuint, GLuint) {  }
void glVertexAttribDivisorANGLE(GLuint, GLuint) {  }
void glVertexAttribI4i(GLuint, GLint, GLint, GLint, GLint) {  }
void glVertexAttribI4iv(GLuint, const GLint *) {  }
void glVertexAttribI4ui(GLuint, GLuint, GLuint, GLuint, GLuint) {  }
void glVertexAttribI4uiv(GLuint, const GLuint *) {  }
void glVertexAttribIPointer(GLuint, GLint, GLenum, GLsizei, const void *) {  }
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) {  }
void glViewport(GLint, GLint, GLsizei, GLsizei) {  }
void glWaitSync(GLsync, GLbitfield, GLuint64) {  }

} // extern "C"

// --- EGL no-device null backend (DeBeOS/Haiku has no EGL implementation) ---
// eglGetPlatformDisplay/eglInitialize report no display, so the Compositor
// WebGL-GPU path (OpenGLContext) never comes up and pages render via Skia CPU
// raster. Prototypes come from the authored EGL/*.h above (keeps -Werror happy).
extern "C" {

EGLDisplay eglGetPlatformDisplay(EGLenum, void*, EGLAttrib const*) { return EGL_NO_DISPLAY; }
EGLBoolean eglInitialize(EGLDisplay, EGLint* major, EGLint* minor) { if (major) *major = 0; if (minor) *minor = 0; return EGL_FALSE; }
EGLBoolean eglChooseConfig(EGLDisplay, EGLint const*, EGLConfig*, EGLint, EGLint* num_config) { if (num_config) *num_config = 0; return EGL_FALSE; }
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint* value) { if (value) *value = 0; return EGL_FALSE; }
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, EGLint const*) { return EGL_NO_CONTEXT; }
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { return EGL_TRUE; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return EGL_FALSE; }
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, EGLint const*) { return EGL_NO_SURFACE; }
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return EGL_TRUE; }
EGLBoolean eglBindTexImage(EGLDisplay, EGLSurface, EGLint) { return EGL_FALSE; }
EGLBoolean eglReleaseTexImage(EGLDisplay, EGLSurface, EGLint) { return EGL_FALSE; }
EGLImage eglCreateImage(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, EGLAttrib const*) { return EGL_NO_IMAGE; }
EGLBoolean eglDestroyImage(EGLDisplay, EGLImage) { return EGL_TRUE; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(char const*) { return nullptr; }
void eglWaitUntilWorkScheduledANGLE(EGLDisplay) { }

}
