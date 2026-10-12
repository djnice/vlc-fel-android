/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * mvc3dplay - 3D MVC (Blu-ray 3D) playback on Android with an Amlogic S5
 * (S928X) and the mvc3d kernel module, for any MediaCodec based player.
 *
 * The player decodes the stream with its own MediaCodec H.264 decoder as
 * usual (the hardware decoder shows the base view). This library starts a
 * helper process that decodes both views in software and composes a frame
 * packed picture (both views at full resolution) for every frame; the mvc3d
 * kernel module (between video_composer.0 and video_render.0) shows it in
 * place of the base view, so the player's timing stays.
 *
 * Usage, per video stream:
 *
 *   access units:   mvc3d_probe() -> MVC? mvc3d_available()? 3D wanted
 *                   (mvc3d_display_3d())?
 *   decoder setup:  mvc3d_open(w, h) before the first frame can reach the
 *                   display; the numbering of the decoder's input starts
 *   every buffer queued to MediaCodec:  mvc3d_commit() (access units) or
 *                   mvc3d_config() (codec config data), in queue order
 *   seek / flush:   mvc3d_flush() with the MediaCodec flush
 *   decoder (re)created:  mvc3d_restart()
 *   end:            mvc3d_close()
 *
 * Input is Annex-B. All functions of one handle must be called from one
 * thread or under one lock.
 */
#ifndef MVC3DPLAY_H
#define MVC3DPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVC3D_API_VERSION 1

#if defined(__GNUC__)
#define MVC3D_EXPORT __attribute__((visibility("default")))
#else
#define MVC3D_EXPORT
#endif

typedef struct mvc3d mvc3d;

MVC3D_EXPORT int mvc3d_api_version(void);
MVC3D_EXPORT const char* mvc3d_version(void);

/* 1: the mvc3d kernel module and the helper are there */
MVC3D_EXPORT int mvc3d_available(void);

/*
 * Installs the kernel module bundled with this library (Dune HD / R_volution
 * firmware): loads it and installs it with a boot script (loaded at every
 * boot), unless the same or a newer module is loaded already. Needs root
 * (su); when the firmware's root access is off, ask(opaque, title, text)
 * decides whether to switch it on for the installation (> 0: yes; it is
 * switched off again afterwards). A "no" is remembered for this module
 * version. Blocks for the installation (a few seconds) and the question; the
 * result is kept for the process (one try). dir: a writable directory of
 * the app (NULL: /data/data/<package>/files). msg (optional): a line about
 * the outcome for the user.
 */
enum mvc3d_module_status
{
  MVC3D_MODULE_INSTALLED = 1, /* installed now */
  MVC3D_MODULE_READY = 0,     /* loaded already */
  MVC3D_MODULE_DECLINED = -1, /* the user said no (now or before) */
  MVC3D_MODULE_NO_ROOT = -2,  /* no su, or root access could not be switched on */
  MVC3D_MODULE_FAILED = -3,
};
typedef int (*mvc3d_ask_fn)(void* opaque, const char* title, const char* text);
MVC3D_EXPORT int mvc3d_module_install(const char* dir, mvc3d_ask_fn ask, void* opaque, char* msg,
                                      size_t msg_size);
/* the version of the bundled kernel module ("" if none) */
MVC3D_EXPORT const char* mvc3d_bundled_module_version(void);
/* 1 if the Annex-B data holds MVC NAL units (subset SPS, slice extension) */
MVC3D_EXPORT int mvc3d_probe(const uint8_t* au, size_t size);
/* 1 if the display takes 3D (frame packing, HDMI 1.4 3D_present in its EDID) */
MVC3D_EXPORT int mvc3d_display_3d(void);

/* the size of one view (1920x1080 or 1280x720); NULL on failure (play 2D) */
MVC3D_EXPORT mvc3d* mvc3d_open(int width, int height);
MVC3D_EXPORT void mvc3d_close(mvc3d* h);

/* the access unit just queued to MediaCodec with this pts (us) */
MVC3D_EXPORT void mvc3d_commit(mvc3d* h, const uint8_t* au, size_t size, int64_t pts_us);
/* codec config data (SPS/PPS) just queued to MediaCodec */
MVC3D_EXPORT void mvc3d_config(mvc3d* h, const uint8_t* data, size_t size);
MVC3D_EXPORT void mvc3d_flush(mvc3d* h);
/* the decoder was created anew: its input numbering starts again */
MVC3D_EXPORT void mvc3d_restart(mvc3d* h);

/*
 * the player renders every frame this many ns before its time (whole
 * vsyncs); the module holds each picture to the vsync it was meant for.
 * 0 until the module knows the display's vsync period. Follows the display
 * rate: call it per frame (output thread; thread safe against the rest).
 */
MVC3D_EXPORT int64_t mvc3d_present_lead_ns(const mvc3d* h);

#ifdef __cplusplus
}
#endif

#endif
