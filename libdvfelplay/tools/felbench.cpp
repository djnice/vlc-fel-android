/*
 *  Copyright (C) 2026 djnice <djnice@freemail.hu>
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * felbench - speed and bit exactness of felgpu on the box, with frames dumped
 * by libdvfelplay (debug.dvfp.dump): <prefix>_bl.bin, _el.bin, _meta.bin and
 * _out.bin (the output of the composer that played them).
 *
 *   felbench [-w 3840 -h 2160] [-n iterations] [-d dither] [-p] [-f 10] [-r] prefix...
 *
 * -f 10: the buffers imported as DRM_FORMAT_ABGR2101010 (FELGPU_BUF_RGB10A2),
 * -r: no comparison, the output is written as the frame's _out.bin (the
 * reference of later runs), -s: the EL given as planes (felgpu_set_el)
 * instead of its picture, -D: demo mode (prints its measurement), -P: the
 * test pattern, -o name: the reference is <prefix>_out<name>.bin (one per
 * mode). Environment: FELBENCH_AB30=1 only tests the 10-bit import,
 * FELGPU_NOCOMPUTE=1 keeps the RGB10_A2 path's vertical luma pass a fragment
 * shader (felgpu.c).
 *
 * Buffers are ION dma-bufs of the codec_mm heap imported as EGL images, as in
 * libdvfelplay (run as root). Reports per frame the GPU time (set_el_tex +
 * run + glFinish: min / median / max over the iterations), with -p the per
 * pass averages, and the pixels that differ from _out.bin.
 */
#include "felgpu.h"
#include "dvfel_uapi.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl32.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif
#ifndef DRM_FORMAT_ABGR2101010
#define DRM_FORMAT_ABGR2101010 0x30334241 /* 'AB30' */
#endif

namespace
{
struct ion_allocation_data
{
  uint64_t len;
  uint32_t heap_id_mask, flags, fd, unused;
};
struct ion_heap_data
{
  char name[32];
  uint32_t type, heap_id, reserved0, reserved1, reserved2;
};
struct ion_heap_query
{
  uint32_t cnt, reserved0;
  uint64_t heaps;
  uint32_t reserved1, reserved2;
};
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY _IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)

int g_ion = -1, g_heap = -1;
EGLint g_fourcc = DRM_FORMAT_ABGR8888; // how the buffers are imported
EGLDisplay g_dpy;
PFNEGLCREATEIMAGEKHRPROC g_createImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_targetTexture;

bool IonInit()
{
  g_ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
  ion_heap_query q{};
  if (g_ion < 0 || ioctl(g_ion, ION_IOC_HEAP_QUERY, &q) < 0 || !q.cnt)
    return false;
  std::vector<ion_heap_data> heaps(q.cnt);
  q.heaps = reinterpret_cast<uintptr_t>(heaps.data());
  if (ioctl(g_ion, ION_IOC_HEAP_QUERY, &q) < 0)
    return false;
  for (auto& h : heaps)
    if (!strcmp(h.name, "codec_mm_cma"))
      g_heap = h.heap_id;
  return g_heap >= 0;
}

struct Buf
{
  int fd{-1};
  size_t len{0};
  void* map{nullptr};
  GLuint tex{0}, fbo{0};
};

bool MakeBuf(Buf& b, int w, int h, bool fbo)
{
  ion_allocation_data a{};
  a.len = DVFEL_BUF_SIZE(w, h);
  a.heap_id_mask = 1u << g_heap;
  a.flags = 1u << 30;
  if (ioctl(g_ion, ION_IOC_ALLOC, &a) < 0)
    return false;
  b.fd = a.fd;
  b.len = a.len;
  b.map = mmap(nullptr, b.len, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (b.map == MAP_FAILED)
    return false;
  const EGLint attr[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, g_fourcc,
                         EGL_DMA_BUF_PLANE0_FD_EXT, b.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                         EGL_DMA_BUF_PLANE0_PITCH_EXT, w * 4, EGL_NONE};
  EGLImageKHR img = g_createImage(g_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
  if (img == EGL_NO_IMAGE_KHR)
  {
    fprintf(stderr, "eglCreateImageKHR(fourcc %#x): %#x\n", g_fourcc, eglGetError());
    return false;
  }
  glGenTextures(1, &b.tex);
  glBindTexture(GL_TEXTURE_2D, b.tex);
  g_targetTexture(GL_TEXTURE_2D, img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (fbo)
  {
    glGenFramebuffers(1, &b.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
    {
      fprintf(stderr, "framebuffer of fourcc %#x: status %#x\n", g_fourcc, st);
      return false;
    }
  }
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR)
    fprintf(stderr, "import of fourcc %#x: GL error %#x\n", g_fourcc, err);
  return err == GL_NO_ERROR;
}

bool ReadFile(const std::string& path, void* dst, size_t size);

GLuint Program(const char* fs)
{
  static const char* vs = "#version 320 es\n"
                          "void main() {\n"
                          "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                          "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                          "}\n";
  GLuint p = glCreateProgram();
  for (int i = 0; i < 2; i++)
  {
    GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(s, 1, i ? &fs : &vs, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
      char log[2048];
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      fprintf(stderr, "shader: %s\n", log);
    }
    glAttachShader(p, s);
  }
  glLinkProgram(p);
  return p;
}

// FELBENCH_AB30: can the buffers be imported as DRM_FORMAT_ABGR2101010 (the
// layout Cr | Cb << 10 | Y << 20 is GL_RGB10_A2 with R = Cr, G = Cb, B = Y),
// and do sampling, gathering and rendering keep every code?
int ProbeAb30(int w, int h, const char* blFile)
{
  g_fourcc = DRM_FORMAT_ABGR2101010;
  Buf in, out;
  if (!MakeBuf(in, w, h, false))
    return printf("AB30: import for sampling FAILED\n"), 1;
  if (!MakeBuf(out, w, h, true))
    return printf("AB30: import as a render target FAILED\n"), 1;
  GLint ifmt = 0, rs = 0, as = 0;
  glBindTexture(GL_TEXTURE_2D, in.tex);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &rs);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &as);
  printf("AB30: imported, internal format %#x, red %d bits, alpha %d bits\n", ifmt, rs, as);

  const size_t px = size_t(w) * h;
  uint32_t* src = static_cast<uint32_t*>(in.map);
  if (!blFile || !ReadFile(blFile, src, px * 4))
  {
    uint32_t seed = 12345;
    for (size_t i = 0; i < px; i++)
      src[i] = (seed = seed * 1664525u + 1013904223u) >> 2; // bits 30-31 zero
  }
  static const char* const tests[][2] = {
      {"fetch, written as (code + 0.25) / 1023",
       "#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
       "uniform sampler2D s;\nout vec4 o;\n"
       "void main() {\n"
       "  vec3 c = floor(texelFetch(s, ivec2(gl_FragCoord.xy), 0).rgb * 1023.0 + 0.5);\n"
       "  o = vec4((c + 0.25) / 1023.0, 0.0);\n"
       "}\n"},
      {"fetch, written as code / 1023",
       "#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
       "uniform sampler2D s;\nout vec4 o;\n"
       "void main() {\n"
       "  vec3 c = floor(texelFetch(s, ivec2(gl_FragCoord.xy), 0).rgb * 1023.0 + 0.5);\n"
       "  o = vec4(c / 1023.0, 0.0);\n"
       "}\n"},
      {"fetch passed through unchanged",
       "#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
       "uniform sampler2D s;\nout vec4 o;\n"
       "void main() {\n"
       "  o = vec4(texelFetch(s, ivec2(gl_FragCoord.xy), 0).rgb, 0.0);\n"
       "}\n"},
      {"Y by textureGather (.w .z .x .y of the 2x2 at the pixel), Cb Cr by fetch",
       "#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
       "uniform sampler2D s;\nout vec4 o;\n"
       "void main() {\n"
       "  ivec2 p = ivec2(gl_FragCoord.xy), q = p & ~1;\n"
       "  vec4 g = textureGather(s, (vec2(q) + 1.0) / vec2(textureSize(s, 0)), 2);\n"
       "  float y = (p.y & 1) == 0 ? ((p.x & 1) == 0 ? g.w : g.z) : ((p.x & 1) == 0 ? g.x : g.y);\n"
       "  vec2 c = floor(texelFetch(s, p, 0).rg * 1023.0 + 0.5);\n"
       "  o = vec4((vec3(c, floor(y * 1023.0 + 0.5)) + 0.25) / 1023.0, 0.0);\n"
       "}\n"},
  };
  int failed = 0;
  for (const auto& t : tests)
  {
    GLuint p = Program(t[1]);
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in.tex);
    glUniform1i(glGetUniformLocation(p, "s"), 0);
    memset(out.map, 0xff, px * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    const uint32_t* o = static_cast<const uint32_t*>(out.map);
    long bad = 0, first = -1;
    for (size_t i = 0; i < px; i++)
      if (o[i] != src[i] && bad++ == 0)
        first = long(i);
    printf("AB30 %s: %ld of %zu words differ", t[0], bad, px);
    if (bad)
      printf(" (first at %ld: %08x, expected %08x)", first, o[first], src[first]);
    printf(", GL error %#x\n", glGetError());
    failed += bad != 0;
    glDeleteProgram(p);
  }
  return failed;
}

// the whole file, which has to be size bytes long
bool ReadFile(const std::string& path, void* dst, size_t size)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  const bool ok = fread(dst, 1, size, f) == size && fgetc(f) == EOF;
  fclose(f);
  return ok;
}

bool WriteFile(const std::string& path, const void* src, size_t size)
{
  FILE* f = fopen(path.c_str(), "wb");
  if (!f)
    return false;
  const bool ok = fwrite(src, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
  return std::chrono::duration<double, std::milli>(b - a).count();
}
} // namespace

int main(int argc, char** argv)
{
  int w = 3840, h = 2160, iters = 20, dither = 1, prof = 0, taps = 6;
  bool writeRef = false, planes = false, demo = false, pattern = false;
  std::string refName;
  int bufFormat = FELGPU_BUF_RGBA8;
  std::vector<std::string> frames;
  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    if (a == "-w" && i + 1 < argc) w = atoi(argv[++i]);
    else if (a == "-h" && i + 1 < argc) h = atoi(argv[++i]);
    else if (a == "-n" && i + 1 < argc) iters = atoi(argv[++i]);
    else if (a == "-d" && i + 1 < argc) dither = atoi(argv[++i]);
    else if (a == "-p") prof = 1;
    else if (a == "-r") writeRef = true;
    else if (a == "-s") planes = true;
    else if (a == "-D") demo = true;
    else if (a == "-P") pattern = true;
    else if (a == "-o" && i + 1 < argc) refName = argv[++i];
    else if (a == "-f" && i + 1 < argc) bufFormat = atoi(argv[++i]) == 10 ? FELGPU_BUF_RGB10A2 : FELGPU_BUF_RGBA8;
    else if (a == "-t" && i + 1 < argc) taps = atoi(argv[++i]);
    else frames.push_back(a);
  }
  if (frames.empty())
  {
    fprintf(stderr, "usage: felbench [-w W -h H] [-n iterations] [-d dither] [-p] prefix...\n");
    return 2;
  }

  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj, min;
  if (!eglInitialize(g_dpy, &maj, &min))
    return fprintf(stderr, "eglInitialize failed\n"), 1;
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    return fprintf(stderr, "no GLES 3.2 context\n"), 1;
  g_createImage = PFNEGLCREATEIMAGEKHRPROC(eglGetProcAddress("eglCreateImageKHR"));
  g_targetTexture = PFNGLEGLIMAGETARGETTEXTURE2DOESPROC(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!IonInit())
    return fprintf(stderr, "no ION codec_mm heap (run as root)\n"), 1;

  if (getenv("FELBENCH_AB30"))
    return ProbeAb30(w, h, frames.empty() ? nullptr : (frames[0] + "_bl.bin").c_str());
  if (bufFormat == FELGPU_BUF_RGB10A2)
    g_fourcc = DRM_FORMAT_ABGR2101010;
  Buf bl, el, out;
  if (!MakeBuf(bl, w, h, false) || !MakeBuf(el, w / 2, h / 2, false) || !MakeBuf(out, w, h, true))
    return fprintf(stderr, "buffer setup failed\n"), 1;
  if (getenv("FELBENCH_IMAGE"))
  {
    // can a compute shader write the dma-buf output (no GL_EXT_EGL_image_storage)?
    glGetError();
    glBindImageTexture(0, out.tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    const GLenum e1 = glGetError();
    GLint immutable = -1;
    glBindTexture(GL_TEXTURE_2D, out.tex);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    printf("glBindImageTexture(EGL image texture, rgba8): error %#x, immutable %d\n", e1, immutable);
    // a tiny compute shader writing a pattern through the image
    const char* cs = "#version 320 es\nlayout(local_size_x = 8, local_size_y = 8) in;\n"
                     "layout(rgba8, binding = 0) writeonly uniform highp image2D o;\n"
                     "void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
                     "  imageStore(o, p, unpackUnorm4x8(uint(p.x * 7 + p.y * 13))); }\n";
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &cs, nullptr);
    glCompileShader(s);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, s);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    memset(out.map, 0, size_t(w) * h * 4);
    glUseProgram(prog);
    glDispatchCompute(w / 8, h / 8, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();
    const GLenum e2 = glGetError();
    const uint32_t* o = static_cast<const uint32_t*>(out.map);
    long good = 0;
    for (int y = 0; y < h; y += 37)
      for (int x = 0; x < w; x += 41)
        good += o[size_t(y) * w + x] == uint32_t(x * 7 + y * 13);
    printf("compute imageStore: link %d, error %#x, %ld of %d samples correct\n", ok, e2, good,
           ((h + 36) / 37) * ((w + 40) / 41));
    return 0;
  }
  const auto c0 = std::chrono::steady_clock::now();
  felgpu* f = felgpu_create_fmt(w, h, taps, bufFormat);
  if (!f)
    return fprintf(stderr, "felgpu_create failed\n"), 1;
  glFinish();
  const double createMs = Ms(c0, std::chrono::steady_clock::now());
  felgpu_set_dither(f, dither);
  felgpu_set_demo(f, demo);
  felgpu_set_pattern(f, pattern);
  if (demo)
  {
    // the text overlay, both halves
    felgpu_set_overlay(f, 0, "FEL  MAX 1000.0  AVG 12.3 NIT");
    felgpu_set_overlay(f, 4, "BL   MAX 100  AVG 1.5 NIT");
  }
  if (getenv("FELBENCH_EXT"))
    printf("%s\n", (const char*)glGetString(GL_EXTENSIONS));
  printf("%s, %dx%d, %d iterations per frame, dither %d, buffers %s, composer set up in %.0f ms\n",
         (const char*)glGetString(GL_RENDERER), w, h, iters, dither,
         bufFormat == FELGPU_BUF_RGB10A2 ? "RGB10_A2" : "RGBA8", createMs);

  const size_t px = size_t(w) * h;
  std::vector<uint32_t> ref(px);
  std::vector<double> all;
  long badTotal = 0;
  for (const auto& pre : frames)
  {
    felgpu_meta meta;
    if (!ReadFile(pre + "_meta.bin", &meta, sizeof(meta)) || !ReadFile(pre + "_bl.bin", bl.map, px * 4) ||
        !ReadFile(pre + "_el.bin", el.map, px) ||
        (!writeRef && !ReadFile(pre + "_out" + refName + ".bin", ref.data(), px * 4)))
    {
      fprintf(stderr, "%s: cannot read the dump\n", pre.c_str());
      continue;
    }
    felgpu_set_meta(f, &meta);
    // -s: the EL planes as a software decoder gives them (chroma: the samples at even x/y)
    const int ew = w / 2, eh = h / 2;
    std::vector<uint16_t> ey(planes ? size_t(ew) * eh : 0), ecb(ey.size() / 4), ecr(ey.size() / 4);
    if (planes)
    {
      const uint32_t* e = static_cast<const uint32_t*>(el.map);
      for (int y = 0; y < eh; y++)
        for (int x = 0; x < ew; x++)
        {
          const uint32_t v = e[size_t(y) * ew + x];
          ey[size_t(y) * ew + x] = (v >> 20) & 1023;
          if (!(x & 1) && !(y & 1))
          {
            ecb[size_t(y / 2) * (ew / 2) + x / 2] = (v >> 10) & 1023;
            ecr[size_t(y / 2) * (ew / 2) + x / 2] = v & 1023;
          }
        }
    }
    auto setEl = [&] {
      if (planes)
        felgpu_set_el(f, ey.data(), ew, ecb.data(), ecr.data(), ew / 2);
      else
        felgpu_set_el_tex(f, el.tex);
    };
    // warm up and check
    memset(out.map, 0, px * 4);
    setEl();
    felgpu_run(f, bl.tex, out.fbo);
    glFinish();
    const uint32_t* o = static_cast<const uint32_t*>(out.map);
    if (writeRef)
    {
      // -r: this composer's output becomes the reference of the frame
      if (!WriteFile(pre + "_out" + refName + ".bin", o, px * 4))
        fprintf(stderr, "%s: cannot write the reference\n", pre.c_str());
      memcpy(ref.data(), o, px * 4);
    }
    long bad = 0;
    int maxd[3] = {0, 0, 0};
    for (size_t i = 0; i < px; i++)
    {
      if (o[i] == ref[i])
        continue;
      bad++;
      for (int c = 0; c < 3; c++)
      {
        const int a = (o[i] >> (20 - 10 * c)) & 1023, b = (ref[i] >> (20 - 10 * c)) & 1023;
        maxd[c] = std::max(maxd[c], std::abs(a - b));
      }
    }
    badTotal += bad;

    std::vector<double> t;
    felgpu_set_profile(f, prof);
    double pms[5];
    felgpu_get_profile(f, pms, 1);
    for (int i = 0; i < iters; i++)
    {
      const auto t0 = std::chrono::steady_clock::now();
      setEl();
      felgpu_run(f, bl.tex, out.fbo);
      glFinish();
      t.push_back(Ms(t0, std::chrono::steady_clock::now()));
    }
    felgpu_get_profile(f, pms, 1);
    felgpu_set_profile(f, 0);
    std::sort(t.begin(), t.end());
    all.insert(all.end(), t.begin(), t.end());
    const char* name = strrchr(pre.c_str(), '/');
    printf("%-10s %s: %s %ld px differ (max Y %d Cb %d Cr %d); ms min %.2f med %.2f max %.2f",
           name ? name + 1 : pre.c_str(), meta.mapping[1][0] || meta.mapping[2][0] ? "MMR " : "poly",
           bad ? "DIFF" : "EXACT", bad, maxd[0], maxd[1], maxd[2], t.front(), t[t.size() / 2], t.back());
    if (prof)
      printf(" | passes %.2f %.2f %.2f %.2f unpack %.2f", pms[0], pms[1], pms[2], pms[3], pms[4]);
    printf("\n");
    felgpu_stats st;
    if (demo && felgpu_get_stats(f, &st))
      printf("           measured: FEL max %.3f avg %.3f, BL max %.3f avg %.3f cd/m2, area %d %d %d %d (%d)\n",
             st.nits[0], st.nits[1], st.nits[2], st.nits[3], st.x0, st.y0, st.x1, st.y1, st.source);
  }
  std::sort(all.begin(), all.end());
  if (!all.empty())
    printf("all: median %.2f ms, p95 %.2f, max %.2f; %s\n", all[all.size() / 2], all[all.size() * 95 / 100],
           all.back(), badTotal ? "OUTPUT DIFFERS" : "bit exact");
  felgpu_destroy(f);
  return badTotal ? 3 : 0;
}
