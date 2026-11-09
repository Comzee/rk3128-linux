/* SPDX-License-Identifier: MIT
 * Off-screen GLES2 comparison for #620: cached NV12 CPU -> texture -> packed
 * NV12 RGBA FBO -> CPU. It never opens a KMS card or owns the display.
 * cc -O2 -Wall -Wextra -Werror tools/iep-gpu-bench.c -o iep-gpu-bench \
 *    $(pkg-config --cflags --libs egl glesv2 gbm)
 */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void require(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "%s: EGL=%x GL=%x\n", what, eglGetError(), glGetError()); exit(1); }
}
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static GLuint shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
    }
    require(ok, "compile shader");
    return s;
}
static GLuint texture(unsigned w, unsigned h, GLenum format, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, GL_UNSIGNED_BYTE, data);
    require(glGetError() == GL_NO_ERROR, "texture allocation");
    return t;
}
/* Mali-400 fragment shaders are mediump. Sample three 480x540 packed
 * RGBA tiles so every texture coordinate stays below the 1024-pixel
 * precision limit. Render them into successive rows of the packed NV12 FBO.
 */
static void draw(const GLuint input[3], GLint add, unsigned brightness)
{
    for (unsigned tile = 0; tile < 3; tile++) {
        glBindTexture(GL_TEXTURE_2D, input[tile]);
        glViewport(0, tile * 540, 480, 540);
        glUniform1f(add, tile < 2 ? brightness / 255.f : 0.f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}
__attribute__((noinline)) static void cpu(const uint8_t *src, uint8_t *dst,
                                         size_t ysize, size_t size, unsigned brightness)
{
    if (!brightness) { memcpy(dst, src, size); return; }
    for (size_t i = 0; i < ysize; i++) {
        unsigned v = src[i] + brightness;
        dst[i] = v > 255 ? 255 : v;
    }
    memcpy(dst + ysize, src + ysize, size - ysize);
}
int main(int argc, char **argv)
{
    if (argc != 1) { fprintf(stderr, "usage: %s (finite 1920x1080 copy/brightness benchmark)\n", argv[0]); return 2; }
    const unsigned w = 1920, h = 1080, rows = h * 3 / 2, rounds = 4;
    const size_t size = (size_t)w * rows, ysize = (size_t)w * h;
    uint8_t *src = malloc(size), *dst = malloc(size), *ref = malloc(size);
    require(src && dst && ref, "CPU allocation");
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < w; x++)
            src[(size_t)y * w + x] = y < h ? 40 + ((x / 8 + y / 8) % 12) * 12 :
                96 + ((x & 1 ? y - h : x) / 8 % 4) * 16;
    memset(dst, 0, size);
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("renderD128"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    require(gbm != NULL, "gbm_create_device");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    require(get_display != NULL, "EGL_EXT_platform_base");
    EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    require(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL), "eglInitialize");
    require(strstr(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context") != NULL,
            "EGL_KHR_surfaceless_context");
    require(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
    const EGLint attrs[] = { EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count;
    require(eglChooseConfig(dpy, attrs, &config, 1, &count) && count, "eglChooseConfig");
    const EGLint ctxattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxattrs);
    require(ctx != EGL_NO_CONTEXT, "eglCreateContext");
    require(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "eglMakeCurrent");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    require(renderer && strstr(renderer, "Mali400"), "require the board's Mali400 renderer");
    fprintf(stderr, "renderer=%s; packed NV12, no KMS/display access\n", renderer);
    const char *vs = "attribute vec2 pos;varying mediump vec2 uv;"
        "void main(){gl_Position=vec4(pos,0.,1.);uv=pos*.5+.5;}";
    const char *fs = "precision mediump float;varying vec2 uv;"
        "uniform sampler2D src;uniform float add;"
        "void main(){gl_FragColor=clamp(texture2D(src,uv)+vec4(add),0.,1.);}";
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    GLuint program = glCreateProgram();
    glAttachShader(program, v); glAttachShader(program, f);
    glBindAttribLocation(program, 0, "pos");
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "link shader");
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "src"), 0);
    GLint add = glGetUniformLocation(program, "add");
    require(add >= 0, "brightness uniform");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLuint input[3];
    for (unsigned tile = 0; tile < 3; tile++)
        input[tile] = texture(w / 4, 540, GL_RGBA, src + (size_t)tile * w * 540);
    GLuint output = texture(w / 4, rows, GL_RGBA, NULL);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "FBO");
    const GLfloat positions[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glEnableVertexAttribArray(0);
    glDisable(GL_BLEND); glDisable(GL_DITHER);
    bool pass = true;
    for (unsigned op = 0; op < 2; op++) {
        unsigned brightness = op ? 12 : 0;
        draw(input, add, brightness);
        glFinish(); /* shader compilation, faults and upload outside timer */
        uint64_t start = ns();
        for (unsigned n = 0; n < rounds; n++) {
            draw(input, add, brightness);
            glFinish();
        }
        double draw_us = (ns() - start) / (rounds * 1000.0);
        start = ns();
        for (unsigned n = 0; n < rounds; n++) {
            draw(input, add, brightness);
            glReadPixels(0, 0, w / 4, rows, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        }
        double readback_us = (ns() - start) / (rounds * 1000.0);
        require(glGetError() == GL_NO_ERROR, "benchmark draws/readback");
        cpu(src, ref, ysize, size, brightness);
        start = ns();
        for (unsigned n = 0; n < rounds; n++) cpu(src, ref, ysize, size, brightness);
        double cpu_us = (ns() - start) / (rounds * 1000.0);
        unsigned max_error = 0;
        for (size_t i = 0; i < size; i++) {
            unsigned error = abs((int)dst[i] - ref[i]);
            if (error > max_error) max_error = error;
        }
        bool ok = max_error <= (op ? 1u : 0u);
        pass &= ok;
        printf("{\"op\":\"%s\",\"width\":%u,\"height\":%u,\"frames\":%u,"
               "\"gpu_draw_us\":%.3f,\"gpu_draw_readback_us\":%.3f,\"cpu_cached_us\":%.3f,"
               "\"max_error\":%u,\"pass\":%s}\n", op ? "brightness" : "copy", w, h,
               rounds, draw_us, readback_us, cpu_us, max_error, ok ? "true" : "false");
    }
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(3, input); glDeleteTextures(1, &output);
    glDeleteProgram(program); glDeleteShader(v); glDeleteShader(f);
    require(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release context");
    eglDestroyContext(dpy, ctx); eglTerminate(dpy);
    gbm_device_destroy(gbm); close(fd);
    free(src); free(dst); free(ref);
    return pass ? 0 : 1;
}
