/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * dvfp_test - a minimal player for dvfelplay tests on the box: plays the
 * access units of an .au file (tools/mkau.py) without audio through the Dolby
 * Vision MediaCodec decoder into the activity window, as a player would:
 * dvfp_probe on the first access unit, dvfp_filter / dvfp_commit for each one.
 *
 * File: <external files dir>/test.au
 *   (/sdcard/Android/data/org.dvfel.dvfptest/files/test.au)
 * Switches (adb shell setprop, read at start):
 *   debug.dvfp.test.fel    0: profile 8.1 conversion only (default 1: FEL)
 *   debug.dvfp.test.loops  times the file is played (default 1); a flush
 *                          (seek) between the runs
 *   debug.dvfp.test.seek   n > 0: a flush and a jump of n seconds forward
 *                          every 10 s of playback
 *   debug.dvfp.test.codec  base layer decoder name (default: by type)
 * Log: adb logcat -s dvfp_test dvfelplay
 */

#include "dvfelplay.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define TAG "dvfp_test"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct au
{
  uint32_t size, flags;
  int64_t pts;
  uint8_t* data;
};

static struct
{
  ANativeWindow* window;
  pthread_t thread;
  int running;
  volatile int stop;
  char path[512];
} g;

static int prop_int(const char* name, int fallback)
{
  char v[PROP_VALUE_MAX] = {0};
  if (__system_property_get(name, v) <= 0)
    return fallback;
  return atoi(v);
}

static int64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* the output side: renders each frame at its time on the monotonic clock */
struct out_ctx
{
  AMediaCodec* codec;
  volatile int stop;
  pthread_mutex_t lock;
  int64_t base_ns, base_pts; /* clock anchor, base_ns 0: set by the next frame */
  uint64_t rendered, late;
};

static void* out_thread(void* arg)
{
  struct out_ctx* o = arg;
  while (!o->stop)
  {
    AMediaCodecBufferInfo info;
    ssize_t i = AMediaCodec_dequeueOutputBuffer(o->codec, &info, 10000);
    if (i < 0)
    {
      if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        AMediaFormat* f = AMediaCodec_getOutputFormat(o->codec);
        LOG("output format %s", f ? AMediaFormat_toString(f) : "?");
        if (f)
          AMediaFormat_delete(f);
      }
      continue;
    }
    pthread_mutex_lock(&o->lock);
    if (!o->base_ns)
    {
      o->base_ns = now_ns() + 100000000LL;
      o->base_pts = info.presentationTimeUs;
    }
    const int64_t t = o->base_ns + (info.presentationTimeUs - o->base_pts) * 1000;
    pthread_mutex_unlock(&o->lock);
    if (t < now_ns() - 5000000LL)
    {
      AMediaCodec_releaseOutputBuffer(o->codec, i, 0);
      o->late++;
    }
    else
    {
      AMediaCodec_releaseOutputBufferAtTime(o->codec, i, t);
      o->rendered++;
    }
  }
  return NULL;
}

static int read_au(FILE* f, struct au* a)
{
  uint8_t h[16];
  if (fread(h, 1, 16, f) != 16)
    return 0;
  memcpy(&a->size, h, 4);
  memcpy(&a->flags, h + 4, 4);
  memcpy(&a->pts, h + 8, 8);
  a->data = realloc(a->data, a->size);
  return a->data && fread(a->data, 1, a->size, f) == a->size;
}

/* the VPS/SPS/PPS in front of a key frame (Annex-B), for csd-0 */
static size_t param_sets(const struct au* a)
{
  size_t i = 0;
  while (i + 5 < a->size)
  {
    if (a->data[i] == 0 && a->data[i + 1] == 0 && a->data[i + 2] == 0 && a->data[i + 3] == 1)
    {
      const int type = (a->data[i + 4] >> 1) & 0x3f;
      if (type < 32 || type > 34)
        return i;
      i += 4;
    }
    else
      i++;
  }
  return 0;
}

static void play(void)
{
  FILE* f = fopen(g.path, "rb");
  if (!f)
  {
    LOG("cannot open %s", g.path);
    return;
  }
  uint8_t hdr[24];
  uint32_t w = 0, h = 0, num = 0, den = 0;
  if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "DVAU", 4))
  {
    LOG("%s: not an .au file", g.path);
    fclose(f);
    return;
  }
  memcpy(&w, hdr + 8, 4);
  memcpy(&h, hdr + 12, 4);
  memcpy(&num, hdr + 16, 4);
  memcpy(&den, hdr + 20, 4);
  const long data_start = ftell(f);

  struct au a = {0};
  if (!read_au(f, &a))
  {
    LOG("no access unit");
    fclose(f);
    return;
  }

  char ver[32];
  struct dvfp_rpu_info ri;
  dvfp_probe(a.data, a.size, &ri);
  const int want_fel = prop_int("debug.dvfp.test.fel", 1);
  const int loops = prop_int("debug.dvfp.test.loops", 1);
  const int seek = prop_int("debug.dvfp.test.seek", 0);
  LOG("%s: %ux%u %u/%u fps; RPU %d profile %d el_type %d el %d; module '%s', available %d",
      g.path, w, h, num, den, ri.has_rpu, ri.profile, ri.el_type, ri.has_el,
      dvfp_module_version(ver, sizeof(ver)), dvfp_available());

  const int fel = want_fel && ri.profile == 7 && ri.el_type == DVFP_EL_FEL && dvfp_available();
  dvfp* d = ri.profile == 7 ? dvfp_open(w, h, fel ? DVFP_MODE_FEL : DVFP_MODE_CONVERT) : NULL;
  if (fel && !d)
    d = dvfp_open(w, h, DVFP_MODE_CONVERT);
  LOG("dvfelplay %s: %s", dvfp_version(),
      !d ? "not used" : dvfp_mode(d) == DVFP_MODE_FEL ? "FEL composition" : "8.1 conversion");

  /* the base layer decoder */
  char codec_name[PROP_VALUE_MAX] = {0};
  __system_property_get("debug.dvfp.test.codec", codec_name);
  const char* mime = ri.has_rpu ? "video/dolby-vision" : "video/hevc";
  AMediaCodec* codec = codec_name[0] ? AMediaCodec_createCodecByName(codec_name)
                                     : AMediaCodec_createDecoderByType(mime);
  AMediaFormat* fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, (int32_t)w);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)h);
  const size_t ps = param_sets(&a);
  if (ps)
    AMediaFormat_setBuffer(fmt, "csd-0", a.data, ps);
  if (!codec || AMediaCodec_configure(codec, fmt, g.window, NULL, 0) != AMEDIA_OK ||
      AMediaCodec_start(codec) != AMEDIA_OK)
  {
    LOG("cannot start the decoder (%s %s)", mime, codec_name);
    AMediaFormat_delete(fmt);
    if (codec)
      AMediaCodec_delete(codec);
    dvfp_close(d);
    free(a.data);
    fclose(f);
    return;
  }
  AMediaFormat_delete(fmt);
  /* API 28 */
  media_status_t (*get_name)(AMediaCodec*, char**) =
      (media_status_t(*)(AMediaCodec*, char**))dlsym(RTLD_DEFAULT, "AMediaCodec_getName");
  void (*release_name)(AMediaCodec*, char*) =
      (void (*)(AMediaCodec*, char*))dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName");
  char* name = NULL;
  if (get_name && release_name && get_name(codec, &name) == AMEDIA_OK && name)
  {
    LOG("base layer decoder %s (%s)", name, mime);
    release_name(codec, name);
  }

  struct out_ctx o = {.codec = codec};
  pthread_mutex_init(&o.lock, NULL);
  pthread_t ot;
  pthread_create(&ot, NULL, out_thread, &o);

  uint64_t queued = 0;
  int have = 1;
  int64_t first_pts = a.pts, next_seek = a.pts + 10000000LL;
  for (int loop = 0; loop < loops && !g.stop; loop++)
  {
    if (loop)
    {
      /* a seek back to the start */
      fseek(f, data_start, SEEK_SET);
      have = read_au(f, &a);
      AMediaCodec_flush(codec);
      dvfp_flush(d);
      pthread_mutex_lock(&o.lock);
      o.base_ns = 0;
      pthread_mutex_unlock(&o.lock);
      next_seek = a.pts + 10000000LL;
      LOG("loop %d: flush, from the start", loop + 1);
    }
    for (; have && !g.stop; have = read_au(f, &a))
    {
      if (seek > 0 && a.pts >= next_seek)
      {
        /* forward to the first key frame n seconds later */
        const int64_t target = a.pts + seek * 1000000LL;
        while ((have = read_au(f, &a)) && !(a.flags & 1 && a.pts >= target))
          ;
        if (!have)
          break;
        AMediaCodec_flush(codec);
        dvfp_flush(d);
        pthread_mutex_lock(&o.lock);
        o.base_ns = 0;
        pthread_mutex_unlock(&o.lock);
        next_seek = a.pts + 10000000LL;
        LOG("seek: flush, on at %.3f s", (a.pts - first_pts) / 1e6);
      }
      const uint8_t* p = a.data;
      size_t n = a.size;
      if (d)
        dvfp_filter(d, a.data, a.size, &p, &n);
      ssize_t idx;
      while ((idx = AMediaCodec_dequeueInputBuffer(codec, 10000)) < 0 && !g.stop)
        ;
      if (idx < 0)
        break;
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getInputBuffer(codec, idx, &cap);
      if (!buf || n > cap)
      {
        LOG("input buffer too small (%zu > %zu)", n, cap);
        AMediaCodec_queueInputBuffer(codec, idx, 0, 0, a.pts, 0);
        dvfp_drop(d);
        continue;
      }
      memcpy(buf, p, n);
      if (AMediaCodec_queueInputBuffer(codec, idx, 0, n, a.pts, 0) == AMEDIA_OK)
      {
        dvfp_commit(d, a.pts);
        queued++;
      }
      else
        dvfp_drop(d);
    }
    have = 0;
    /* the last frames */
    for (int i = 0; i < 30 && !g.stop; i++)
      usleep(100000);
  }

  o.stop = 1;
  pthread_join(ot, NULL);
  struct dvfp_stats s;
  dvfp_get_stats(d, &s);
  LOG("done: %llu queued, %llu rendered, %llu late; composed %llu, mapped only %llu, without EL %llu "
      "(no wait %llu), EL in %llu out %llu decoded %llu overflow %llu, no CRC match %llu, stale %llu, "
      "gpu avg %.1f max %.1f ms",
      (unsigned long long)queued, (unsigned long long)o.rendered, (unsigned long long)o.late,
      (unsigned long long)s.composed, (unsigned long long)s.mapped_only,
      (unsigned long long)s.passthrough, (unsigned long long)s.no_el_skipped,
      (unsigned long long)s.el_inputs, (unsigned long long)s.el_outputs,
      (unsigned long long)s.el_decoded, (unsigned long long)s.el_overflow,
      (unsigned long long)s.no_crc_match, (unsigned long long)s.stale, s.gpu_ms_avg, s.gpu_ms_max);
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  dvfp_close(d);
  free(a.data);
  fclose(f);
}

static void* play_thread(void* arg)
{
  (void)arg;
  play();
  return NULL;
}

static void on_window_created(ANativeActivity* act, ANativeWindow* window)
{
  (void)act;
  if (g.running)
    return;
  ANativeWindow_acquire(window);
  g.window = window;
  g.stop = 0;
  g.running = 1;
  pthread_create(&g.thread, NULL, play_thread, NULL);
}

static void on_window_destroyed(ANativeActivity* act, ANativeWindow* window)
{
  (void)act;
  (void)window;
  if (!g.running)
    return;
  g.stop = 1;
  pthread_join(g.thread, NULL);
  g.running = 0;
  ANativeWindow_release(g.window);
  g.window = NULL;
}

void ANativeActivity_onCreate(ANativeActivity* act, void* saved, size_t size)
{
  (void)saved;
  (void)size;
  act->callbacks->onNativeWindowCreated = on_window_created;
  act->callbacks->onNativeWindowDestroyed = on_window_destroyed;
  ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
  snprintf(g.path, sizeof(g.path), "%s/test.au",
           act->externalDataPath ? act->externalDataPath : "/sdcard");
}
