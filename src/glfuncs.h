// The OpenGL functions the game uses that are newer than version 1.1 (Windows only).
// Linux's GL library exports every function, so there they are called directly. On Windows they are looked up
// when the context is made (gfx_win32.c). A new GL call that is missing here still builds on Linux but not on
// Windows: add it to this list, with its PFN...PROC type from GL/glext.h.
#ifndef GLFUNCS_H
#define GLFUNCS_H
#define GL_FUNCS(X)                                                    \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                           \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                               \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                     \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                     \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                     \
    X(PFNGLBUFFERDATAPROC, glBufferData)                               \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                         \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                         \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                         \
    X(PFNGLCREATESHADERPROC, glCreateShader)                           \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)               \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                           \
    X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced)             \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                             \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)     \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)           \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                               \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                     \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                     \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                 \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                           \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                   \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                             \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)               \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                             \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                           \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)                           \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                 \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv)                               \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                 \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                 \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                 \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                 \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                               \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                   \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                   \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                               \
    X(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor)             \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define GL_FUNC_DECLARE(type, name) extern type name;
GL_FUNCS(GL_FUNC_DECLARE)
#undef GL_FUNC_DECLARE
#endif
