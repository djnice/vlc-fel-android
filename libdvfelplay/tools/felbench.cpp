/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * felbench - speed and bit exactness of felgpu on the box, with frames dumped
 * by libdvfelplay (debug.dvfp.dump): <prefix>_bl.bin, _el.bin, _meta.bin and
 * _out.bin (the output of the composer that played them).
 *
 *   felbench [-w 3840 -h 2160] [-n iterations] [-d dither] [-p] prefix...
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
  const EGLint attr[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
                         EGL_DMA_BUF_PLANE0_FD_EXT, b.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                         EGL_DMA_BUF_PLANE0_PITCH_EXT, w * 4, EGL_NONE};
  EGLImageKHR img = g_createImage(g_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
  if (img == EGL_NO_IMAGE_KHR)
    return false;
  glGenTextures(1, &b.tex);
  glBindTexture(GL_TEXTURE_2D, b.tex);
  g_targetTexture(GL_TEXTURE_2D, img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (fbo)
  {
    glGenFramebuffers(1, &b.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return false;
  }
  return glGetError() == GL_NO_ERROR;
}

bool ReadFile(const std::string& path, void* dst, size_t size)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  const bool ok = fread(dst, 1, size, f) == size;
  fclose(f);
  return ok;
}

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
  return std::chrono::duration<double, std::milli>(b - a).count();
}
} // namespace

int main(int argc, char** argv)
{
  int w = 3840, h = 2160, iters = 20, dither = 1, prof = 0, taps = 6;
  std::vector<std::string> frames;
  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    if (a == "-w" && i + 1 < argc) w = atoi(argv[++i]);
    else if (a == "-h" && i + 1 < argc) h = atoi(argv[++i]);
    else if (a == "-n" && i + 1 < argc) iters = atoi(argv[++i]);
    else if (a == "-d" && i + 1 < argc) dither = atoi(argv[++i]);
    else if (a == "-p") prof = 1;
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
  felgpu* f = felgpu_create(w, h, taps);
  if (!f)
    return fprintf(stderr, "felgpu_create failed\n"), 1;
  felgpu_set_dither(f, dither);
  if (getenv("FELBENCH_EXT"))
    printf("%s\n", (const char*)glGetString(GL_EXTENSIONS));
  printf("%s, %dx%d, %d iterations per frame, dither %d\n", (const char*)glGetString(GL_RENDERER), w,
         h, iters, dither);

  const size_t px = size_t(w) * h;
  std::vector<uint32_t> ref(px);
  std::vector<double> all;
  long badTotal = 0;
  for (const auto& pre : frames)
  {
    felgpu_meta meta;
    if (!ReadFile(pre + "_meta.bin", &meta, sizeof(meta)) || !ReadFile(pre + "_bl.bin", bl.map, px * 4) ||
        !ReadFile(pre + "_el.bin", el.map, px) || !ReadFile(pre + "_out.bin", ref.data(), px * 4))
    {
      fprintf(stderr, "%s: cannot read the dump\n", pre.c_str());
      continue;
    }
    felgpu_set_meta(f, &meta);
    // warm up and check
    memset(out.map, 0, px * 4);
    felgpu_set_el_tex(f, el.tex);
    felgpu_run(f, bl.tex, out.fbo);
    glFinish();
    const uint32_t* o = static_cast<const uint32_t*>(out.map);
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
      felgpu_set_el_tex(f, el.tex);
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
  }
  std::sort(all.begin(), all.end());
  if (!all.empty())
    printf("all: median %.2f ms, p95 %.2f, max %.2f; %s\n", all[all.size() / 2], all[all.size() * 95 / 100],
           all.back(), badTotal ? "OUTPUT DIFFERS" : "bit exact");
  felgpu_destroy(f);
  return badTotal ? 3 : 0;
}
