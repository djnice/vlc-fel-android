// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3dplay - the player side of 3D MVC playback (see mvc3dplay.h): starts
 * mvc3dhelper (a static aarch64 program shipped as libmvc3dhelper.so next to
 * this library, the player's userspace being 32-bit) and streams the access
 * units the player queues to MediaCodec to it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <android/log.h>

#include "mvc3dplay.h"
#include "mvc3d_proto.h"

#define TAG "mvc3dplay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MVC3D_LIB_VERSION "2026.10.12"
#define HELPER_NAME "libmvc3dhelper.so"
#define MODULE_DEV "/dev/mvc3d"

struct mvc3d {
	pid_t pid;
	int sock;		/* the helper's stdin */
	int err_fd;		/* its stderr */
	pthread_t log_thread;
	int64_t next_au;	/* number of the next access unit queued */
	int dead;
	pthread_mutex_t lock;	/* the socket: decoder and output threads */
	int64_t lead_ns;	/* last told to the helper */
	struct timespec lead_checked;
};

int mvc3d_api_version(void) { return MVC3D_API_VERSION; }
const char* mvc3d_version(void) { return MVC3D_LIB_VERSION; }

/* the helper next to this library (the app's native library directory) */
static int helper_path(char* buf, size_t size)
{
	Dl_info info;
	const char* slash;

	if (!dladdr((void*)mvc3d_open, &info) || !info.dli_fname)
		return -1;
	slash = strrchr(info.dli_fname, '/');
	if (!slash || (size_t)(slash - info.dli_fname) + sizeof(HELPER_NAME) + 1 > size)
		return -1;
	memcpy(buf, info.dli_fname, slash - info.dli_fname + 1);
	strcpy(buf + (slash - info.dli_fname + 1), HELPER_NAME);
	return 0;
}

int mvc3d_available(void)
{
	char path[512];

	return access(MODULE_DEV, R_OK | W_OK) == 0 && helper_path(path, sizeof(path)) == 0 &&
	       access(path, X_OK) == 0;
}

int mvc3d_probe(const uint8_t* p, size_t size)
{
	for (size_t i = 0; i + 3 < size; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			int type = p[i + 3] & 0x1f;

			if (type == 15 || type == 20)
				return 1;
			i += 2;
		}
	}
	return 0;
}

int mvc3d_display_3d(void)
{
	char c = '0';
	int f = open("/sys/class/amhdmitx/amhdmitx0/support_3d", O_RDONLY | O_CLOEXEC);

	if (f >= 0) {
		if (read(f, &c, 1) != 1)
			c = '0';
		close(f);
	}
	return c == '1';
}

static void* log_thread_fn(void* arg)
{
	mvc3d* h = arg;
	char buf[1024];
	size_t n = 0;

	for (;;) {
		ssize_t r = read(h->err_fd, buf + n, sizeof(buf) - 1 - n);

		if (r <= 0)
			break;
		n += r;
		for (;;) {
			char* nl = memchr(buf, '\n', n);

			if (!nl) {
				if (n == sizeof(buf) - 1) {
					buf[n] = 0;
					LOGI("helper: %s", buf);
					n = 0;
				}
				break;
			}
			*nl = 0;
			LOGI("helper: %s", buf);
			n -= nl + 1 - buf;
			memmove(buf, nl + 1, n);
		}
	}
	return NULL;
}

/* one line from fd within timeout_ms; 0 on success */
static int read_line(int fd, char* buf, size_t size, int timeout_ms)
{
	size_t n = 0;
	struct timespec t0, t;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (n + 1 < size) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int left;

		clock_gettime(CLOCK_MONOTONIC, &t);
		left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
		if (left <= 0 || poll(&pfd, 1, left) <= 0)
			return -1;
		if (read(fd, buf + n, 1) != 1)
			return -1;
		if (buf[n] == '\n') {
			buf[n] = 0;
			return 0;
		}
		n++;
	}
	return -1;
}

static int send_all(mvc3d* h, const void* data, size_t size)
{
	const uint8_t* p = data;

	while (size && !h->dead) {
		ssize_t r = send(h->sock, p, size, MSG_NOSIGNAL);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			LOGE("helper gone (%s): 3D off", strerror(errno));
			h->dead = 1;
			return -1;
		}
		p += r;
		size -= r;
	}
	return h->dead ? -1 : 0;
}

static void send_msg(mvc3d* h, uint32_t type, const uint8_t* data, size_t size, int64_t pts,
		     int64_t au, int64_t arg)
{
	struct m3p_hdr hdr = { M3P_MAGIC, type, (uint32_t)size, 0, pts, au, arg };

	if (!h || h->dead)
		return;
	pthread_mutex_lock(&h->lock);
	if (!send_all(h, &hdr, sizeof(hdr)) && size)
		send_all(h, data, size);
	pthread_mutex_unlock(&h->lock);
}

mvc3d* mvc3d_open(int width, int height)
{
	char path[512], arg_w[16], arg_h[16], line[256];
	int in[2], out[2], err[2], fp;
	mvc3d* h;
	pid_t pid;

	if ((width != 1920 || height != 1080) && (width != 1280 || height != 720)) {
		LOGW("%dx%d: no frame packing for this size", width, height);
		return NULL;
	}
	if (helper_path(path, sizeof(path))) {
		LOGE("no helper path");
		return NULL;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in))
		return NULL;
	if (pipe2(out, O_CLOEXEC)) {
		close(in[0]);
		close(in[1]);
		return NULL;
	}
	if (pipe2(err, O_CLOEXEC)) {
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		return NULL;
	}
	{
		/* access units are large: a big send buffer keeps the decoder thread from blocking */
		int sz = 4 << 20;

		setsockopt(in[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		setsockopt(in[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	}
	snprintf(arg_w, sizeof(arg_w), "%d", width);
	snprintf(arg_h, sizeof(arg_h), "%d", height);
	/* a 3D display: HDMI frame packing (else the frame packed picture goes out as 2D) */
	fp = mvc3d_display_3d();

	pid = fork();
	if (pid == 0) {
		/* async-signal-safe only from here on */
		char* argv[] = { path, arg_w, arg_h, fp ? "fp" : "2d", NULL };
		char* envp[] = { NULL };

		dup2(in[1], 0);
		dup2(out[1], 1);
		dup2(err[1], 2);
		execve(path, argv, envp);
		_exit(127);
	}
	close(in[1]);
	close(out[1]);
	close(err[1]);
	if (pid < 0) {
		LOGE("fork: %s", strerror(errno));
		close(in[0]);
		close(out[0]);
		close(err[0]);
		return NULL;
	}
	h = calloc(1, sizeof(*h));
	if (!h) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		close(in[0]);
		close(out[0]);
		close(err[0]);
		return NULL;
	}
	pthread_mutex_init(&h->lock, NULL);
	h->pid = pid;
	h->sock = in[0];
	h->err_fd = err[0];
	pthread_create(&h->log_thread, NULL, log_thread_fn, h);

	if (read_line(out[0], line, sizeof(line), 3000) || strncmp(line, "ready ", 6)) {
		LOGE("helper %s did not start: %s", path, line[0] ? line : "no answer");
		close(out[0]);
		h->dead = 1;
		mvc3d_close(h);
		return NULL;
	}
	close(out[0]);
	LOGI("helper started (pid %d): %s", pid, line);
	return h;
}

void mvc3d_close(mvc3d* h)
{
	int status, i;

	if (!h)
		return;
	send_msg(h, M3P_QUIT, NULL, 0, 0, 0, 0);
	shutdown(h->sock, SHUT_WR);
	for (i = 0; i < 100 && waitpid(h->pid, &status, WNOHANG) == 0; i++)
		usleep(10000);
	if (i == 100) {
		LOGW("helper did not quit: killed");
		kill(h->pid, SIGKILL);
		waitpid(h->pid, &status, 0);
	}
	close(h->sock);
	pthread_join(h->log_thread, NULL);
	close(h->err_fd);
	LOGI("helper stopped");
	pthread_mutex_destroy(&h->lock);
	free(h);
}

void mvc3d_commit(mvc3d* h, const uint8_t* au, size_t size, int64_t pts_us)
{
	if (!h)
		return;
	send_msg(h, M3P_AU, au, size, pts_us, h->next_au, 0);
	h->next_au++;
}

void mvc3d_config(mvc3d* h, const uint8_t* data, size_t size)
{
	send_msg(h, M3P_AU, data, size, 0, -1, 0);
}

void mvc3d_flush(mvc3d* h)
{
	if (!h)
		return;
	send_msg(h, M3P_FLUSH, NULL, 0, 0, 0, 0);
	/* the Amlogic decoder numbers its input from 0 again after a flush too */
	h->next_au = 0;
}

void mvc3d_restart(mvc3d* h)
{
	if (!h)
		return;
	send_msg(h, M3P_FLUSH, NULL, 0, 0, 0, 0);
	h->next_au = 0;
}

static int read_int(const char* path)
{
	char buf[32];
	int f = open(path, O_RDONLY | O_CLOEXEC), n = 0;

	if (f >= 0) {
		ssize_t r = read(f, buf, sizeof(buf) - 1);

		if (r > 0) {
			buf[r] = 0;
			n = atoi(buf);
		}
		close(f);
	}
	return n;
}

/*
 * the module's suggested lead (out_lead_ms) in whole vsyncs of the display
 * (vsync_us, measured by the module while frames go through); looked up
 * twice a second, told to the helper (the module) when it changes
 */
int64_t mvc3d_present_lead_ns(const mvc3d* hc)
{
	mvc3d* h = (mvc3d*)hc;
	struct timespec now;

	if (!h || h->dead)
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if ((now.tv_sec - h->lead_checked.tv_sec) * 1000 +
	    (now.tv_nsec - h->lead_checked.tv_nsec) / 1000000 >= 500) {
		int lead_ms = read_int("/sys/module/mvc3d/parameters/out_lead_ms");
		int vs = read_int("/sys/module/mvc3d/parameters/vsync_us");
		int64_t lead = 0;

		h->lead_checked = now;
		if (lead_ms > 0 && vs >= 4000 && vs <= 100000) {
			int64_t n = ((int64_t)lead_ms * 1000 + vs / 2) / vs;

			lead = n * vs * INT64_C(1000);
		}
		/* the measured period wanders by a few us: only real changes */
		if (llabs(lead - h->lead_ns) > 1000000) {
			LOGI("lead %lld us (vsync %d us)", (long long)(lead / 1000), vs);
			send_msg(h, M3P_LEAD, NULL, 0, 0, 0, lead / 1000);
			h->lead_ns = lead;
		}
	}
	return h->lead_ns;
}
