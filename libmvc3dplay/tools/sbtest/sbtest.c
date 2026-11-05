// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sbtest - does the Amlogic HWC show the legacy amvideo path (amstream decoder -> vfm "default" ->
 * amvideo) under an app window that carries an Amlogic sideband stream?
 *
 * NativeActivity: when the window comes, it gets a sideband stream handle like
 * am_gralloc_create_sideband_handle(type, channel) builds it (native_handle: 0 fds, 3 ints
 * {0xabcdcdef, type bit, channel}; type 1 TV 0x10, 2 OMX 0x20, 3 AMCODEC 0x40, 241 0x400) through
 * ANativeWindow perform(NATIVE_WINDOW_SET_SIDEBAND_STREAM).
 *   adb shell setprop debug.sbtest.type 3; setprop debug.sbtest.channel 0
 * Log: adb logcat -s sbtest
 */
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#define TAG "sbtest"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define NATIVE_WINDOW_SET_SIDEBAND_STREAM 18

/* system/window.h ANativeWindow up to perform (32 bit layout checked by the magic) */
struct anw_priv {
	int magic, version;
	void *reserved[4];
	void (*incRef)(void *), (*decRef)(void *);
	const uint32_t flags;
	const int min_swap, max_swap;
	const float xdpi, ydpi;
	intptr_t oem[4];
	int (*setSwapInterval)(void *, int);
	void *dequeue_dep, *lock_dep, *queue_dep;
	int (*query)(const void *, int, int *);
	int (*perform)(void *, int, ...);
};

struct sb_handle {
	int version, num_fds, num_ints;
	int data[3];
};

static int prop_int(const char *name, int def)
{
	char v[PROP_VALUE_MAX] = "";
	return __system_property_get(name, v) > 0 ? atoi(v) : def;
}

static struct sb_handle sb;

static void on_window(ANativeActivity *a, ANativeWindow *w)
{
	struct anw_priv *p = (struct anw_priv *)w;
	int type = prop_int("debug.sbtest.type", 3), ch = prop_int("debug.sbtest.channel", 0);
	int bit = type == 1 ? 0x10 : type == 2 ? 0x20 : type == 241 ? 0x400 : 0x40;

	if (p->magic != (('_' << 24) | ('w' << 16) | ('n' << 8) | 'd')) {
		LOG("unexpected ANativeWindow magic 0x%x", p->magic);
		return;
	}
	sb.version = sizeof(int) * 3;
	sb.num_fds = 0;
	sb.num_ints = 3;
	sb.data[0] = (int)0xabcdcdef;
	sb.data[1] = bit;
	sb.data[2] = ch;
	int r = p->perform(w, NATIVE_WINDOW_SET_SIDEBAND_STREAM, &sb);
	LOG("window %dx%d: sideband type %d (0x%x) channel %d -> %d", ANativeWindow_getWidth(w),
	    ANativeWindow_getHeight(w), type, bit, ch, r);
}

static void on_window_gone(ANativeActivity *a, ANativeWindow *w)
{
	LOG("window destroyed");
}

void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size)
{
	a->callbacks->onNativeWindowCreated = on_window;
	a->callbacks->onNativeWindowDestroyed = on_window_gone;
	LOG("created");
}
