// SPDX-License-Identifier: GPL-2.0-only
// Offscreen EGL/GLES2 shader-cache and compiler-release checks.
// Uses a render node by default; --surfaceless allows a host software-renderer check.
// cc -O2 -Wall -Wextra shader_probe.c -o shader-probe $(pkg-config --cflags --libs egl glesv2 gbm)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "shader-probe failed: %s, GL=%x EGL=%x\n", #x, glGetError(), eglGetError());               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static const char *vs = "attribute vec2 p; void main(){gl_Position=vec4(p,0.0,1.0);}";
static const char *fs = "precision mediump float; void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";
static const char *builtin_fs =
    "precision mediump float; void main(){gl_FragColor=vec4(sin(0.0)+1.0,cos(0.0)-1.0,0.0,1.0);}";
static GLuint shader(GLenum type, const char *source, int valid) {
    GLuint s = glCreateShader(type);
    CHECK(s);
    glShaderSource(s, 1, &source, NULL);
    double t = ms();
    glCompileShader(s);
    printf("compile type=%x valid=%d ms=%.3f\n", type, valid, ms() - t);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    CHECK((ok != 0) == valid);
    return s;
}
static GLuint program(const char *fragment, int attrib) {
    GLuint v = shader(GL_VERTEX_SHADER, vs, 1), f = shader(GL_FRAGMENT_SHADER, fragment, 1);
    GLuint p = glCreateProgram();
    CHECK(p);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, (GLuint)attrib, "p");
    double t = ms();
    glLinkProgram(p);
    printf("link attrib=%d ms=%.3f\n", attrib, ms() - t);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    CHECK(ok);
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}
static void red(GLuint p) {
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(p);
    glViewport(0, 0, 1, 1);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    GLint attrib = glGetAttribLocation(p, "p");
    CHECK(attrib >= 0);
    glVertexAttribPointer((GLuint)attrib, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray((GLuint)attrib);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    CHECK(glGetError() == GL_NO_ERROR);
}
int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [render-node|--surfaceless]\n", argv[0]);
        return 2;
    }
    int host = argc == 2 && !strcmp(argv[1], "--surfaceless");
    int fd = -1;
    struct gbm_device *gbm = NULL;
    if (!host) {
        const char *path = argc == 2 ? argv[1] : "/dev/dri/renderD128";
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror(path); return 1; }
        gbm = gbm_create_device(fd);
        CHECK(gbm);
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);
    EGLDisplay display = get_display(host ? EGL_PLATFORM_SURFACELESS_MESA : EGL_PLATFORM_GBM_KHR,
                                    host ? EGL_DEFAULT_DISPLAY : (void *)gbm, NULL);
    CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL));
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    CHECK(extensions && strstr(extensions, "EGL_KHR_surfaceless_context"));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attrs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                           EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count);
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    GLuint texture, fbo;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glDisable(GL_DITHER);
    printf("GL=%s renderer=%s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    GLuint p = program(fs, 0);
    red(p);
    glReleaseShaderCompiler();
    red(p);
    glDeleteProgram(p);
    p = program(builtin_fs, 0);
    red(p);
    glDeleteProgram(p);
    GLuint bad = shader(GL_FRAGMENT_SHADER, "precision mediump float; void main(){this is not GLSL;}", 0);
    glDeleteShader(bad);
    glReleaseShaderCompiler();
    p = program(fs, 0);
    red(p);
    glDeleteProgram(p);
    // Same cached shader sources, new attribute binding: force the program-cache-miss link fallback.
    glReleaseShaderCompiler();
    p = program(fs, 3);
    red(p);
    glDeleteProgram(p);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display));
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0) close(fd);
    puts("shader-probe PASS: two valid programs/red pixels, cached recompile, invalid source, compiler release, link "
         "fallback");
    return 0;
}
