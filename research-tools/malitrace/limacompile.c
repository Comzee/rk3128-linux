// limacompile: compiles the GLSL our apps gave the Mali blob (captured by malitrace, written out by decode.py as
// <dir>/prog-<n>.vert and .frag) with Mesa's lima compiler (gpir for the vertex shader, ppir for the fragment shader), on
// On the build host, through lima's drm-shim (no Mali needed): LIMA_DEBUG=gp,pp,shaderdb,precompile prints each program's
// disassembly and instruction counts. Run by tools/malitrace/lima-compare.sh (#345). Host tool only.
//   limacompile <dir> <n>...
// Each program is linked once (LIMA_DEBUG=precompile: the compile happens at link); "=== PROGRAM n ===" lines separate
// them on stdout and stderr. The drm-shim must report a Mali-400 MP2: lima-compare.sh patches Mesa's lima_noop.c so that
// LIMA_NOOP_MALI400=1 does (upstream it always reports a Mali-450 MP6).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, (size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    return s;
}

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("compile failed: %s\n", log);
    }
    return s;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: limacompile <dir> <n>...\n"); return 2; }
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) { fprintf(stderr, "limacompile: no EGL display\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        fprintf(stderr, "limacompile: no context\n");
        return 1;
    }
    printf("renderer %s\n", (const char *)glGetString(GL_RENDERER));
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    for (int i = 2; i < argc; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/prog-%s.vert", argv[1], argv[i]);
        char *vs = slurp(p);
        snprintf(p, sizeof(p), "%s/prog-%s.frag", argv[1], argv[i]);
        char *fs = slurp(p);
        if (!vs || !fs) { printf("=== PROGRAM %s === missing source\n", argv[i]); continue; }
        printf("=== PROGRAM %s ===\n", argv[i]);
        fprintf(stderr, "=== PROGRAM %s ===\n", argv[i]);
        GLuint prog = glCreateProgram();
        glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
        glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048] = "";
            glGetProgramInfoLog(prog, sizeof(log), NULL, log);
            printf("link failed: %s\n", log);
            continue;
        }
        // LIMA_DEBUG=precompile compiles at link with the default key; a draw through the no-op drm-shim hangs in its
        // fence wait, so nothing is drawn (our programs need no other variant: no point sprites, no lowered formats)
        fprintf(stderr, "=== END %s ===\n", argv[i]);
        free(vs);
        free(fs);
    }
    return 0;
}
