// LD_PRELOAD for the gpu test (hardware change 472): eglSwapBuffers reads the back buffer back (glReadPixels, RGBA)
// before it swaps and prints `FRAMEHASH <frame> <w>x<h> <fnv1a-64>` to stderr each time the hash differs from the last
// one, so a frozen Demo shows one line per held time when what it renders is stable, and more when it is not. This is the
// picture the GPU wrote, apart from anything the display server does with it (its --shot reads what is on the screen).
// FRAMEHASH_EVERY=n hashes every nth frame (default 1). Never part of an image.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    static EGLBoolean (*real)(EGLDisplay, EGLSurface);
    static EGLBoolean (*query)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    static void (*readpx)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    static void (*pstore)(GLenum, GLint);
    static unsigned frame, every;
    static uint64_t last;
    static uint8_t *buf;
    static size_t cap;
    if (!real) {
        // reference UI opens libEGL itself (dlopen, local): the symbols come from that copy when the global scope has none
        void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
        real = (EGLBoolean (*)(EGLDisplay, EGLSurface))dlsym(RTLD_NEXT, "eglSwapBuffers");
        if (!real && egl) real = (EGLBoolean (*)(EGLDisplay, EGLSurface))dlsym(egl, "eglSwapBuffers");
        query = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint *))dlsym(egl ? egl : RTLD_DEFAULT, "eglQuerySurface");
        readpx = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))dlsym(gl ? gl : RTLD_DEFAULT, "glReadPixels");
        pstore = (void (*)(GLenum, GLint))dlsym(gl ? gl : RTLD_DEFAULT, "glPixelStorei");
        fprintf(stderr, "FRAMEHASH init: swap %p query %p readpixels %p\n", (void *)real, (void *)query, (void *)readpx);
        const char *e = getenv("FRAMEHASH_EVERY");
        every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 1;
    }
    frame++;
    EGLint w = 0, h = 0;
    if (query && readpx && pstore && frame % every == 0 && query(dpy, surf, EGL_WIDTH, &w) && query(dpy, surf, EGL_HEIGHT, &h) && w > 0 &&
        h > 0) {
        size_t n = (size_t)w * h * 4;
        if (n > cap) {
            free(buf);
            buf = malloc(n);
            cap = buf ? n : 0;
        }
        if (buf) {
            pstore(GL_PACK_ALIGNMENT, 1);
            readpx(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            uint64_t x = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < n; i++) x = (x ^ buf[i]) * 0x100000001b3ull;
            if (x != last) fprintf(stderr, "FRAMEHASH %u %dx%d %016llx\n", frame, w, h, (unsigned long long)x);
            last = x;
        }
    }
    return real ? real(dpy, surf) : EGL_FALSE;
}
