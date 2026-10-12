// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Installation of the mvc3d kernel module bundled with this library
 * (mvc3d_module_install): the module is loaded with root (su) and installed
 * into the Dune HD firmware's config directory together with a boot script,
 * which the firmware runs as root at every boot (R24: binit.sh, R22: the
 * /system/xbin/dunehd/init service itself). A newer module is never
 * replaced by an older one.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>

#include "mvc3dplay.h"
#include "mvc3d_uapi.h"

#define TAG "mvc3dplay"

#ifdef MVC3D_HAVE_KO
/* the module (module_blob.S) */
extern const unsigned char mvc3d_ko_start[], mvc3d_ko_end[];
#endif

/* the Dune firmware's switch for su (its sud daemon): "" (default) or "1" on */
#define ROOT_PROP "persist.vendor.root_access"

/*
 * run as root: $1 = the module to install. One module build for all firmware
 * versions: their kernel only warns about symbol CRC mismatches ("disagrees
 * about version of symbol") and loads it anyway; the warnings go to the log.
 */
static const char INSTALL_SCRIPT[] =
	"#!/system/bin/sh\n"
	"KO=\"$1\"\n"
	"FSP=$(sed -n 's/^FSP=//p' /system/xbin/dunehd/init 2>/dev/null | head -1)\n"
	"[ -n \"$FSP\" ] || FSP=/data/data/com.dunehd.app\n"
	"grep -qs 'config/boot' /system/dunehd/firmware/scripts/binit.sh /system/xbin/dunehd/init ||\n"
	"  { echo \"no Dune HD firmware boot hook (binit.sh, dunehd/init)\"; exit 2; }\n"
	"D=$FSP/config\n"
	"[ -d \"$D\" ] || { echo \"$D missing\"; exit 2; }\n"
	"echo \"firmware $(grep -h firmware_version \"$D/last_fw_info.txt\" 2>/dev/null), $(getprop ro.product.model), kernel $(uname -r)\"\n"
	"mkdir -p \"$D/mvc3d\" \"$D/boot\" && chmod 755 \"$D/mvc3d\" \"$D/boot\" || exit 3\n"
	"INST=\"$D/mvc3d/mvc3d.ko\"\n"
	"\n"
	"# an older module: replaced (not in use while nothing plays 3D)\n"
	"if [ -d /sys/module/mvc3d ]; then\n"
	"  rmmod mvc3d || { echo \"the loaded module is in use, try again after a reboot\"; exit 4; }\n"
	"fi\n"
	"b=$(dmesg | grep -c 'mvc3d: disagrees about version')\n"
	"if ! insmod \"$KO\"; then\n"
	"  echo \"insmod failed:\"\n"
	"  dmesg | grep -i mvc3d | tail -3\n"
	"  [ -f \"$INST\" ] && insmod \"$INST\" 2>/dev/null && echo \"the installed module is loaded again\"\n"
	"  exit 5\n"
	"fi\n"
	"a=$(dmesg | grep -c 'mvc3d: disagrees about version')\n"
	"if [ \"$a\" -gt \"$b\" ]; then\n"
	"  echo \"note: symbol versions differ from the build firmware (loaded anyway):\"\n"
	"  dmesg | grep 'mvc3d: disagrees about version' | tail -n $((a - b)) | head -10\n"
	"fi\n"
	"dmesg | grep -E 'mvc3d: no probe' | tail -3\n"
	"if ! cmp -s \"$KO\" \"$INST\"; then\n"
	"  cp \"$KO\" \"$INST.tmp\" && chmod 644 \"$INST.tmp\" && mv \"$INST.tmp\" \"$INST\" || exit 6\n"
	"  echo \"module installed to $INST\"\n"
	"fi\n"
	"\n"
	"cat > \"$D/boot/mvc3d.sh.tmp\" <<EOS\n"
	"#!/system/bin/sh\n"
	"# 3D MVC frame packed output kernel module (mvc3d)\n"
	"insmod $INST || exit 1\n"
	"# ueventd creates the device node asynchronously\n"
	"i=0\n"
	"while [ ! -e /dev/mvc3d ] && [ \\$i -lt 50 ]; do sleep 0.1; i=\\$((i + 1)); done\n"
	"chmod 666 /dev/mvc3d\n"
	"EOS\n"
	"if cmp -s \"$D/boot/mvc3d.sh.tmp\" \"$D/boot/mvc3d.sh\"; then\n"
	"  rm -f \"$D/boot/mvc3d.sh.tmp\"\n"
	"else\n"
	"  mv \"$D/boot/mvc3d.sh.tmp\" \"$D/boot/mvc3d.sh\" && echo \"boot script installed\"\n"
	"fi\n"
	"chmod 755 \"$D/boot/mvc3d.sh\"\n"
	"i=0\n"
	"while [ ! -e /dev/mvc3d ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done\n"
	"chmod 666 /dev/mvc3d || exit 7\n"
	"echo \"mvc3d $(cat /sys/module/mvc3d/version 2>/dev/null) ready\"\n";

static void logi(const char *fmt, const char *a, const char *b)
{
	__android_log_print(ANDROID_LOG_INFO, TAG, fmt, a, b);
}

/* runs a shell command; its output lines to the log under tag; the first line to out */
static int run(const char *cmd, const char *tag, char *out, size_t out_size)
{
	char line[256], full[1024];
	FILE *p;
	int first = 1;

	snprintf(full, sizeof(full), "%s 2>&1", cmd);
	p = popen(full, "r");
	if (!p)
		return -1;
	if (out && out_size)
		out[0] = 0;
	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\r\n")] = 0;
		if (tag)
			logi("%s: %s", tag, line);
		if (out && out_size && first)
			snprintf(out, out_size, "%s", line);
		first = 0;
	}
	return pclose(p);
}

static int su_works(void)
{
	return run("timeout 10 su -c true", NULL, NULL, 0) == 0;
}

static void read_line(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");

	buf[0] = 0;
	if (f) {
		if (fgets(buf, size, f))
			buf[strcspn(buf, "\r\n")] = 0;
		fclose(f);
	}
}

/* the bundled module or a newer one is loaded and usable */
static int ready(void)
{
	char v[64];

	read_line("/sys/module/mvc3d/version", v, sizeof(v));
	/* versions are dates (YYYY.MM.DD[.n]): compare as strings */
	return v[0] && strcmp(v, MVC3D_VERSION) >= 0 && access(MVC3D_DEV, R_OK | W_OK) == 0;
}

/* the app's private directory: /data/data/<package>/files */
static int app_dir(char *buf, size_t size)
{
	char pkg[256];
	char *colon;

	read_line("/proc/self/cmdline", pkg, sizeof(pkg));
	colon = strchr(pkg, ':');	/* a process of the app (pkg:name) */
	if (colon)
		*colon = 0;
	if (!pkg[0] || strchr(pkg, '/'))
		return -1;
	snprintf(buf, size, "/data/data/%s/files", pkg);
	mkdir(buf, 0700);
	return access(buf, W_OK);
}

static void set_msg(char *msg, size_t size, const char *text)
{
	if (msg && size)
		snprintf(msg, size, "%s", text);
}

static int write_file(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return -1;
	ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok ? 0 : -1;
}

int mvc3d_module_install(const char *dir, mvc3d_ask_fn ask, void *opaque, char *msg,
			 size_t msg_size)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static int result = 1;	/* > 0: not tried in this process */
	int ret;

	pthread_mutex_lock(&lock);
	set_msg(msg, msg_size, "");
	if (ready()) {
		ret = MVC3D_MODULE_READY;
		goto out;
	}
#ifndef MVC3D_HAVE_KO
	set_msg(msg, msg_size, "no kernel module in this build");
	ret = MVC3D_MODULE_FAILED;
#else
	{
		char d[512], ko[600], script[600], declined[600], cmd[1400], prev[64] = "", v[64];
		int switched = 0, rc, i;

		if (result <= 0) {
			/* once per process */
			set_msg(msg, msg_size, result == MVC3D_MODULE_DECLINED ? "installation declined"
						: "installation failed (see the log)");
			ret = result;
			goto out;
		}
		if (dir && *dir)
			snprintf(d, sizeof(d), "%s", dir);
		else if (app_dir(d, sizeof(d))) {
			set_msg(msg, msg_size, "no app directory for the installation");
			ret = result = MVC3D_MODULE_FAILED;
			goto out;
		}
		snprintf(ko, sizeof(ko), "%s/mvc3d.ko", d);
		snprintf(script, sizeof(script), "%s/mvc3d_install.sh", d);
		snprintf(declined, sizeof(declined), "%s/mvc3d_root_declined", d);
		if (write_file(ko, mvc3d_ko_start, mvc3d_ko_end - mvc3d_ko_start) ||
		    write_file(script, INSTALL_SCRIPT, sizeof(INSTALL_SCRIPT) - 1)) {
			set_msg(msg, msg_size, "cannot write the installation files");
			ret = result = MVC3D_MODULE_FAILED;
			goto out;
		}
		if (access("/system/bin/su", X_OK) && access("/system/xbin/su", X_OK)) {
			__android_log_print(ANDROID_LOG_WARN, TAG, "no su: the kernel module cannot be installed");
			set_msg(msg, msg_size, "the kernel module cannot be installed: no su on this player");
			ret = result = MVC3D_MODULE_NO_ROOT;
			goto out;
		}
		/*
		 * su answers only while the firmware's root access is on: ask the
		 * user to switch it on for the installation (an app may set the
		 * property on this firmware), then restore the previous state
		 */
		if (!su_works()) {
			read_line(declined, v, sizeof(v));
			if (!strcmp(v, MVC3D_VERSION)) {
				logi("%s%s", "root access off, the module installation was declined earlier", "");
				set_msg(msg, msg_size, "installation declined");
				ret = result = MVC3D_MODULE_DECLINED;
				goto out;
			}
			if (!ask || ask(opaque, "3D MVC",
					"To play 3D Blu-ray (MVC) in 3D, a kernel module is installed once. "
					"This needs root access, which is switched off on this player.\n\n"
					"Switch root access on for the installation? It is switched off "
					"again afterwards.") <= 0) {
				write_file(declined, MVC3D_VERSION "\n", strlen(MVC3D_VERSION) + 1);
				logi("%s%s", "root access off, the module installation declined", "");
				set_msg(msg, msg_size, "not installed: 3D MVC plays in 2D");
				ret = result = MVC3D_MODULE_DECLINED;
				goto out;
			}
			run("getprop " ROOT_PROP, NULL, prev, sizeof(prev));
			run("setprop " ROOT_PROP " 1", "root access", NULL, 0);
			switched = 1;
			for (i = 0; i < 20 && !su_works(); i++)
				usleep(500000);
			if (i == 20) {
				snprintf(cmd, sizeof(cmd), "setprop " ROOT_PROP " '%s'", prev);
				run(cmd, "root access", NULL, 0);
				set_msg(msg, msg_size, "root access could not be switched on");
				ret = result = MVC3D_MODULE_NO_ROOT;
				goto out;
			}
			logi("root access switched on (was '%s')%s", prev, "");
		}
		snprintf(cmd, sizeof(cmd), "timeout 90 su -c 'sh %s %s'", script, ko);
		rc = run(cmd, "module install", NULL, 0);
		if (switched) {
			snprintf(cmd, sizeof(cmd), "setprop " ROOT_PROP " '%s'", prev);
			run(cmd, "root access", NULL, 0);
			logi("root access restored to '%s'%s", prev, "");
		}
		unlink(ko);
		if (!ready()) {
			__android_log_print(ANDROID_LOG_WARN, TAG,
					    "kernel module installation failed (status %d)", rc);
			set_msg(msg, msg_size, "kernel module not installed (see the log, tag mvc3dplay)");
			ret = result = MVC3D_MODULE_FAILED;
			goto out;
		}
		unlink(declined);
		logi("kernel module %s installed%s", MVC3D_VERSION, "");
		set_msg(msg, msg_size, "kernel module installed");
		ret = result = MVC3D_MODULE_INSTALLED;
	}
#endif
out:
	pthread_mutex_unlock(&lock);
	return ret;
}

const char *mvc3d_bundled_module_version(void)
{
#ifdef MVC3D_HAVE_KO
	return MVC3D_VERSION;
#else
	return "";
#endif
}
