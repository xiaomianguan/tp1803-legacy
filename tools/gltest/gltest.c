/* Minimal Mesa/KGSL smoke test: EGL pbuffer + GLES2 shader draw + readback. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(x, msg) do { if (!(x)) { printf("FAIL: %s (egl 0x%x gl 0x%x)\n", msg, eglGetError(), glGetError()); return 1; } } while (0)

static GLuint sh(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(s, sizeof l, NULL, l); printf("shader: %s\n", l); }
    return s;
}

int main(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    CHECK(eglInitialize(d, &maj, &min), "eglInitialize");
    printf("EGL %d.%d vendor=%s\n", maj, min, eglQueryString(d, EGL_VENDOR));
    const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    CHECK(eglChooseConfig(d, ca, &cfg, 1, &n) && n > 0, "eglChooseConfig");
    const EGLint sa[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    CHECK(s != EGL_NO_SURFACE, "pbuffer");
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    CHECK(c != EGL_NO_CONTEXT, "context");
    CHECK(eglMakeCurrent(d, s, s, c), "makecurrent");
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 a; varying vec2 v; void main(){ v=a*0.5+0.5; gl_Position=vec4(a,0.0,1.0); }"));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 v; void main(){ gl_FragColor=vec4(v.x, v.y, 0.5, 1.0); }"));
    glBindAttribLocation(p, 0, "a");
    glLinkProgram(p);
    glUseProgram(p);
    static const float tri[] = { -1, -1, 3, -1, -1, 3 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 256, 256);
    glClearColor(1, 0, 1, 1);

    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 200; i++) { glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3); }
    glFinish();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("200 frames in %.2f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    unsigned char px[4 * 4];
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned char hi[4];
    glReadPixels(255, 255, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
    printf("pixel(0,0)=%d,%d,%d,%d  pixel(255,255)=%d,%d,%d,%d  glError=0x%x\n",
           px[0], px[1], px[2], px[3], hi[0], hi[1], hi[2], hi[3], glGetError());
    int ok = px[0] < 8 && px[1] < 8 && px[2] > 120 && hi[0] > 240 && hi[1] > 240;
    printf("%s\n", ok ? "PASS: GPU rendered the gradient" : "MISMATCH");
    return ok ? 0 : 2;
}
