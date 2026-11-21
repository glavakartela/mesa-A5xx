#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint compile(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader: %s\n", log);
      exit(1);
   }
   return s;
}

int main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_platform_display ?
      get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) :
      eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major, minor;
   if (!eglInitialize(dpy, &major, &minor)) {
      fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError());
      return 1;
   }
   EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE
   };
   EGLConfig cfg;
   EGLint ncfg = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || !ncfg) {
      fprintf(stderr, "no config\n");
      return 1;
   }
   EGLint pb_attrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
   if (surf == EGL_NO_SURFACE) {
      fprintf(stderr, "pbuffer failed 0x%x\n", eglGetError());
      return 1;
   }
   EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "makecurrent failed 0x%x\n", eglGetError());
      return 1;
   }
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));

   const char *vs_src =
      "attribute vec2 pos;\n"
      "varying highp vec2 uv;\n"
      "void main() { gl_Position = vec4(pos, 0.0, 1.0); uv = pos * 0.5 + 0.5; }\n";
   const char *fs_src =
      "precision highp float;\n"
      "varying highp vec2 uv;\n"
      "uniform sampler2D tex;\n"
      "void main() { gl_FragColor = texture2D(tex, uv); }\n";
   GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
   GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
   GLuint prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   GLint ok;
   glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[1024]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      fprintf(stderr, "link: %s\n", log); return 1;
   }
   glUseProgram(prog);

   static const float verts[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
   GLuint vbo;
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

   static uint32_t texels[64 * 64];
   for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 64; x++)
         texels[y * 64 + x] = x < 32 ? 0xff0000ffu : 0xff00ff00u;

   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, texels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   glViewport(0, 0, 64, 64);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   unsigned char l[4] = {0}, r[4] = {0};
   glReadPixels(16, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
   glReadPixels(48, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
   printf("left=%02x%02x%02x%02x right=%02x%02x%02x%02x\n",
          l[0], l[1], l[2], l[3], r[0], r[1], r[2], r[3]);
   return 0;
}
