// SPDX-License-Identifier: BSD-2-Clause
//
// GLES2/gl2ext_angle.h -- DeBeOS/Haiku hand-authored declarations for the
// ANGLE-private GLES2 entry points that Ladybird's generated
// LibCompositing/WebGL/GLFunctions.{h,cpp} reference but that are NOT part of
// the Khronos GLES2/GLES3 registry headers (gl2.h, gl2ext.h, gl3.h).
//
// Upstream Ladybird gets this header from an ANGLE checkout. DeBeOS has no
// ANGLE; this file is DeBeOS's own, authored from the Khronos extension specs
// (GL_ANGLE_request_extension, GL_ANGLE_robust_client_memory) and from the
// exact signatures the generator emits from GLFunctions.json. It declares only;
// the entry points are backed by the DeBeOS GLES shim (gles_null_backend.cpp).
//
// Include order note: GLFunctions.{h,cpp} include this AFTER <GLES2/gl2.h> and
// <GLES2/gl2ext.h> but BEFORE <GLES3/gl3.h>. GLint64/GLuint64/GLsync are needed
// here (glGetInteger64vRobustANGLE) and are defined by the Khronos gl2.h, but we
// guard-define them so the header is correct even against a gl2.h variant that
// defers them to gl3.h.

#ifndef INCLUDE_GLES2_GL2EXT_ANGLE_H_
#define INCLUDE_GLES2_GL2EXT_ANGLE_H_

/* gl2.h is expected to have been included first (it brings in khrplatform.h,
   GL_APICALL, GL_APIENTRY, GLenum, GLint, GLsizei, GLuint, GLboolean, GLfloat,
   GLchar). We only backfill the 64-bit/sync types defensively. */
#include <KHR/khrplatform.h>

#ifndef GL_APIENTRYP
#define GL_APIENTRYP GL_APIENTRY*
#endif

/* Backfill the 64-bit integer types used below (glGetInteger64vRobustANGLE
   takes GLint64*). The Khronos gl2.h has defined GLint64/GLuint64 since the
   GLES3 header unification, so in the normal include path these are already
   present; an identical typedef redefinition is well-formed in C11 (6.7p3) and
   C++, making this a safe no-op there and a correct backfill against an older
   gl2.h that defers them to gl3.h. */
typedef khronos_int64_t GLint64;
typedef khronos_uint64_t GLuint64;

#ifdef __cplusplus
extern "C" {
#endif


#ifndef GL_ANGLE_request_extension
#define GL_ANGLE_request_extension 1
#define GL_REQUESTABLE_EXTENSIONS_ANGLE   0x93A8
#define GL_NUM_REQUESTABLE_EXTENSIONS_ANGLE 0x93A9
typedef void (GL_APIENTRYP PFNGLREQUESTEXTENSIONANGLEPROC) (const GLchar *);
typedef void (GL_APIENTRYP PFNGLDISABLEEXTENSIONANGLEPROC) (const GLchar *name);
#ifdef GL_GLEXT_PROTOTYPES
GL_APICALL void GL_APIENTRY glRequestExtensionANGLE (const GLchar * name);
GL_APICALL void GL_APIENTRY glDisableExtensionANGLE (const GLchar *name);
#endif
#endif /* GL_ANGLE_request_extension */

#ifndef GL_ANGLE_texture_rectangle
#define GL_ANGLE_texture_rectangle 1
#define GL_TEXTURE_RECTANGLE_ANGLE        0x84F5
#define GL_TEXTURE_BINDING_RECTANGLE_ANGLE 0x84F6
#define GL_MAX_RECTANGLE_TEXTURE_SIZE_ANGLE 0x84F8
#define GL_SAMPLER_2D_RECT_ANGLE          0x8B63
#endif /* GL_ANGLE_texture_rectangle */

#ifndef GL_ANGLE_robust_client_memory
#define GL_ANGLE_robust_client_memory 1
typedef void (GL_APIENTRYP PFNGLCOMPRESSEDTEXIMAGE2DROBUSTANGLEPROC) (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLCOMPRESSEDTEXIMAGE3DROBUSTANGLEPROC) (GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLCOMPRESSEDTEXSUBIMAGE2DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLCOMPRESSEDTEXSUBIMAGE3DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLGETACTIVEUNIFORMBLOCKIVROBUSTANGLEPROC) (GLuint, GLuint, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETBOOLEANVROBUSTANGLEPROC) (GLenum, GLsizei, GLsizei*, GLboolean*);
typedef void (GL_APIENTRYP PFNGLGETBUFFERPARAMETERIVROBUSTANGLEPROC) (GLenum, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETFLOATVROBUSTANGLEPROC) (GLenum, GLsizei, GLsizei*, GLfloat*);
typedef void (GL_APIENTRYP PFNGLGETINTEGER64VROBUSTANGLEPROC) (GLenum, GLsizei, GLsizei*, GLint64*);
typedef void (GL_APIENTRYP PFNGLGETINTEGERVROBUSTANGLEPROC) (GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETINTERNALFORMATIVROBUSTANGLEPROC) (GLenum, GLenum, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETPROGRAMIVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETQUERYOBJECTUIVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, GLuint*);
typedef void (GL_APIENTRYP PFNGLGETRENDERBUFFERPARAMETERIVROBUSTANGLEPROC) (GLenum, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETSHADERIVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETTEXPARAMETERFVROBUSTANGLEPROC) (GLenum, GLenum, GLsizei, GLsizei*, GLfloat*);
typedef void (GL_APIENTRYP PFNGLGETTEXPARAMETERIVROBUSTANGLEPROC) (GLenum, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLGETVERTEXATTRIBPOINTERVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, void**);
typedef void (GL_APIENTRYP PFNGLGETVERTEXATTRIBFVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, GLfloat*);
typedef void (GL_APIENTRYP PFNGLGETVERTEXATTRIBIVROBUSTANGLEPROC) (GLuint, GLenum, GLsizei, GLsizei*, GLint*);
typedef void (GL_APIENTRYP PFNGLREADPIXELSROBUSTANGLEPROC) (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, GLsizei*, GLsizei*, GLsizei*, void*);
typedef void (GL_APIENTRYP PFNGLTEXIMAGE2DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLTEXIMAGE3DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLTEXSUBIMAGE2DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *);
typedef void (GL_APIENTRYP PFNGLTEXSUBIMAGE3DROBUSTANGLEPROC) (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *);
#ifdef GL_GLEXT_PROTOTYPES
GL_APICALL void GL_APIENTRY glCompressedTexImage2DRobustANGLE (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, GLsizei bufSize, const void * data);
GL_APICALL void GL_APIENTRY glCompressedTexImage3DRobustANGLE (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, GLsizei bufSize, const void * data);
GL_APICALL void GL_APIENTRY glCompressedTexSubImage2DRobustANGLE (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, GLsizei bufSize, const void * data);
GL_APICALL void GL_APIENTRY glCompressedTexSubImage3DRobustANGLE (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, GLsizei bufSize, const void * data);
GL_APICALL void GL_APIENTRY glGetActiveUniformBlockivRobustANGLE (GLuint program, GLuint uniformBlockIndex, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetBooleanvRobustANGLE (GLenum pname, GLsizei bufSize, GLsizei* length, GLboolean* data);
GL_APICALL void GL_APIENTRY glGetBufferParameterivRobustANGLE (GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetFloatvRobustANGLE (GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* data);
GL_APICALL void GL_APIENTRY glGetInteger64vRobustANGLE (GLenum pname, GLsizei bufSize, GLsizei* length, GLint64* data);
GL_APICALL void GL_APIENTRY glGetIntegervRobustANGLE (GLenum pname, GLsizei bufSize, GLsizei* length, GLint* data);
GL_APICALL void GL_APIENTRY glGetInternalformativRobustANGLE (GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetProgramivRobustANGLE (GLuint program, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetQueryObjectuivRobustANGLE (GLuint id, GLenum pname, GLsizei bufSize, GLsizei* length, GLuint* params);
GL_APICALL void GL_APIENTRY glGetRenderbufferParameterivRobustANGLE (GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetShaderivRobustANGLE (GLuint shader, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetTexParameterfvRobustANGLE (GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* params);
GL_APICALL void GL_APIENTRY glGetTexParameterivRobustANGLE (GLenum target, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glGetVertexAttribPointervRobustANGLE (GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, void** pointer);
GL_APICALL void GL_APIENTRY glGetVertexAttribfvRobustANGLE (GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, GLfloat* params);
GL_APICALL void GL_APIENTRY glGetVertexAttribivRobustANGLE (GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* params);
GL_APICALL void GL_APIENTRY glReadPixelsRobustANGLE (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels);
GL_APICALL void GL_APIENTRY glTexImage2DRobustANGLE (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, GLsizei bufSize, const void * pixels);
GL_APICALL void GL_APIENTRY glTexImage3DRobustANGLE (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, GLsizei bufSize, const void * pixels);
GL_APICALL void GL_APIENTRY glTexSubImage2DRobustANGLE (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, const void * pixels);
GL_APICALL void GL_APIENTRY glTexSubImage3DRobustANGLE (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, const void * pixels);
#endif
#endif /* GL_ANGLE_robust_client_memory */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_GLES2_GL2EXT_ANGLE_H_ */
